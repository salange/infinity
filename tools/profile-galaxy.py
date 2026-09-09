# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Summarize an entire flight: uv run --script tools/profile-galaxy.py FILE.csv."""

import argparse
import csv
import json
import math

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("csv")
args = parser.parse_args()
with open(args.csv, encoding="utf-8") as source:
    lines = source.readlines()
metadata = {}
for line in lines:
    if line.startswith("# "):
        metadata.update(item.split("=", 1) for item in line[2:].strip().split(";"))
rows = [
    {key: float(value) for key, value in row.items()}
    for row in csv.DictReader(line for line in lines if not line.startswith("#"))
]
if (
    not rows
    or metadata.get("complete") != "1"
    or rows[0]["elapsed_s"] != 0
    or rows[-1]["elapsed_s"] != 120
):
    raise SystemExit("Incomplete flight: no full-flight performance claim is valid")
if any(not math.isfinite(value) for row in rows for value in row.values()):
    raise SystemExit("Invalid non-finite measurement")
if any(b["elapsed_s"] < a["elapsed_s"] for a, b in zip(rows, rows[1:])):
    raise SystemExit("Non-monotonic flight timestamps")


def summarize(frames):
    if not frames:
        return {"frames": 0}
    times = sorted(row["frame_ms"] for row in frames)

    def percentile(p):
        return times[max(0, math.ceil(len(times) * p) - 1)]

    summary = {
        "frames": len(times),
        "mean_ms": sum(times) / len(times),
        "p50_ms": percentile(0.5),
        "p95_ms": percentile(0.95),
        "p99_ms": percentile(0.99),
        "max_ms": max(times),
        "over_24fps_budget": sum(t >= 1000 / 24 for t in times),
        "over_60fps_budget": sum(t > 1000 / 60 for t in times),
        "failed_presentations": sum(row["presented"] != 1 for row in frames),
        "max_catalog_ms": max(row["catalog_ms"] for row in frames),
    }
    for stage in ("resources_ms", "world_ms", "stars_ms", "draw_ms", "render_ms", "acquire_ms", "poll_ms", "submit_ms", "present_ms"):
        if stage in frames[0]:
            summary[f"max_{stage}"] = max(row[stage] for row in frames)
    return summary


center = float(metadata["center_s"])
result = {
    "metadata": metadata,
    "framebuffer_sizes": sorted(
        {f"{int(r['width'])}x{int(r['height'])}" for r in rows}
    ),
    "whole": summarize(rows),
}
for label, lo, hi in [
    ("departure", 0, 2),
    ("entry", 2, max(2, center - 10)),
    ("center", max(2, center - 10), center + 10),
    ("exit", center + 10, 121),
]:
    result[label] = summarize([row for row in rows if lo <= row["elapsed_s"] < hi])
print(json.dumps(result, indent=2))
