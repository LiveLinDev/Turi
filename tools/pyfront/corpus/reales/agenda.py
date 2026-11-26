#!/usr/bin/env python3
"""Parse appointment lines from stdin, detect overlapping appointments
per day, and print a summary for each day.

Expected line format:
    YYYY-MM-DD HH:MM duration_minutes title
"""

import re
import sys
from datetime import datetime, timedelta

LINE_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2})\s+(\d+)\s+(.+)$"
)


class Appointment:
    __slots__ = ("date", "start", "end", "title")

    def __init__(self, date_str, start_str, duration_minutes, title):
        self.date = date_str
        self.start = datetime.strptime(f"{date_str} {start_str}",
                                       "%Y-%m-%d %H:%M")
        self.end = self.start + timedelta(minutes=int(duration_minutes))
        self.title = title

    def time_range(self):
        return f"{self.start:%H:%M}-{self.end:%H:%M}"


def parse_line(line, lineno):
    match = LINE_RE.match(line)
    if match is None:
        return None
    date_str, start_str, duration, title = match.groups()
    try:
        return Appointment(date_str, start_str, duration, title.strip())
    except ValueError:
        return None


def find_overlaps(appointments):
    """Return a list of (appt_a, appt_b, overlap_minutes) pairs."""
    overlaps = []
    ordered = sorted(appointments, key=lambda a: (a.start, a.end))
    for i, a in enumerate(ordered):
        for b in ordered[i + 1:]:
            if b.start >= a.end:
                break  # sorted by start: no later appointment can overlap a
            overlap_start = max(a.start, b.start)
            overlap_end = min(a.end, b.end)
            minutes = int((overlap_end - overlap_start).total_seconds() // 60)
            overlaps.append((a, b, minutes))
    return overlaps


def print_day_summary(date_str, appointments):
    ordered = sorted(appointments, key=lambda a: (a.start, a.end))
    print(date_str)
    for appt in ordered:
        print(f"  {appt.time_range()}  {appt.title}")

    overlaps = find_overlaps(ordered)
    if overlaps:
        print("  Conflicts:")
        for a, b, minutes in overlaps:
            print(f"    {a.title} <-> {b.title} ({minutes} min)")


def main():
    by_date = {}
    lineno = 0
    for raw_line in sys.stdin:
        lineno += 1
        line = raw_line.strip()
        if not line:
            continue
        appt = parse_line(line, lineno)
        if appt is None:
            print(f"warning: skipping malformed line {lineno}: {line!r}",
                  file=sys.stderr)
            continue
        by_date.setdefault(appt.date, []).append(appt)

    for date_str in sorted(by_date):
        print_day_summary(date_str, by_date[date_str])


if __name__ == "__main__":
    sys.exit(main())
