#!/usr/bin/env python3
"""Estimate pi with a Monte Carlo simulation of random points in a unit square."""

import math
import random
import sys


def estimate_pi(n: int) -> float:
    """Estimate pi using n random points in the unit square."""
    random.seed(42)
    inside = 0
    for _ in range(n):
        x = random.random()
        y = random.random()
        if x * x + y * y <= 1.0:
            inside += 1
    return 4.0 * inside / n


def main() -> None:
    n = 2000000
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: N must be an integer, got {sys.argv[1]!r}")
        if n <= 0:
            sys.exit("error: N must be positive")

    estimate = estimate_pi(n)
    error = abs(estimate - math.pi)
    print(f"pi estimate: {estimate:.6f}")
    print(f"absolute error: {error:.6f}")


if __name__ == "__main__":
    main()
