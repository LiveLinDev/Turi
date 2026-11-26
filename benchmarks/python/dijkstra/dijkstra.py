"""Caminos mínimos en una cuadrícula con pesos (Dijkstra con heapq y tuplas)."""
import heapq
import sys

INF = 10 ** 18


def shortest(n):
    dist = [[INF] * n for _ in range(n)]
    dist[0][0] = 0
    heap = [(0, 0, 0)]
    while heap:
        d, i, j = heapq.heappop(heap)
        if d > dist[i][j]:
            continue
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ni, nj = i + di, j + dj
            if 0 <= ni < n and 0 <= nj < n:
                nd = d + (ni * 7 + nj * 13) % 10 + 1
                if nd < dist[ni][nj]:
                    dist[ni][nj] = nd
                    heapq.heappush(heap, (nd, ni, nj))
    return dist


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    dist = shortest(n)
    print(dist[n - 1][n - 1], sum(sum(row) for row in dist))


main()
