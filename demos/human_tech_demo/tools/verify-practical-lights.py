# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Native practical-light reach/optical proof, or CPU audit of saved raw images.

Primary fixtures use the daytime preset; reflections use the normal night
preset to resolve two successive dielectric responses without raising source
power. Point shadows are explicitly disabled to isolate source lists and BRDFs.
The real-wall control records that deliberate negative control; independent
verify-point-visibility.py tests the production visibility path.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def configurations():
    for suffix in ('on', 'off', 'reference-on', 'reversed-on'):
        yield 'selection-' + suffix, 'indirect-proof-practical-selection-' + suffix, ['--no-reflections']
    for prefix in ('wall', 'glass', 'glass-zero', 'glass-beyond'):
        for mode in ('on', 'off'):
            yield prefix + '-' + mode, 'indirect-proof-practical-' + prefix + '-' + mode, ['--no-reflections']
    for plane in ('ocean', 'puddle', 'pond'):
        for disabled in (False, True):
            for mode in ('on', 'off'):
                yield (plane + ('-disabled' if disabled else '') + '-' + mode,
                       'reflection-proof-practical-' + plane + '-' + mode,
                       ['--night'] + (['--no-reflections'] if disabled else []))


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
    spec = importlib.util.spec_from_file_location('transport_verifier', Path(__file__).with_name('verify-renderer-transport.py'))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    if not args.verify_existing:
        record = {'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                  'shader_files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in sorted(args.shader_dir.glob('*.wgsl'))}, 'captures': []}
        def sanitize(text):
            for original, replacement in ((str(game), '<game-worktree>'),
                                          (str(args.output.resolve()), '<proof-output>'),
                                          (str(args.binary.parent.resolve()), '<binary-directory>'),
                                          (str(args.shader_dir.resolve()), '<shader-directory>')):
                text = text.replace(original, replacement)
            return text
        for label, asset, extra in configurations():
            command = [str(args.binary), '--shader-dir', str(args.shader_dir), '--hidden', '--sky', 'authored',
                       '--width', '1280', '--height', '720', '--tex-size', '512', '--frames', '8', '--msaa', '4',
                       '--asset', asset, '--no-indirect', '--no-shadows', '--no-point-shadows', '--no-ssao', '--no-taa',
                       '--capture', str(args.output / (label + '.png'))] + extra
            print('Capturing ' + label, flush=True)
            result = subprocess.run(command, capture_output=True, text=True, timeout=600)
            log = sanitize(result.stdout + result.stderr)
            (args.output / (label + '.log')).write_text(log)
            if result.returncode or '[gpu-failure]' in log or 'Validation Error' in log or '8 fixed-time frames' not in log:
                raise RuntimeError(label + ' failed to complete cleanly')
            record['captures'].append({'label': label, 'arguments': [sanitize(a) for a in command[1:]],
                                       'image_sha256': hashlib.sha256((args.output / (label + '.png')).read_bytes()).hexdigest()})
        (args.output / 'capture-records.json').write_text(json.dumps(record, indent=2) + '\n')

    cache = {}
    def pixels(name):
        if name not in cache:
            w, h, rgb = verifier.pixels(args.output / (name + '.png'))
            if (w, h) != (1280, 720):
                raise ValueError('This physical fixture and its ROIs require 1280x720 raw captures')
            cache[name] = rgb
        return cache[name]
    def difference(a, b, roi=None):
        a, b = pixels(a), pixels(b)
        if roi:
            x, y, w, h = roi
            coords = ((yy * 1280 + xx) * 3 for yy in range(y, y + h) for xx in range(x, x + w))
        else:
            coords = range(0, len(a), 3)
        maximum = changed = total = 0
        for i in coords:
            d = max(abs(a[i + c] - b[i + c]) for c in range(3))
            maximum = max(maximum, d)
            changed += d > 3
            total += d
        return {'max_error_255': maximum, 'pixels_above3': changed, 'sum_max_channel_error': total}

    report = {'checks': {}, 'known_limits': {}, 'reflection_receiver_roi_checks': {},
              'scope': 'Untruncated source reach, actual reflected-view lighting and glass BRDF response. '
                       'No source-power/material/exposure edits. This does not establish point-shadow visibility or final concept parity.'}
    for a, b, identity in [('selection-on', 'selection-reference-on', True),
                           ('selection-on', 'selection-reversed-on', True),
                           ('glass-zero-on', 'glass-zero-off', True),
                           ('glass-beyond-on', 'glass-beyond-off', True),
                           ('selection-on', 'selection-off', False)]:
        d = difference(a, b)
        d['passed'] = d['max_error_255'] == 0 if identity else d['max_error_255'] > 20 and d['pixels_above3'] > 1000
        report['checks'][a + ' vs ' + b] = d
    for x, name in ((525, 'diffuse'), (640, 'occupied'), (755, 'true_glass')):
        d = difference('glass-on', 'glass-off', (x - 30, 315, 60, 90))
        d['passed'] = d['max_error_255'] > 10 and d['pixels_above3'] > 50
        report['checks']['primary ' + name] = d
    for plane in ('ocean', 'puddle', 'pond'):
        d = difference(plane + '-on', plane + '-off')
        d['passed'] = d['max_error_255'] > 3 and d['pixels_above3'] > 100
        report['checks'][plane + ' reflected response'] = d
        d = difference(plane + '-disabled-on', plane + '-disabled-off')
        d['passed'] = d['max_error_255'] == 0
        report['checks'][plane + ' reflection disabled'] = d
        for x, name in ((505, 'diffuse'), (640, 'occupied'), (775, 'true_glass')):
            d = difference(plane + '-on', plane + '-off', (x - 45, 570, 90, 135))
            d['passed'] = d['max_error_255'] > 3 and d['pixels_above3'] > 10
            report['reflection_receiver_roi_checks'][plane + ' ' + name] = d
    report['known_limits']['unshadowed_point_source_wall_control'] = {
        'scene': 'A real wall intersects the source-to-visible-floor-origin segment.',
        'centre_roi': difference('wall-on', 'wall-off', (630, 350, 20, 20)),
        'expected_for_true_visibility': 'The occluded receiver should remain unchanged.',
        'status': 'Point-source visibility explicitly disabled for this source-list/BRDF negative control.',
        'shadow_visibility_accepted': False}
    report['passed'] = all(d['passed'] for group in ('checks', 'reflection_receiver_roi_checks')
                           for d in report[group].values())
    (args.output / 'practical-proof.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
