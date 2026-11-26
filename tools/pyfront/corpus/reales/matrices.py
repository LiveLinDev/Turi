#!/usr/bin/env python3
"""Multiply two N x N matrices built from a simple formula and report
the trace and total sum of the product."""

import sys


def build_matrix(n):
    """Create an N x N matrix of floats using a deterministic formula."""
    return [
        [((i * n + j) % 13) + (i + j) / 7.0 for j in range(n)]
        for i in range(n)
    ]


def matmul(a, b):
    """Multiply two square matrices represented as lists of lists."""
    n = len(a)
    result = [[0.0] * n for _ in range(n)]
    for i in range(n):
        a_row = a[i]
        res_row = result[i]
        for k in range(n):
            a_ik = a_row[k]
            if a_ik == 0.0:
                continue
            b_row = b[k]
            for j in range(n):
                res_row[j] += a_ik * b_row[j]
    return result


def trace(m):
    return sum(m[i][i] for i in range(len(m)))


def total_sum(m):
    return sum(sum(row) for row in m)


def main():
    n = 160
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: matrix size must be an integer, got {sys.argv[1]!r}")
        if n <= 0:
            sys.exit("error: matrix size must be positive")

    a = build_matrix(n)
    b = build_matrix(n)
    c = matmul(a, b)

    print(f"trace: {round(trace(c), 6):.6f}")
    print(f"sum:   {round(total_sum(c), 6):.6f}")


if __name__ == "__main__":
    main()
