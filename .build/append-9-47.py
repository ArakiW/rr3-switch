# -*- coding: utf-8 -*-
"""Section 9.47: the 27-to-2 start-lights collapse was the MEASUREMENT.

Also: the two stale readings that section 9.47 corrects (section 9.44.3's
"next experiment" and section 9.46's closing sentence), and the version
bump to 0.22.14.
"""
import io
import os

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


def sub(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit('FAIL %s: expected 1 occurrence, found %d' % (label, count))
    return text.replace(old, new)


# ------------------------------------------------------- 1. OPEN-ISSUES.md
path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)

# 1a. section 9.44.3's proposed next experiment was the wrong question.
old = """THE NEXT EXPERIMENT IS
ONE VARIABLE: the same binary with the trace restored.**"""
new = """THE NEXT EXPERIMENT WAS
ONE VARIABLE: the same binary with the trace restored.**  **=> AND THAT
EXPERIMENT WAS THE WRONG QUESTION, AND SECTION 9.47 SHOWS WHY: THE NUMBER
WAS COUNTING THE PORT'S OWN LOG LINES, SO RESTORING THE TRACE WOULD HAVE
RESTORED THE COUNT WITHOUT CHANGING WHAT WAS COUNTED.**"""
text = sub(text, old, new, '9.44.3 next-experiment sentence')

# 1b. section 9.46's closing sentence called the collapse a regression.
old = """fix.  The 27-to-2 start-lights regression (section 9.44.3) and the 211
compiler warnings are still open and untouched by this."""
new = """fix.  The 27-to-2 start-lights collapse was NOT a game regression -- section
9.47 shows the census was counting the port's own log lines -- and the 211
compiler warnings are still open and untouched by this."""
text = sub(text, old, new, '9.46 closing sentence')

# 1c. the new section.
section = """
### 9.47 The 27-to-2 start-lights collapse was the MEASUREMENT, not the game

**AND THE NUMBER WAS NEVER A COUNT OF RACES.**  The round census counted
the STRINGS "StartLightsScreen" and "InGameScreen" ANYWHERE in the
guest-view log.  The only occurrences of those strings in that log are the
port's OWN trace lines, and how many of them there are is decided by the
port's logging policy -- the trace's thinning, its dedup and the guest-log
quota -- not by the game.

**=> => => => => AND THE ARCHIVE PROVES IT LINE BY LINE.**  Round
`mapgate1` (2026-11-16, 409 s) archived a 67,270,351 B / 422,319-line
guest log.  In it:

    "StartLightsScreen"   2 occurrences
    "InGameScreen"        2 occurrences

and BOTH occurrences of each string are the port's own trace lines:

    phase=map-app     ordinal=0 target=0000000000000000 rr3core:/files/.depot/gui/processed/StartLightsScreen.gui
    phase=stat-layout ordinal=3244 target=0000000000000000 true=4387 distinct=3244 repeat=0 size_at=[18=4295000356,28=0,30=4387,38=512,40=1] o30_is_size=1 rr3core:/files/.depot/gui/processed/StartLightsScreen.gui

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
changing what was being counted, which is why section 9.47 replaces it.

**AND THE FIX MAKES THE MEASUREMENT INDEPENDENT OF WHAT THE PORT WRITES.**
The port now keeps the two race markers as COUNTS:

- `native-switch/source/guest_filesystem.cpp`: `g_startLightsOpens` and
  `g_raceScreenOpens`, incremented in `logResolvedPathOnce()` BEFORE the
  thinning and BEFORE the dedup -- i.e. counted as RESOLUTION EVENTS, which
  is what the census always meant to measure.
- `native-switch/source/guest_view_probe.cpp`: the census line now carries
  `lights=%llu race=%llu` next to `gui=%llu twosq=%llu`.
- `tools/Test-OnEden.ps1`: the round census reads the PORT's counters.  The
  old text counts survive as `msStartLightsText` / `msRaceScreenText`, as
  a cross-check, and a port whose counters and whose log disagree is itself
  worth knowing about.
- `native-switch/tests/test_guest_race_marker_contract.py`: a contract test
  that parses the body of `logResolvedPathOnce()` and asserts the counters
  are incremented BEFORE the thinning line, so the bug cannot come back.

**=> => => => => => => => => => => => => => MEASURED, SAME HOST, SAME 400 s.**
Round `racecount1` (2026-11-16, 409 s, the new binary):

    port counters     30 start-lights resolutions / 45 race-screen resolutions
    old text count     2 start lights / 2 race screen
    frames            2400        pixels lit  921600 of 921600
    md5 mismatch        0         TrackDesc::load failures  0
    race HUD string   110         ("Reload HUD planes" in the guest log)

**=> => => => => => => => => => => => => => AND THE GAME WAS RACING THE
WHOLE TIME.**  The guest asked for the race screens 30 and 45 times in 400
seconds, which is one every 13 and 9 seconds -- the game was in and out of
races continuously and never once sat still.  **=> THE 27 OF `yield2` AND THE
30 HERE ARE THE SAME GAME BEHAVIOUR**, measured through two different
instruments, and the "regression" was the instrument.

**=> => => => => => => => => => => => => => AND ONE HONEST LIMIT OF THE NEW
NUMBER.**  The counters count RESOLUTION EVENTS -- how many times the guest
asked for that layout -- not "races completed".  The race-screen count is
therefore HIGHER than the start-lights count (45 vs 30): `InGameScreen.gui`
is re-resolved during a race, while the start-lights layout is asked for
once per race start.  **=> 30 IS A LOWER BOUND ON THE RACES IN 400 s, AND
45 IS A COUNT OF LAYOUT REQUESTS, NOT OF RACES.**  Both are read off the
guest's own behaviour, and neither depends on the log.

**=> => => => => => => => => => => => => => AND THE RULE THIS SETTLES.**  A
port must not measure the game by what it wrote to its own log.  The
instrument's policy -- a line budget, a thinning rule, a dedup table -- may
never move the measurement; the measurement has to be a number the port
keeps, so that changing what is written cannot change what is measured.
**=> THIS IS THE SAME RULE AS SECTION 9.46'S, IN THE OTHER DIRECTION: A
CRITERION MUST ANSWER THE REALITY, NOT THE LEDGER, AND A PORT MUST NOT
DEPEND ON ITS OWN LOG TO ANSWER IT.**
"""

if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + section
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md')

# ------------------------------------------------------ 2. PORT_PROGRESS.md
path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
entry = """
### 2026-11-16 The 27-to-2 start-lights collapse was the CENSUS, not the game

**WHAT WAS MEASURED.**  The round census counted the strings
"StartLightsScreen" and "InGameScreen" anywhere in the guest-view log.  The
only occurrences of those strings left in the log are the port's OWN path
trace lines, and their number is set by the trace's thinning and its
2048-slot dedup -- not by the game.  The archived round `mapgate1` log
(67,270,351 B / 422,319 lines) holds exactly 2 of each, and both are
`phase=map-app` / `phase=stat-layout` lines.  The guest-log stream is
quota-limited at 4,000 lines (ordinal max = exactly 4000), so the guest's
own screen names are not in the part that survived.

**WHAT WAS CHANGED.**  The port now counts the two race markers as
RESOLUTION EVENTS (`g_startLightsOpens` / `g_raceScreenOpens`, counted
before the trace is thinned and de-duplicated), writes them into the census
line as `lights=%llu race=%llu`, and the round census reads those counters.
The old text counts stay as a cross-check.  A new contract test
(`native-switch/tests/test_guest_race_marker_contract.py`) asserts the
counters are incremented before the thinning, so the bug cannot come back.

**WHAT IT MEASURES NOW.**  Round `racecount1` (2026-11-16, 409 s):
**30 start-lights resolutions / 45 race-screen resolutions** from the port's
counters, against the old text count of 2 / 2; frames 2400, pixels lit
921600 of 921600, md5 mismatch 0, TrackDesc::load failures 0, race HUD
string 110.  The guest asked for the race screens every 9 to 13 seconds for
the whole round -- **the game was racing continuously and the "regression"
was the instrument.**  Full reasoning: docs/OPEN-ISSUES.md section 9.47.
"""
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md')

# ------------------------------------------------------------ 3. the version
for rel, old, new, label in (
    (os.path.join('native-switch', 'Makefile'),
     'APP_VERSION := 0.22.13', 'APP_VERSION := 0.22.14', 'Makefile'),
    (os.path.join('native-switch', 'rr3.nmeta'),
     '<DisplayVersion>0.22.13</DisplayVersion>',
     '<DisplayVersion>0.22.14</DisplayVersion>', 'rr3.nmeta'),
    (os.path.join('native-switch', 'include', 'rr3', 'version.hpp'),
     '#define RR3_PORT_VERSION "0.22.13"',
     '#define RR3_PORT_VERSION "0.22.14"', 'version.hpp'),
):
    path = os.path.join(ROOT, rel)
    text, crlf, bom = load(path)
    text = sub(text, old, new, label)
    save(path, text, crlf, bom)
    print('OK ' + label)
