"""CORRECT section 9.33: the front end was NOT broken; the CENSUS was blind.

WHY: 9.33 was written from the census alone, and the census counts the PORT's
own trace of the layout files.  With the per-call logging switched off, that
trace is gone, so the census reads zero -- and so does the port's own front
end navigation.  The frames prove the game was fine all along.
"""
import io
import re
import sys

SECTION = """
### 9.33 The front end did not break: the CENSUS went blind, and so did the port

**THIS SECTION CORRECTS ITSELF.  The first version of it said the resolution
cache broke the front end.  That was wrong, and the frames say so.**

**MEASURED 2026-11-10: four rounds, the same card, the same guest state.**

    round   build                                      layout menu event race  frames
    chunk2  before                                      4    20   193   10   5640
    chunk3  before                                      4    20   179   10   5280
    fps1    the pacing fix alone                       13    67   324   28   1800
    fps2    + percall switch + instrument                0     0   143    0   2640
    yield1  + the pursuit's yield criterion             0     0   137    0   2640

The first reading of that table is "the front end broke in fps2".  **⇒ ⇒ AND
THE FRAMES SAY OTHERWISE:** `frames/frame-000340.png` of round yield1 shows
the front end fully drawn -- the "choose event" screen, the three daily-race
cards with the Audi R8, the orange McLaren and the Nissan GT-R, every one of
them textured correctly and not one of them magenta, and the UI automation's
own red glove marker on the first card.  **THE GAME WAS FINE.**

**⇒ ⇒ SO WHAT READS ZERO IS THE CENSUS, AND THE REASON IS THAT IT COUNTS THE
PORT'S OWN TRACE.**  `tools/Test-OnEden.ps1` counts the main-menu layout in
the guest-view log's text:

    $summary.msTwosquare = [regex]::Matches(
        $guestText, 'gui/processed/main_menu/main_menu_twosquare\.gui').Count

and that trace is the path-resolution log, which the `percall` switch turns
off.  **⇒ ⇒ AND THE PORT'S OWN FRONT END NAVIGATION READS THE SAME TRACE**,
so with the switch off the port cannot tell which screen the guest is on, it
stops navigating, the guest sits in the event map, and it starts asking for
the same assets over and over:

    phase                  chunk3    yield1    per frame (chunk3 -> yield1)
    guest-scanf-bounded      1,005    14,297     0.19  ->  5.42

**⇒ ⇒ WHICH IS ALSO WHY fps2's FRAME TIME DID NOT MOVE.**  The per-call lines
went away and the asset-service lines went UP by twenty-eight times, because
the guest was stuck asking for the same thing.  The frame was slow for a
completely different reason than the one the switch was meant to fix.

**⇒ ⇒ SO TWO RULES, AND THE SECOND ONE IS THE GENERAL ONE:**
1. **THE PORT'S OWN INSTRUMENTS MUST NOT BE SWITCHED OFF.**  The census reads
   the path trace; the front end navigation reads the path trace.  A switch
   that silences the port's eyes silences the port's ability to drive the
   guest, and the round then measures a guest that is stuck rather than the
   thing the round was for.  The path-resolution trace therefore STAYS ON
   whatever the switch says; only the high-volume per-call sites go.
2. **A MEASUREMENT THAT READS A STRING OUT OF THE LOG IS ONLY AS GOOD AS THE
   LOG IT READS.**  Every census counter in `Test-OnEden.ps1` is a regex over
   the guest-view log, so turning a phase off does not turn a counter down --
   it turns it into a LIE.  A census counter must read a counter the port
   keeps, not a string the port may stop writing.

**⇒ ⇒ AND THE CACHE IS CLEARED OF THE CHARGE.**  The build logs say the
incremental build did not recompile `guest_filesystem.cpp` for fps2 or yield1
(`build-fps2.log` and `build-pursuit-yield.log` mention it zero times), so
**neither of those rounds contained the cache at all**.  The cache is still
untested in a round, and it has to be tested in one -- with the front end's
eye open.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    start = text.find("### 9.33 The cache broke the front end")
    if start < 0:
        print("section 9.33 not found")
        return 1
    end = text.find("\n### ", start + 10)
    if end < 0:
        end = len(text)
    text = text[:start] + SECTION.lstrip("\n") + text[end:]
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("section 9.33 rewritten (%d chars)" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
