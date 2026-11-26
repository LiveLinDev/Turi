#!/usr/bin/env python3
"""Compute sums of primes and Fibonacci numbers using generator pipelines.

Usage: fib_primes.py [N]

N (default 20000) controls the amount of work: the number of primes summed.
"""

import argparse
import itertools
import sys


def fibonacci():
    """Yield the Fibonacci sequence: 1, 1, 2, 3, 5, 8, ..."""
    a, b = 0, 1
    while True:
        yield b
        a, b = b, a + b


def primes():
    """Yield prime numbers in ascending order: 2, 3, 5, 7, ..."""
    yield 2
    # Composite candidates already seen, keyed by the prime that generated
    # them. When candidate k is reached, each entry p marks k as divisible
    # by p and advances to k + p.
    composites = {}
    candidate = 3
    while True:
        factor = composites.pop(candidate, None)
        if factor is None:
            yield candidate
            composites[candidate * candidate] = candidate
        else:
            # Reinsert at the next multiple of this factor.
            composites[candidate + factor] = factor
        candidate += 2


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Sum primes and Fibonacci numbers."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=20000,
        help="number of primes to sum (default: %(default)s)",
    )
    return parser.parse_args(argv)


def main(argv=None):
    n = parse_args(argv).n
    if n < 0:
        print("N must be non-negative", file=sys.stderr)
        return 1

    prime_sum = sum(itertools.islice(primes(), n))
    fib_sum = sum(itertools.takewhile(lambda f: f < 10**15, fibonacci()))

    print(f"Sum of the first {n} primes: {prime_sum}")
    print(f"Sum of Fibonacci numbers below 10**15: {fib_sum}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
