#!/usr/bin/env python3
"""Where did the guest stop with the guest's own FMOD?"""
import collections, io, os, sys
base = sys.argv[1]
log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
lines = io.open(log, encoding="utf-8", errors="replace").read().split("\n")
hist = collections.Counter()
for l in lines:
    if "phase=" in l:
        i = l.index("phase=")
        j = l.find(" ", i)
        hist[l[i + 6:j]] += 1
print("total lines:", len(lines))
print("--- phases ---")
for k, v in hist.most_common(12):
    print("  %-28s %d" % (k, v))
print("--- last 25 lines ---")
for l in [x for x in lines if x.strip()][-25:]:
    print("  " + l.strip()[:230])
print("--- the reaper lines (first 3, last 3) ---")
reaper = [l for l in lines if "reaper" in l]
for l in reaper[:3] + reaper[-3:]:
    print("  " + l.strip()[:260])
print("--- crash files in the archive ---")
for n in sorted(os.listdir(base)):
    if "crash" in n.lower():
        p = os.path.join(base, n)
        print("  " + n, os.path.getsize(p))
        print(io.open(p, encoding="utf-8", errors="replace").read()[:1500])
