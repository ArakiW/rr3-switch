# -*- coding: utf-8 -*-
"""What is the guest waiting for, and what did the port's asset machinery do?"""
import collections, io, os, sys
base = sys.argv[1]
log = [os.path.join(base, n) for n in os.listdir(base) if n.endswith("guest-view.log")][0]
lines = [l.strip() for l in io.open(log, encoding="utf-8", errors="replace").read().split("\n") if l.strip()]
print("--- cond-wait lines ---")
for l in [x for x in lines if "cond-wait" in x][:14]:
    print("  " + l[:300])
print()
print("--- asset-invariant (first 6, last 4) ---")
ai = [x for x in lines if "asset-invariant" in x]
for l in ai[:6] + ai[-4:]:
    print("  " + l[:300])
print()
print("--- assetlist-entry / manifest-asset samples ---")
for l in [x for x in lines if x.startswith("phase=assetlist-entry")][:4]:
    print("  " + l[:240])
for l in [x for x in lines if x.startswith("phase=manifest-asset")][:4]:
    print("  " + l[:240])
print()
print("--- repair-candidate samples ---")
for l in [x for x in lines if x.startswith("phase=repair-candidate")][:5]:
    print("  " + l[:240])
print()
print("--- the guest's last 6 log lines with their ordinals ---")
for l in [x for x in lines if x.startswith("phase=guest-log-any")][-6:]:
    print("  " + l[:200])
