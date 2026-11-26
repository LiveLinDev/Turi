#!/usr/bin/env python3
"""Ten-pin bowling scoring.

Generates N deterministic games, scores each one, and reports the total
of all scores plus the best single game.
"""

import argparse
import random
import sys


class BowlingGame:
    """Scores a ten-pin bowling game roll by roll."""

    MAX_ROLLS = 21  # 12 strikes would exceed this; we cap rolls explicitly.

    def __init__(self):
        self.rolls = []

    def roll(self, pins):
        if not 0 <= pins <= 10:
            raise ValueError("pins knocked down must be between 0 and 10")
        self.rolls.append(pins)

    def score(self):
        """Return the total score, or None if the game is unfinished."""
        total = 0
        frame = 0
        i = 0
        n = len(self.rolls)
        while frame < 10:
            if i >= n:
                return None  # game not complete
            if self.rolls[i] == 10:  # strike
                if i + 2 >= n:
                    return None
                total += 10 + self.rolls[i + 1] + self.rolls[i + 2]
                i += 1
            elif i + 1 >= n:
                return None
            elif self.rolls[i] + self.rolls[i + 1] == 10:  # spare
                if i + 2 >= n:
                    return None
                total += 10 + self.rolls[i + 2]
                i += 2
            else:
                total += self.rolls[i] + self.rolls[i + 1]
                i += 2
            frame += 1
        return total


def generate_game(rng):
    """Play one complete game deterministically from the given RNG."""
    game = BowlingGame()
    frame = 0
    while frame < 10:
        first = rng.randint(0, 10)
        game.roll(first)
        if first == 10:
            # Strike: in frames 1-9 the frame ends; in frame 10 bonus rolls follow.
            frame += 1
            if frame == 10:
                # Tenth frame: two more rolls, pins reset after a strike.
                game.roll(rng.randint(0, 10))
                if game.rolls[-1] == 10:
                    game.roll(rng.randint(0, 10))
                else:
                    game.roll(rng.randint(0, 10 - game.rolls[-1]))
            continue
        second = rng.randint(0, 10 - first)
        game.roll(second)
        if first + second == 10:
            # Spare: frame 10 gets one bonus roll.
            frame += 1
            if frame == 10:
                game.roll(rng.randint(0, 10))
            continue
        frame += 1
    return game


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Score a batch of deterministic bowling games."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=50000,
        help="number of games to score (default: 50000)",
    )
    args = parser.parse_args(argv)
    if args.n <= 0:
        parser.error("N must be a positive integer")

    rng = random.Random(12345)
    total = 0
    best = -1
    for _ in range(args.n):
        game = generate_game(rng)
        s = game.score()
        if s is None:
            raise RuntimeError("generated an incomplete game")
        total += s
        if s > best:
            best = s

    print(f"games: {args.n}")
    print(f"total score: {total}")
    print(f"best score: {best}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
