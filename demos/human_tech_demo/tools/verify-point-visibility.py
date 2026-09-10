# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Capture/verify finite practical shadows, including deliberately disabled controls.

These prove local voxel surface visibility; they do not establish exact thin-wall
visibility throughout the coarse8m field or beyond the known world volumes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def configurations():
    prefix = 'indirect-proof-practical-visibility-'
    for case in ('wall', 'thin', 'oblique', 'contact', 'boundary', 'radius',
                 'open', 'aperture', 'behind', 'endpoint', 'cavity'):
        for mode in ('on', 'off'):
            # Bright adjacent surfaces otherwise scatter post-process bloom
            # into these close shadow ROIs, independent of physical visibility.
            yield case + '-' + mode, prefix + case + '-' + mode, \
                ['--no-indirect', '--no-reflections'] + (['--no-bloom'] if case in ('oblique', 'contact') else [])
    for case in ('wall', 'open', 'endpoint'):
        for mode in ('on', 'off') if case == 'wall' else ('on',):
            yield case + '-disabled-' + mode, prefix + case + '-' + mode, ['--no-indirect', '--no-reflections', '--no-point-shadows']
    for mode in ('on', 'off'):
        yield 'cavity-gi-' + mode, prefix + 'cavity-' + mode, ['--no-reflections']
    for disabled in (False, True):
        for mode in ('on', 'off'):
            yield 'reflected-' + ('disabled-' if disabled else '') + mode, \
                'reflection-proof-practical-visibility-pond-' + mode, \
                ['--no-indirect'] + (['--no-point-shadows'] if disabled else [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    game = Path(__file__).resolve().parents[3]
    demo = Path(__file__).resolve().parents[1]
    parser.add_argument('--binary', type=Path, default=game / 'build/demos/human_tech_demo/human_tech_demo')
    parser.add_argument('--shader-dir', type=Path, default=demo / 'shaders')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-existing', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.verify_existing:
        record = {'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  'shaders': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted(args.shader_dir.glob('*.wgsl'))}, 'captures': []}
        def sanitize(value):
            for original, replacement in ((str(args.output.resolve()), '<proof-output>'),
                                          (str(args.shader_dir.resolve()), '<shader-directory>'),
                                          (str(args.binary.parent.resolve()), '<binary-directory>'),
                                          (str(game), '<game-worktree>')):
                value = value.replace(original, replacement)
            return value
        for label, asset, flags in configurations():
            command = [str(args.binary), '--shader-dir', str(args.shader_dir), '--hidden',
                       '--sky', 'authored', '--night', '--width', '1280', '--height', '720',
                       '--tex-size', '512', '--frames', '8', '--msaa', '4', '--asset', asset,
                       '--no-shadows', '--no-ssao', '--no-taa', '--capture',
                       str(args.output / (label + '.png'))] + flags
            print('Capturing ' + label, flush=True)
            result = subprocess.run(command, capture_output=True, text=True, timeout=600)
            log = sanitize(result.stdout + result.stderr)
            (args.output / (label + '.log')).write_text(log)
            if result.returncode or '[gpu-failure]' in log or 'Validation Error' in log or '8 fixed-time frames' not in log:
                raise RuntimeError(label + ' failed: ' + log[-3000:])
            record['captures'].append({'label': label, 'arguments': [sanitize(a) for a in command[1:]],
                                       'image_sha256': hashlib.sha256((args.output / (label + '.png')).read_bytes()).hexdigest()})
        (args.output / 'captures.json').write_text(json.dumps(record, indent=2) + '\n')
    spec = importlib.util.spec_from_file_location('transport_verifier', Path(__file__).with_name('verify-renderer-transport.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cache = {}
    def pixels(name):
        if name not in cache:
            w, h, data = module.pixels(args.output / (name + '.png'))
            if (w, h) != (1280, 720):
                raise ValueError('This physical fixture uses1280x720 camera/ROI coordinates')
            cache[name] = data
        return cache[name]
    def difference(a, b, roi):
        aa, bb = pixels(a), pixels(b)
        x, y, w, h = roi
        errors = [max(abs(aa[(yy * 1280 + xx) * 3 + c] - bb[(yy * 1280 + xx) * 3 + c]) for c in range(3))
                  for yy in range(y, y + h) for xx in range(x, x + w)]
        return {'max_error_255': max(errors), 'pixels_above3': sum(d > 3 for d in errors),
                'sum_max_channel_error': sum(errors), 'roi': roi}
    centre = (630, 350, 20, 20)
    report = {'checks': {}, 'reflection_controls': {}, 'scope': __doc__}
    for case in ('wall', 'thin', 'oblique', 'contact', 'boundary', 'radius', 'cavity', 'cavity-gi'):
        result = difference(case + '-on', case + '-off', centre)
        result['passed'] = result['max_error_255'] == 0
        report['checks'][case + ' occluded/null receiver'] = result
    for case in ('open', 'aperture', 'behind', 'endpoint', 'wall-disabled'):
        result = difference(case + '-on', case + '-off', centre)
        result['passed'] = result['max_error_255'] > 10
        report['checks'][case + ' physically reaching receiver'] = result
    for case in ('open', 'endpoint'):
        result = difference(case + '-on', case + '-disabled-on', (400, 310, 500, 330))
        result['passed'] = result['max_error_255'] == 0
        report['checks'][case + ' no false floor self-shadow'] = result
    for x, material in ((505, 'diffuse'), (640, 'occupied'), (775, 'true_glass')):
        # Reflected source lies above the receiver; actual horizontal blocker is
        # also above it. Lower pane portions are shadowed in the mirrored view.
        roi = (x - 18, 585, 36, 20)
        active = difference('reflected-on', 'reflected-off', roi)
        disabled = difference('reflected-disabled-on', 'reflected-disabled-off', roi)
        report['reflection_controls'][material] = {'active': active, 'disabled': disabled,
            'passed': active['max_error_255'] <= 1 and disabled['max_error_255'] > 3}
    report['passed'] = all(c['passed'] for key in ('checks', 'reflection_controls') for c in report[key].values())
    (args.output / 'visibility-proof.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
