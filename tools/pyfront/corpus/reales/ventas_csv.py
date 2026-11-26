#!/usr/bin/env python3
"""Aggregate CSV sales data (date,region,product,amount) into totals
per region and per product, printed in descending order of total."""

import csv
import sys
from collections import defaultdict


def parse_amount(value):
    try:
        return float(value)
    except ValueError:
        return None


def read_rows(stream):
    reader = csv.reader(stream)
    header = next(reader, None)
    if header is None:
        return
    for row in reader:
        if len(row) != 4:
            continue
        date, region, product, amount_raw = (field.strip() for field in row)
        amount = parse_amount(amount_raw)
        if amount is None:
            sys.stderr.write(
                "skipping line with invalid amount: %r\n" % (amount_raw,)
            )
            continue
        yield region, product, amount


def main():
    region_totals = defaultdict(float)
    product_totals = defaultdict(float)

    for region, product, amount in read_rows(sys.stdin):
        region_totals[region] += amount
        product_totals[product] += amount

    print("Totals by region:")
    for region, total in sorted(region_totals.items(), key=lambda kv: kv[1], reverse=True):
        print(f"{region}\t{total:.2f}")

    print()
    print("Totals by product:")
    for product, total in sorted(product_totals.items(), key=lambda kv: kv[1], reverse=True):
        print(f"{product}\t{total:.2f}")


if __name__ == "__main__":
    main()
