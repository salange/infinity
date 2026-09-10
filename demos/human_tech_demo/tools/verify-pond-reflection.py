# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Capture and verify the native shallow-pond offscreen reflection controls."""

import argparse
import hashlib
import importlib.util
import itertools
import json
import math
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not args.verify_existing and any(out.iterdir()):
        parser.error("output must be empty unless --verify-existing is used")
    demo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location(
        "transport_verifier", Path(__file__).with_name("verify-renderer-transport.py")
    )
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    common = [str(binary), "--hidden", "--sky", "authored", "--width", "1280",
              "--height", "720", "--tex-size", "512", "--frames", "8", "--no-indirect"]
    for disabled, state in itertools.product((False, True), ("on", "off")):
        label = ("disabled-" if disabled else "active-") + state
        if not args.verify_existing:
            flags = ["--asset", "reflection-proof-pond-" + state,
                     "--capture", str(out / (label + ".png"))]
            if disabled:
                flags += ["--no-reflections"]
            print("Capturing", label, flush=True)
            result = subprocess.run(common + flags, capture_output=True,
                                    text=True, timeout=600)
            log = result.stdout + result.stderr
            if result.returncode:
                raise RuntimeError(f"{label} failed with exit {result.returncode}")
        else:
            log = (out / (label + ".log")).read_text()
        log = (log.replace(str(out), "<output-dir>")
               .replace(str(binary.parent), "<binary-dir>")
               .replace(str(demo.parents[1]), "<game-worktree>"))
        (out / (label + ".log")).write_text(log)
        if "Validation Error" in log or "device lost" in log.lower():
            raise RuntimeError(f"{label} contains a GPU error")
        if "8 fixed-time frames" not in log:
            raise RuntimeError(f"{label} lacks successful capture evidence")
    active = verifier.compare(out / "active-on.png", out / "active-off.png")
    negative = verifier.compare(out / "disabled-on.png", out / "disabled-off.png")
    # Camera is (0,3.04,6), facing (0,1.04,0). Project every corner of
    # the changed emitter: its lower edge must remain above the direct frame.
    vertical_pixels = []
    for y, z in itertools.product((4.04, 6.04), (-3.5, -2.5)):
        dy, dz = y - 3.04, z - 6
        up = (3 * dy - dz) / math.sqrt(10)
        forward = (-dy - 3 * dz) / math.sqrt(10)
        vertical_pixels.append(.5 - .5 * up / forward / math.tan(math.radians(20)))
    passed = (active["changed_pixels_over_3"] > 100
              and active["maximum_channel_difference"] > 20
              and negative["maximum_channel_difference"] == 0
              and max(vertical_pixels) < 0)
    sources = ("src/renderer.cpp", "src/renderer.hpp", "src/renderer_proof.hpp",
               "shaders/common.wgsl", "shaders/main.wgsl", "shaders/post.wgsl")
    report = {
        "passed": passed, "resolution": [1280, 720],
        "surface_height_m": 1.04, "bottom_height_m": .78,
        "optics": {"ior": 1.333, "transmission": .96, "coverage": .98,
                   "thickness_m": .26, "roughness": .045,
                   "tint": [.975, .989, .985], "flags": 128 | 1024},
        "direct_emitter_normalized_y_range": [min(vertical_pixels), max(vertical_pixels)],
        "active": active, "disabled_negative_control": negative,
        "source_sha256": {p: hashlib.sha256((demo / p).read_bytes()).hexdigest()
                          for p in sources},
        "scope": "Actual shallow bottom and third mirrored scene plane. Single-layer transmission; no stacked volume refraction or caustics.",
    }
    (out / "pond-reflection-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
