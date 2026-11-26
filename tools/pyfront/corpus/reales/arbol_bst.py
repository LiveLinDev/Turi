#!/usr/bin/env python3
"""Exercise a binary search tree: insert pseudo-random keys, then report
size, height, the smallest keys in order, and how many keys in a range
are actually present."""

import itertools
import random
import sys


class BST:
    """A plain unbalanced binary search tree with set semantics."""

    class Node:
        __slots__ = ("key", "left", "right")

        def __init__(self, key):
            self.key = key
            self.left = None
            self.right = None

    def __init__(self):
        self.root = None
        self._size = 0

    def __len__(self):
        return self._size

    def insert(self, key):
        """Add key; duplicates are ignored."""
        if self.root is None:
            self.root = self.Node(key)
            self._size = 1
            return
        node = self.root
        while True:
            if key == node.key:
                return
            if key < node.key:
                if node.left is None:
                    node.left = self.Node(key)
                    self._size += 1
                    return
                node = node.left
            else:
                if node.right is None:
                    node.right = self.Node(key)
                    self._size += 1
                    return
                node = node.right

    def contains(self, key):
        node = self.root
        while node is not None:
            if key == node.key:
                return True
            node = node.left if key < node.key else node.right
        return False

    def __iter__(self):
        """In-order traversal, lazily, from smallest to largest key."""
        yield from self._inorder(self.root)

    def _inorder(self, node):
        if node is not None:
            yield from self._inorder(node.left)
            yield node.key
            yield from self._inorder(node.right)

    def height(self):
        return self._height(self.root)

    def _height(self, node):
        if node is None:
            return 0
        return 1 + max(self._height(node.left), self._height(node.right))


def main(argv):
    n = int(argv[1]) if len(argv) > 1 else 50000
    if n < 0:
        raise SystemExit("N must be non-negative")

    rng = random.Random(3)
    tree = BST()
    for _ in range(n):
        tree.insert(rng.randrange(n))

    first_ten = list(itertools.islice(tree, 10))
    present = sum(1 for k in range(n) if tree.contains(k))

    print(f"size: {len(tree)}")
    print(f"height: {tree.height()}")
    print("first ten in order:", ", ".join(str(k) for k in first_ten))
    print(f"present from 0..{n - 1}: {present} of {n}")


if __name__ == "__main__":
    main(sys.argv)
