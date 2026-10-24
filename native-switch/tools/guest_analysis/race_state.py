#!/usr/bin/env python3
"""Read the race-state probe's own log out of an archived guest-view.log.

The probe (native-switch/source/race_state_probe.cpp) writes one
"phase=race-state" line every 30 frames and one "phase=race-lap" line
whenever the guest's own total-laps member changes. This tool turns those
lines into the answer to the question the probe was built for:

    did the guest's own lap counter move, and if not, how far did the car get?

Usage:
    python race_state.py <guest-view.log> [--every N]
"""

import argparse
import re
import sys
from collections import OrderedDict

STATE_RE = re.compile(
    r'phase=race-state ordinal=100 target=\S+ f=(\d+) .*?gm=0x([0-9a-f]+) '
    r'gm_ok=(\d) .*?metrics=(\d+) active=(-?\d+)')
LAP_RE = re.compile(
    r'phase=race-lap ordinal=101 target=\S+ f=(\d+) total_laps=(\d+)')
FIELD_RE = re.compile(r'\b(laps|total|prog|rate|steer|mag|sign|best|flips|stalls|'
                      r'sawprog|drive|vptr_ok|cls|gm_ok|metrics|active)='
                      r'(-?[0-9.]+|\S+)')


def parse_fields(line):
    out = {}
    for m in FIELD_RE.finditer(line):
        out[m.group(1)] = m.group(2)
    return out


def as_float(text, default=0.0):
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--every", type=int, default=1,
                    help="print every Nth sample")
    args = ap.parse_args()

    samples = []
    laps = []
    with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            if "phase=race-state" in line:
                m = STATE_RE.search(line)
                if m is None:
                    continue
                fields = parse_fields(line)
                fields["line"] = lineno
                fields["frame"] = int(m.group(1))
                fields["gm"] = m.group(2)
                fields["gm_ok"] = int(m.group(3))
                fields["metrics"] = int(m.group(4))
                fields["active"] = int(m.group(5))
                samples.append(fields)
            elif "phase=race-lap" in line:
                m = LAP_RE.search(line)
                if m is not None:
                    laps.append((int(m.group(1)), int(m.group(2))))

    print(f"# race-state lines: {len(samples)}   lap-change lines: {len(laps)}")
    if not samples:
        print("# the probe never wrote a line")
        return 1

    first = samples[0]
    last = samples[-1]
    print(f"# first: frame={first['frame']} gm_ok={first['gm_ok']} "
          f"metrics={first['metrics']} cls={first.get('cls','-')} "
          f"vptr_ok={first.get('vptr_ok','-')}")
    print(f"# last : frame={last['frame']} gm_ok={last['gm_ok']} "
          f"metrics={last['metrics']} active={last['active']} "
          f"laps={last.get('laps','-')} total={last.get('total','-')} "
          f"prog={last.get('prog','-')} drive={last.get('drive','-')} "
          f"steer={last.get('steer','-')} flips={last.get('flips','-')} "
          f"stalls={last.get('stalls','-')}")

    # The identity of the metrics object, once.
    for s in samples:
        if s.get("vptr_ok") == "1":
            print(f"# metrics object: 0x{s.get('m','?')}  "
                  f"class={s.get('cls','?')}  vptr_ok=1")
            break

    if laps:
        print("# the guest's own total-laps member changed:")
        for frame, value in laps:
            print(f"#   frame={frame:<8} total_laps={value}")
    else:
        print("# the guest's own total-laps member NEVER changed")

    # The progress and the steering, sampled.
    print(f"# {'frame':<9} {'laps':<6} {'total':<12} {'prog':<12} "
          f"{'rate':<12} {'steer':<7} {'mag':<6} {'sign':<5} {'drive':<6}")
    for index, s in enumerate(samples):
        if index % args.every != 0:
            continue
        print(f"  {s['frame']:<9} {s.get('laps','-'):<6} "
              f"{s.get('total','-'):<12} {s.get('prog','-'):<12} "
              f"{s.get('rate','-'):<12} {s.get('steer','-'):<7} "
              f"{s.get('mag','-'):<6} {s.get('sign','-'):<5} "
              f"{s.get('drive','-'):<6}")

    # A summary of the movement.
    totals = []
    for s in samples:
        if s.get("active", "-1") == "-1":
            continue
        try:
            totals.append((s["frame"], as_float(s.get("total"))))
        except (KeyError, TypeError):
            pass
    if len(totals) >= 2:
        lo = min(totals, key=lambda t: t[1])
        hi = max(totals, key=lambda t: t[1])
        moved = sum(1 for a, b in zip(totals, totals[1:]) if b != a)
        print(f"# total-spline: min={lo[1]:.1f} (f={lo[0]}) "
              f"max={hi[1]:.1f} (f={hi[0]}) span={hi[1]-lo[1]:.1f} "
              f"changed_on={moved}/{len(totals)-1} samples")
    return 0


if __name__ == "__main__":
    sys.exit(main())
