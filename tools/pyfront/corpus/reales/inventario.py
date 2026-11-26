#!/usr/bin/env python3
"""Shop inventory simulation.

Generates a deterministic inventory, applies sales and restocks,
then reports the total stock value, the most valuable items and
out-of-stock items.
"""

import argparse
import random
import sys

ADJECTIVES = [
    "red", "blue", "green", "black", "white", "silver", "golden",
    "wooden", "plastic", "ceramic", "leather", "steel", "copper",
]
NOUNS = [
    "chair", "table", "lamp", "mug", "plate", "spoon", "knife",
    "book", "pen", "notebook", "bag", "wallet", "clock", "mirror",
    "candle", "vase", "frame", "shelf", "box", "basket",
]

SEED = 12345


class Item:
    """A single product in the shop inventory."""

    def __init__(self, name: str, price: float, quantity: int) -> None:
        self.name = name
        self.price = price
        self.quantity = quantity

    @property
    def value(self) -> float:
        """Total value of the stock held for this item."""
        return self.price * self.quantity

    def sell(self, amount: int) -> int:
        """Sell up to `amount` units; return the number actually sold."""
        sold = min(amount, self.quantity)
        self.quantity -= sold
        return sold

    def restock(self, amount: int) -> None:
        self.quantity += amount

    def __repr__(self) -> str:
        return f"Item({self.name!r}, price={self.price:.2f}, qty={self.quantity})"


class Inventory:
    """Holds items keyed by name."""

    def __init__(self) -> None:
        self.items: dict[str, Item] = {}

    def add(self, item: Item) -> None:
        self.items[item.name] = item

    def get(self, name: str) -> Item | None:
        return self.items.get(name)

    def total_value(self) -> float:
        return sum(item.value for item in self.items.values())

    def top_by_value(self, count: int) -> list[Item]:
        return sorted(self.items.values(), key=lambda it: it.value,
                      reverse=True)[:count]

    def out_of_stock(self) -> list[Item]:
        return [item for item in self.items.values() if item.quantity == 0]


def generate_inventory(rng: random.Random, count: int) -> Inventory:
    """Create `count` distinct items deterministically from `rng`."""
    inventory = Inventory()
    for i in range(count):
        name = f"{rng.choice(ADJECTIVES)} {rng.choice(NOUNS)} #{i}"
        price = round(rng.uniform(0.5, 500.0), 2)
        quantity = rng.randint(0, 500)
        inventory.add(Item(name, price, quantity))
    return inventory


def apply_operations(rng: random.Random, inventory: Inventory,
                     operations: int) -> None:
    """Apply `operations` sales/restocks chosen at random."""
    names = list(inventory.items)
    for _ in range(operations):
        name = rng.choice(names)
        item = inventory.get(name)
        if rng.random() < 0.5:
            item.sell(rng.randint(1, 50))
        else:
            item.restock(rng.randint(1, 100))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Simulate a small shop inventory.")
    parser.add_argument("n", nargs="?", type=int, default=20000,
                        help="number of items to generate (default: 20000)")
    args = parser.parse_args()

    n = args.n
    if n <= 0:
        parser.error("N must be a positive integer")

    rng = random.Random(SEED)
    inventory = generate_inventory(rng, n)
    apply_operations(rng, inventory, n // 2)

    total = inventory.total_value()
    top5 = inventory.top_by_value(5)
    zero = inventory.out_of_stock()

    print(f"Inventory size: {len(inventory.items)} items")
    print(f"Total stock value: {total:.2f}")
    print("\nFive most valuable items:")
    for item in top5:
        print(f"  {item.name:<30} {item.value:>10.2f}")
    print(f"\nItems with zero stock: {len(zero)}")
    for item in zero[:10]:
        print(f"  {item.name}")
    if len(zero) > 10:
        print(f"  ... and {len(zero) - 10} more")


if __name__ == "__main__":
    sys.exit(main())
