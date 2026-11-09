import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.13 The four-cell A/B, and the number that was wrong

**⇒ FIRST, THE INSTRUMENT WAS LYING AGAIN.**  The report printed the
guest's `prog` member under the name "progress".  The project's own record
says "The car's best distance ever | 186,921 units = 10.35 % of a lap |
q145", and q145's last race-state line reads `total=186921 ... prog=9799`.
**⇒ The lap progress is `total`, the guest's total spline distance, and the
number that matters is its MAXIMUM over the round.**  The report now prints
both, named, and `.build/lap-progress.py` prints the comparable table.

**⇒ THE FOUR CELLS (2026-11-09, the same build, the same card, the same
binary, the guest's save restored before each one):**

    cell                          best total   % of a lap   rres   flips  stalls
    fixed    + centre line          69,367        3.84 %     11.80     2       3
    adaptive + centre line          58,110        3.22 %      1.37     2       0
    fixed    + racing line          57,091        3.16 %      2.08     1       0
    adaptive + racing line          74,016        4.10 %      5.63     7       7
    ------------------------------------------------------------------
    q145 (2026-11-04, the reference) 186,921     10.35 %      0.32     7      10

**⇒ THREE THINGS COME OUT OF IT, AND ONLY THE FIRST IS COMFORTABLE.**

  1. **THE PURSUIT IS REPEATABLY MEASURABLE NOW.**  All four cells reached
     the race, with the normal 7 start lights and 10 race screens, the
     metrics object alive, and the port's closed loop driving.  That is the
     thing that was missing for a month, and it is worth more than any one
     of these four numbers.
  2. **ALL FOUR ARE BELOW THE REFERENCE, AND THE FIRST CELL HAS THE SAME
     CONFIGURATION AS q145.**  Centre line, constant 20 m, and it is 2.7
     times WORSE.  The only differences are the changes of 2026-11-09.  And
     the shape of the failure is in the log: at the end of that cell the
     car's progress rate is **2.76 units a frame against q145's 23.88**,
     with the steering **pinned at full lock (steer=1.000)**.  A pursuit
     that is saturated at full lock is not tracking the line.
  3. **THE ADAPTIVE LAW AND THE FIXED LAW BARELY DIFFER ON THIS TRACK.**
     `la` has median 20.00 m in ALL FOUR cells -- identical -- because most
     of the road ahead is straight and the law only shortens the look-ahead
     in the corners.  **⇒ This A/B could not have separated the two laws
     even if the pursuit were perfect, and it did not.**  The spread
     between the four cells (3.16 % to 4.10 %) is of the same order as
     the difference between them, with one round per cell.

**⇒ AND THE PRIME SUSPECT FOR (2) IS THE FLIP'S OWN CRITERION.**  The flip
recovery inverts the steering sign when the rate stays negative.  The rate
is a 120 frame window, and **the recovery reverses the car ON PURPOSE**, so
the recovery's own reverse drives the rate negative.  **⇒ The flip can
fire on the recovery's signal, and inverting the direction LOCK on that
signal puts the look-ahead point on the wrong side of the car -- the one
thing the lock exists to prevent.**  The lock inversion is therefore
DISABLED, with the reason recorded in the code, and the next round measures
whether the distance comes back.

**⇒ AND THE NEXT QUESTION AFTER THAT IS NOT THE LOOK-AHEAD LAW AT ALL.**  It
is the pursuit's ENGAGEMENT: the port refuses to aim at a car more than 25 m
from the fitted line, so the car spends the opening of the race unguided
(9.12), and the direction lock is decided from a geometric comparison at a
moment when the car is in no state to be compared.  The robust answer is the
guest's own signed `total`, which is already in the log.
"""

if "### 9.13 The four-cell A/B" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.13")
