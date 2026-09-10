# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Verify invisible-pane identity and finite-coverage atmospheric continuity."""
import argparse
import hashlib
import importlib.util
import itertools
import json
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
        "transport_verifier", Path(__file__).with_name("verify-renderer-transport.py"))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    common = [str(binary), "--hidden", "--sky", "authored", "--width", "1280",
              "--height", "720", "--tex-size", "512", "--frames", "8", "--msaa", "1",
              "--no-indirect", "--no-reflections", "--no-shadows", "--no-ssao", "--no-taa"]
    for fixture, state in itertools.product(("zero", "finite"), ("on", "off")):
        label = fixture + "-" + state
        if not args.verify_existing:
            print("Capturing", label, flush=True)
            result = subprocess.run(
                common + ["--asset", "reflection-proof-null-" + label,
                          "--capture", str(out / (label + ".png"))],
                text=True, capture_output=True, timeout=600)
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
    zero = verifier.compare(out / "zero-on.png", out / "zero-off.png")
    finite = verifier.compare(out / "finite-on.png", out / "finite-off.png")
    passed = (zero["maximum_channel_difference"] == 0 and
              finite["maximum_channel_difference"] <= 3 and
              finite["mean_absolute_rgb_255"] < .25)
    sources = ("src/renderer_proof.hpp", "shaders/main.wgsl", "shaders/prepass.wgsl")
    report = {
        "passed": passed, "resolution": [1280, 720],
        "zero_coverage": zero, "near_index_matched_finite_coverage": finite,
        "fixtures": {"pane_distance_m": 500, "opaque_wall_distance_m": 1000,
                     "zero": {"coverage": 0, "ior": 1.5, "thickness_m": .26,
                              "background": "opaque wall and authored sky"},
                     "finite": {"coverage": .5, "ior": 1.001, "thickness_m": 0,
                                "background": "opaque wall", "transmission": 1}},
        "source_sha256": {p: hashlib.sha256((demo / p).read_bytes()).hexdigest()
                          for p in sources},
        "scope": "Zero coverage is exact identity. Finite proof retains small Fresnel and half-float filtering differences; no stacked refraction claim.",
    }
    (out / "glass-atmosphere-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
