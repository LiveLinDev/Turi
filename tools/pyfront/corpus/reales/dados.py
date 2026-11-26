#!/usr/bin/env python3
"""Roll two dice N times and print a histogram of the sums."""

import argparse
import random
import sys


def roll_sums(n, rng):
    counts = {total: 0 for total in range(2, 13)}
    for _ in range(n):
        total = rng.randint(1, 6) + rng.randint(1, 6)
        counts[total] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(
        description="Histogram of sums from rolling two dice N times."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=500000,
        help="number of rolls (default: 500000)",
    )
    args = parser.parse_args()

    if args.n < 1:
        parser.error("N must be a positive integer")

    rng = random.Random(2)
    counts = roll_sums(args.n, rng)
    max_count = max(counts.values())

    print(f"Rolls: {args.n}")
    for total in range(2, 13):
        count = counts[total]
        bar_len = round(50 * count / max_count)
        print(f"{total:2d}: {count:7d}  {'*' * bar_len}")


if __name__ == "__main__":
    sys.exit(main())
