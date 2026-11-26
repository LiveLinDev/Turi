#!/usr/bin/env python3
"""Deterministic k-means (k=5) on synthetic 2D points."""

import argparse
import math
import random
import sys

K = 5
ITERATIONS = 20
SEED = 42

# Fixed generator centers (deterministic workload).
TRUE_CENTERS = [(10.0, 10.0), (-20.0, 30.0), (25.0, -15.0),
                (-35.0, -35.0), (40.0, 40.0)]
SPREAD = 8.0


def generate_points(n, rng):
    """Generate n points clustered around TRUE_CENTERS."""
    points = []
    for _ in range(n):
        cx, cy = rng.choice(TRUE_CENTERS)
        x = rng.gauss(cx, SPREAD)
        y = rng.gauss(cy, SPREAD)
        points.append((x, y))
    return points


def init_centroids(points, rng, k):
    """Pick k distinct points as initial centroids."""
    return [points[i] for i in rng.sample(range(len(points)), k)]


def nearest_centroid(point, centroids):
    """Return index of the centroid closest to point."""
    best_idx = 0
    best_dist = math.inf
    px, py = point
    for idx, (cx, cy) in enumerate(centroids):
        dx = px - cx
        dy = py - cy
        dist = dx * dx + dy * dy
        if dist < best_dist:
            best_dist = dist
            best_idx = idx
    return best_idx


def run_kmeans(points, centroids, iterations):
    """Run Lloyd's algorithm for a fixed number of iterations."""
    n = len(points)
    assignments = [0] * n
    for _ in range(iterations):
        sums = [[0.0, 0.0, 0] for _ in centroids]
        for i, p in enumerate(points):
            c = nearest_centroid(p, centroids)
            assignments[i] = c
            sums[c][0] += p[0]
            sums[c][1] += p[1]
            sums[c][2] += 1
        for j in range(len(centroids)):
            count = sums[j][2]
            if count > 0:
                centroids[j] = (sums[j][0] / count, sums[j][1] / count)
            # Empty cluster: keep its previous position.
    return centroids, assignments


def main():
    parser = argparse.ArgumentParser(
        description="k-means clustering (k=5) on deterministic 2D points.")
    parser.add_argument("n", nargs="?", type=int, default=20000,
                        help="number of points (default: 20000)")
    args = parser.parse_args()

    if args.n <= 0:
        parser.error("N must be a positive integer")

    rng = random.Random(SEED)
    points = generate_points(args.n, rng)
    centroids = init_centroids(points, rng, K)
    centroids, assignments = run_kmeans(points, centroids, ITERATIONS)

    sizes = [0] * K
    for c in assignments:
        sizes[c] += 1

    for i in range(K):
        cx, cy = centroids[i]
        print(f"cluster {i}: center=({cx:.3f}, {cy:.3f}) size={sizes[i]}")


if __name__ == "__main__":
    sys.exit(main())
