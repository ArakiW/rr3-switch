"""Append section 9.34 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: four rounds in a row failed to reach the race and the archive blamed the
port.  The real cause was the HOST: the rounds that raced ran when the machine
was quiet, and the ones that did not ran while a build, a gate and an agent
were all running.  The guest's loading is CPU bound, so the host's load is
part of the measurement -- and it has to be controlled, like any other
variable.
"""
import io
import sys

SECTION = """
### 9.34 The round is a measurement of the HOST as well as of the port

**MEASURED 2026-11-10: six rounds of the same card, the same guest state, the
same 400 s, and they do not agree about what the game does.**

    round   build                                         layout menu event race  frames  frames/s
    chunk2  before                                         4    20   193   10   5640   14.2
    chunk3  before                                         4    20   179   10   5280   13.0
    fps1    the pacing fix alone                          13    67   324   28   1800    4.5
    fps2    + percall switch + instrument                  0     0   143    0   2640    6.6
    yield1  + the pursuit's yield criterion               0     0   137    0   2640    6.6
    fps3    + the asset-service logging behind the switch   0     0   134    0   2040    5.1
    fps4    + the path trace always on                     3    18   150    0   1069    2.7

**⇒ ⇒ THE ROUNDS THAT REACHED A RACE RAN AT 13-14 FRAMES A SECOND, AND THE
ONES THAT DID NOT RAN AT 2.7-6.6.**  The difference is not the build: it is
WHETHER THE MACHINE WAS BUSY.  The guest's loading is CPU bound (the loading
phase's frame time is 300-600 ms, of which the port's own work is 23-27 ms),
so a machine that is also compiling a build, running a gate and driving an
agent makes the guest's loading four times slower -- and a 400 s round that
spends 400 s loading has no race in it to measure.

**⇒ ⇒ SO THE MEASUREMENT PROTOCOL HAS A VARIABLE IN IT THAT WAS NOT BEING
CONTROLLED.**  The rounds that raced (chunk2, chunk3, fps1) ran while the
machine was quiet; the ones that did not ran while a build, a gate and an
agent were running at the same time.  **⇒ ⇒ AND THE FIX IS THE SAME AS EVERY
OTHER FIX HERE: NAME THE VARIABLE AND HOLD IT.**

    A ROUND IS ONLY COMPARABLE WITH ANOTHER ROUND IF NOTHING ELSE WAS RUNNING.

**⇒ ⇒ AND THIS IS WHY THE CENSUS LOOKED LIKE THE CULPRIT.**  With the
per-call logging off, the census reads zero (section 9.33: it counts the main
menu layout in the PORT's own trace, and the trace was switched off with it).
The census was blind AND the guest was slow, and the two looked like one
fault.  The path trace is now always on (the census sees the front end again:
fps4 reads layout 3 and menu 18 where fps3 read 0 and 0), and the round that
has a quiet machine is the one whose numbers can be compared.

**⇒ ⇒ AND THE GENERAL FORM:** a measurement of a system that runs on the host
is a measurement of the HOST TOO.  The frame time of a guest that is loading
on a machine that is also building is not the guest's frame time.  Before
concluding that a change is good or bad, ASK WHAT ELSE WAS RUNNING.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.34 The round is a measurement of the HOST" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
