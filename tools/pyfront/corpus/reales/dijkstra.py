#!/usr/bin/env python3
"""Shortest paths on a weighted N x N grid using Dijkstra's algorithm.

Node (r, c) is connected to its four orthogonal neighbours. Moving into a
cell costs that cell's weight, computed from its coordinates. The program
prints the shortest-path distance from the top-left corner to the
bottom-right corner, followed by the sum of all distances.
"""

import argparse
import heapq
import sys


def cell_cost(r, c):
    """Deterministic weight for the cell at (r, c)."""
    return (r * r + c * c) % 10 + 1


def dijkstra(n):
    """Return the distance grid from (0, 0) to every cell of an n x n grid."""
    dist = [[float("inf")] * n for _ in range(n)]
    dist[0][0] = 0
    heap = [(0, 0, 0)]  # (distance, row, col)

    while heap:
        d, r, c = heapq.heappop(heap)
        if d > dist[r][c]:
            continue  # stale entry
        for dr, dc in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            nr, nc = r + dr, c + dc
            if 0 <= nr < n and 0 <= nc < n:
                nd = d + cell_cost(nr, nc)
                if nd < dist[nr][nc]:
                    dist[nr][nc] = nd
                    heapq.heappush(heap, (nd, nr, nc))

    return dist


def main():
    parser = argparse.ArgumentParser(
        description="Dijkstra shortest paths on an N x N weighted grid."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=120,
        help="grid size (default: 120)",
    )
    args = parser.parse_args()

    n = args.n
    if n <= 0:
        parser.error("grid size must be a positive integer")

    dist = dijkstra(n)
    total = sum(sum(row) for row in dist)

    print(dist[n - 1][n - 1])
    print(total)


if __name__ == "__main__":
    main()
