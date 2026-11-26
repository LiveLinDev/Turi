#!/usr/bin/env python3
"""Build a singly linked list of N numbers, reverse it, and report
its length, sum, and first five values."""

import argparse
import sys


class LinkedList:
    """A singly linked list supporting append, prepend, reverse,
    iteration, and len()."""

    class _Node:
        __slots__ = ("value", "next")

        def __init__(self, value, next=None):
            self.value = value
            self.next = next

    def __init__(self):
        self._head = None
        self._tail = None
        self._size = 0

    def append(self, value):
        """Add a value at the end of the list."""
        node = self._Node(value)
        if self._tail is None:
            self._head = self._tail = node
        else:
            self._tail.next = node
            self._tail = node
        self._size += 1

    def prepend(self, value):
        """Add a value at the front of the list."""
        self._head = self._Node(value, self._head)
        if self._tail is None:
            self._tail = self._head
        self._size += 1

    def reverse(self):
        """Reverse the list in place."""
        prev = None
        current = self._head
        self._tail = self._head
        while current is not None:
            nxt = current.next
            current.next = prev
            prev = current
            current = nxt
        self._head = prev

    def __len__(self):
        return self._size

    def __iter__(self):
        current = self._head
        while current is not None:
            yield current.value
            current = current.next


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Reverse a linked list of N numbers and report stats."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=200000,
        help="number of elements to build (default: %(default)s)",
    )
    return parser.parse_args(argv)


def main(argv=None):
    n = parse_args(argv).n
    if n < 0:
        print("error: N must be non-negative", file=sys.stderr)
        return 1

    lst = LinkedList()
    for i in range(n):
        lst.append(i)

    lst.reverse()

    total = sum(lst)
    first_five = list(itertools_slice(lst, 5))

    print(f"length: {len(lst)}")
    print(f"sum: {total}")
    print("first five:", " ".join(map(str, first_five)))
    return 0


def itertools_slice(seq, count):
    """Yield at most `count` items from an iterable."""
    it = iter(seq)
    for _ in range(count):
        try:
            yield next(it)
        except StopIteration:
            break


if __name__ == "__main__":
    sys.exit(main())
