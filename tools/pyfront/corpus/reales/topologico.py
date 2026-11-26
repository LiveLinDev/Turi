#!/usr/bin/env python3
"""Generate a random DAG, topologically sort it (Kahn's algorithm),
and compute the length of the longest path."""

import random
import sys


def build_dag(n, rng):
    """Return adjacency dict {node: [successors]} for a DAG on nodes 0..n-1.

    Edges only go from lower to higher indices, which guarantees acyclicity.
    """
    dag = {i: [] for i in range(n)}
    for u in range(n):
        for v in range(u + 1, n):
            if rng.random() < 0.5:
                dag[u].append(v)
    return dag


def topological_order(dag, n):
    """Kahn's algorithm: repeatedly remove nodes with no remaining predecessors."""
    indegree = {i: 0 for i in range(n)}
    for u in range(n):
        for v in dag[u]:
            indegree[v] += 1

    ready = [i for i in range(n) if indegree[i] == 0]
    order = []
    while ready:
        u = ready.pop()
        order.append(u)
        for v in dag[u]:
            indegree[v] -= 1
            if indegree[v] == 0:
                ready.append(v)

    if len(order) != n:
        raise RuntimeError("graph is not a DAG")
    return order


def longest_path_length(dag, order):
    """Longest path (in edges) through the DAG, computed in topological order."""
    dist = {u: 0 for u in order}
    best = 0
    for u in order:
        for v in dag[u]:
            if dist[u] + 1 > dist[v]:
                dist[v] = dist[u] + 1
                if dist[v] > best:
                    best = dist[v]
    return best


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000

    rng = random.Random(5)
    dag = build_dag(n, rng)
    order = topological_order(dag, n)
    length = longest_path_length(dag, order)

    print(f"longest path length: {length}")
    print(f"first ten nodes of topological order: {order[:10]}")


if __name__ == "__main__":
    main()
