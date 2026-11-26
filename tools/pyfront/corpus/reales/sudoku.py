#!/usr/bin/env python3
"""Solve a Sudoku puzzle by backtracking and report the recursion count."""

import sys

PUZZLE = (
    "530070000"
    "600195000"
    "098000060"
    "800060003"
    "400080001"
    "700020006"
    "060000280"
    "000419005"
    "000080079"
)

DEFAULT_RUNS = 20


def parse_puzzle(text):
    """Convert an 81-character puzzle string into a 9x9 grid of ints."""
    if len(text) != 81 or not text.isdigit():
        raise ValueError("puzzle must be exactly 81 digits")
    return [[int(ch) for ch in text[row * 9:(row + 1) * 9]] for row in range(9)]


def find_empty(grid):
    """Return the (row, col) of the first empty cell, or None if full."""
    for row in range(9):
        for col in range(9):
            if grid[row][col] == 0:
                return row, col
    return None


def is_valid(grid, row, col, value):
    """Check whether value can be placed at (row, col) without conflict."""
    if any(grid[row][other] == value for other in range(9)):
        return False
    if any(grid[other][col] == value for other in range(9)):
        return False
    box_row, box_col = (row // 3) * 3, (col // 3) * 3
    for r in range(box_row, box_row + 3):
        for c in range(box_col, box_col + 3):
            if grid[r][c] == value:
                return False
    return True


class SudokuSolver:
    """Backtracking solver that counts its recursive calls."""

    def __init__(self, grid):
        self.grid = grid
        self.calls = 0

    def solve(self):
        self.calls += 1
        spot = find_empty(self.grid)
        if spot is None:
            return True
        row, col = spot
        for value in range(1, 10):
            if is_valid(self.grid, row, col, value):
                self.grid[row][col] = value
                if self.solve():
                    return True
                self.grid[row][col] = 0
        return False


def format_grid(grid):
    """Render the grid as aligned rows with box separators."""
    lines = []
    for row in range(9):
        lines.append(" ".join(str(value) for value in grid[row]))
        if row in (2, 5):
            lines.append("-" * 17)
    return "\n".join(lines)


def main(argv):
    runs = DEFAULT_RUNS
    if len(argv) > 1:
        try:
            runs = int(argv[1])
        except ValueError:
            print(f"error: run count must be an integer, got {argv[1]!r}",
                  file=sys.stderr)
            return 1
        if runs < 1:
            print("error: run count must be at least 1", file=sys.stderr)
            return 1

    solver = None
    for _ in range(runs):
        solver = SudokuSolver(parse_puzzle(PUZZLE))
        if not solver.solve():
            print("error: puzzle has no solution", file=sys.stderr)
            return 1

    print(format_grid(solver.grid))
    print(f"Recursive calls in the last run: {solver.calls}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
