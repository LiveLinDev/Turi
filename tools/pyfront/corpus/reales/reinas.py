#!/usr/bin/env python3
"""Count solutions to the N-queens problem for board sizes 4..N."""

import sys


def count_queens(size):
    """Count the number of distinct solutions for a size x size board."""
    solutions = 0

    def place(row, cols, diag1, diag2):
        """Recursively place queens row by row.

        cols  : bitmask of columns already occupied
        diag1 : bitmask of "/" diagonals occupied
        diag2 : bitmask of "\" diagonals occupied
        """
        nonlocal solutions
        if row == size:
            solutions += 1
            return
        available = ((1 << size) - 1) & ~(cols | diag1 | diag2)
        while available:
            bit = available & -available   # lowest set bit = leftmost free column
            available ^= bit
            place(row + 1,
                  cols | bit,
                  (diag1 | bit) << 1,
                  (diag2 | bit) >> 1)

    place(0, 0, 0, 0)
    return solutions


def main():
    n = 9
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: board size must be an integer, got {sys.argv[1]!r}")
        if n < 1:
            sys.exit("error: board size must be at least 1")

    for size in range(4, n + 1):
        print(f"{size}: {count_queens(size)}")


if __name__ == "__main__":
    main()
