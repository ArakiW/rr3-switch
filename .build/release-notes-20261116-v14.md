# Real Racing 3 -- Switch native re-host, v0.22.14

## WHAT THIS FIXES

The "27 -> 2 start lights" collapse was never a game regression.  The round
census counted the STRINGS "StartLightsScreen" and "InGameScreen" anywhere
in the guest-view log, and the only occurrences of those strings left in
that log are the port's OWN path-trace lines.  How many of them there are is
decided by the port's logging policy, not by the game:

- the path trace is thinned to `gui/processed/` and de-duplicated by a
  2048-slot table with no eviction, so a path is written AT MOST ONCE for
  the whole run -- `StartLightsScreen.gui` can produce at most one trace
  line no matter how many times the guest asks for that screen;
- the guest's own log stream is quota-limited (4000 lines,
  `guest_view_trace.cpp:37`), and the archived round `mapgate1` ends with
  the ordinal at EXACTLY 4000.

Measured in that archive (67 MB guest log): "StartLightsScreen" 2
occurrences, "InGameScreen" 2 occurrences, and BOTH of each are the port's
own trace lines.

**THE FIX.**  The port keeps the two markers as counters, counted in
`resolvePath()` -- THE ONE PLACE EVERY PATH TAKES -- as RESOLUTION EVENTS.
The census line carries them at its HEAD, the round census reads them out
of the COMPLETE census lines, says whether each number came from the port
or from the old text fallback, and prints the frame the sample was taken
at.

**AND AN ADVERSARIAL REVIEW OF THAT FIRST FIX FOUND THREE REAL DEFECTS IN
IT, AND THEY ARE IN HERE TOO.**

- the first version counted the markers in the trace function, which ONE
  resolver branch calls; six other branches resolved the same files
  without counting, so it measured "requests that arrived in one spelling"
  and not "requests";
- the first contract test caught ONE of the five ways of breaking the
  property.  It is mutation-tested now: seven ways, all seven caught;
- the log file is written by two threads into one file, so a line can be
  SPLIT -- MEASURED: one spliced census line per round, in every archive
  -- and the two numbers sat at the END of the census line, which is
  exactly where a split can take them away from their line.  They are at
  the HEAD now.

**MEASURED** (round `rr3-racecount3`, 408 s, the same host and the same starting save
as the round before it, which changed only the counter's placement):

    start lights          6      (the guest asked for that layout)
    race screen           9
    the old text count    2 start lights / 2 race screen
    frames                12240
    census sample frame   12298
    pixels lit            9 of 2
    md5 mismatch          0      TrackDesc::load failures  0
    race HUD string       22

**READ IT AS "THE GUEST ASKED FOR THAT LAYOUT N TIMES", NOT "N RACES RAN".**
The counters count RESOLUTION EVENTS, the guest's event progression is not
deterministic, and the number moves from round to round.  What does NOT
move is the old text count: 2, in every round -- that is the instrument's
signature, and the whole reason this release exists.

## ALSO IN THIS RELEASE

- The 2026-11-10 console crash fix (v0.22.13) is in here unchanged: the
  load bias is no longer published before the image is mapped,
  `guestImageContains()` and `resolveMainImageLoadBias()` fail closed
  until the mapping exists, and the crash dump decodes its own ESR and
  prints `fault_class=`, `far_attribution=` and `main_image_mapped=`.
- The resolution counter and the two front-end layout counters moved to the
  resolver as well, for the same reason the race markers did: they were
  counted in the same one-branch function.
- The gate now PARSES the PowerShell harness before running it.  A change to
  the round census's print block put an apostrophe inside a single-quoted
  string, and the parse error surfaced twelve minutes later, in the middle
  of an Eden round.  The gate reads the harness now.
- `docs/OPEN-ISSUES.md` section 9.47 records the whole chain, including the
  wrong readings that preceded it and what the review of the fix found.

## HOW TO TEST

1. Delete the old "RR3 Native Rehost" icon from the home menu first, then
   install this NSP.
2. Launch it from the home menu.
3. If it crashes, send `rr3-native-rehost.crash-early.txt` and
   `rr3-native-rehost.guest-view.log` back -- the three new lines in the
   crash file (`fault_class`, `far_attribution`, `main_image_mapped`)
   are the point of that fix.
4. The census line in the guest-view log now reads
   `phase=guest-frame-census ... frame=N lights=N race=N ...`, so the race
   markers can be read straight out of it.

## WHAT IS IN THE PACKAGE

- `rr3-native-rehost.nsp` -- 50,535,128 bytes, sha256 `D5655008F6A6B9FB32F974351A6EA0C98E1703568A9DD2CA4693552022F64C83`
- `rr3-switch-app-20261116-v0.22.14.zip` -- 98,241,226 bytes, sha256 `3A5B25B8B54768640CC348D9C8D475B65498BEDD28FF4687855968536A1BE799`
  (the app-only package: the NRO, the romfs tree, the card control files,
  the install scripts and the README)

## SUPERSEDES

v0.22.14 replaces v0.22.13 and every earlier release.  v0.22.13 carried the
console crash fix with the three defects the review found closed; v0.22.14
adds the measurement fix, and the three defects an adversarial review found
in THAT fix are closed here too.
