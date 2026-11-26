#!/usr/bin/env python3
"""Simulate bank account transfers and report failures, totals, and the richest account."""

import random
import sys


class InsufficientFunds(Exception):
    """Raised when a transfer would drive an account balance below zero."""


class Account:
    def __init__(self, number: int, balance: float = 0.0) -> None:
        self.number = number
        self.balance = balance

    def deposit(self, amount: float) -> None:
        self.balance += amount

    def withdraw(self, amount: float) -> None:
        if amount > self.balance:
            raise InsufficientFunds(
                f"account {self.number}: balance {self.balance:.2f} "
                f"is less than withdrawal {amount:.2f}"
            )
        self.balance -= amount


def run_transfers(accounts, num_transfers, rng):
    """Perform deterministic transfers; return the number that failed."""
    failed = 0
    n_accounts = len(accounts)
    for _ in range(num_transfers):
        sender = accounts[rng.randrange(n_accounts)]
        receiver = accounts[rng.randrange(n_accounts)]
        amount = round(rng.uniform(1.0, 500.0), 2)
        try:
            sender.withdraw(amount)
            receiver.deposit(amount)
        except InsufficientFunds:
            failed += 1
    return failed


def main() -> None:
    if len(sys.argv) > 1:
        try:
            num_transfers = int(sys.argv[1])
        except ValueError:
            sys.exit(f"error: N must be an integer, got {sys.argv[1]!r}")
        if num_transfers < 0:
            sys.exit("error: N must be non-negative")
    else:
        num_transfers = 50000

    rng = random.Random(7)
    accounts = [Account(i, balance=1000.0) for i in range(100)]

    failed = run_transfers(accounts, num_transfers, rng)

    total = sum(a.balance for a in accounts)
    richest = max(accounts, key=lambda a: a.balance)

    print(f"transfers attempted: {num_transfers}")
    print(f"transfers failed:    {failed}")
    print(f"total balance:       {total:.2f}")
    print(
        f"richest account:     #{richest.number} "
        f"with {richest.balance:.2f}"
    )


if __name__ == "__main__":
    main()
