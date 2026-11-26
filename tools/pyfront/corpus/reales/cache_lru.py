#!/usr/bin/env python3
"""Replay a deterministic get/put workload against an LRU cache and report the hit rate."""

import argparse
import random
import sys
from collections import OrderedDict


class LRUCache:
    """Fixed-capacity LRU cache backed by collections.OrderedDict."""

    def __init__(self, capacity: int):
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        self.capacity = capacity
        self._data = OrderedDict()

    def get(self, key):
        """Return the value for key, or None if absent. Counts as a use."""
        entry = self._data.get(key)
        if entry is None:
            return None
        self._data.move_to_end(key)
        return entry

    def put(self, key, value):
        """Insert or update key, evicting the least recently used item if full."""
        if key in self._data:
            self._data.move_to_end(key)
        self._data[key] = value
        if len(self._data) > self.capacity:
            self._data.popitem(last=False)

    def __len__(self):
        return len(self._data)


def run_workload(cache: LRUCache, n_ops: int, seed: int = 12345):
    """Replay n_ops deterministic get/put operations over keys 0..999."""
    rng = random.Random(seed)
    keys = range(1000)
    hits = 0
    gets = 0

    for i in range(n_ops):
        key = rng.randrange(1000)
        if i % 3 == 0:
            cache.put(key, key * 2)
        else:
            gets += 1
            if cache.get(key) is not None:
                hits += 1

    return hits, gets


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="LRU cache hit-rate benchmark over keys 0..999."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=300000,
        help="number of operations to replay (default: 300000)",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.n < 0:
        print("error: N must be non-negative", file=sys.stderr)
        return 1

    cache = LRUCache(capacity=200)
    hits, gets = run_workload(cache, args.n)

    rate = hits / gets if gets else 0.0
    print(f"operations: {args.n}")
    print(f"gets:       {gets}")
    print(f"hits:       {hits}")
    print(f"hit rate:   {rate:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
