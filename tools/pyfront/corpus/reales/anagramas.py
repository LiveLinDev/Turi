#!/usr/bin/env python3
"""Group words from stdin into anagram groups and print the multi-word groups,
largest first."""

import sys
from collections import defaultdict


def read_words(stream):
    return stream.read().split()


def anagram_groups(words):
    groups = defaultdict(list)
    for word in words:
        key = "".join(sorted(word))
        groups[key].append(word)
    return groups


def main():
    groups = anagram_groups(read_words(sys.stdin))
    multi = [sorted(g) for g in groups.values() if len(g) > 1]
    multi.sort(key=len, reverse=True)
    for group in multi:
        print(" ".join(group))


if __name__ == "__main__":
    main()
