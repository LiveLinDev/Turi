"""N reinas: cuántas soluciones hay, con una función interna que lleva la cuenta en la de fuera."""
import sys


def count_solutions(n):
    count = 0
    cols = set()
    diag1 = set()
    diag2 = set()

    def place(row):
        nonlocal count
        if row == n:
            count += 1
            return
        for c in range(n):
            if c in cols or (row - c) in diag1 or (row + c) in diag2:
                continue
            cols.add(c)
            diag1.add(row - c)
            diag2.add(row + c)
            place(row + 1)
            cols.remove(c)
            diag1.remove(row - c)
            diag2.remove(row + c)

    place(0)
    return count


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    print(count_solutions(n))


main()
