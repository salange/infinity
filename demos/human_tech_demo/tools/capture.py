# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Repeatable, bounded GPU captures with PNG content and repeatability checks.

uv run --no-project --python 3.12 python demos/human_tech_demo/tools/capture.py \
    --binary build/demos/human_tech_demo/human_tech_demo --output build/human-tech-captures --repeat
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


def png_metrics(path, silhouette=False):
    """Decode the renderer's 8-bit RGB(A) PNG; reject blank/invalid captures."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    offset, compressed = 8, bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset+4:offset+8]
        payload = data[offset+8:offset+8+size]
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError("unsupported capture PNG")
            channels = 4 if color == 6 else 3
        if kind == b"IDAT":
            compressed.extend(payload)
        offset += size + 12
    raw = zlib.decompress(compressed)
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ValueError("incomplete PNG")
    previous = bytearray(stride)
    bins, total = set(), 0
    for y in range(height):
        offset = y*(stride+1)
        mode, row = raw[offset], bytearray(raw[offset+1:offset+stride+1])
        for x in range(stride):
            left = row[x-channels] if x >= channels else 0
            above = previous[x]
            corner = previous[x-channels] if x >= channels else 0
            if mode == 1:
                predictor = left
            elif mode == 2:
                predictor = above
            elif mode == 3:
                predictor = (left+above)//2
            elif mode == 4:
                p = left+above-corner
                da, db, dc = abs(p-left), abs(p-above), abs(p-corner)
                predictor = left if da <= db and da <= dc else above if db <= dc else corner
            elif mode == 0:
                predictor = 0
            else:
                raise ValueError("invalid PNG filter")
            row[x] = (row[x]+predictor)&255
        for x in range(0,stride,channels):
            r,g,b = row[x:x+3]
            total += (r+g+b)/3
            bins.add((r//16,g//16,b//16))
        previous = row
    mean = total/(width*height)
    if len(bins) < (2 if silhouette else 32) or not 3 < mean < 252:
        raise ValueError(f"blank or degenerate capture: {mean=}, colors={len(bins)}")
    return dict(width=width,height=height,mean_rgb=round(mean,3),color_bins=len(bins),sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--shader-dir", type=Path, help="Runtime shaders paired with the selected binary")
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--analysis", action="store_true", help="Include clay, silhouette, neutral and movement views")
    parser.add_argument("--procedural-only", action="store_true", help="Explicit reduced-detail preview; excluded from production acceptance")
    parser.add_argument("--kit", type=Path)
    parser.add_argument("--frames", type=int, default=16)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    demo = Path(__file__).resolve().parents[1]
    # Shaders are loaded at runtime. Pin them as well as the executable so a
    # rebuild or source edit cannot silently mix different rendering versions.
    shader_source = (args.shader_dir or demo / "shaders").resolve(strict=True)
    shader_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in sorted(shader_source.glob("*.wgsl"))}
    if not shader_hashes:
        parser.error("--shader-dir contains no runtime shaders")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("--output must be empty so stale images cannot pass the required-view checks")
    if args.frames < 1:
        parser.error("--frames must be positive")
    shader_snapshot = output / "shaders"
    shader_snapshot.mkdir()
    for name in shader_hashes:
        shutil.copy2(shader_source / name, shader_snapshot / name)
    surfaces, skies, kit_evidence = [], [], None
    if not args.procedural_only:
        manifest_path = demo / "assets" / "surface-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for asset in manifest["assets"]:
            for filename, expected_hash in asset["files"].items():
                path = demo / "assets" / "textures" / asset["name"] / filename
                if hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
                    raise RuntimeError(f"reviewed surface integrity mismatch: {asset['name']}/{filename}")
            surfaces.append(dict(name=asset["name"], asset_id=asset["asset_id"], files=asset["files"]))
        for asset in json.loads((demo / "assets" / "sky-manifest.json").read_text())["assets"]:
            path = demo / "assets" / "generated" / asset["file"]
            if hashlib.sha256(path.read_bytes()).hexdigest() != asset["sha256"]:
                raise RuntimeError(f"reviewed sky integrity mismatch: {asset['file']}")
            skies.append(dict(id=asset["id"], file=asset["file"], sha256=asset["sha256"]))
        kit_manifest = json.loads((demo / "assets" / "authored-kit-manifest.json").read_text())
        kit = args.kit or demo / "assets" / "generated" / "human_tech_kit.htkit"
        kit_hash = hashlib.sha256(kit.read_bytes()).hexdigest()
        if kit_hash != kit_manifest["runtime_sha256"]:
            raise RuntimeError("production capture requires the reviewed native asset kit")
        kit_evidence = dict(runtime_sha256=kit_hash, source_sha256=kit_manifest["source_sha256"])
    common = ["--hidden", "--sky", "authored", "--seed", "83", "--width", "1280", "--height", "720", "--frames", str(args.frames), "--tex-size", "512", "--shader-dir", str(shader_snapshot)]
    if args.procedural_only:
        common.append("--procedural-only")
    if args.kit:
        common += ["--kit", str(args.kit.resolve(strict=True))]
    def run(flags, log_name):
        def preserve_log(stdout, stderr):
            def decoded(value):
                return value.decode(errors="replace") if isinstance(value, bytes) else (value or "")
            log = (decoded(stdout) + decoded(stderr)).replace(str(binary.parent), "<binary-dir>").replace(str(output), "<output-dir>")
            (output / log_name).write_text(log)
            return log
        try:
            result = subprocess.run([str(binary), *common, *flags], capture_output=True, text=True, timeout=2400)
        except subprocess.TimeoutExpired as error:
            preserve_log(error.stdout, error.stderr)
            raise RuntimeError(f"renderer exceeded the capture limit; partial output retained in {log_name}") from error
        log = preserve_log(result.stdout, result.stderr)
        if result.returncode or "[wgpu] error" in log or "FAILED" in log:
            raise RuntimeError(f"renderer failed; see {log_name}")
    flags = ["--gallery", str(output), "--scene-manifest", str(output / "scene.json")]
    if args.analysis:
        flags.append("--gallery-analysis")
    run(flags, "gallery.log")
    captures = []
    for path in sorted(output.glob("*.png")):
        if path.name == "aerial-repeat.png":
            continue
        metrics = png_metrics(path, silhouette="silhouette" in path.name)
        if (metrics["width"], metrics["height"]) != (1280, 720):
            raise RuntimeError("unexpected capture dimensions")
        captures.append(dict(file=path.name, **metrics))
        print(f"{path.name}: content check passed", flush=True)
    expected = {f"{shot}.png" for shot in ("aerial", "galaxy", "civic", "street", "garden", "landing")}
    if not expected.issubset({row["file"] for row in captures}):
        raise RuntimeError("missing required view")
    repeat = None
    if args.repeat:
        path = output / "aerial-repeat.png"
        run(["--shot", "aerial", "--capture", str(path)], "repeat.log")
        repeat = png_metrics(path)["sha256"] == next(row["sha256"] for row in captures if row["file"] == "aerial.png")
        if not repeat:
            raise RuntimeError("aerial repeat differs on this GPU/build")
    timings = {path.stem: json.loads(path.read_text()) for path in sorted(output.glob("*-timings.json"))}
    if hashlib.sha256(binary.read_bytes()).hexdigest() != binary_hash:
        raise RuntimeError("renderer binary changed during the capture session")
    current_shaders = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in sorted(shader_snapshot.glob("*.wgsl"))}
    if current_shaders != shader_hashes:
        raise RuntimeError("renderer shaders changed during the capture session")
    (output / "captures.json").write_text(json.dumps(dict(
        schema_version=2, content_tier="procedural preview" if args.procedural_only else "authored production kit",
        binary_sha256=binary_hash, shader_sha256=shader_hashes,
        resolution=[1280, 720], fixed_seed="83", accumulated_frames=args.frames,
        captures=captures, repeat_identical=repeat, timings=timings, verified_surface_maps=surfaces,
        verified_sky_maps=skies, verified_asset_kit=kit_evidence,
        visual_acceptance="pending separate reference review; content checks do not establish parity"
    ), indent=2) + "\n")


if __name__ == "__main__":
    main()
