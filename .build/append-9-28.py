"""Append section 9.28 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the control-law fix has been MEASURED, and the measurement has to be in the
repository with its residue -- including the new defect the round exposed, which
is the reason the next iteration has a spec.
"""
import io
import sys

SECTION = """
### 9.28 The control law, measured: better, and not yet a lap

**MEASURED 2026-11-10, round `chunk3`, the build with the projection aim
point, the along-track flip criterion, the along-track stuck test and the
recovery's ownership of the wheel.**

**⇒ ⇒ THE ERROR CAME DOWN BY A FACTOR OF FOUR:**

    round chunk2:  perr = 1.5 to 2.9 rad (86 to 166 degrees), steadily
    round chunk3:  perr = -0.49 to -2.52 rad, and -0.53 (30 degrees) for the
                   last 150 frames of the pursuit phase

and the stall counter went 7 (chunk1) -> 6 (chunk2) -> 3 (chunk3).

**⇒ ⇒ AND THE NEW INSTRUMENT SHOWS WHY IT IS STILL NOT A LAP.**  The last
pursuit phase of the round, every 30 frames:

    f     perr     along    pdist   fdelta   total   steer  thr brk owner     flips stalls
    4830  -2.3116   114.32    6.99  -48.33   33004   1.000   0  0  pursuit     2     3
    4890  -2.5128   112.82   13.14   80.56   34473   1.000   0  0  pursuit     2     3
    4980  -2.5211   112.68   13.45  205.21   34523   1.000   0  0  pursuit     2     3
    5130  -2.0302   123.87    9.47    0.00   34496   1.000   0  0  pursuit     3     3
    5190  -0.4892   123.85    9.47    0.00   34501   0.978   1  0  pursuit     3     3
    5340  -0.5307   123.87    9.50    0.00   34507   1.000   1  0  pursuit     3     3

**⇒ ⇒ THREE THINGS ARE NOW MEASURED THAT WERE NOT BEFORE.**

1. **THE CAR CAN SIT STILL WITH THE PURSUIT AT FULL LOCK AND THE THROTTLE ON,
   AND NOTHING CATCHES IT IN TIME.**  From f=5190 the along-track coordinate is
   FROZEN (123.85 -> 123.87 over 150 frames) and the guest's own progress is
   frozen too (34,501 -> 34,507), while `steer` is at full lock, `thr` is 1 and
   `brk` is 0.  The recovery's brake is not on, so the recovery's decisive angle
   -- the thing that unjams a car -- is not being applied.  The limit-cycle
   detector needs an error above 1.40 rad and this state has 0.53 rad, so it
   does not fire; the stuck test needs 600 frames (18.2 s) of no along-track
   progress, and the round ended before that.  **⇒ THE CRITERION FOR "THE
   PURSUIT MUST YIELD" HAS TO BE "THE CAR IS NOT ADVANCING", NOT "THE ERROR IS
   LARGE".**  A car pinned against a barrier with a 30 degree error is still
   pinned.

2. **THE ALONG-TRACK COORDINATE IS NOT COMPARABLE ACROSS A CHUNK SWITCH.**
   The chunk picker chooses the chunk the car is on, and the arc coordinate is
   measured on THAT chunk: at f=4530 `along` is 32.26 and at f=4800 it is
   132.11, with the car in the same place.  The flip criterion's window
   therefore saw a +100 m "progress" it never made, and `fdelta` printed
   205.21 m -- a delta no car on that track covers in 360 frames.
   **⇒ THE RING HAS TO BE CLEARED WHEN THE CHUNK CHANGES**, or the criterion
   has to be measured on a chunk-independent quantity (the guest's own signed
   distance along ITS spline).

3. **THE PORT CAN NOW MEASURE WHY THE CAR IS STUCK**: `pdist` (the live
   distance to the line, 6.99 to 13.45 m) and `along` (the arc coordinate)
   together say "this car is 13 m off the line and has not moved along the
   track for 150 frames", which is the whole story in two numbers.  Before this
   round the same fact was invisible: `lineResidual` held the FIT's residual,
   a constant.

**⇒ ⇒ SO THE NEXT ITERATION HAS A SPEC, AND IT IS TWO ITEMS:** the pursuit
yields when the car is not advancing (whatever the error is), and the flip
criterion's window is cleared when the chunk changes.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.28 The control law, measured" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
