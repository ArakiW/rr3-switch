#!/usr/bin/env python3
"""Did the guest's audio path reach the port at all?"""
import collections, io, re, sys
path = sys.argv[1]
pats = ["fmod", "FMOD", "audiotrack", "AudioTrack", "opensl", "OpenSL", "aaudio", "AAudio",
        "audio", "Audio", "sound", "Sound", "music", "Music", "engine", "Engine"]
hist = collections.Counter()
samples = {}
total = 0
with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        total += 1
        for p in pats:
            if p in line:
                hist[p] += 1
                samples.setdefault(p, line.strip()[:220])
print("total lines:", total)
for k, v in hist.most_common():
    print("%-12s %6d   %s" % (k, v, samples[k]))
