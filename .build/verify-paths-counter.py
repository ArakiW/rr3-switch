#!/usr/bin/env python3
"""The resolution counter before and after the placement fix."""
import io, json, os, re
RUNS = r"E:\AI\RealRacing3-switch\.build\eden-runs"
out = []
for tag in ("20261116-102657-eden-rr3-racecount2", "20261116-105607-eden-rr3-racecount3"):
    base = os.path.join(RUNS, tag)
    log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
    last = None
    hits = 0
    with io.open(log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.search(r"\bpaths=(\d+)", line)
            if m:
                hits += 1
                last = int(m.group(1))
    out.append({"tag": tag, "race_state_lines_with_paths": hits, "last_paths": last})
print(json.dumps(out, indent=2))
