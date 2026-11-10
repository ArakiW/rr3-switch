"""Append section 9.30 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: two findings that are worth keeping: the scope of the pacing change (it
can only touch a measurable few frames, which RULES IT OUT as the cause of a
round that got slower), and the fact that the guest's loading phase varies by
2-3x between rounds, which makes a single round useless as a comparison.
"""
import io
import sys

SECTION = """
### 9.30 A change that got slower, and the arithmetic that cleared it

**MEASURED 2026-11-10: rounds `chunk3` (before the pacing change) and `fps1`
(after it), same card, same guest state, same 400 s.**

The same frame ranges, computed by the same script (.build/frame-stats.py):

    frames        chunk3 tt          fps1 tt
    0 - 600       485.6 ms           597.7 ms
    600 - 1200    122.0 ms           353.3 ms
    1200 - 1855   185.7 ms           485.4 ms

and `fps1` never got past frame 1855, so it never reached the race phase that
`chunk3` measured at 43.6 ms.

**⇒ ⇒ THAT LOOKS LIKE A REGRESSION, AND IT IS NOT ONE.**  The pacing change
(the sleep is now the remainder of the 16.67 ms budget) can only affect a
frame whose WORK is under 16.67 ms; a frame that takes longer than the budget
gets no sleep either way, before or after.  So the change's SCOPE is exactly
the frames faster than 17 ms, and they can be counted:

    chunk3:  29 of 5342 frames faster than 17 ms  (0.54 %)
    fps1:    50 of 1856 frames faster than 17 ms  (2.69 %)

Even if every one of those 50 frames had become 16.67 ms faster, the whole
round would have saved 0.83 s out of 408 s -- **0.2 %**.  A 0.2 % effect
cannot explain a 2-3x difference.

**⇒ ⇒ SO THE DIFFERENCE IS THE GUEST'S LOADING PHASE, AND IT VARIES.**  Both
rounds contain a single enormous frame -- 51,155 ms in chunk3 and 54,511 ms in
fps1 -- which is the guest's cloud/login wait, and the loading phase around it
runs anywhere between 96 ms and 597 ms a frame depending on the round.  A
single round is NOT a comparison of a build change: it is a comparison of a
build change AND the round's luck.

**⇒ ⇒ THE TECHNIQUE, WHICH IS THE GENERAL PART:** when a change appears to
have made things worse, COUNT THE SAMPLES THE CHANGE CAN TOUCH.  If the
change's scope is arithmetic -- a few frames, a few calls, one branch -- then
its maximum possible effect can be computed, and a measurement outside that
bound clears the change without needing another round.  Only if the measured
effect is INSIDE the bound does the change become the suspect, and only then is
a control round worth running.

**⇒ ⇒ AND THE CONSEQUENCE FOR 60 FPS IS THE SAME ARITHMETIC.**  The frame's
work is 43.6 ms, of which the port's own work outside the guest's render call
is 0.37 ms and the rest is inside it -- the guest's own simulation plus the
port's per-call work (42 log lines a frame, 4.8 path resolutions a frame, a
full-screen readback every 30 frames).  To reach 16.67 ms the frame's work
has to come down by 27 ms, and every millisecond of that has to come out of
the per-call work, because the guest's own simulation is not the port's to
optimise.  The order is therefore fixed: **the per-call overhead first, and
the pacing second** -- the pacing change alone cannot buy a frame rate.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.30 A change that got slower" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
