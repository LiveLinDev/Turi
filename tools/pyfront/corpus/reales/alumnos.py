#!/usr/bin/env python3
"""Generate deterministic student records and report summary statistics."""

import argparse
import statistics
from dataclasses import dataclass, field
from typing import List


GRADES = (9, 10, 11, 12)
NUM_SCORES = 6
SEED = 20240517


@dataclass
class Student:
    name: str
    grade: int
    scores: List[float] = field(default_factory=list)

    @property
    def average(self) -> float:
        return statistics.mean(self.scores)


def make_students(n: int) -> List[Student]:
    import random

    rng = random.Random(SEED)
    first_names = ["Alice", "Bob", "Carol", "Dave", "Erin", "Frank", "Grace",
                   "Heidi", "Ivan", "Judy", "Mallory", "Niaj", "Olivia", "Peggy",
                   "Rupert", "Sybil", "Trent", "Uma", "Victor", "Wendy"]
    last_names = ["Adler", "Baker", "Chen", "Diaz", "Evans", "Fischer", "Garcia",
                  "Huang", "Iverson", "Jensen", "Kaur", "Lopez", "Moreau",
                  "Novak", "Okafor", "Patel", "Quinn", "Rossi", "Silva", "Tanaka"]

    students = []
    for i in range(n):
        name = f"{rng.choice(first_names)} {rng.choice(last_names)}"
        grade = rng.choice(GRADES)
        scores = [rng.randint(50, 100) for _ in range(NUM_SCORES)]
        students.append(Student(name, grade, scores))
    return students


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Report per-grade averages, top students, and median score.")
    parser.add_argument("n", nargs="?", type=int, default=30000,
                        help="number of students to generate (default: 30000)")
    args = parser.parse_args()

    students = make_students(args.n)

    # Average score per grade.
    by_grade = {g: [] for g in GRADES}
    for s in students:
        by_grade[s.grade].append(s.average)
    print("Average score per grade:")
    for g in sorted(by_grade):
        avg = statistics.mean(by_grade[g])
        print(f"  Grade {g}: {avg:.2f}  ({len(by_grade[g])} students)")

    # Top 5 students by average score; ties broken by name.
    top = sorted(students, key=lambda s: (-s.average, s.name))[:5]
    print("\nTop 5 students by average score:")
    for rank, s in enumerate(top, start=1):
        print(f"  {rank}. {s.name:<20} grade {s.grade}: {s.average:.2f}")

    # Median score across all recorded scores.
    all_scores = [score for s in students for score in s.scores]
    print(f"\nMedian score across all {len(all_scores)} scores: "
          f"{statistics.median(all_scores):.2f}")


if __name__ == "__main__":
    main()
