#!/usr/bin/env python3
"""How long are the census lines?  Is cdiag[512] big enough?"""
import io, json, os, sys
ROOT = r"E:\AI\RealRacing3-switch"
RUNS = os.path.join(ROOT, ".build", "eden-runs")
tag = sys.argv[1]
base = os.path.join(RUNS, tag)
log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
best = 0
n = 0
with io.open(log, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if " gui=" in line and " twosq=" in line:
            n += 1
            if len(line.rstrip("\n")) > best:
                best = len(line.rstrip("\n"))
                tail = line.rstrip("\n")[-120:]
print(json.dumps({"tag": tag, "census_lines": n, "max_line_len": best, "tail_of_longest": tail}, indent=2))
