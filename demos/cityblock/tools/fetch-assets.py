#!/usr/bin/env python3
"""Fetch the CC0 assets listed in demos/cityblock/assets/manifest.json.

Textures: ambientCG 1K JPG sets, extracted into assets/textures/<name>/
{color,normal,roughness,ao,height}.jpg. Skies: Poly Haven HDRIs into
assets/sky/<name>.hdr. Records sha256 of every download in
assets/fetched.json. Idempotent; failures are reported and the demo falls
back to flat materials / an analytic sky.

Usage: fetch-assets.py [--only name,name] [--force]
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
TEX_OUT = os.path.join(ROOT, "assets", "textures")
SKY_OUT = os.path.join(ROOT, "assets", "sky")
UA = {"User-Agent": "infinity-cityblock-fetch/1.0"}
MAP_FILES = {
    "Color": "color.jpg",
    "NormalGL": "normal.jpg",
    "Roughness": "roughness.jpg",
    "AmbientOcclusion": "ao.jpg",
    "Displacement": "height.jpg",
}


def get(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=180) as resp:
        return resp.read()


def complete(dir_path):
    return all(os.path.isfile(os.path.join(dir_path, f)) for f in MAP_FILES.values())


def main(argv):
    only = None
    force = "--force" in argv
    for i, arg in enumerate(argv):
        if arg == "--only" and i + 1 < len(argv):
            only = set(argv[i + 1].split(","))
    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)
    resolution = manifest.get("resolution", "1K-JPG")
    os.makedirs(TEX_OUT, exist_ok=True)
    os.makedirs(SKY_OUT, exist_ok=True)
    record_path = os.path.join(ROOT, "assets", "fetched.json")
    record = {}
    if os.path.isfile(record_path):
        with open(record_path, encoding="utf-8") as f:
            record = json.load(f)
    failures = []
    for name, asset_id in manifest["materials"].items():
        if only and name not in only:
            continue
        out_dir = os.path.join(TEX_OUT, name)
        if complete(out_dir) and not force:
            print(f"  {name}: present")
            continue
        url = f"https://ambientcg.com/get?file={asset_id}_{resolution}.zip"
        try:
            data = get(url)
            digest = hashlib.sha256(data).hexdigest()
            os.makedirs(out_dir, exist_ok=True)
            found = 0
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                for member in zf.namelist():
                    base = os.path.basename(member)
                    for key, target in MAP_FILES.items():
                        if f"_{key}." in base:
                            with open(os.path.join(out_dir, target), "wb") as f:
                                f.write(zf.read(member))
                            found += 1
            record[name] = {"source": url, "sha256": digest, "maps": found}
            print(f"  {name}: {asset_id} ({found} maps, {len(data) // 1024} KiB)")
        except Exception as exc:  # noqa: BLE001
            failures.append((name, str(exc)))
            print(f"  {name}: FAILED {exc}")
    for name, url in manifest.get("skies", {}).items():
        if only and name not in only:
            continue
        out = os.path.join(SKY_OUT, f"{name}.hdr")
        if os.path.isfile(out) and not force:
            print(f"  sky/{name}: present")
            continue
        try:
            data = get(url)
            with open(out, "wb") as f:
                f.write(data)
            record[f"sky/{name}"] = {"source": url, "sha256": hashlib.sha256(data).hexdigest()}
            print(f"  sky/{name}: {len(data) // 1024} KiB")
        except Exception as exc:  # noqa: BLE001
            failures.append((f"sky/{name}", str(exc)))
            print(f"  sky/{name}: FAILED {exc}")
    with open(record_path, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, sort_keys=True)
    if failures:
        print(f"{len(failures)} failure(s); the demo falls back where files are missing")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
