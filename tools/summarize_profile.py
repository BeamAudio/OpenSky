#!/usr/bin/env python3
"""Summarize measured OpenSky routes from a profiler capture directory."""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean


def read_rows(directory, name):
    with (directory / name).open(newline="") as file:
        return list(csv.DictReader(file))


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)] if ordered else math.nan


def summarize(directory, requested_scenes):
    print(f"\n{directory}")
    print((directory / "metadata.txt").read_text().strip())
    startup = read_rows(directory, "startup.csv")
    print("Startup stages (ms)")
    for row in startup:
        print(f"  {row['stage']:30} {float(row['stage_ms']):8.1f}")
    frames = defaultdict(list)
    gpu = defaultdict(list)
    for row in read_rows(directory, "frames.csv"):
        scene = row["scene"]
        if requested_scenes and scene not in requested_scenes:
            continue
        if not requested_scenes and (
            scene.endswith(("_warmup", " warmup", "_capture", ".png")) or scene == "debug_chase"
        ):
            continue
        frames[(scene, row["quality"])].append(row)
    for row in read_rows(directory, "gpu_pass_times.csv"):
        gpu[(row["scene"], row["quality"])].append(row)

    for key, samples in frames.items():
        elapsed = [float(row["frame_ms"]) for row in samples if float(row["frame_ms"]) > 0]
        if not elapsed:
            continue
        scene, quality = key
        first, last = samples[0], samples[-1]

        def delta(name):
            return float(last[name]) - float(first[name])

        built = delta("pages_built_total")
        build = delta("worker_build_total_ms") / built if built > 0 else math.nan
        encode = delta("worker_bc7_total_ms") / built if built > 0 else math.nan
        steps = delta("weather_steps_total")
        solve = delta("weather_solve_total_ms") / steps if steps > 0 else math.nan
        print(f"\n{scene} (quality {quality}, {len(elapsed)} frame intervals)")
        print(f"  {1000 / mean(elapsed):.1f} FPS | p95 {percentile(elapsed, .95):.2f} ms"
              f" | p99 {percentile(elapsed, .99):.2f} ms | max {max(elapsed):.2f} ms")
        print(f"  Frames over 20 ms: {sum(ms > 20.0 for ms in elapsed)}"
              f" | over 33.3 ms: {sum(ms > 1000.0 / 30 for ms in elapsed)}")
        if "scene_w" in first and "scene_h" in first:
            sizes = sorted({(int(row["scene_w"]), int(row["scene_h"])) for row in samples})
            print("  Scene resolution " + ", ".join(f"{width}x{height}" for width, height in sizes))
        print(f"  Pages built {built:.0f} | final queue {last['pages_queued']}"
              f" | last draw L{last['max_draw_lod']} | resident L7/L8/L9 "
              f"{last['pages_l7']}/{last['pages_l8']}/{last['pages_l9']}")
        print(f"  Worker ms/page generation {build:.1f}, BC7 {encode:.1f} | weather {solve:.1f} ms/step")
        for column in ("input_physics_ms", "weather_main_ms", "present_ms", "upload_cpu_ms"):
            values = [float(row[column]) for row in samples]
            print(f"  {column:25} mean {mean(values):6.3f}  p95 {percentile(values, .95):6.3f}")
        if gpu[key]:
            print(f"  {len(gpu[key])} sampled GPU frames")
            for column in gpu[key][0]:
                if column.endswith("_gpu_ms"):
                    values = [float(row[column]) for row in gpu[key] if float(row[column]) >= 0]
                    if values:
                        print(f"  {column:25} mean {mean(values):6.3f}  p95 {percentile(values, .95):6.3f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="+", type=Path)
    parser.add_argument("--scene", action="append", default=[], help="Include the exact scene label")
    args = parser.parse_args()
    for directory in args.directory:
        summarize(directory, args.scene)


if __name__ == "__main__":
    main()
