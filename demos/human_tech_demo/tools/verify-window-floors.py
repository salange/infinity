# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Matched native controls for separable occupied-floor filtering.

Baseline shaders are the preserved stage23 shader with diagnostics only added.
Its Frame ABI must match the supplied fixture-capable binary. The independent
CPU quadrature test supplies the continuous integral/whole-floor energy oracle;
these raw native controls verify endpoints, physical mullions, footprint masks,
and the conditional floor response on actual tilted panes at three rolls.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    demo = Path(__file__).resolve().parents[1]
    game = demo.parents[1]
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--baseline-shaders', type=Path, required=True)
    parser.add_argument('--candidate-shaders', type=Path, default=demo / 'shaders')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-existing', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output / 'window-floor-proof.json'
    report = json.loads(path.read_text()) if args.verify_existing and path.exists() else {}
    spec = importlib.util.spec_from_file_location('pixels', Path(__file__).with_name('verify-renderer-transport.py'))
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    def sanitize(value):
        for original, replacement in ((str(args.output.resolve()), '<proof-output>'),
                                      (str(args.binary.resolve()), '<matching-binary>'),
                                      (str(args.baseline_shaders.resolve()), '<baseline-diagnostics>'),
                                      (str(args.candidate_shaders.resolve()), '<candidate-shaders>'),
                                      (str(game.resolve()), '<game-worktree>')):
            value = value.replace(original, replacement)
        return value
    jobs = [('endpoints', v, 16) for v in ('baseline', 'candidate')]
    jobs += [('oblique', v, d) for d in (16, 19) for v in ('baseline', 'candidate')]
    jobs += [('oblique', 'candidate', 20)]
    images = {}
    if not args.verify_existing:
        report['captures'] = []
        report['binary_sha256'] = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    for fixture, variant, debug in jobs:
        label = f'{fixture}-{variant}-{debug}'
        if not args.verify_existing:
            shaders = args.baseline_shaders if variant == 'baseline' else args.candidate_shaders
            asset = 'indirect-proof-window-' + ('filter-on' if fixture == 'endpoints' else 'oblique-on')
            command = [str(args.binary.resolve()), '--shader-dir', str(shaders.resolve()),
                       '--hidden', '--sky', 'authored', '--night', '--width', '1280', '--height', '720',
                       '--tex-size', '512', '--frames', '2', '--msaa', '4', '--asset', asset,
                       '--debug', str(debug), '--no-indirect', '--no-reflections', '--no-shadows',
                       '--no-point-shadows', '--no-ssao', '--no-taa', '--no-bloom',
                       '--capture', str((args.output / (label + '.png')).resolve())]
            print('Capturing ' + label, flush=True)
            result = subprocess.run(command, capture_output=True, text=True, timeout=600)
            log = sanitize(result.stdout + result.stderr)
            (args.output / (label + '.log')).write_text(log)
            if result.returncode or '[gpu-failure]' in log or 'Validation Error' in log or '2 fixed-time frames' not in log:
                raise RuntimeError(label + ' failed; see its retained log')
            report['captures'].append({'label': label, 'arguments': [sanitize(v) for v in command[1:]],
                                       'shader_sha256': hashlib.sha256((shaders / 'main.wgsl').read_bytes()).hexdigest()})
        w, h, rgb = reader.pixels(args.output / (label + '.png'))
        if (w, h) != (1280, 720):
            raise ValueError('fixture requires 1280x720')
        images[label] = rgb
    cases = []
    for row in range(5):
        for pi, probability in enumerate((.045, .19, .5)):
            for ri, roll in enumerate((0, 27, 63)):
                x0, y0 = 24 + (pi * 3 + ri) * 139, 28 + row * 136
                indices = [(y * 1280 + x) * 3 for y in range(y0, y0 + 112) for x in range(x0, x0 + 116)]
                a, b = images['endpoints-baseline-16'], images['endpoints-candidate-16']
                grid = max(abs(a[i + 1] - b[i + 1]) for i in indices)
                delta = max(abs(a[i] - b[i]) for i in indices)
                expectation = .725 * (.1 + .75 * .78 * probability)
                error = max(abs(b[i] / 255 - expectation) for i in indices)
                cases.append({'kind': 'legacy endpoint', 'row': row, 'p': probability, 'roll': roll,
                              'mullion_max_byte_difference': grid, 'occupancy_max_byte_difference': delta,
                              'unresolved_expectation_error': error if row < 2 else None,
                              'passed': grid == 0 and (row not in (0, 1, 4) or delta == 0)
                                        and (row > 1 or error <= 1 / 255 + .0001)})
    conditional_count = 0
    for row in range(5):
        for pi, probability in enumerate((.045, .19, .5)):
            for ri, roll in enumerate((0, 27, 63)):
                x0, y0 = 24 + (pi * 3 + ri) * 139, 28 + row * 136
                ids = [(y * 1280 + x) * 3 for y in range(y0, y0 + 92) for x in range(x0, x0 + 92)]
                a, b = images['oblique-baseline-16'], images['oblique-candidate-16']
                footprint = images['oblique-candidate-20']
                conditional = [i for i in ids if footprint[i] <= 11 and footprint[i + 2] == 255]
                # A raw fade byte of255 can still represent .998; require
                # quantized axis sizes safely above the exact5.5px endpoint.
                resolved = [i for i in ids if footprint[i] >= 45 and footprint[i + 1] >= 45]
                coarse = [i for i in ids if footprint[i + 1] <= 11]
                grid = max(abs(a[i + 1] - b[i + 1]) for i in ids)
                resolved_delta = max((abs(a[i] - b[i]) for i in resolved), default=0)
                coarse_delta = max((abs(a[i] - b[i]) for i in coarse), default=0)
                expected = .725 * (.1 + .75 * .78 * probability)
                old_error = max((abs(a[i] / 255 - expected) for i in conditional), default=0)
                lo = min((b[i] / 255 for i in conditional), default=.0725)
                hi = max((b[i] / 255 for i in conditional), default=.61625)
                changed = sum(abs(b[i] - a[i]) > 2 for i in conditional)
                conditional_count += changed
                cases.append({'kind': 'actual oblique pane', 'row': row, 'p': probability, 'roll': roll,
                              'conditional_pixels': len(conditional), 'changed_conditional_pixels': changed,
                              'conditional_range': [lo, hi], 'legacy_constant_error': old_error,
                              'fully_resolved_pixels': len(resolved), 'resolved_max_byte_difference': resolved_delta,
                              'fully_unresolved_pixels': len(coarse), 'coarse_max_byte_difference': coarse_delta,
                              'mullion_max_byte_difference': grid,
                              'passed': grid == 0 and resolved_delta == 0 and coarse_delta == 0
                                        and old_error <= 1 / 255 and lo >= .0725 - 1 / 255
                                        and hi <= .61625 + 1 / 255})
    report['cases'] = cases
    report['changed_conditional_pixels'] = conditional_count
    report['passed'] = all(c['passed'] for c in cases) and conditional_count > 1000
    report['scope'] = ('RGBA8 raw fields. Native checks preserve legacy resolved/unresolved occupancy and every '
                       'physical mullion sample, and show restored vertical-floor response in truly foreshortened '
                       'geometry at three rolls. Debug20 selects actual derivative footprints, not nominal panel '
                       'sizes. Debug19 retains lit/gradient/joint evidence. Whole-bank energy conservation and '
                       'joint integration use the independent CPU quadrature test; a finite stochastic panel is '
                       'not expected to equal the unconditional infinite-bank mean. No city visual acceptance.')
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': len(cases),
                      'changed_conditional_pixels': conditional_count,
                      'failed': [c for c in cases if not c['passed']]}, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
