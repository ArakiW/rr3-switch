"""Append section 9.32 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the pursuit's two remaining defects (section 9.28's spec) are now fixed,
and the fix has to be on the record with the measurement that motivated it.
"""
import io
import sys

SECTION = """
### 9.32 The pursuit yields when the car is not ADVANCING, and a chunk switch clears the window

**MEASURED 2026-11-10 (round chunk3), and both defects are in the pursuit's own
criteria.**

**⇒ ⇒ DEFECT 1: THE CRITERION WAS "THE ERROR IS LARGE", AND A PINNED CAR HAS
A SMALL ERROR.**  The last pursuit phase of the round:

    f     perr     along    pdist   total   steer  thr brk owner
    5190  -0.4892   123.85    9.47   34501   0.978   1  0  pursuit
    5340  -0.5307   123.87    9.50   34507   1.000   1  0  pursuit

`along` is FROZEN (123.85 -> 123.87 over 150 frames) and so is the guest's own
progress (34,501 -> 34,507), while the pursuit holds steer = -1.000 (FULL LOCK)
with the throttle on and NO recovery (brk = 0).  The limit-cycle detector
needed an error above 1.40 rad, this state has 0.53 rad, so it never fired and
the car stayed pinned for the rest of the round.

**⇒ ⇒ A CAR PINNED AGAINST A BARRIER WITH A 30 DEGREE ERROR IS STILL PINNED.**
So the criterion is now the car's own PROGRESS ALONG THE TRACK: the pursuit
gives the wheel to the recovery when the car advanced less than **1.0 m in the
last 120 frames** (3.6 s), and the old error condition is kept as an
ADDITIONAL trigger rather than as the only one.

**⇒ ⇒ AND THE THRESHOLD IS A DISTANCE, NOT A TIME, BECAUSE THAT IS WHAT
SEPARATES "STUCK" FROM "CRAWLING".**  The pinned car measured above advanced
0.02 m in 3.6 s; a car crawling along a barrier at 0.3 m/s covers 1.08 m in
the same window.  A time would not separate them -- the same 3.6 s is both.

**⇒ ⇒ DEFECT 2: THE ALONG-TRACK COORDINATE IS A PROPERTY OF ITS CHUNK, AND
THE WINDOW WAS NOT CLEARED WHEN THE CHUNK CHANGED.**  MEASURED (round chunk3):
the coordinate jumped from 32.26 to 132.11 with the car in the same place --
the chunk picker had moved to another chunk -- and the flip criterion's window
read that jump as **205.21 m of progress the car never made**.  A window that
spans two chunks is not a measurement of anything.

So the sample now carries the chunk the coordinate was measured on
(`targetChunk`), the race-state line reports it (`tchunk=`), and the window is
CLEARED whenever the chunk changes: the ring, the anchor and the stuck timer
all restart, and the criteria begin again from the new chunk.

**⇒ ⇒ AND THE NEW SIGNAL IS ON THE RECORD**, because a criterion that is not
logged is a criterion nobody can check:

    tchunk=  the chunk the along-track coordinate belongs to
    adv=     how far the car advanced along the track in the last 120 frames

`adv` is what the yield criterion reads, and it is the number that says
"pinned" (0.02 m) against "crawling" (1.08 m).
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.32 The pursuit yields when the car is not ADVANCING" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
