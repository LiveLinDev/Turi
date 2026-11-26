#!/usr/bin/env python3
"""Union-find (disjoint set) demo: add N random edges among N nodes
and report the number of connected components and the largest one."""

import random
import sys


class UnionFind:
    """Disjoint-set forest with path compression and union by rank."""

    def __init__(self, n):
        self.parent = list(range(n))
        self.rank = [0] * n
        self.size = [1] * n
        self.count = n

    def find(self, x):
        root = x
        while self.parent[root] != root:
            root = self.parent[root]
        # Path compression: point every node on the path directly at the root.
        while self.parent[x] != root:
            self.parent[x], x = root, self.parent[x]
        return root

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return False
        # Union by rank: attach the shallower tree under the deeper one.
        if self.rank[ra] < self.rank[rb]:
            ra, rb = rb, ra
        self.parent[rb] = ra
        self.size[ra] += self.size[rb]
        if self.rank[ra] == self.rank[rb]:
            self.rank[ra] += 1
        self.count -= 1
        return True

    def largest_component(self):
        return max(self.size)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    rng = random.Random(9)

    uf = UnionFind(n)
    for _ in range(n):
        uf.union(rng.randrange(n), rng.randrange(n))

    print(f"nodes: {n}")
    print(f"components: {uf.count}")
    print(f"largest component: {uf.largest_component()}")


if __name__ == "__main__":
    main()
