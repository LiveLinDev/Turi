"""Criba de Eratóstenes: cuántos primos hay por debajo de N y cuánto suman."""
import sys


def sieve(n):
    is_prime = [True] * n
    is_prime[0] = False
    is_prime[1] = False
    i = 2
    while i * i < n:
        if is_prime[i]:
            for j in range(i * i, n, i):
                is_prime[j] = False
        i += 1
    return is_prime


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
    flags = sieve(n)
    count = 0
    total = 0
    for k in range(n):
        if flags[k]:
            count += 1
            total += k
    print(count, total)


main()
