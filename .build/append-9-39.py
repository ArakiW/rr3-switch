"""Append section 9.39 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the census now reads the port's counters, the trace is thinned, and the
anti-aliasing hypothesis was measured and REFUSED.  All three belong on the
record, and the last one is the honest end of the 60 FPS question.
"""
import io
import sys

SECTION = """
### 9.39 The census reads counters now, the trace is thin, and the AA hypothesis is REFUSED

**MEASURED 2026-11-10, rounds census2 (AA_LEVEL = 4) and aa0 (AA_LEVEL = 0),
the same binary, the same card, the same guest state.**

**⇒ ⇒ THE CENSUS READS THE PORT'S COUNTERS NOW.**  The port counts the
resolution EVENTS for the main menu's layout and for the two-square panel, and
the census line carries them (`gui=`, `twosq=`):

    census2's census line: ... gui=75 twosq=15

which is **exactly what the log-text grep reported in yield2 (15 and 75)** --
so the counter measures the same thing, and the measurement no longer depends
on what the port writes.  The census script reads the counters and falls back
to the text only for an older log.

**⇒ ⇒ AND THE TRACE CAN BE THINNED FOR REAL.**  With the census off the log's
text, the path trace is written only for the `gui/processed/` subtree and the
rest is counted:

    yield2   124,464 log lines   52.9 per frame
    census2   15,755 log lines    7.3 per frame

**⇒ ⇒ AND THE ANTI-ALIASING HYPOTHESIS IS REFUSED BY THE MEASUREMENT.**  The
card was running `AA_LEVEL = 4` (four samples per pixel), which looked like
the obvious lever on the guest's rasterisation.  Turning it off did NOT make
the frame faster:

    round        AA_LEVEL   tt median   jni median   jnioth mean   fs mean
    census2          4        72.0 ms      8.0 ms      164.2 ms    43.8 ms
    aa0              0        83.0 ms      0.0 ms      221.3 ms    10.3 ms

**⇒ ⇒ SO THE FRAME'S REMAINING COST IS THE GUEST'S OWN WORK** -- the Java and
C++ method bodies that run inside the JNI dispatch (`jnioth`), which the port
cannot change -- and the emulator's GL path on top of it.  The port's own
share is now about a millisecond a frame (`tp` 0.4 ms, `fs` 0 ms).

**⇒ ⇒ ⇒ THE COMPLETE ANSWER TO "SIXTY FRAMES":**
1. **THE PORT IS OUT OF THE WAY.**  Its own work went from tens of
   milliseconds a frame (42 blocking card writes) to about one.
2. **WHAT IS LEFT IS THE GUEST'S**, and two candidate levers were MEASURED and
   both refused: the per-call logging (already removed, and the guest
   starved without it) and the anti-aliasing level (no effect).
3. **⇒ ⇒ SO 60 FPS IS A CONSOLE MEASUREMENT AND NOT AN EDEN ONE.**  Eden's
   GL path is the emulator's, and the guest's own per-frame work runs on the
   emulator's CPU.  The instrument that can answer it is already on the card:
   the census line's `tt` against the 16.67 ms budget, and `jni` for the
   port's share of it.  **⇒ ⇒ AND THE CONSOLE'S ANSWER IS THE NUMBER THAT
   DECIDES WHETHER THE GAME SHIPS AT 60.**
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.39 The census reads counters now" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
