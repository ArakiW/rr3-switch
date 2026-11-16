#!/usr/bin/env python3
"""What does the game's own audio asset list say, and do we have the files?"""
import io, os, re, sys
listpath = sys.argv[1]
audiotree = os.path.join(os.path.dirname(listpath), "audio")
text = io.open(listpath, encoding="utf-8", errors="replace").read()
lines = [l.strip() for l in text.split("\n") if l.strip()]
print("asset list lines:", len(lines))
print("--- first 5 lines ---")
for l in lines[:5]:
    print("   ", l[:160])
dep = [l for l in lines if ".decomp" in l]
print("--- .decomp entries:", len(dep), "---")
for l in dep[:6]:
    print("   ", l[:200])
veh = [l for l in lines if "Vehicles" in l]
print("--- Vehicles entries:", len(veh), "---")
for l in veh[:6]:
    print("   ", l[:200])
# what the list asks for vs what we have
have = set()
for root, dirs, files in os.walk(audiotree):
    for f in files:
        p = os.path.join(root, f)
        rel = os.path.relpath(p, os.path.dirname(audiotree)).replace("\\", "/")
        have.add(rel.lower())
print("--- files we have under .depot/audio:", len(have), "---")
missing = 0
checked = 0
samples = []
for l in lines:
    if not l.lower().endswith(".decomp"):
        continue
    path = l.split(",")[-1].strip().strip('"').replace("\\", "/")
    if not path:
        continue
    checked += 1
    rel = path.lower()
    rel = rel[rel.find("audio/"):] if "audio/" in rel else rel
    if rel not in have:
        missing += 1
        if len(samples) < 6:
            samples.append(path)
print("decomp entries checked:", checked, "missing on disk:", missing)
for s in samples:
    print("   MISSING:", s)
