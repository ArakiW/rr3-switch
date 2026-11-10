"""Append section 9.35 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the per-call logging turned out not to be a cost that could simply be
switched off: its CADENCE is part of how the port and the guest keep step.
That is the finding of this round and it has to be on the record, because it
changes the plan for 60 FPS.
"""
import io
import sys

SECTION = """
### 9.35 The per-call log is not only a cost: its CADENCE is part of the sync

**MEASURED 2026-11-10: two rounds, the SAME binary, the same card, the same
guest state, the same 400 s, differing only in the `percall` key on the card.**

    round      percall   layout menu event race  lights  frames  frames/s
    fps5          0         3    27   481    0      0      2520     6.3
    percall1      1         7    40   183   10      7      1055    15.2

**⇒ ⇒ WITH THE LOGGING OFF THE GUEST NEVER GETS PAST THE EVENT MAP.**  With
it on, the same build reaches the race (race screen 10, start lights 7) in
1,055 frames -- the fastest loading of any round.

**⇒ ⇒ AND THE GAME IS NOT BROKEN WITH THE LOGGING OFF.**  The frames of the
percall = 0 rounds show the front end fully drawn and correctly textured
(yield1/frames/frame-000340.png: the "choose event" screen with the Audi R8,
the McLaren and the GT-R cards, every texture right).  **⇒ ⇒ SO IT IS NOT THE
GAME THAT BREAKS; IT IS THE PORT THAT STOPS DRIVING.**  The front end
navigation presses controls by name and position, and it is not reading the
log to do it -- so the coupling is in the TIMING: the guest's progress and the
port's cadence are entangled, and the port writes 42 lines a frame.

**⇒ ⇒ AND NONE OF THE SWITCH'S SITES CHANGES WHAT THE GUEST RECEIVES.**  All
eight of them are in LOG functions (`logWatchedOpen`, `logStatLayout`,
`logStatPath`, `guestViewNoteAndroidLog`, `logRefusal`, `perCallLogAllow` and
two `if (gfxPerCallLog() && ...)` probes that only decide whether to PROBE).
So the switch does not change a single byte the guest reads; it changes WHEN
the port writes, and that is enough to change whether the guest gets anywhere.

**⇒ ⇒ SO THE PLAN FOR 60 FPS CHANGES, AND THIS IS THE POINT OF THE ROUND:**
"turn the per-call logging off" was the plan, and it is WRONG.  It does not
buy a frame rate -- the rounds that "won" with it off never reached a race, so
they never measured a race's frame time -- and it costs the port its ability
to drive the guest.  **⇒ ⇒ THE DEFAULT IS THEREFORE BACK ON (`percall = 1`),
and the work becomes: make the lines CHEAP without making them LESS
FREQUENT.**

    the lines stay (the cadence is load-bearing)
    the writes do not (RAM buffering, flushed in bulk)

**⇒ ⇒ AND THE SAME RULE GOVERNS THE RESOLUTION CACHE.**  Its hit path was
writing a line per hit, which is the repetition the dedup existed to stop: the
asset-service phases went from 0.19-0.48 lines a frame (chunk3) to 1.17-5.42
(yield1), and the guest's own requests went with them (guest-scanf-bounded:
1,005 -> 14,297).  **⇒ ⇒ SO A CACHE MUST NOT ADD A WRITE PER HIT EITHER** --
the same rule, the same reason.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.35 The per-call log is not only a cost" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
