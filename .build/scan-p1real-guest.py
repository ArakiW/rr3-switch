#!/usr/bin/env python3
"""What was the guest doing when its FMOD engine came back empty?"""
import io, os, re, sys
base = sys.argv[1]
log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
lines = [l.strip() for l in io.open(log, encoding="utf-8", errors="replace").read().split("\n") if l.strip()]
print("--- the guest's own log lines (last 30) ---")
guest = [l for l in lines if l.startswith("phase=guest-log-any")]
for l in guest[-30:]:
    print("  " + l[:220])
print()
print("--- anything mentioning fmod / sound / audio / mixer ---")
hits = [l for l in lines if re.search(r"(?i)fmod|sound|audio|mixer|engine", l)]
print("count:", len(hits))
for l in hits[:18]:
    print("  " + l[:220])
print()
print("--- api-touch (what the guest asked for) last 12 ---")
for l in [x for x in lines if x.startswith("phase=api-touch")][-12:]:
    print("  " + l[:220])
print()
print("--- the first 12 lines of the round ---")
for l in lines[:12]:
    print("  " + l[:220])
