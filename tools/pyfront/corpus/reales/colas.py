#!/usr/bin/env python3
"""Simulate a bank with three tellers and a single waiting line.

Customers arrive at random (exponentially distributed) intervals and are
served by the first teller who becomes free.  The simulation reports the
average and maximum time a customer spends waiting in line.
"""

import argparse
from collections import deque
from random import Random

SEED = 11
MEAN_ARRIVAL_GAP = 1.0    # minutes between customer arrivals
MEAN_SERVICE_TIME = 0.45  # minutes to serve one customer


def simulate(n_customers, seed=SEED):
    """Run the queueing simulation and return (total_wait, max_wait)."""
    rng = Random(seed)

    line = deque()                 # arrival times of customers waiting in line
    tellers = [0.0, 0.0, 0.0]      # time at which each teller becomes free

    total_wait = 0.0
    max_wait = 0.0
    served = 0

    next_arrival = rng.expovariate(1.0 / MEAN_ARRIVAL_GAP)

    while served < n_customers:
        if not line:
            # No one waiting: the only event is the next arrival.
            now = next_arrival
            line.append(now)
            next_arrival = now + rng.expovariate(1.0 / MEAN_ARRIVAL_GAP)
            continue

        free_at = min(tellers)
        if next_arrival < free_at:
            # Next arrival happens before any teller frees up.
            now = next_arrival
            line.append(now)
            next_arrival = now + rng.expovariate(1.0 / MEAN_ARRIVAL_GAP)
        else:
            # A teller frees up and takes the next customer in line.
            now = free_at
            arrival = line.popleft()
            wait = now - arrival
            total_wait += wait
            if wait > max_wait:
                max_wait = wait
            tellers[tellers.index(free_at)] = (
                now + rng.expovariate(1.0 / MEAN_SERVICE_TIME)
            )
            served += 1

    return total_wait, max_wait


def main():
    parser = argparse.ArgumentParser(
        description="Simulate a bank with three tellers."
    )
    parser.add_argument(
        "n",
        nargs="?",
        type=int,
        default=100000,
        help="number of customers to simulate (default: %(default)s)",
    )
    args = parser.parse_args()

    total_wait, max_wait = simulate(args.n)
    avg_wait = total_wait / args.n

    print(f"Customers simulated : {args.n}")
    print(f"Average wait time   : {avg_wait:.3f} minutes")
    print(f"Maximum wait time   : {max_wait:.3f} minutes")


if __name__ == "__main__":
    main()
