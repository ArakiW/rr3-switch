import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.14 The hypothesis was killed by its own experiment

The suspicion in 9.13 was that the direction-lock inversion -- added on
2026-11-09 -- was the regression, because the first A/B cell has the SAME
configuration as q145 and is 2.7 times worse.  So the inversion was turned
off and ONE cell was rerun, with everything else identical (same binary
except the one line, same card, the guest's save restored, the same
navigation).

    m6   the lock inversion OFF, centre line, constant 20 m
         best total 75,141 units = 4.16 % of a lap
    the same cell with the inversion ON
         best total 69,367 units = 3.84 % of a lap
    the four A/B cells, for scale
         3.16 % ... 4.10 %

**⇒ 4.16 % AGAINST 3.84 % IS INSIDE THE NOISE BAND OF THE FOUR CELLS, so
THE INVERSION IS NOT THE REGRESSION.**  The hypothesis was killed by the
experiment that was run to confirm it, and the inversion stays off until
its criterion can be rebuilt on something the recovery cannot poison.

**⇒ AND THE ONE REMAINING SHAPE IN THE LOG IS MORE INTERESTING THAN THE
ONE THAT WAS SUSPECTED.**

    m6   the best total is reached at FRAME 1890 -- 450 frames after the
         race started -- and the round then runs for another 5,000 frames
         with flips 6 and stalls 9 and no further progress.
    q145 the car makes steady progress for 5,760 frames at 32.5 units a
         frame and is still going at the end of the round.

And the race itself starts much later today: the metrics object goes live
at frame 1440, against frame 540 in q145, so the guest spends 900 frames
more in the front end before there is a race to drive.

**⇒ SO THE REMAINING DIFFERENCES BETWEEN q145 AND A TODAY'S CELL ARE THESE
FOUR, AND NONE OF THEM HAS BEEN ISOLATED YET:**

  1. the census: ON in q145, OFF in every 2026-11-09 round;
  2. the NAVIGATION: q145 had the blind A press and an rr3-ui.txt that
     asked for BTN_RACE -- a control that does not exist on the event map,
     so nothing was tapped; today the port TAPS MAP_EVENT_CARD_BTN, so it
     selects whatever card is under the glove, which may not be the card
     the blind press used, and the race starts 900 frames later;
  3. the cond-wait change (9.5);
  4. the clean loader (the asset fallback chain and the console release).

**⇒ THE CHEAPEST NEXT DISCRIMINATOR IS (2), and it costs no build: take
MAP_EVENT_CARD_BTN out of rr3-ui.txt, so the guest is navigated by the blind
A press exactly as it was in q145, and rerun the same cell.**  If the
distance comes back, the port's own navigation was changing which event
raced; if it does not, (1) is next.

**⇒ AND THE LESSON IS THE SAME ONE THE FLIP WAS, ONE LEVEL UP: the thing
that changed between two measurements is not necessarily the thing that
broke them, and the way to find out is to put the old thing back and
measure, not to reason about it.**
"""

if "### 9.14 The hypothesis was killed" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.14")
