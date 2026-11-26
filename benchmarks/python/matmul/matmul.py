"""Producto de dos matrices de flotantes con listas de listas, escrito como lo escribe cualquiera."""
import sys


def make(n, seed):
    m = []
    for i in range(n):
        row = []
        for j in range(n):
            row.append(((i * 31 + j * 17 + seed) % 97) / 97.0)
        m.append(row)
    return m


def matmul(a, b):
    n = len(a)
    m = len(b[0])
    inner = len(b)
    c = [[0.0] * m for _ in range(n)]
    for i in range(n):
        for j in range(m):
            total = 0.0
            for k in range(inner):
                total += a[i][k] * b[k][j]
            c[i][j] = total
    return c


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    a = make(n, 1)
    b = make(n, 2)
    c = matmul(a, b)
    trace = 0.0
    for i in range(n):
        trace += c[i][i]
    total = 0.0
    for row in c:
        total += sum(row)
    print(f"{trace:.6f} {total:.6f}")


main()
