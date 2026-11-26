#!/usr/bin/env python3
"""0/1 knapsack solver using memoized recursion (functools.lru_cache).

Item weights and values are generated from a deterministic formula, so the
result depends only on N. Capacity is 10 * N.
"""

import sys
from functools import lru_cache


def solve(n):
    """Return (best_value, number_of_chosen_items) for the knapsack instance."""
    weights = [(i * 7) % 13 + 1 for i in range(n)]
    values = [(i * 11) % 17 + 3 for i in range(n)]
    capacity = 10 * n

    @lru_cache(maxsize=None)
    def best(i, cap):
        """Best value achievable using items i..n-1 with remaining capacity cap."""
        if i == n:
            return 0
        skip = best(i + 1, cap)
        w = weights[i]
        if w <= cap:
            take = values[i] + best(i + 1, cap - w)
            return max(skip, take)
        return skip

    total = best(0, capacity)

    # Walk the memoized table to count how many items the optimum uses.
    chosen = 0
    cap = capacity
    for i in range(n):
        skip = best(i + 1, cap)
        w = weights[i]
        if w <= cap and values[i] + best(i + 1, cap - w) > skip:
            chosen += 1
            cap -= w

    return total, chosen


def main():
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: N must be an integer, got {sys.argv[1]!r}")
        if n < 0:
            sys.exit("error: N must be non-negative")
    else:
        n = 120

    total, chosen = solve(n)
    print(f"N: {n}")
    print(f"Best value: {total}")
    print(f"Items chosen: {chosen}")


if __name__ == "__main__":
    main()
