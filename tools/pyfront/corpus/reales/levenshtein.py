#!/usr/bin/env python3
"""Compute Levenshtein edit distances between deterministic string pairs.

Usage: edit_distance.py [N]

N (default 300) is the number of string pairs. Each pair consists of two
deterministically generated strings of length 60; the program computes the
edit distance of each pair with the classic dynamic-programming algorithm
and prints the total distance over all pairs to standard output.
"""

import sys


def generate_pair(index: int, length: int = 60) -> tuple[str, str]:
    """Deterministically produce the i-th pair of strings of the given length."""
    alphabet = "ACGT"
    a = "".join(alphabet[(index * 7 + k * 3) % 4] for k in range(length))
    b = "".join(alphabet[(index * 11 + k * 5 + index // 13) % 4] for k in range(length))
    return a, b


def levenshtein(a: str, b: str) -> int:
    """Levenshtein edit distance via dynamic programming (two-row variant)."""
    m, n = len(a), len(b)
    if m == 0:
        return n
    if n == 0:
        return m

    previous = list(range(n + 1))
    for i in range(1, m + 1):
        current = [i] + [0] * n
        ai = a[i - 1]
        for j in range(1, n + 1):
            cost = 0 if ai == b[j - 1] else 1
            current[j] = min(
                previous[j] + 1,        # deletion
                current[j - 1] + 1,     # insertion
                previous[j - 1] + cost, # substitution / match
            )
        previous = current
    return previous[n]


def main() -> None:
    n = 300
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: N must be an integer, got {sys.argv[1]!r}")
        if n < 0:
            sys.exit("error: N must be non-negative")

    total = 0
    for i in range(n):
        a, b = generate_pair(i)
        total += levenshtein(a, b)

    print(total)


if __name__ == "__main__":
    main()
