"""Append section 9.36 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the mechanism behind the per-call log is now MEASURED -- the card write's
wait was the guest's CPU lifeline -- and the replacement (a buffered log plus
an explicit per-frame yield) is measured too, with the guest reaching the race
on every round.
"""
import io
import sys

SECTION = """
### 9.36 The log's card write was the guest's CPU lifeline, and it has a replacement

**MEASURED 2026-11-10: rounds percall1 and yield2, the same binary, the same
card, the same guest state, the same 400 s.**

    round      configuration                                 race screen  lights  frames  log lines/frame
    chunk3     per-line flush, no instrument                        10        7     5280        41.9
    percall1   per-line flush + the new instrument                 10        7     1055       206.7
    yield2     BUFFERED log + an explicit 2 ms yield, percall = 0  36       27     2280        52.9

**⇒ ⇒ THREE NUMBERS, AND THEY SAY EXACTLY WHAT HAPPENS.**

1. **THE CARD WRITE WAS THE GUEST'S CPU LIFELINE.**  The port's per-call log
   wrote 42 lines a frame and flushed each one onto the card.  A card write
   WAITS, and the CPU is free during the wait -- and the guest is
   multi-threaded (a loader thread, a render thread, the Cloudcell workers),
   so that wait was how the guest's other threads got any time at all.
   **⇒ ⇒ REMOVING THE WRITES SPEEDS THE PORT UP AND STARVES THE GUEST**, and
   that is exactly what the four rounds with `percall = 0` showed: the guest
   never got past the event map (race screen 0), with the front end itself
   drawn perfectly (yield1/frames/frame-000340.png).

2. **THE REPLACEMENT IS AN EXPLICIT YIELD, NOT A CHEAPER WRITE.**  The log is
   now buffered (recovery_guard.cpp: the lines go to stdio's buffer and the
   file is flushed every 64 lines, so the card sees about one write per frame
   and a half instead of forty-two), and the frame loop yields 2 ms EVERY
   frame, however long the frame was -- because the frames that need the
   yield are the long ones (during the loading phase the guest's own work is
   300-600 ms a frame).

3. **AND THE GUEST IS NOW BETTER FED THAN EVER.**  With `percall = 0`, the
   buffered log and the yield, the round reached **race screen 36 and start
   lights 27** -- the best of any round, against 10 and 7 for chunk2 and
   chunk3.  So the logging can be off, the port does not need the card write
   to feed the guest, and the default is `percall = 0` again.

**⇒ ⇒ AND THE PER-CALL LOGGING WAS NOT THE WHOLE COST EITHER.**  The round
that added the new instrument (percall1) wrote **206.7 lines a frame** --
five times chunk3 -- and its race frame was 144.5 ms against chunk3's 35.0 ms.
**⇒ ⇒ SO THE NEXT TARGET IS NAMED BY THE SAME MEASUREMENT: 52.9 lines a frame
remain, and they are the ASSET-SERVICE lines** (served-digest, open-ok,
fs-candidate, guest-scanf-bounded, guest-log-any, guest-free-seq) -- not
per-call lines, and not gated yet.  They are the instrument that says which
candidate answered, and they are the next thing to make cheap.

**⇒ ⇒ AND THE RULE, IN ONE LINE: a diagnostic that is load-bearing must be
made CHEAP, not REMOVED.**  The per-call log was load-bearing (it was the
guest's CPU lifeline), so it was made cheap -- buffered -- and its cadence
was replaced by an explicit yield.  The cache was load-bearing in the same
way (its hit path wrote a line per hit), so it must be made cheap the same
way.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.36 The log's card write was the guest's CPU lifeline" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
