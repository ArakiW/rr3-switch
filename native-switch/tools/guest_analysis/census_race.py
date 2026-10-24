#!/usr/bin/env python3
"""Read the probe's per-frame census and line it up with the race.

The census line names the frame, the number of draws the guest made, and the
programs it made them with. The race is located by the guest's OWN markers
(the strings the guest prints), so the race window is the guest's, not the
harness's.

Usage:
    python census_race.py <archive-dir> [--watch 136] [--window 40]
"""

import argparse
import os
import re
import sys

CENSUS = re.compile(
    r"^phase=hud-census ordinal=(\d+) target=([0-9a-fx]+) (.*)$")
MARKS = ("InGameScreen", "Reload HUD planes", "StartLightsScreen", "EventMapScreen")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--watch", type=int, default=136)
    ap.add_argument("--window", type=int, default=30)
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log -> VOID RUN")
        return 1

    census = []       # (line, frame, details)
    marks = []        # (line, frame-ish, text)
    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        for index, line in enumerate(handle):
            m = CENSUS.match(line)
            if m:
                census.append((index, int(m.group(1)), m.group(3).strip()))
                continue
            for mark in MARKS:
                if mark in line:
                    marks.append((index, mark))
                    break

    print("census lines: %d   guest markers: %d" % (len(census), len(marks)))
    print("\n== the guest's own markers (log line -> marker) ==")
    for index, mark in marks:
        print("  line %-8d %s" % (index, mark))

    # The race window: the first and last InGameScreen marker.
    race = [i for i, m in marks if m == "InGameScreen"]
    if not race:
        print("\nNO InGameScreen marker: the guest never reached the race")
        return 2
    first_line, last_line = race[0], race[-1]
    print("\nrace log lines: %d .. %d" % (first_line, last_line))

    # The census entries inside the race.
    in_race = [c for c in census if first_line <= c[0] <= last_line]
    print("census frames inside the race: %d" % len(in_race))
    if not in_race:
        print("NO CENSUS LINE IN THE RACE")
        return 3

    def watch_draws(details):
        m = re.search(r" watch%d=(\d+)" % args.watch, details)
        if m:
            return int(m.group(1))
        m = re.search(r" p%d=(\d+)" % args.watch, details)
        return int(m.group(1)) if m else 0

    print("\n== the watched program (id %d) in the race ==" % args.watch)
    counts = [watch_draws(c[2]) for c in in_race]
    nonzero = [c for c in counts if c > 0]
    print("frames in the race with the watched program: %d of %d"
          % (len(nonzero), len(in_race)))
    if counts:
        print("draws per frame: min=%d max=%d" % (min(counts), max(counts)))
    print("\nfirst %d census frames in the race:" % args.window)
    for index, frame, details in in_race[:args.window]:
        print("  line %-8d frame=%-6d %s" % (index, frame, details[:200]))
    print("\nlast %d census frames in the race:" % args.window)
    for index, frame, details in in_race[-args.window:]:
        print("  line %-8d frame=%-6d %s" % (index, frame, details[:200]))

    # The whole session, for contrast.
    before = [c for c in census if c[0] < first_line]
    after = [c for c in census if c[0] > last_line]
    def summary(rows, label):
        if not rows:
            print("%-14s no census lines" % label)
            return
        vals = [watch_draws(c[2]) for c in rows]
        print("%-14s frames=%-6d watched-draws min=%d max=%d  nonzero=%d"
              % (label, len(rows), min(vals), max(vals),
                 sum(1 for v in vals if v > 0)))
    print("\n== the watched program over the session ==")
    summary(before, "before race")
    summary(in_race, "IN RACE")
    summary(after, "after race")
    return 0


if __name__ == "__main__":
    sys.exit(main())
