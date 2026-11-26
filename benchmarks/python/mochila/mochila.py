"""Mochila 0/1 con una función recursiva memorizada con functools.lru_cache."""
import sys
from functools import lru_cache


def items(n):
    weights, values = [], []
    for i in range(n):
        weights.append(5 + (i * 37) % 41)
        values.append(10 + (i * 53) % 97)
    return weights, values


def solve(weights, values, capacity):
    n = len(weights)

    @lru_cache(maxsize=None)
    def best(i, cap):
        if i == n:
            return 0
        skip = best(i + 1, cap)
        if weights[i] <= cap:
            return max(skip, values[i] + best(i + 1, cap - weights[i]))
        return skip

    return best(0, capacity)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    weights, values = items(n)
    print(solve(weights, values, 10 * n))


main()
