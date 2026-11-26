#!/usr/bin/env python3
"""Compute the longest common subsequence (LCS) of two deterministic strings.

Usage: lcs.py [N]

N is the length of both input strings (default 1500). The strings are
generated deterministically from a fixed seed, so results are reproducible.
"""

import sys
from random import Random

DEFAULT_N = 1500
SEED = 20240517
ALPHABET = "ACGT"


def make_strings(n):
    """Return two deterministic strings of length n."""
    rng = Random(SEED)
    a = "".join(rng.choice(ALPHABET) for _ in range(n))
    b = "".join(rng.choice(ALPHABET) for _ in range(n))
    return a, b


def lcs_table(a, b):
    """Build the (len(a)+1) x (len(b)+1) DP table for the LCS problem."""
    la, lb = len(a), len(b)
    dp = [[0] * (lb + 1) for _ in range(la + 1)]
    for i in range(1, la + 1):
        ca = a[i - 1]
        prev_row = dp[i - 1]
        row = dp[i]
        for j in range(1, lb + 1):
            if ca == b[j - 1]:
                row[j] = prev_row[j - 1] + 1
            else:
                up = prev_row[j]
                left = row[j - 1]
                row[j] = up if up > left else left
    return dp


def reconstruct(dp, a, b):
    """Walk the DP table backwards to recover one LCS of a and b."""
    i, j = len(a), len(b)
    chars = []
    while i > 0 and j > 0:
        if a[i - 1] == b[j - 1] and dp[i][j] == dp[i - 1][j - 1] + 1:
            chars.append(a[i - 1])
            i -= 1
            j -= 1
        elif dp[i - 1][j] == dp[i][j]:
            i -= 1
        else:
            j -= 1
    return "".join(reversed(chars))


def parse_args(argv):
    if len(argv) <= 1:
        return DEFAULT_N
    try:
        n = int(argv[1])
    except ValueError:
        raise SystemExit(f"error: N must be an integer, got {argv[1]!r}")
    if n < 0:
        raise SystemExit("error: N must be non-negative")
    return n


def main(argv):
    n = parse_args(argv)
    a, b = make_strings(n)
    dp = lcs_table(a, b)
    subsequence = reconstruct(dp, a, b)
    print(f"length: {dp[n][n]}")
    print(f"subsequence: {subsequence}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
