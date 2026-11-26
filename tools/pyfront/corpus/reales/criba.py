#!/usr/bin/env python3
"""Count primes below N with a sieve of Eratosthenes, show the last ten,
and report the largest gap between consecutive primes.

Usage: primes.py [N]   (default N = 3000000)
"""

import sys
from itertools import compress
from math import isqrt


def sieve_primes(limit):
    """Return a list of all primes strictly below limit."""
    if limit <= 2:
        return []
    flags = bytearray(b"\x01") * limit
    flags[0] = flags[1] = 0
    for i in range(2, isqrt(limit - 1) + 1):
        if flags[i]:
            start = i * i
            flags[start::i] = b"\x00" * ((limit - start - 1) // i + 1)
    return list(compress(range(limit), flags))


def main(argv):
    try:
        n = int(argv[1]) if len(argv) > 1 else 3_000_000
    except ValueError:
        sys.exit(f"error: N must be an integer, got {argv[1]!r}")
    if n < 0:
        sys.exit("error: N must be non-negative")

    primes = sieve_primes(n)

    print(f"Primes below {n}: {len(primes)}")

    if primes:
        tail = primes[-10:]
        print("Last ten: " + ", ".join(map(str, tail)))
    else:
        print("Last ten: (none)")

    if len(primes) >= 2:
        gaps = (b - a for a, b in zip(primes, primes[1:]))
        print(f"Largest gap between consecutive primes: {max(gaps)}")
    else:
        print("Largest gap between consecutive primes: (undefined)")


if __name__ == "__main__":
    main(sys.argv)
