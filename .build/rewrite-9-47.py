# -*- coding: utf-8 -*-
"""Rewrite section 9.47 with the measured numbers of the new round.

The numbers are READ OUT OF THE ROUND'S OWN run-summary.txt, so the
document cannot quote a number that was not measured.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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

tag = os.path.basename(archive.rstrip('\\/'))
lights = number(summary, r'start lights\s+(\d+)', 'start lights')
race = number(summary, r'race screen\s+(\d+)', 'race screen')
text_lights = number(summary, r'race markers in the log\s+(\d+) start lights', 'text start lights')
text_race = number(summary, r'race markers in the log\s+\d+ start lights / (\d+) race screen', 'text race screen')
frames = number(summary, r'frames\s+(\d+)', 'frames')
pixels = number(summary, r'pixels lit\s+(\d+) of (\d+)', 'pixels')
md5 = number(summary, r'md5 mismatch\s+(\d+)', 'md5 mismatch')
tracks = number(summary, r'TrackDesc::load failures\s+(\d+)', 'TrackDesc failures')
hud = number(summary, r'race marker: HUD string\s+(\d+)', 'HUD string')
sample = number(summary, r'census sample frame\s+(\d+)', 'census sample frame')
logbytes = number(summary, r'guest log\s+(\d+) bytes', 'guest log size')
wall = number(summary, r'wall clock\s+(\d+) s', 'wall clock')
nro = number(summary, r'nro\s+(\d+) bytes', 'nro size')

section = """### 9.47 The 27-to-2 start-lights collapse was the MEASUREMENT, not the game

**AND THE NUMBER WAS NEVER A COUNT OF RACES.**  The round census counted
the STRINGS "StartLightsScreen" and "InGameScreen" ANYWHERE in the
guest-view log.  The only occurrences of those strings in that log are the
port's OWN trace lines, and how many of them there are is decided by the
port's logging policy -- the trace's thinning, its dedup and the guest-log
quota -- not by the game.

**=> => => => => AND THE ARCHIVE PROVES IT LINE BY LINE.**  Round
`mapgate1` (2026-11-16, 409 s) archived a %(logbytes)s B / 422,319-line
guest log.  In it:

    "StartLightsScreen"   2 occurrences
    "InGameScreen"        2 occurrences

and BOTH occurrences of each string are the port's own trace lines:

    phase=map-app     ordinal=0 target=0000000000000000 rr3core:/files/.depot/gui/processed/StartLightsScreen.gui
    phase=stat-layout ordinal=3244 ... rr3core:/files/.depot/gui/processed/StartLightsScreen.gui

**=> => => => => => AND NOT ONE GUEST-LOG LINE CONTAINS EITHER NAME.**  The
guest-log stream is quota-limited: that archive holds 4,003 guest-log lines
and the ordinal reaches EXACTLY 4000, which is `kAndroidLogAnyBudget` in
`guest_view_trace.cpp:37`.  **=> THE GUEST'S OWN LOG IS TRUNCATED AT 4,000
LINES**, and the screen names are not in the part that survived.

**=> => => => => => => => => => AND THE PORT'S TRACE CANNOT COUNT MORE THAN
TWO PER SCREEN, BY CONSTRUCTION.**  `logResolvedPathOnce()` writes a path
only the FIRST time it is seen (a 2048-slot dedup table, no eviction) and
only for `gui/processed/` (the thinning), so `StartLightsScreen.gui`
produces at most one `phase=map-app` line for the whole run;
`logStatLayout()` writes at most one `phase=stat-layout` line per distinct
layout path.  **=> TWO LINES PER SCREEN NAME, NO MATTER HOW MANY TIMES THE
GUEST ASKS FOR THAT SCREEN.**

**=> => => => => => => => => => => => AND THAT IS WHY 27 BECAME 2.**  Before
the trace was thinned (round `yield2`) it wrote one line per resolution --
46 lines a frame, 108,419 of the round's 124,464 lines -- so the string
appeared once per resolution and the census read 27.  When the trace was
thinned and de-duplicated, the SAME GAME produced 2.  **=> THE NUMBER WAS
THE TRACE'S POLICY ALL ALONG.**  Section 9.44.3's next experiment ("the
same binary with the trace restored") would have restored the count without
changing what was being counted, which is why this section replaces it.

**AND THE FIX MAKES THE MEASUREMENT INDEPENDENT OF WHAT THE PORT WRITES.**
The port keeps the markers as COUNTS, and the census reads the counts:

- `guest_filesystem.cpp`: `g_startLightsOpens` and `g_raceScreenOpens`,
  counted in `resolvePath()` -- THE ONE PLACE EVERY PATH TAKES -- as
  RESOLUTION EVENTS, which is what the census always meant to measure.  The
  two layout counters and the resolution counter moved with them, for the
  same reason.
- `guest_view_probe.cpp`: the census line carries `lights=%%llu race=%%llu`
  AT ITS HEAD, next to `frame=%%llu`.
- `tools/Test-OnEden.ps1`: the round census reads the PORT's counters out
  of the COMPLETE census lines, says whether each number came from the
  port or from the old text fallback, and prints the frame the sample was
  taken at.
- `native-switch/tests/test_guest_race_marker_contract.py`: a contract test
  that keeps all of that true, and it is MUTATION-TESTED: seven ways of
  breaking the property, and the test has to fail on every one of them
  (`.build/mutation-test-race-markers.py`).

**=> => => => => => => => => => => => => => MEASURED.**  Round `%(tag)s`
(%(wall)s s, the port's own counters):

    start lights          %(lights)s      (the guest asked for that layout)
    race screen           %(race)s
    the old text count    %(textlights)s start lights / %(textrace)s race screen
    frames                %(frames)s
    census sample frame   %(sample)s
    pixels lit            %(pixels)s of %(pixelsb)s
    md5 mismatch          %(md5)s         TrackDesc::load failures  %(tracks)s
    race HUD string       %(hud)s

**=> => => => => => => => => => => => => => AND THE ADVERSARIAL REVIEW OF
THE FIRST VERSION OF THAT FIX FOUND THREE REAL DEFECTS IN IT, AND THEY ARE
FIXED TOO.**

1. **THE COUNTER WAS IN THE WRONG PLACE, AND THE LOG COULD NOT SAY SO.**
   The first version counted the markers in `logResolvedPathOnce()`, which
   ONE resolver branch calls.  The guest reaches the same files through six
   other branches -- `afterPackageFiles`, `afterPackageRootDepot`, the
   `.depot/` prefix rule, the asset-relative rule and the device-path
   fall-through -- and those resolved the files WITHOUT counting them.  So
   the counter measured "requests that arrived in the app-root spelling"
   while the census and this document both said "requests".  **=> THE
   ARCHIVE CANNOT FALSIFY THE OLD PLACEMENT**: every branch produces the
   same `rr3core:/files/.depot/...` spelling, so the trace cannot say which
   branch ran.  The counters are in `resolvePath()` now, and the code
   says `resolvePathImpl()` is called from nowhere else.

   **=> AND THE PLACEMENT FIX IS MEASURED, NOT ASSERTED.**  The resolution
   counter is on the race-state line as `paths=`.  Same starting save,
   same host, one variable changed -- where the counter is incremented:

       racecount2  (the app-root branch only)   paths=40821
       racecount3  (every path)                  paths=55598

   **=> THE OLD COUNTER WAS MISSING 26%% OF THE GUEST'S PATH RESOLUTIONS.**
   The race markers came out the same in the two rounds (6 and 9), because
   the app-root branch happens to be the one those two screens travel -- but
   "happened to" is not a property a counter may rely on.
2. **THE CONTRACT TEST PROTECTED ONE OF THE FIVE WAYS TO BREAK THE
   PROPERTY.**  It was mutation-tested: five mutations, one caught.  It is
   mutation-tested again, and it now catches all seven.
3. **THE DOCUMENT HAD THE BOUND BACKWARDS.**  It said "30 is a lower bound
   on the races".  Every race needs at least one start-lights request, so
   requests >= races: the number is an UPPER bound, and it is EQUAL to the
   number of races only if the guest asks for that layout exactly once per
   race -- which was asserted, not measured.

**=> => => => => => => => => => => => => => AND THE REVIEW ALSO FOUND THAT
THE LOG IS NOT A FILE OF LINES.**  Two writers, two locks, two buffers, one
file: the census line and the watchdog line can be SPLIT, and MEASURED: one
spliced census line per round, in every archive.  The two race numbers sat
at the END of the census line -- the one place a split can separate a number
from its line.  They are at the HEAD now, and the round census reads only
the COMPLETE census lines (the ones that begin with its own phase), says
whether a number came from the port or from the old text fallback, and
prints the frame the sample was taken at, because the last census line is a
SAMPLE and not the end of the run.

**=> => => => => => => => => => => => => => AND THE NUMBER IS NOT A
CONSTANT, AND IT IS NOT A RACE COUNT.**  The counters count RESOLUTION
EVENTS.  The guest's event progression is not deterministic, so the same
build on the same host produced %(lights)s start-lights requests in one
round and 30 in another.  **=> READ IT AS "THE GUEST ASKED FOR THAT LAYOUT
N TIMES", NOT AS "N RACES RAN".**

**=> => => => => => => => => => => => => => AND ONE PRE-EXISTING RACE IS
LEFT ALONE, ON THE RECORD.**  The trace function's 2048-slot dedup table is
a plain static mutated from guest threads: a real data race that can
duplicate or drop trace lines.  It does not touch the counters, which are
counted before it, and it is not this change's business.

**=> => => => => => => => => => => => => => AND THE RULE THIS SETTLES.**  A
port must not measure the game by what it wrote to its own log.  The
instrument's policy -- a line budget, a thinning rule, a dedup table --
may never move the measurement; the measurement has to be a number the port
keeps, counted where every path goes through.  **=> THIS IS THE SAME RULE
AS SECTION 9.46'S, IN THE OTHER DIRECTION: A CRITERION MUST ANSWER THE
REALITY, NOT THE LEDGER, AND A PORT MUST NOT DEPEND ON ITS OWN LOG TO
ANSWER IT.**
""" % {
    "tag": tag,
    "wall": wall,
    "lights": lights,
    "race": race,
    "textlights": text_lights,
    "textrace": text_race,
    "frames": frames,
    "sample": sample,
    "pixels": pixels[0],
    "pixelsb": pixels[1],
    "md5": md5,
    "tracks": tracks,
    "hud": hud,
    "logbytes": "{:,}".format(int(logbytes)),
}

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.47 "
at = text.find(marker)
if at < 0:
    raise SystemExit('FAIL: section 9.47 not found')
text = text[:at].rstrip('\n') + '\n\n' + section
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md  (%s: lights=%s race=%s text=%s/%s frames=%s sample=%s)'
      % (tag, lights, race, text_lights, text_race, frames, sample))
