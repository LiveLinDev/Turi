#!/usr/bin/env python3
"""Find the starting number below N with the longest Collatz chain."""

import argparse
import sys


def collatz_length(n: int, cache: dict) -> int:
    """Return the Collatz chain length for n, memoizing results in cache."""
    if n in cache:
        return cache[n]

    # Walk down until we hit a value we've already computed.
    stack = []
    m = n
    while m not in cache:
        stack.append(m)
        if m % 2 == 0:
            m //= 2
        else:
            m = 3 * m + 1

    length = cache[m]
    # Unwind the stack, assigning lengths in reverse order.
    for value in reversed(stack):
        length += 1
        cache[value] = length

    return cache[n]


def longest_chain(limit: int) -> tuple[int, int]:
    """Return (number, chain length) for the longest chain under limit."""
    cache = {1: 1}
    best_num = 1
    best_len = 1
    for start in range(2, limit):
        length = collatz_length(start, cache)
        if length > best_len:
            best_len = length
            best_num = start
    return best_num, best_len


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Find the number below N with the longest Collatz chain."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=1_000_000,
        help="upper bound (exclusive) for starting numbers, default: 1000000",
    )
    args = parser.parse_args()

    if args.n < 2:
        parser.error("N must be at least 2")

    number, length = longest_chain(args.n)
    print(f"{number} {length}")


if __name__ == "__main__":
    sys.exit(main())
