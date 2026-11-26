"""Juego de la vida en un tablero sin bordes guardado como conjunto de tuplas (x, y)."""
import sys


def soup(size):
    seed = 7
    live = set()
    for x in range(size):
        for y in range(size):
            seed = (seed * 1103515245 + 12345) % 2147483648
            if (seed >> 16) % 3 == 0:
                live.add((x, y))
    return live


def step(live):
    counts = {}
    for (x, y) in live:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx or dy:
                    key = (x + dx, y + dy)
                    counts[key] = counts.get(key, 0) + 1
    return {cell for cell, c in counts.items() if c == 3 or (c == 2 and cell in live)}


def main():
    generations = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    live = soup(64)
    for g in range(generations):
        if g % 10 == 0:
            print(g, len(live))
        live = step(live)
    print(generations, len(live))


main()
