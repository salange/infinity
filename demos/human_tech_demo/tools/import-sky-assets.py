#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Verify immutable source environment textures and install renderer resources."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True,help='Directory containing the source PNGs named in assets/sky-manifest.json')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    demo=Path(__file__).resolve().parents[1]
    output=args.output or demo/'assets'/'generated'
    manifest=json.loads((demo/'assets'/'sky-manifest.json').read_text())
    verified=[]
    for row in manifest['assets']:
        path=args.source/row['file']
        if hashlib.sha256(path.read_bytes()).hexdigest()!=row['sha256']:
            raise SystemExit(f'Source integrity mismatch: {row["file"]}')
        verified.append((path,row['file']))
    output.mkdir(parents=True,exist_ok=True)
    for path,name in verified:
        if path.resolve()!=(output/name).resolve():
            shutil.copyfile(path,output/name)
        print(f'Installed {name}, verified SHA-256')


if __name__=='__main__':
    main()
