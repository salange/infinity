# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Capture or verify the 45-pane native occupied-window filtering atlas.

The baseline directory must contain the historical shader with the same debug16
instrumentation. If its storage ABI differs, --baseline-binary must name the
matching historical fixture-capable binary. Do not substitute a different fixture.
Use --verify-existing to audit saved raw images without invoking the GPU.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess


def halton(index, base):
    result, fraction = 0., 1. / base
    while index:
        result += fraction * (index % base)
        index //= base
        fraction /= base
    return result


def finite_grid_reference(roi, room_pixels, roll, samples):
    """Integrate the original pointwise triangular profile on the actual pane.

    This independently checks finite panel phase under rotation, rather than
    reimplementing the shader's box filter. Two deterministic, disjoint Halton
    sequences also expose the integration's observed convergence. Pixel-box
    filtering, packed-aux quantization and RGBA8 rounding are separate limits.
    """
    x, y, w, h = roi
    scale = 4.5 / room_pixels
    c, s = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    totals = [0., 0.]
    for i, (a, b) in enumerate(samples):
        wx = (x + a * w - 640) * scale
        wy = (360 - y - b * h) * scale
        u, v = wx * c + wy * s, -wx * s + wy * c
        du = abs((u / 4.5 + .5) % 1 - .5) * 4.5
        dv = abs((v / 3.6 + .5) % 1 - .5) * 3.6
        totals[int(i >= len(samples) // 2)] += max(0., 1 - min(du, dv) / .04)
    means = [value / (len(samples) / 2) for value in totals]
    return sum(means) / 2, abs(means[0] - means[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    game = Path(__file__).resolve().parents[3]
    parser.add_argument('--binary', type=Path,
                        default=game / 'build/demos/human_tech_demo/human_tech_demo')
    parser.add_argument('--baseline-shaders', type=Path)
    parser.add_argument('--baseline-binary', type=Path,
                        help='Matching historical renderer when its shader storage ABI differs')
    parser.add_argument('--candidate-shaders', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'shaders')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-existing', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location(
        'transport_verifier', Path(__file__).with_name('verify-renderer-transport.py'))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    report_path = args.output / 'window-filter-proof.json'
    report = json.loads(report_path.read_text()) if args.verify_existing and report_path.exists() else {}
    report.update({'resolution': [1280, 720], 'frames': 8, 'cases': []})
    if not args.verify_existing:
        if not args.baseline_shaders:
            parser.error('--baseline-shaders is required for native capture')
        report['binary_sha256'] = hashlib.sha256(args.binary.read_bytes()).hexdigest()
        report['binary_sha256_by_variant'] = {}
        report['shader_sha256'] = {}
    images = {}
    for label, shaders in (('baseline', args.baseline_shaders), ('candidate', args.candidate_shaders)):
        if not args.verify_existing:
            binary = args.baseline_binary if label == 'baseline' and args.baseline_binary else args.binary
            report['binary_sha256_by_variant'][label] = hashlib.sha256(binary.read_bytes()).hexdigest()
            report['shader_sha256'][label] = hashlib.sha256((shaders / 'main.wgsl').read_bytes()).hexdigest()
            command = [str(binary), '--shader-dir', str(shaders), '--hidden', '--sky', 'authored',
                       '--night', '--width', '1280', '--height', '720', '--tex-size', '512',
                       '--frames', '8', '--msaa', '4', '--asset', 'indirect-proof-window-filter-on',
                       '--debug', '16', '--no-indirect', '--no-reflections', '--no-shadows',
                       '--no-ssao', '--no-taa', '--capture', str(args.output / (label + '.png'))]
            print('Capturing ' + label, flush=True)
            result = subprocess.run(command, capture_output=True, text=True, timeout=600)
            def sanitize(text):
                for original, replacement in ((str(game), '<game-worktree>'),
                                              (str(args.output.resolve()), '<proof-output>'),
                                              (str(binary.parent.resolve()), '<binary-directory>'),
                                              (str(shaders.resolve()), '<shader-directory>')):
                    text = text.replace(original, replacement)
                return text
            log = sanitize(result.stdout + result.stderr)
            (args.output / (label + '.log')).write_text(log)
            if result.returncode or '[gpu-failure]' in log or 'Validation Error' in log or '8 fixed-time frames' not in log:
                raise RuntimeError(label + ' did not complete cleanly')
            report[label + '_arguments'] = [sanitize(arg) for arg in command[1:]]
        width, height, rgb = verifier.pixels(args.output / (label + '.png'))
        if (width, height) != (1280, 720):
            raise ValueError('The fixture and proof require 1280x720 raw captures')
        images[label] = rgb

    period_mean = .04 / 4.5 + .04 / 3.6 - 4 * .04 * .04 / (3 * 4.5 * 3.6)
    samples = [(halton(i, 2), halton(i, 3)) for i in range(1, 524289)]
    for row, room_pixels in enumerate((.75, 1.2, 3., 8., 24.)):
        for pi, p in enumerate((.045, .19, .5)):
            for ri, roll in enumerate((0., 27., 63.)):
                x0, y0 = 24 + (pi * 3 + ri) * 139, 28 + row * 136
                roi = [x0, y0, 116, 112]
                coords = [(y * 1280 + x) * 3 for y in range(y0, y0 + 112) for x in range(x0, x0 + 116)]
                means = {name: [sum(rgb[i + c] for i in coords) / (255 * len(coords))
                                for c in range(3)] for name, rgb in images.items()}
                expected = .725 * (.10 + .75 * .78 * p)
                identity = max(abs(images['candidate'][i] - images['baseline'][i]) for i in coords)
                constant_error = max(abs(images['candidate'][i] / 255 - expected) for i in coords)
                finite_mean, convergence = finite_grid_reference(roi, room_pixels, roll, samples)
                finite_error = abs(means['candidate'][1] - finite_mean)
                # Half an RGBA8 code plus a bounded integration/pixel-filter
                # allowance. The periodic check is retained as an independent
                # coarse guard against the old 50%-coverage asymptote.
                finite_tolerance = .5 / 255 + .001
                passed = ((row > 1 or constant_error <= 1 / 255 + .0001) and
                          (row != 4 or identity == 0) and
                          abs(means['candidate'][1] - period_mean) <= .006 and
                          finite_error <= finite_tolerance and convergence <= .0015)
                report['cases'].append({
                    'p': p, 'roll_degrees': roll, 'nominal_room_pixels': room_pixels,
                    'roi': roi, 'baseline_channels': means['baseline'],
                    'candidate_channels': means['candidate'], 'expected_lit_mean': expected,
                    'subpixel_max_error': constant_error if row < 2 else None,
                    'resolved_max_byte_difference': identity if row == 4 else None,
                    'grid_physical_period_mean': period_mean,
                    'grid_finite_panel_reference': finite_mean,
                    'grid_reference_split_difference': convergence,
                    'grid_finite_panel_error': finite_error,
                    'grid_finite_panel_tolerance': finite_tolerance, 'passed': passed})
    report['passed'] = all(case['passed'] for case in report['cases'])
    report['scope'] = (
        'Raw R tests occupancy independently of optics/exposure; raw G is filtered physical mullion coverage; '
        'raw B is detail blend. Strict subpixel convergence and resolved-pattern identity checks are supplemented '
        'by independent finite-pane integration of the original triangular geometry profile under camera roll. '
        'RGBA8 quantization, conservative derivative-box filtering and packed-aux precision remain explicit limits. '
        'Indirect light is disabled to isolate this proof; voxel expectation is checked analytically, not by this image pair. '
        'This is not final-city acceptance.')
    report['finite_reference_samples'] = len(samples)
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': len(report['cases']),
                      'max_finite_grid_error': max(c['grid_finite_panel_error'] for c in report['cases']),
                      'max_reference_split_difference': max(c['grid_reference_split_difference'] for c in report['cases']),
                      'failed': [case for case in report['cases'] if not case['passed']]}, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
