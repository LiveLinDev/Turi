#!/usr/bin/env python3
"""Generate a deterministic collection of shapes and report aggregate stats."""

import argparse
import math
import random
from abc import ABC, abstractmethod


class Shape(ABC):
    @abstractmethod
    def area(self) -> float:
        """Return the area of the shape."""

    @abstractmethod
    def perimeter(self) -> float:
        """Return the perimeter of the shape."""


class Circle(Shape):
    def __init__(self, radius: float) -> None:
        self.radius = radius

    def area(self) -> float:
        return math.pi * self.radius ** 2

    def perimeter(self) -> float:
        return 2.0 * math.pi * self.radius


class Rectangle(Shape):
    def __init__(self, width: float, height: float) -> None:
        self.width = width
        self.height = height

    def area(self) -> float:
        return self.width * self.height

    def perimeter(self) -> float:
        return 2.0 * (self.width + self.height)


class Triangle(Shape):
    """Isosceles triangle with two equal sides and a base."""

    def __init__(self, side: float, base: float) -> None:
        if base >= 2.0 * side:
            raise ValueError("base must be shorter than the sum of the equal sides")
        self.side = side
        self.base = base

    def area(self) -> float:
        half_base = self.base / 2.0
        height = math.sqrt(self.side ** 2 - half_base ** 2)
        return 0.5 * self.base * height

    def perimeter(self) -> float:
        return 2.0 * self.side + self.base


def make_shapes(n: int, rng: random.Random) -> list:
    """Build n shapes deterministically from the given seeded RNG."""
    shapes = []
    for i in range(n):
        kind = i % 3
        if kind == 0:
            shapes.append(Circle(rng.uniform(1.0, 10.0)))
        elif kind == 1:
            shapes.append(Rectangle(rng.uniform(1.0, 10.0), rng.uniform(1.0, 10.0)))
        else:
            side = rng.uniform(2.0, 10.0)
            base = rng.uniform(1.0, side)  # keeps base < 2*side, so valid
            shapes.append(Triangle(side, base))
    return shapes


def summarize(shapes: list) -> dict:
    counts = {"Circle": 0, "Rectangle": 0, "Triangle": 0}
    total_area = 0.0
    total_perimeter = 0.0
    for shape in shapes:
        counts[type(shape).__name__] += 1
        total_area += shape.area()
        total_perimeter += shape.perimeter()
    return {
        "counts": counts,
        "area": total_area,
        "perimeter": total_perimeter,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Aggregate area/perimeter statistics over generated shapes."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=100000,
        help="number of shapes to generate (default: 100000)",
    )
    args = parser.parse_args()

    rng = random.Random(42)  # fixed seed: deterministic output
    shapes = make_shapes(args.n, rng)
    stats = summarize(shapes)

    print(f"Shapes: {len(shapes)}")
    print(f"Total area: {stats['area']:.6f}")
    print(f"Total perimeter: {stats['perimeter']:.6f}")
    for kind, count in stats["counts"].items():
        print(f"{kind}: {count}")


if __name__ == "__main__":
    main()
