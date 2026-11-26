"""binary-trees con una clase Node, como lo escribe cualquiera en Python."""
import sys


class Node:
    def __init__(self, left, right):
        self.left = left
        self.right = right


def make(depth):
    if depth == 0:
        return Node(None, None)
    return Node(make(depth - 1), make(depth - 1))


def check(node):
    if node.left is None:
        return 1
    return 1 + check(node.left) + check(node.right)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    min_depth = 4
    max_depth = max(min_depth + 2, n)
    print(check(make(max_depth + 1)))
    long_tree = make(max_depth)
    for depth in range(min_depth, max_depth + 1, 2):
        iterations = 1 << (max_depth - depth + min_depth)
        total = 0
        for _ in range(iterations):
            total += check(make(depth))
        print(total)
    print(check(long_tree))


main()
