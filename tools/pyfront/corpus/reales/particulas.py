#!/usr/bin/env python3
"""Simulate particles under gravity bouncing inside a box.

Usage: python3 particles.py [N]

N is the number of simulation steps (default 2000).
"""

import sys


class Vector:
    """A simple 2D vector."""

    def __init__(self, x=0.0, y=0.0):
        self.x = float(x)
        self.y = float(y)

    def __add__(self, other):
        return Vector(self.x + other.x, self.y + other.y)

    def __sub__(self, other):
        return Vector(self.x - other.x, self.y - other.y)

    def __mul__(self, scalar):
        return Vector(self.x * scalar, self.y * scalar)

    __rmul__ = __mul__

    def length(self):
        return (self.x * self.x + self.y * self.y) ** 0.5

    def __repr__(self):
        return "Vector({:.3f}, {:.3f})".format(self.x, self.y)


GRAVITY = Vector(0.0, -9.81)   # acceleration per unit time
BOX_MIN = Vector(0.0, 0.0)     # lower corner of the box
BOX_MAX = Vector(10.0, 10.0)   # upper corner of the box
RESTITUTION = 0.8              # fraction of speed kept after a bounce
DT = 0.01                      # time step
NUM_PARTICLES = 50


def simulate(steps):
    """Run the simulation for `steps` steps and return the particles."""
    rng = random.Random(42)
    particles = []
    for _ in range(NUM_PARTICLES):
        pos = Vector(rng.uniform(1.0, 9.0), rng.uniform(1.0, 9.0))
        vel = Vector(rng.uniform(-2.0, 2.0), rng.uniform(-2.0, 2.0))
        particles.append([pos, vel])

    for _ in range(steps):
        for p in particles:
            pos, vel = p
            vel = vel + GRAVITY * DT
            pos = pos + vel * DT

            # Bounce off each wall.
            if pos.x < BOX_MIN.x:
                pos.x = BOX_MIN.x
                vel.x = -vel.x * RESTITUTION
            elif pos.x > BOX_MAX.x:
                pos.x = BOX_MAX.x
                vel.x = -vel.x * RESTITUTION
            if pos.y < BOX_MIN.y:
                pos.y = BOX_MIN.y
                vel.y = -vel.y * RESTITUTION
            elif pos.y > BOX_MAX.y:
                pos.y = BOX_MAX.y
                vel.y = -vel.y * RESTITUTION

    return particles


def total_kinetic_energy(particles):
    """Return the total kinetic energy of all particles."""
    total = 0.0
    for pos, vel in particles:
        v = vel.length()
        total += 0.5 * v * v
    return total


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    particles = simulate(n)

    print("Steps: {}".format(n))
    print("Particles: {}".format(NUM_PARTICLES))
    print("Final positions of the first three particles:")
    for pos, vel in particles[:3]:
        print("  {}".format(pos))
    print("Total kinetic energy: {:.6f}".format(
        total_kinetic_energy(particles)))


if __name__ == "__main__":
    main()
