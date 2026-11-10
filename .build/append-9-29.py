"""Append section 9.29 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the user asked for 60 FPS.  This section is the measurement of where the
frame goes and the first fix, so the next iteration starts from a number
instead of an opinion.
"""
import io
import sys

SECTION = """
### 9.29 Sixty frames: the frame was measured before it was touched

**MEASURED 2026-11-10 (round chunk3, the census lines the port already
writes).**  The port has carried a frame-time instrument since the beginning:
the `guest-frame-census` line reports the PREVIOUS iteration's four numbers
every 60 frames, so the measurement costs no extra log writes.

    tt  the whole iteration
    tr  the guest's render call (the port calls the guest's render entry)
    ts  eglSwapBuffers -- the present
    tp  the port's own per-frame work outside those two (tt - tr - ts)

Round chunk3, the RACE phase (frames > 2500, 2,841 census samples):

    tt = 43.59 ms   tr = 39.29 ms   ts = 3.94 ms   tp = 0.37 ms

and the WHOLE run (the boot and the loading phase dominate it):

    tt = 125.82 ms  tr = 121.54 ms  ts = 3.80 ms   tp = 0.47 ms

**⇒ ⇒ SO THE GAME RUNS AT 22.9 FPS IN A RACE AND AT ABOUT 8.8 FPS WHILE IT
IS LOADING**, and the round's own average (5,280 frames in 406 s = 13.0 fps)
is the average of those two, not a third thing.

**⇒ ⇒ AND 90 % OF THE FRAME IS INSIDE THE GUEST'S RENDER CALL.**  The port's
own work outside it is 0.37 ms.  The guest's render call is where the port's
per-call work happens, because the guest reaches the port through JNI, through
the GL wrappers and through the filesystem resolver.

**⇒ ⇒ THE PORT WRITES 42 LOG LINES PER FRAME, AND EVERY ONE OF THEM IS ONE
fprintf PLUS ONE fflush ON THE CARD.**  Round chunk3: 223,677 lines for 5,280
frames.

    scissor           7.1 per frame      jni-void          4.0 per frame
    map-app           4.8 per frame      jni-utf163        3.9 per frame
    cc-dispatch       4.0 per frame      jni-obj           1.9 per frame
    cc-dispatch-bool  2.1 per frame      stat-path         1.6 per frame

and `map-app` is the path-resolution log: the guest asks for the SAME assets
over and over.  `logResolvedPathOnce()` already dedupes the LOG -- its own
comment says the point was "every one of them is an fprintf plus an fflush on
the emulated card" -- **but the dedupe is only on the log**: the resolution
itself still happens on every request, in `resolveAssetLocalFile()`
(guest_filesystem.cpp:5112), which maps the path and touches the card every
time.

**⇒ ⇒ AND THE PACE WAS WRONG, WHICH IS THE FIRST FIX.**  The frame loop slept
a FIXED 16.67 ms AFTER the work (guest_view_probe.cpp:9988), so the frame
PERIOD was the work PLUS a whole frame: every millisecond of the guest's own
work was paid for twice, and 43.6 ms of work produced a 60.3 ms frame
(16.6 fps) instead of the 22.9 fps the work alone allows.

**⇒ ⇒ THE SLEEP IS NOW THE REMAINDER OF THE BUDGET**, so the period is
max(work, 16.67 ms): the port runs at 60 Hz whenever the work fits in the
budget, and an idle frame still sleeps the whole budget, so the front end does
not spin either.  The budget is converted from nanoseconds to
`armGetSystemTick()` ticks explicitly, because the two units differ by 19.2
(the tick counter runs at 19.2 MHz and `svcSleepThread` takes nanoseconds), and
mixing them would make the budget 19.2 times too long.

**⇒ ⇒ WHAT 60 FPS NEEDS, WITH THE NUMBERS:**

    now                43.6 ms of work   => 22.9 fps after the pacing fix
    - the per-call log lines (42 per frame)  => the log stops being the cost
    - the resolution cache (4.8 resolutions per frame)  => the card stops
      being touched for the same path over and over
    ---------------------------------------------
    16.67 ms of work   => 60 fps

**⇒ ⇒ AND THE HONEST CEILING ON THE EMULATOR:** the present alone is 3.9 ms
there, and the guest's own simulation is not the port's to optimise.  So
Eden's realistic target is 35-40 fps, and the 60 fps figure is a CONSOLE
target -- the same port overhead, but NVN's present instead of the
emulator's, and a faster CPU.  That has to be measured on the console; the
emulator cannot show it.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.29 Sixty frames" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
