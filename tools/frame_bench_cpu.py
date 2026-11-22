r"""Summarises btbh_gpu_frame_bench's CPU mode (BTBH_FRAME_BENCH_CPU=1) for RFC 0011's ring windows.

    python tools/frame_bench_cpu.py label:log [label:log ...] [--limit 1.5]

Each log is one process; a label groups the processes of one build (for example base:a.log new:b.log base:c.log ...,
alternating). A ring window alternates frames with its ring and without it, so each turn gives pairs of a frame with
the ring and the frame after it without. A frame over --limit times the 25th percentile of its configuration's
frames of the same kind (another process on the CPU or GPU) drops its pair. Per configuration (label and mode):

- the marginal cost a ring batch, in microseconds: the median of the pairs' differences over the ring's batch count,
  with the 25th and 75th percentiles of the pairs, for the scene report's geometry encode, prepare and shadow encode
  and the present call's wall clock;
- the frame without the ring: its geometry encode and the microseconds a batch of it.
"""
import argparse
import re
import statistics
from collections import defaultdict

FIELDS = ("geometry", "prepare", "shadow", "present")


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    position = fraction * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def parse(entries):
    """pairs[(label, mode)] = [(with ring, without), ...] of field dicts; ring sizes; frame draw counts."""
    pairs = defaultdict(list)
    ring_sizes = {}
    draws = {}
    for entry in entries:
        label, path = entry.split(":", 1)
        windows = {}
        pending = {}
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = re.match(r"BENCH window (\d+) .* mode=(\S+) .*ring=(\d+)", line)
                if match:
                    windows[int(match.group(1))] = match.group(2)
                    ring_sizes[(label, match.group(2))] = int(match.group(3))
                    continue
                match = re.match(r"BENCH cpu (\d+) (\d+) ring=(\d) (.*)", line)
                if not match:
                    continue
                window = int(match.group(1))
                key = (label, windows[window])
                values = dict((name, float(value)) for name, value in re.findall(r"(\w+)=([\d.]+)", match.group(4)))
                if match.group(3) == "1":
                    pending[window] = values
                elif window in pending:
                    pairs[key].append((pending.pop(window), values))
                    draws[key] = int(values["draws"])
    return pairs, ring_sizes, draws


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--limit", type=float, default=1.5)
    arguments = parser.parse_args()
    pairs, ring_sizes, draws = parse(arguments.logs)
    modes = sorted({mode for _, mode in pairs})
    labels = []
    for entry in arguments.logs:
        label = entry.split(":", 1)[0]
        if label not in labels:
            labels.append(label)
    for mode in modes:
        print(f"== {mode}")
        for label in labels:
            key = (label, mode)
            if key not in pairs:
                continue
            all_pairs = pairs[key]
            on_floor = percentile([on["geometry"] for on, _ in all_pairs], 0.25)
            off_floor = percentile([off["geometry"] for _, off in all_pairs], 0.25)
            kept = [(on, off) for on, off in all_pairs
                    if on["geometry"] <= arguments.limit * on_floor and off["geometry"] <= arguments.limit * off_floor
                    and on["present"] <= arguments.limit * percentile([p[0]["present"] for p in all_pairs], 0.25)
                    and off["present"] <= arguments.limit * percentile([p[1]["present"] for p in all_pairs], 0.25)]
            ring = ring_sizes[key]
            parts = []
            for field in FIELDS:
                differences = [1000.0 * (on[field] - off[field]) / ring for on, off in kept]
                parts.append(f"{field} {statistics.median(differences):.2f} "
                             f"[{percentile(differences, 0.25):.2f}, {percentile(differences, 0.75):.2f}]")
            off_geometry = [off["geometry"] for _, off in kept]
            frame = statistics.median(off_geometry)
            print(f"  {label:6s} us a ring batch: " + "; ".join(parts) +
                  f"  | frame without the ring: geometry {frame:.3f} ms, {1000.0 * frame / draws[key]:.2f} us a "
                  f"batch ({draws[key]})  | {len(kept)} of {len(all_pairs)} pairs")


if __name__ == "__main__":
    main()
