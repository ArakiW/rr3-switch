"""Append section 9.24 to docs/OPEN-ISSUES.md, in UTF-8, without a BOM.

WHY: the file is UTF-8 and the shell's console encoding mangles it on read, so
the append is done from Python with an explicit encoding rather than with
Add-Content, which would write the console's code page.

WHY: the section is the answer to "how far from playable", and it has to carry
the measurements, because the number is only as good as the evidence behind it.
"""
import io
import sys

SECTION = """
### 9.24 The instrument was broken for one round, and what it was hiding

**MEASURED 2026-11-10, the commit that added the `cchunk` field.**

The commit that added the race-state line's `cchunk` field put the new
argument BEFORE `dir`/`mot`, while the format string puts `cchunk=%.2f` AFTER
`mot=%.3f,%.3f`.  The compiler said so, in the build log nobody read:

    race_state_probe.cpp:2810:73: warning: format '%d' expects argument of
      type 'int', but argument 79 has type 'double' [-Wformat=]
    race_state_probe.cpp:2810:82: warning: format '%f' expects argument of
      type 'double', but argument 80 has type 'int' [-Wformat=]

**THE MISPLACEMENT WAS LOCAL, NOT GLOBAL.**  The number of specifiers and the
number of arguments stayed equal, so only the four fields BETWEEN the two
insertion points were printed from the wrong arguments: `dir`, `mot.x`,
`mot.y` and `cchunk`.  Everything from `perr` onwards was aligned.  The log
proves it: `dir` printed `-328052707` and `2102253012` (the low words of a
double read as an int), `mot` printed `5292966687608640881647220322813173589
4073144488517008954246440160691924632290202386921155628859625199857425267511
39174547456.000` (an int read as a double), and `cchunk` printed `-0.42`
(the motion vector's Y).  `perr`, `pur`, `flips`, `stalls`, `total` and
`ctdist` were right.

=> SO THE `chunk1` ROUND'S READING OF `cchunk` WAS MEANINGLESS, and the
reading of `dir` was meaningless, but its reading of `pur` (the pursuit was
on in 150 of 193 samples), `flips` (9 -> 6), `stalls` (7) and `total` (94,034)
was sound.  The chunk picker's own new field was the one field the round
could not see.

**⇒ ⇒ AND THE FIX IS NOW MEASURED (round `chunk2`, the instrument fixed):**

    f=3390  dir=1  mot=-1.000,-0.007  cchunk= 4.80  perr= 1.775  pur=1  total=66106  ctdist=19.80  flips=2  stalls=3  steer=-1.000  thr=0  brk=1
    f=3870  dir=1  mot=-1.000,-0.007  cchunk= 4.80  perr= 1.518  pur=1  total=71949  ctdist=17.92  flips=2  stalls=5  steer=-1.000  thr=0  brk=1
    f=4590  dir=1  mot=-0.997, 0.076  cchunk= 6.32  perr= 2.911  pur=1  total=58507  ctdist=20.73  flips=3  stalls=5  steer=-1.000  thr=0  brk=0

* `cchunk` is 2.5 to 9.3 m: **the chunk picker works.**  The aim point comes
  from a chunk the car is on, where before it came from one 13,215 m away.
* `dir` is 1 and `mot` is (-1.000, ~0.000): the direction lock is decided and
  the car's own 20 m displacement vector agrees with itself.
* `perr` sits at 1.5 to 2.9 rad (86 to 166 degrees) and STAYS there.  The aim
  point is 17 to 25 m away, which is the 20 m look-ahead, so an error of 180
  degrees is not geometry: **the aim point is behind the car.**
* The guest's own progress along the track FALLS (71,949 -> 58,507 -> 30,865):
  **the car is driving BACKWARDS down the track.**
* `brk=1` at the same time and `steer=-1.000`: **the recovery is braking at
  full lock.**  A brake plus a full lock is a spin.

**⇒ ⇒ THE CAUSAL CHAIN, EVERY LINK MEASURED:**

1. The aim point is behind the car, so the pursuit holds full lock.
2. The car ends up driving backwards down the track.
3. The stall test fires; the recovery brakes for 240 frames.
4. The pursuit OVERRIDES the recovery's steering -- the source says "BUT THE
   PURSUIT WINS" -- so the recovery's reversing angle never applies, and a
   brake plus a full lock is a spin.
5. The steering-sign flip cannot help either: while the pursuit is on, `steer`
   is `steersign * perr / steergain`, so `g_drive.sign` has NO effect on the
   wheel at all.  The `flips` counter counts events that never reach the car.
6. The one thing that could fix an aim point that is behind the car -- the
   direction-lock inversion -- is disabled in the source (`if (false && ...)`),
   and the criterion that used to drive it (a 120 frame smoothed rate) is
   exactly the signal the recovery poisons, because the recovery reverses the
   car on purpose.

=> **THE PORT CANNOT ESCAPE AN INVERTED LOCK.**  That is the difference
between "the pursuit drives badly" and "the pursuit cannot recover", and it is
why the round ends with the car creeping in circles instead of a lap.

**⇒ ⇒ AND THE STALL TEST'S EXEMPTION IS NOT A LIVE MEASUREMENT:**
`lineResidual` is assigned the FIT's residual (a constant, 1.22 to 3.88 m in
this round), so "the car is on the line" is a constant boolean for the whole
race instead of the current distance to the line.  The discrimination the
comment claims is not the one the code makes.

**⇒ ⇒ AND THE PICTURE SAYS IT TOO.**  The archived frame `frame-005600.png`
of round `chunk2` shows the guest's own message, "hold the brake to reverse",
over the player's car, which is nose-first into the barrier.

### 9.25 The magenta fix, confirmed by eye

**MEASURED 2026-11-10, round `chunk2`, 122 archived frames, 112,435,200
pixels scanned.**

    EXACT (255,0,255)   : 0
    green<=12, r,b>=128 : 0  (0.00000% of all pixels)

**⇒ ⇒ AND THE FRAME LOOKS RIGHT.**  `frame-001600.png` is the front end:
the Chinese UI renders ("Season 5 Championship", "reach player level 4 to
unlock the championship"), the Porsche 911 RSR promo car is red and white with
its sponsor decals, the track thumbnail car is painted, the currency bar and
the bottom navigation are all there.  **No magenta car, anywhere.**

**⇒ ⇒ AND THE SAME FRAME SET SAYS WHERE THE HUD GAP IS.**  The FRONT END is
fully styled -- it has its own styles -- while the IN-RACE HUD
(`frame-005600.png`) is EMPTY: a pause button, a camera button and one icon,
and no speedometer, no gear, no lap counter, no position, no minimap.  That is
the `LocalStyles.gui` / `LocalStyles_480x320.gui` gap, and it is now visible in
a picture rather than only in a file listing.

### 9.26 What "playable" means here, and where the port stands

The question needs a definition, otherwise it is a feeling.  This is the
definition this project measures against, with the state of each line on
2026-11-10.

| # | Acceptance criterion | How it is measured | State |
|---|---|---|---|
| 1 | The guest boots to the front end, and the front end's screens are the baseline set | the `front-end` census in the round's `run-summary.txt` | **DONE** (4 / 20 / 194 / 7 / 10, item by item equal to the baseline) |
| 2 | No magenta or noise textures | the pixel scan of the archived frames, plus the `ctex-upload` lines' FourCC | **DONE AND CONFIRMED BY EYE** (section 9.25) |
| 3 | The in-race HUD is readable | the guest finds `gui/processed/LocalStyles.gui` and `LocalStyles_480x320.gui` | **BLOCKED ON THE USER**: the two files are generated by the guest's first run on Android and exist in no package; a substitute breaks the game (measured: the front end does not come up) |
| 4 | A controller reaches the car | the `gamepad-state` / `scripted-state` lines: `lx_script` against `lx_guest`, `loop_steered` | **DONE on the port side** (q149: the axis the human asked for is the axis the guest got, `loop_steered=0`). **NOT DONE in Eden**: the host controller is bound to no player, so the port sees an idle Pro Controller (`state=npad-idle`). **NOT DONE on hardware** |
| 5 | A race starts and the car moves | the `race-state` line: `total` rising | **DONE** (the guest enters a race and the car drives) |
| 6 | The car drives a whole lap and the lap counter moves | `laps` and `total` against the lap length `lapd` | **NOT DONE**: the best measured round reached 15.96 % of a lap, `laps` stayed -1.  The cause is section 9.24 |
| 7 | The AI opponents look right | a screenshot of the race | **PARTLY DONE**: the cars that are visible in the archived frames are correctly textured; no round has yet reached the event with a full grid of rivals in frame |
| 8 | The port does not freeze on hardware | the `rr3-native-rehost.guest-boot.progress` file on the card | **AWAITING THE USER**: v0.22.8 (the condvar fix) has not been re-tested on the console |

**⇒ ⇒ SO THE PORT'S OWN WORK IS ALMOST DONE AND THE REMAINING GAPS ARE
THREE:** the lap (section 9.24, the pursuit's control law), the two HUD files
(only an Android device has them), and the controller binding plus the
hardware re-test (only the user can do those).
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.24 The instrument was broken" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
