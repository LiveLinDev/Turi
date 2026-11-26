#!/usr/bin/env python3
"""Payroll demo: build an organization of employees and managers,
apply raises, and report payroll per department plus the manager
with the largest team."""

import argparse
import random
import sys

DEPARTMENTS = ("Engineering", "Sales", "Marketing", "Finance", "Operations")

FIRST_NAMES = ("Alice", "Bob", "Carol", "Dave", "Erin", "Frank", "Grace",
               "Heidi", "Ivan", "Judy", "Mallory", "Niaj", "Olivia", "Peggy",
               "Rupert", "Sybil", "Trent", "Uma", "Victor", "Wendy")
LAST_NAMES = ("Anderson", "Baker", "Chen", "Diaz", "Evans", "Fischer",
              "Garcia", "Huang", "Iverson", "Johnson", "Kim", "Lopez",
              "Martinez", "Novak", "Okafor", "Patel", "Quinn", "Rossi",
              "Silva", "Tanaka")


class Employee:
    """A salaried employee in one department."""

    def __init__(self, name, dept, salary):
        self.name = name
        self.dept = dept
        self.salary = float(salary)

    def give_raise(self, pct):
        """Raise salary by pct (a fraction, e.g. 0.05 for 5%)."""
        self.salary = round(self.salary * (1.0 + pct), 2)

    def __str__(self):
        return f"{self.name} [{self.dept}] ${self.salary:,.2f}"


class Manager(Employee):
    """An employee who manages a list of reports."""

    def __init__(self, name, dept, salary):
        super().__init__(name, dept, salary)
        self.reports = []

    def add_report(self, employee):
        self.reports.append(employee)

    def give_raise(self, pct):
        # Managers get an extra 5% on top of the standard raise.
        super().give_raise(pct + 0.05)

    def __str__(self):
        return (f"{self.name} [{self.dept}] ${self.salary:,.2f} "
                f"({len(self.reports)} reports)")


def build_org(n, rng):
    """Create an organization with n employees (about 1 in 10 a manager)."""
    n_managers = max(1, n // 10)
    managers = []
    staff = []

    for i in range(n_managers):
        dept = DEPARTMENTS[i % len(DEPARTMENTS)]
        name = f"{rng.choice(FIRST_NAMES)} {rng.choice(LAST_NAMES)}"
        managers.append(Manager(name, dept, rng.uniform(90_000, 140_000)))

    for _ in range(n - n_managers):
        dept = rng.choice(DEPARTMENTS)
        name = f"{rng.choice(FIRST_NAMES)} {rng.choice(LAST_NAMES)}"
        emp = Employee(name, dept, rng.uniform(50_000, 90_000))
        staff.append(emp)
        # Assign to a manager in the same department if one exists.
        candidates = [m for m in managers if m.dept == dept]
        rng.choice(candidates).add_report(emp)

    return managers, staff


def payroll_by_dept(people):
    """Return {department: total salary} for a list of employees/managers."""
    totals = {}
    for person in people:
        totals[person.dept] = totals.get(person.dept, 0.0) + person.salary
    return totals


def main():
    parser = argparse.ArgumentParser(
        description="Simulate a company payroll.")
    parser.add_argument("n", nargs="?", type=int, default=30000,
                        help="number of employees (default: 30000)")
    args = parser.parse_args()
    n = args.n
    if n < 1:
        parser.error("N must be a positive integer")

    rng = random.Random(42)  # fixed seed: reproducible output
    managers, staff = build_org(n, rng)

    # Give everyone a raise: 5% standard, managers get +5% extra via override.
    for person in staff + managers:
        person.give_raise(0.05)

    # Payroll per department (managers count toward their department too).
    totals = payroll_by_dept(staff + managers)
    print("Payroll by department:")
    for dept in DEPARTMENTS:
        print(f"  {dept:<12} ${totals.get(dept, 0.0):>14,.2f}")
    print(f"  {'TOTAL':<12} ${sum(totals.values()):>14,.2f}")

    # Manager with the most reports.
    top = max(managers, key=lambda m: len(m.reports))
    print(f"\nManager with the most reports: {top}")


if __name__ == "__main__":
    sys.exit(main())
