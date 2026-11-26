"""Conjunto de Mandelbrot, 50 iteraciones, de 8 en 8 píxeles (como benchmarks/game). Uso: mandelbrot.py [n]"""
import sys


def mandelbrot(n):
    inv = 2.0 / n
    cr = [0.0] * 8
    zr = [0.0] * 8
    zi = [0.0] * 8
    inside = 0
    checksum = 0
    for y in range(n):
        ci = y * inv - 1.0
        x = 0
        while x < n:
            for k in range(8):
                cr[k] = (x + k) * inv - 1.5
                zr[k] = 0.0
                zi[k] = 0.0
            for _ in range(50):
                for k in range(8):
                    r = zr[k]
                    i = zi[k]
                    zr[k] = r * r - i * i + cr[k]
                    zi[k] = 2.0 * r * i + ci
            byte = 0
            for k in range(8):
                byte = byte * 2
                if x + k < n and zr[k] * zr[k] + zi[k] * zi[k] <= 4.0:
                    byte += 1
                    inside += 1
            checksum = (checksum * 31 + byte) % 1000000007
            x += 8
    return inside, checksum


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    inside, checksum = mandelbrot(n)
    print(inside)
    print(checksum)


main()
