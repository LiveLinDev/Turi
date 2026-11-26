"""Norma espectral (benchmarks game), en Python corriente. Uso: spectralnorm.py [n]"""
import sys
from math import sqrt


def eval_a(i, j):
    return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)


def times(u, v, n):
    for i in range(n):
        total = 0.0
        for j in range(n):
            total += eval_a(i, j) * u[j]
        v[i] = total


def times_transposed(u, v, n):
    for i in range(n):
        total = 0.0
        for j in range(n):
            total += eval_a(j, i) * u[j]
        v[i] = total


def spectral(n):
    u = [1.0] * n
    v = [0.0] * n
    t = [0.0] * n
    for _ in range(10):
        times(u, t, n)
        times_transposed(t, v, n)
        times(v, t, n)
        times_transposed(t, u, n)
    vbv = 0.0
    vv = 0.0
    for i in range(n):
        vbv += u[i] * v[i]
        vv += v[i] * v[i]
    return sqrt(vbv / vv)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    print("%.9f" % spectral(n))


main()
