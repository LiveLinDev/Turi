#!/usr/bin/env python3
"""Factorize every integer from 2 to N and report summary statistics.

Usage: python3 factor_stats.py [N]

N defaults to 200000 and sets the size of the work. The program prints:
  * the number with the most distinct prime factors,
  * the most common largest prime factor among the numbers 2..N,
  * the count of primes in [2, N].
"""

import math
import sys

DEFAULT_N = 200000


def smallest_prime_factors(limit):
    """Return a table spf where spf[n] is the smallest prime factor of n."""
    spf = list(range(limit + 1))
    spf[0] = 0
    spf[1] = 1
    for i in range(2, math.isqrt(limit) + 1):
        if spf[i] == i:  # i is prime: no smaller factor was found before it
            for multiple in range(i * i, limit + 1, i):
                if spf[multiple] == multiple:
                    spf[multiple] = i
    return spf


def analyse(limit):
    """Factorize 2..limit and gather the requested statistics."""
    spf = smallest_prime_factors(limit)

    best_number = 0
    best_distinct = 0
    largest_counts = [0] * (limit + 1)
    primes = 0

    for n in range(2, limit + 1):
        distinct = 0
        largest = 0
        m = n
        while m > 1:
            p = spf[m]
            if p != largest:        # factors appear in increasing order,
                distinct += 1       # so the last distinct one is the largest
                largest = p
            m //= p
        largest_counts[largest] += 1
        if distinct == 1:
            primes += 1
        if distinct > best_distinct:
            best_distinct = distinct
            best_number = n

    # Ascending scan makes ties resolve to the smallest prime.
    most_common = 2
    most_common_count = largest_counts[2]
    for p in range(3, limit + 1):
        if largest_counts[p] > most_common_count:
            most_common = p
            most_common_count = largest_counts[p]

    return best_number, best_distinct, most_common, most_common_count, primes


def parse_args(argv):
    """Parse the single optional integer argument."""
    if not argv:
        return DEFAULT_N
    if len(argv) > 1:
        raise SystemExit("usage: factor_stats.py [N]")
    try:
        return int(argv[0])
    except ValueError:
        raise SystemExit("N must be an integer")


def main():
    n = parse_args(sys.argv[1:])
    if n < 2:
        raise SystemExit("N must be at least 2")

    best_number, best_distinct, most_common, count, primes = analyse(n)

    print(f"Number with most distinct prime factors: {best_number} "
          f"({best_distinct} distinct primes)")
    print(f"Most common largest prime factor: {most_common} "
          f"(largest factor of {count} numbers)")
    print(f"Primes in [2, {n}]: {primes}")


if __name__ == "__main__":
    main()
