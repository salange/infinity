#!/usr/bin/env python3
"""Fetch the CC0 surface tile library listed in assets/manifest.json.

Downloads each ambientCG material set (1K JPG zip), extracts the five maps
the renderer uses into assets/textures/<material>/{color,normal,roughness,
ao,height}.jpg, and records the sha256 of each zip in assets/textures/
fetched.json. Idempotent: existing complete sets are skipped. Offline-safe:
failures are reported and the game falls back to procedural tiles.

Usage: tools/fetch-textures.py [--only name,name] [--force]
"""
import hashlib
import io
import json
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "assets", "manifest.json")
OUT = os.path.join(ROOT, "assets", "textures")
UA = {"User-Agent": "infinity-fetch-textures/1.0"}

MAP_FILES = {
    "Color": "color.jpg",
    "NormalGL": "normal.jpg",
    "Roughness": "roughness.jpg",
    "AmbientOcclusion": "ao.jpg",
    "Displacement": "height.jpg",
}


def complete(dir_path):
    return all(os.path.isfile(os.path.join(dir_path, f)) for f in MAP_FILES.values())


def fetch(asset_id, resolution):
    url = f"https://ambientcg.com/get?file={asset_id}_{resolution}.zip"
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def main(argv):
    only = None
    force = "--force" in argv
    for i, arg in enumerate(argv):
        if arg == "--only" and i + 1 < len(argv):
            only = set(argv[i + 1].split(","))
    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)
    resolution = manifest.get("resolution", "1K-JPG")
    os.makedirs(OUT, exist_ok=True)
    record_path = os.path.join(OUT, "fetched.json")
    record = {}
    if os.path.isfile(record_path):
        with open(record_path, encoding="utf-8") as f:
            record = json.load(f)
    failures = []
    for name, entry in manifest["materials"].items():
        if only is not None and name not in only:
            continue
        dir_path = os.path.join(OUT, name)
        if complete(dir_path) and not force:
            print(f"  ok      {name} ({entry['asset']})")
            continue
        try:
            print(f"  fetch   {name} <- {entry['asset']}_{resolution}.zip", flush=True)
            blob = fetch(entry["asset"], resolution)
            digest = hashlib.sha256(blob).hexdigest()
            os.makedirs(dir_path, exist_ok=True)
            with zipfile.ZipFile(io.BytesIO(blob)) as zf:
                names = zf.namelist()
                for map_name, out_file in MAP_FILES.items():
                    match = [n for n in names if n.endswith(f"_{map_name}.jpg")]
                    if not match:
                        print(f"          missing map {map_name} in {entry['asset']}")
                        continue
                    with zf.open(match[0]) as src, open(os.path.join(dir_path, out_file), "wb") as dst:
                        dst.write(src.read())
            record[name] = {"asset": entry["asset"], "resolution": resolution,
                            "sha256": digest, "license": manifest["license"]}
            with open(record_path, "w", encoding="utf-8") as f:
                json.dump(record, f, indent=2, sort_keys=True)
        except Exception as exc:  # noqa: BLE001 - report and continue
            failures.append((name, str(exc)))
            print(f"  FAILED  {name}: {exc}")
    if failures:
        print(f"{len(failures)} set(s) not fetched; the game will use procedural tiles for them.")
        return 1
    print("all sets present")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
