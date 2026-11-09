import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.9 There are TWO wedge screens, and the second one is not a button problem

Round v4 (2026-11-09) repeated v3 with ONE change -- the guest's save data
was restored before the round -- and it did NOT reach the race.  Its last
frame says why, and it is a different screen from the one that wedged m3:

    .build/eden-runs/20261109-162254-eden-rr3-v4-flipfix/frames/frame-005600.png
    -> the NASCAR event screen, with the guest's red glove on the event and
       a progress bar reading the progress at 0 %.

**=> THE GUEST WAS TRYING TO LOAD AN EVENT WHOSE ASSETS ARE NOT ON THE
CARD.**  The load never progresses, the screen never changes, and the race
never starts -- and no button press can fix that, because what is missing
is DATA and not a dismissal.

**=> SO THE TWO WEDGES ARE DIFFERENT, AND THE PULSE ONLY FIXES ONE.**

    m3   the confirm button held down      wedged on the REWARD POPUP
    v3   the confirm button pulsed         got past it, reached the race
    v4   the pulse AND the save restored   wedged on the EVENT LOAD, 0 %

**=> THE PULSE IS CONFIRMED BY m3 AGAINST v3: the script was the only thing
that changed, and the popup stopped wedging the guest.**  What v4 shows is
that the state restore is HARMFUL as implemented: it puts the guest back on
an event whose assets are not on the card.

**=> AND THE BIGGER POINT, WHICH IS NOT A PORT BUG AT ALL: the port cannot
reach a race unless the EVENT's data is on the card.**  That is a shipping
requirement -- the package has to carry the event data, or the user has to
have downloaded it -- and it is also why the "2x2" cells differed: the
guest's persisted state decides WHICH EVENT it lands on, and an event
whose assets are absent is a wedge that no input can clear.

=> The harness therefore has to do two separate things, and only one of
them is about input:

  * PULSE the confirm button, so a modal dialog cannot wedge the guest.
  * PUT THE GUEST ON AN EVENT WHOSE ASSETS ARE ON THE CARD, or the round
    measures the download screen instead of the pursuit.  The guest's
    current event is visible in the archived frames, so this is checkable
    rather than guessed.

**=> AND THE RULE THAT FALLS OUT OF IT, WHICH COSTS THE MOST TO LEARN: a
round whose start state is not controlled is not a measurement.**  Three
readings of the same 2x2 (the code, then the census, then the persisted
state) were all wrong, and each was wrong because the cells were not
comparable.  **The first thing to fix is the experiment, and only then the
thing being measured.**
"""

if "### 9.9 There are TWO wedge screens" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.9")
