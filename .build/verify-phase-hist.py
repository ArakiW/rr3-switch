#!/usr/bin/env python3
"""Histogram of the phase= names in an archived round's guest log."""
import collections, io, json, os, sys
ROOT = r"E:\AI\RealRacing3-switch"
RUNS = os.path.join(ROOT, ".build", "eden-runs")
tag = sys.argv[1]
base = os.path.join(RUNS, tag)
log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
hist = collections.Counter()
guest_ordinals = []
with io.open(log, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if "phase=" not in line:
            continue
        i = line.index("phase=")
        j = line.index(" ", i)
        hist[line[i + 6:j]] += 1
        if line.startswith("phase=guest-log ") or line.startswith("phase=guest-log-any "):
            k = line.find("ordinal=")
            if k >= 0:
                m = line.find(" ", k)
                try:
                    guest_ordinals.append(int(line[k + 8:m]))
                except ValueError:
                    pass
print(json.dumps({
    "tag": tag,
    "phases": hist.most_common(12),
    "guest_log_ordinal_max": max(guest_ordinals) if guest_ordinals else None,
    "guest_log_lines": len(guest_ordinals),
}, indent=2, ensure_ascii=False))
