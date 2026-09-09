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
import struct
import subprocess
import tempfile
import zlib


def png_metrics(path):
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
    if len(bins) < 32 or not 3 < mean < 252:
        raise ValueError(f"blank or degenerate capture: {mean=}, colors={len(bins)}")
    return dict(width=width,height=height,mean_rgb=round(mean,3),color_bins=len(bins),sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--repeat",action="store_true")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True,exist_ok=True)
    common = ["--hidden","--sky","studio","--seed","83","--width","1600","--height","900","--frames","48","--tex-size","512"]
    rows = []
    with tempfile.TemporaryDirectory(prefix="human-tech-assets-") as empty_assets:
        def capture(name, shot, extra):
            output = args.output / f"{name}.png"
            flags = common+["--shot",shot]+extra
            run = subprocess.run([str(binary),*flags,"--assets",empty_assets,"--capture",str(output)],capture_output=True,text=True,timeout=180)
            log = (run.stdout+run.stderr).replace(str(binary.parent),"<binary-dir>").replace(empty_assets,"<empty-assets>")
            (args.output / f"{name}.log").write_text(log)
            if run.returncode or "[wgpu] error" in log or "FAILED" in log:
                raise RuntimeError(f"capture {name} failed; see its log")
            metrics = png_metrics(output)
            if (metrics["width"],metrics["height"]) != (1600,900):
                raise RuntimeError("unexpected capture dimensions")
            print(f"{name}: content check passed",flush=True)
            return dict(file=output.name,flags=flags,**metrics)
        for name,shot,extra in [("aerial","aerial",[]),("civic","civic",[]),("street","street",[]),("terrace","terrace",[]),("blue-hour","civic",["--night"])]:
            rows.append(capture(name,shot,extra))
        repeat = None
        if args.repeat:
            other = capture("aerial-repeat","aerial",[])
            repeat = other["sha256"] == rows[0]["sha256"]
            if not repeat:
                raise RuntimeError("aerial repeat differs on this GPU/build")
    (args.output / "captures.json").write_text(json.dumps(dict(schema_version=1,assets="built-in only",captures=rows,repeat_identical=repeat),indent=2)+"\n")


if __name__ == "__main__":
    main()
