#!/usr/bin/env python3
"""Generate random intervals, merge overlapping ones, and report statistics.

Usage: python3 merge_intervals.py [N]

N is the number of intervals to generate (default: 200000).
"""

import random
import sys


def generate_intervals(n, rng):
    """Return n random intervals as (start, end) tuples."""
    intervals = []
    for _ in range(n):
        start = rng.randint(0, 10_000_000)
        length = rng.randint(1, 100_000)
        intervals.append((start, start + length))
    return intervals


def merge_intervals(intervals):
    """Merge overlapping intervals; returns a list of [start, end] lists."""
    merged = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return merged


def main():
    try:
        n = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
    except ValueError:
        sys.exit("error: N must be an integer")
    if n < 0:
        sys.exit("error: N must be non-negative")

    rng = random.Random(4)
    merged = merge_intervals(generate_intervals(n, rng))

    longest = max(end - start for start, end in merged)
    total = sum(end - start for start, end in merged)

    print(f"Merged intervals:     {len(merged)}")
    print(f"Longest interval:     {longest}")
    print(f"Total covered length: {total}")


if __name__ == "__main__":
    main()
