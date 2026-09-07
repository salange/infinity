# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Summarize a complete live traversal: uv run --script tools/profile-galaxy.py FILE.csv."""
import argparse
import csv
import json
import math

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("csv")
args = parser.parse_args()
with open(args.csv, newline="", encoding="utf-8") as source:
    header = source.readline().strip()
    if not header.startswith("# "):
        raise SystemExit("Missing traversal metadata")
    metadata = dict(item.split("=", 1) for item in header[2:].split(";"))
    rows = [{k: float(v) for k, v in row.items()} for row in csv.DictReader(source)]
if not rows or metadata.get("complete") != "1" or rows[0]["elapsed_s"] != 0 or rows[-1]["elapsed_s"] != 120:
    raise SystemExit("Incomplete route: cannot claim full-length performance")
if any(b["elapsed_s"] < a["elapsed_s"] for a, b in zip(rows, rows[1:])):
    raise SystemExit("Non-monotonic route timestamps")


def summarize(frames):
    values = sorted(row["frame_ms"] for row in frames)
    def percentile(p):
        return values[max(0, math.ceil(len(values) * p) - 1)]
    return {
        "frames": len(values),
        "mean_ms": sum(values) / len(values),
        "p50_ms": percentile(.50), "p95_ms": percentile(.95), "p99_ms": percentile(.99),
        "max_ms": values[-1],
        "over_16_7_ms": sum(value > 16.7 for value in values),
        "failed_presentations": sum(row["presented"] != 1 for row in frames),
        "late_sky_frames": sum(row["sky_lag_s"] > 0 for row in frames),
        "max_sky_lag_s": max(row["sky_lag_s"] for row in frames),
        "max_upload_ms": max(row["upload_ms"] for row in frames),
        "max_source_bake_ms": max(row["bake_ms"] for row in frames),
    }

result = {"metadata": metadata, "framebuffer_sizes": sorted({
    f"{int(r['width'])}x{int(r['height'])}" for r in rows}), "whole": summarize(rows)}
for name, lo, hi in [("entry", 0, 40), ("center", 40, 80), ("exit", 80, 121)]:
    result[name] = summarize([r for r in rows if lo <= r["elapsed_s"] < hi])
print(json.dumps(result, indent=2))
