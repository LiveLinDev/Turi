#!/usr/bin/env python3
"""Conway's Game of Life on an unbounded board.

Usage: life.py [N]

Runs N generations starting from a deterministic pseudo-random 60x60
pattern, printing the population every 10 generations.
"""

import random
import sys


def step(board):
    """Advance the board one generation; board is a set of (x, y) cells."""
    # Count neighbours by incrementing counters around each live cell.
    neighbour_counts = {}
    for (x, y) in board:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                key = (x + dx, y + dy)
                neighbour_counts[key] = neighbour_counts.get(key, 0) + 1

    new_board = set()
    for cell, count in neighbour_counts.items():
        if count == 3 or (count == 2 and cell in board):
            new_board.add(cell)
    return new_board


def seed_board(rng, width=60, height=60):
    """Build the initial pattern deterministically from the given RNG."""
    return {
        (x, y)
        for x in range(width)
        for y in range(height)
        if rng.random() < 0.3
    }


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    rng = random.Random(42)
    board = seed_board(rng)

    for generation in range(n + 1):
        if generation % 10 == 0:
            print(f"generation {generation:4d}: population {len(board)}")
        board = step(board)


if __name__ == "__main__":
    main()
