r"""Summarises btbh_gpu_frame_bench logs (Tests/btbh_gpu_frame_bench.c).

    python tools/frame_bench_summary.py name0,name1,... log [log ...]

A configuration is a window's shader directory and mode ("BENCH window" lines); the names go to the first log's
windows in order, and the first is the reference. The logs may give the configurations to other windows (a run that
rotates them cancels what a window's own resources cost). Per turn the first --skip frames are dropped (the
window's caches are cold after another window's turn); the rest give main_ms (split frames: the opaque pass) and
gpu_ms (whole frames).

- Paired: each turn against the reference's turn of the same round. A round whose slowest turn is over --limit
  times the 10th percentile of all turns is dropped (another process on the GPU). The median difference, with a
  90 % bootstrap interval.
- Low percentile: from every frame (contention only adds time), the 10th percentile against the reference's, with
  a 90 % bootstrap interval.
"""
import argparse
import random
import re
import statistics
from collections import defaultdict


def parse(paths, skip):
    """turns[(log, configuration, round)] = {"main": [...], "frame": [...]}; the configurations of each log."""
    turns = defaultdict(lambda: {"main": [], "frame": []})
    configurations = []
    for log_index, path in enumerate(paths):
        windows = {}
        current = None
        seen = 0
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = re.match(r"BENCH window (\d+) (.*) mode=(\S+) ", line)
                if match:
                    windows[int(match.group(1))] = (match.group(2), match.group(3))
                    continue
                match = re.match(r"BENCH turn (\d+) (\d+)", line)
                if match:
                    current = (log_index, windows[int(match.group(1))], int(match.group(2)))
                    seen = 0
                    continue
                if line.startswith("BENCH done"):
                    current = None
                    continue
                if current is None or not line.startswith("BTBH_GPU_COST gpu_ms"):
                    continue
                seen += 1
                if seen <= skip:
                    continue
                if "split=1" in line:
                    turns[current]["main"].append(float(re.search(r"main_ms=([\d.]+)", line).group(1)))
                else:
                    turns[current]["frame"].append(float(re.search(r"gpu_ms=([\d.]+)", line).group(1)))
        configurations.append([windows[index] for index in sorted(windows)])
    return turns, configurations


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def median_interval(values, seed=7, runs=1000):
    rng = random.Random(seed)
    medians = sorted(statistics.median(rng.choice(values) for _ in values) for _ in range(runs))
    return medians[int(0.05 * runs)], medians[int(0.95 * runs) - 1]


def low_percentile_interval(mine, reference, seed=11, runs=1000):
    rng = random.Random(seed)
    differences = sorted(
        percentile([rng.choice(mine) for _ in mine], 0.1) - percentile([rng.choice(reference) for _ in reference], 0.1)
        for _ in range(runs))
    return differences[int(0.05 * runs)], differences[int(0.95 * runs) - 1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("names")
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--skip", type=int, default=2)
    parser.add_argument("--limit", type=float, default=1.08)
    arguments = parser.parse_args()
    turns, configurations = parse(arguments.logs, arguments.skip)
    order = configurations[0]
    names = dict(zip(order, arguments.names.split(",")))
    reference = order[0]
    rounds = sorted({(log, round_index) for (log, _, round_index) in turns})
    medians = sorted(statistics.median(v["main"]) for v in turns.values() if v["main"])
    floor = percentile(medians, 0.1)
    kept = []
    for log, round_index in rounds:
        values = [statistics.median(turns[(log, c, round_index)]["main"]) for c in order
                  if turns[(log, c, round_index)]["main"]]
        if len(values) == len(order) and max(values) <= arguments.limit * floor:
            kept.append((log, round_index))
    header = f"{'main':>8s} {'vs first':>9s} {'90 % interval':>19s} {'frame':>8s} {'vs first':>9s} {'90 % interval':>19s}"
    print(f"paired: {len(kept)} of {len(rounds)} rounds kept (slowest turn within {arguments.limit:.2f} x "
          f"{floor:.4f} ms)")
    print(f"{'configuration':28s} {header}")
    for configuration in order:
        cells = []
        for kind in ("main", "frame"):
            absolute = []
            paired = []
            for log, round_index in kept:
                mine = turns[(log, configuration, round_index)][kind]
                first = turns[(log, reference, round_index)][kind]
                if mine:
                    absolute.append(statistics.median(mine))
                if mine and first:
                    paired.append(statistics.median(mine) - statistics.median(first))
            if not paired:
                cells.append(f"{'-':>8s} {'-':>9s} {'':19s}")
                continue
            low, high = median_interval(paired)
            cells.append(f"{statistics.median(absolute):8.4f} {statistics.median(paired):+9.4f} "
                         f"[{low:+.4f}, {high:+.4f}]")
        print(f"{names.get(configuration, configuration[0]):28s} {cells[0]} {cells[1]}")
    every = {configuration: {"main": [], "frame": []} for configuration in order}
    for (_, configuration, _), samples in turns.items():
        for kind in ("main", "frame"):
            every[configuration][kind].extend(samples[kind])
    print(f"10th percentile, every frame (main: {len(every[reference]['main'])} of the first's)")
    print(f"{'configuration':28s} {header}")
    for configuration in order:
        cells = []
        for kind in ("main", "frame"):
            mine, first = every[configuration][kind], every[reference][kind]
            low, high = low_percentile_interval(mine, first)
            cells.append(f"{percentile(mine, 0.1):8.4f} {percentile(mine, 0.1) - percentile(first, 0.1):+9.4f} "
                         f"[{low:+.4f}, {high:+.4f}]")
        print(f"{names.get(configuration, configuration[0]):28s} {cells[0]} {cells[1]}")


if __name__ == "__main__":
    main()
