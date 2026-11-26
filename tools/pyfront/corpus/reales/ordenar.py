#!/usr/bin/env python3
"""Sort the same list of pseudo-random integers with merge sort, quicksort,
and the built-in sorted(), verify the results agree, and print a sample."""

import argparse
import random
import sys


def merge_sort(values):
    """Return a sorted copy of values using merge sort."""
    n = len(values)
    if n <= 1:
        return list(values)
    mid = n // 2
    left = merge_sort(values[:mid])
    right = merge_sort(values[mid:])

    merged = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            merged.append(left[i])
            i += 1
        else:
            merged.append(right[j])
            j += 1
    merged.extend(left[i:])
    merged.extend(right[j:])
    return merged


def quicksort(values, lo=0, hi=None, rng=None):
    """Sort values[lo:hi] in place with quicksort using a random pivot.

    Always recurses on the smaller partition first, which bounds the
    recursion depth to O(log n) even in the worst case.
    """
    if hi is None:
        hi = len(values)
    if rng is None:
        rng = random.Random()

    while lo < hi - 1:
        pivot = values[rng.randrange(lo, hi)]
        # Hoare-style partition: first index with value >= pivot.
        i = lo
        j = hi - 1
        while i <= j:
            while values[i] < pivot:
                i += 1
            while values[j] > pivot:
                j -= 1
            if i <= j:
                values[i], values[j] = values[j], values[i]
                i += 1
                j -= 1

        # Recurse on the smaller side, loop on the larger side.
        if j - lo < hi - i:
            quicksort(values, lo, j + 1, rng)
            lo = i
        else:
            quicksort(values, i, hi, rng)
            hi = j + 1
    return values


def main():
    parser = argparse.ArgumentParser(
        description="Compare merge sort, quicksort, and sorted().")
    parser.add_argument("n", nargs="?", type=int, default=200000,
                        help="number of integers to sort (default: 200000)")
    args = parser.parse_args()
    n = args.n
    if n < 0:
        parser.error("N must be non-negative")

    rng = random.Random(42)
    data = [rng.randrange(-10**9, 10**9) for _ in range(n)]

    expected = sorted(data)
    merge_result = merge_sort(data)
    quick_result = quicksort(list(data), rng=random.Random(123))

    if merge_result != expected:
        print("ERROR: merge sort result differs from sorted()",
              file=sys.stderr)
        sys.exit(1)
    if quick_result != expected:
        print("ERROR: quicksort result differs from sorted()",
              file=sys.stderr)
        sys.exit(1)

    print(f"sorted {n} integers: all three results agree")
    print("first five:", " ".join(map(str, expected[:5])))
    print("last five: ", " ".join(map(str, expected[-5:])))


if __name__ == "__main__":
    main()
