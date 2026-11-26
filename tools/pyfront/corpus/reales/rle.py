#!/usr/bin/env python3
"""Run-length encoding / decoding round-trip benchmark."""

import argparse
import random
import sys


def encode(text: str) -> str:
    """Encode text as runs: each run becomes <count><char>."""
    parts = []
    i = 0
    n = len(text)
    while i < n:
        j = i + 1
        while j < n and text[j] == text[i]:
            j += 1
        run_len = j - i
        parts.append(str(run_len))
        parts.append(text[i])
        i = j
    return "".join(parts)


def decode(encoded: str) -> str:
    """Decode an RLE string produced by encode()."""
    parts = []
    i = 0
    n = len(encoded)
    while i < n:
        j = i
        while j < n and encoded[j].isdigit():
            j += 1
        count = int(encoded[i:j])
        parts.append(encoded[j] * count)
        i = j + 1
    return "".join(parts)


def generate_strings(count: int, rng: random.Random) -> list[str]:
    """Deterministically generate `count` test strings."""
    alphabet = "ABCDEF"
    strings = []
    for k in range(count):
        length = rng.randint(10, 400)
        # Bias toward runs by sometimes repeating the previous character.
        chars = []
        prev = None
        for _ in range(length):
            if prev is not None and rng.random() < 0.6:
                chars.append(prev)
            else:
                prev = rng.choice(alphabet)
                chars.append(prev)
        strings.append("".join(chars))
    return strings


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run-length encoding round-trip check."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=20000,
        help="number of strings to generate (default: 20000)",
    )
    args = parser.parse_args()

    if args.n <= 0:
        parser.error("N must be a positive integer")

    rng = random.Random(12345)
    strings = generate_strings(args.n, rng)

    total_original = 0
    total_encoded = 0
    failures = 0

    for s in strings:
        encoded = encode(s)
        decoded = decode(encoded)
        total_original += len(s)
        total_encoded += len(encoded)
        if decoded != s:
            failures += 1

    if failures:
        print(f"ERROR: {failures} round-trip failures", file=sys.stderr)
        return 1

    print(f"Strings processed: {len(strings)}")
    print(f"Total original length: {total_original}")
    print(f"Total encoded length:  {total_encoded}")
    ratio = total_encoded / total_original if total_original else 0.0
    print(f"Compression ratio:     {ratio:.4f}")
    print(f"All round trips OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
