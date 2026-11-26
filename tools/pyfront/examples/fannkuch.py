"""Fannkuch-redux (benchmarks game), en Python corriente. Uso: fannkuch.py [n]"""
import sys


def fannkuch(n):
    perm = [0] * n
    perm1 = [0] * (n + 1)
    count = [0] * (n + 1)
    for i in range(n):
        perm1[i] = i
    max_flips = 0
    checksum = 0
    perm_count = 0
    r = n
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        for i in range(n):
            perm[i] = perm1[i]
        flips = 0
        k = perm[0]
        while k != 0:
            i = 0
            j = k
            while i < j:
                perm[i], perm[j] = perm[j], perm[i]
                i += 1
                j -= 1
            flips += 1
            k = perm[0]
        if flips > max_flips:
            max_flips = flips
        if perm_count % 2 == 0:
            checksum += flips
        else:
            checksum -= flips
        while True:
            if r == n:
                return checksum, max_flips
            first = perm1[0]
            for i in range(r):
                perm1[i] = perm1[i + 1]
            perm1[r] = first
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        perm_count += 1


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 7
    checksum, flips = fannkuch(n)
    print(checksum)
    print(flips)


main()
