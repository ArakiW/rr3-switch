#!/usr/bin/env python3
"""What did the FMOD shim see in this round?"""
import io, re, sys
path = sys.argv[1]
keep = ("phase=fmod-shim", "fmod-shim", "EngineRamp", "AudioTrack", "FMOD")
out = []
with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if any(k in line for k in keep):
            out.append(line.strip()[:260])
print("matching lines:", len(out))
seen = set()
for l in out:
    key = l[:90]
    if key in seen:
        continue
    seen.add(key)
    print(l)
