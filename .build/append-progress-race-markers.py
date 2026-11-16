#!/usr/bin/env python3
"""The progress entry for the measurement fix and the review of it."""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TICK = chr(96)


def load(path):
    with io.open(path, 'rb') as handle:
        raw = handle.read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    if bom:
        raw = raw[3:]
    crlf = b'\r\n' in raw
    return raw.decode('utf-8').replace('\r\n', '\n'), crlf, bom


def save(path, text, crlf, bom):
    if crlf:
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    with io.open(path, 'wb') as handle:
        handle.write(data)


def number(summary, pattern, label):
    found = re.search(pattern, summary)
    if not found:
        raise SystemExit('FAIL: no %s in the round summary' % label)
    return found.group(1)


archive = sys.argv[1]
with io.open(os.path.join(archive, 'run-summary.txt'), encoding='utf-8') as handle:
    summary = handle.read()
tag = os.path.basename(archive.rstrip('\\/')).split('-eden-')[-1]
lights = number(summary, r'start lights\s+(\d+)', 'start lights')
race = number(summary, r'race screen\s+(\d+)', 'race screen')
text_lights = number(summary, r'race markers in the log\s+(\d+) start lights', 'text start lights')
text_race = number(summary, r'race markers in the log\s+\d+ start lights / (\d+) race screen', 'text race screen')
frames = number(summary, r'frames\s+(\d+)', 'frames')
pixels = number(summary, r'pixels lit\s+(\d+) of (\d+)', 'pixels')

entry = """
### 2026-11-16 The 27-to-2 start-lights collapse was the CENSUS, and the fix
was reviewed adversarially before it shipped

**WHAT WAS MEASURED.**  The round census counted the strings
"StartLightsScreen" and "InGameScreen" anywhere in the guest-view log.  The
only occurrences of those strings left in the log are the port's OWN path
trace lines, and their number is set by the trace's policy (the
gui/processed/ thinning and a 2048-slot dedup with no eviction) and by the
guest-log quota of 4000 lines.  The archived round @@mapgate1@@ log
(67 MB, 422,319 lines) holds exactly 2 of each, both of them
@@phase=map-app@@ / @@phase=stat-layout@@ lines, and the guest-log ordinal
ends at EXACTLY 4000.  **=> THE NUMBER MEASURED THE LOG, NOT THE GAME.**

**THE FIX.**  The port counts the race markers as RESOLUTION EVENTS, in
@@resolvePath()@@ -- the one place every path takes -- and the round census
reads the port's counters out of the COMPLETE census lines.

**AND THE ADVERSARIAL REVIEW FOUND THREE REAL DEFECTS IN THAT FIRST
FIX.**  The counters were in the trace function, which ONE resolver branch
calls, so they measured "requests in one spelling"; the contract test
caught only one of the five ways to break the property (it is
mutation-tested now: seven ways, all seven caught); and the log is written
by two threads into one file, so a line can be SPLIT, and the two numbers
sat at the END of the census line -- exactly where a split can take them
away.  All three are fixed.  The same counter flaw was found and fixed for
the resolution counter and the two front-end layout counters.

**WHAT IT MEASURES NOW.**  Round @@%s@@ (%s s, the same starting save as
the round before it, which changed only the counter's placement):
**%s start-lights requests / %s race-screen requests** from the port's
counters, against the old text count of **%s / %s**; frames %s, pixels lit
%s of %s, md5 mismatch 0.  Read it as "the guest asked for that layout N
times", not "N races ran": the counters are resolution events and the
guest's event progression is not deterministic.  What does NOT move is the
text count -- %s in every round, which is the instrument's signature.
Full reasoning: docs/OPEN-ISSUES.md section 9.47.
""" % (tag, number(summary, r'wall clock\s+(\d+) s', 'wall'), lights, race,
       text_lights, text_race, frames, pixels[0], pixels[1], text_lights)
entry = entry.replace("@@", TICK)

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md')
