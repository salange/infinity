#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Verify pinned PBR source maps and install the reviewed material library."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Directory containing the material folders in surface-manifest.json")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    demo = Path(__file__).resolve().parents[1]
    output = args.output or demo / "assets" / "textures"
    manifest = json.loads((demo / "assets" / "surface-manifest.json").read_text())
    verified = []
    for asset in manifest["assets"]:
        for filename, expected in asset["files"].items():
            relative = Path(asset["name"]) / filename
            source = args.source / relative
            if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
                raise SystemExit(f"Source integrity mismatch: {relative}")
            verified.append((source, relative))
    # Validate the complete library before changing any installed resource.
    for source, relative in verified:
        if not args.verify_only:
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.resolve() != target.resolve():
                shutil.copyfile(source, target)
        print(f"Verified {relative}: SHA-256 matches immutable source")
    print(f"{'Verified' if args.verify_only else 'Installed'} {len(verified)} maps in {len(manifest['assets'])} material sets")


if __name__ == "__main__":
    main()
