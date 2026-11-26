#!/usr/bin/env python3
"""Convert integers 1..N to Roman numerals and back, verifying round trips.

Usage: roman.py [N]
"""

import sys

ROMAN_VALUES = [
    (1000, "M"),
    (900, "CM"),
    (500, "D"),
    (400, "CD"),
    (100, "C"),
    (90, "XC"),
    (50, "L"),
    (40, "XL"),
    (10, "X"),
    (9, "IX"),
    (5, "V"),
    (4, "IV"),
    (1, "I"),
]


def to_roman(n):
    """Convert a positive integer to a Roman numeral string."""
    if n <= 0:
        raise ValueError("Roman numerals require a positive integer")
    parts = []
    remaining = n
    for value, symbol in ROMAN_VALUES:
        count, remaining = divmod(remaining, value)
        parts.append(symbol * count)
    return "".join(parts)


def from_roman(s):
    """Convert a Roman numeral string back to an integer."""
    values = {char: value for value, char in ROMAN_VALUES}
    total = 0
    prev = 0
    for char in reversed(s):
        value = values[char]
        if value < prev:
            total -= value
        else:
            total += value
            prev = value
    return total


def main():
    n = 3999
    if len(sys.argv) > 1:
        try:
            n = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: N must be an integer, got {sys.argv[1]!r}")
        if n < 1:
            sys.exit("error: N must be at least 1")

    longest = ""
    total_chars = 0
    for i in range(1, n + 1):
        numeral = to_roman(i)
        back = from_roman(numeral)
        if back != i:
            sys.exit(f"error: round trip failed for {i}: {numeral} -> {back}")
        total_chars += len(numeral)
        if len(numeral) > len(longest):
            longest = numeral

    print(f"Range: 1..{n}")
    print(f"Longest numeral: {longest} ({len(longest)} characters)")
    print(f"Total characters: {total_chars}")


if __name__ == "__main__":
    main()
