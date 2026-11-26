#!/usr/bin/env python3
"""Count word frequencies from standard input.

Reads text from stdin, tokenizes it into words (case-insensitive,
punctuation stripped), and prints the 15 most common words, the
number of distinct words, and the longest word.
"""

import sys
from collections import Counter


def tokenize(text):
    """Split text into lowercase words, stripping punctuation."""
    words = []
    token = []
    for ch in text:
        if ch.isalnum():
            token.append(ch.lower())
        elif token:
            words.append("".join(token))
            token = []
    if token:
        words.append("".join(token))
    return words


def main():
    text = sys.stdin.read()
    words = tokenize(text)

    counts = Counter(words)
    distinct = len(counts)
    longest = max(words, key=len) if words else ""

    print("Top 15 most common words:")
    for word, count in counts.most_common(15):
        print(f"{word}: {count}")
    print(f"Distinct words: {distinct}")
    print(f"Longest word: {longest}")


if __name__ == "__main__":
    main()
