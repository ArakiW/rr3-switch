import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.12 What the dir= and mot= fields say about the pursuit's REAL first fault

Round v5 (2026-11-09) was the first round with the direction lock and the
windowed motion vector IN THE LOG, and they say something the earlier rounds
could not.

    f=2370 .. 2730   dir=0   la=0.00   mot=0.967,-0.255   steer=-0.05
    f=2760           dir=-1  la=4.47   mot=0.967,-0.255   perr=-0.46 steer=0.92
    f=2940           dir=-1  la=4.47   sdist=-20658        total 74964 -> 50141
    f=3030           dir=1   la=4.48   perr=+0.43         steer=-0.85  flips=1

**⇒ READ IT BACKWARDS, BECAUSE THE FAULT IS EARLIER THAN THE DIRECTION.**

  1. **FOR 360 FRAMES THE PORT HAD NO TARGET AT ALL** (`la=0.00` means
     `raceLineTargetAdaptive()` returned false).  It refuses to aim at a car
     more than 25 m from the fitted line, and the car was further out than
     that -- it had already left the road.  `total` grew by only 433 units in
     those 360 frames, so the car was barely moving: it was off the line,
     probably against a barrier, and the port was steering it with a
     0.05-sized correction because it had nothing to aim at.
     **⇒ THE RACE WAS LOST BEFORE THE PURSUIT EVER STEERED IT.**
  2. **THE DIRECTION LOCK WAS DECIDED AT f=2760, IN THE MIDDLE OF THAT
     MESS**, and it was decided as **-1**, while the car's own travel
     direction was `mot=0.967,-0.255`.  So the look-ahead point went on the
     OPPOSITE side of the car, the pursuit error became -0.46 rad, and the
     controller held full lock (0.92) into it.
  3. **THE CAR REVERSED** at f=2940: `sdist` went negative and `total` fell
     from 74964 to 50141.
  4. **THE FLIP RECOVERY CORRECTED IT** at f=3030: `flips=1` and the lock
     went back to `dir=1`, with the steering following.  **⇒ The fix in
     9.8 WORKS: the lock and the steering sign are now corrected together.**
     But it took the 600 frame hysteresis to do it, and by then the car was
     already going the wrong way.

**⇒ SO THERE ARE TWO FAULTS, AND THE SECOND ONE IS THE INTERESTING ONE.**

  * the pursuit does not engage early enough (the 25 m guard means the car
    is unguided through the opening of the race), and
  * the direction is decided ONCE, from a geometric comparison of the car's
    travel direction with the spline's node order, at a moment when the car
    is in a state where that comparison is not trustworthy.

**⇒ AND THE ROBUST ANSWER IS THE GUEST'S OWN TRUTH, NOT OUR GEOMETRY.**
The guest already knows which way round the track it is: it publishes the
car's SPLINE NODE (`snode`) and its signed position along the track
(`total`).  **So the direction round the track is measurable directly from
the guest's own numbers -- does `snode` increase or decrease as the car
moves -- instead of being inferred from a comparison between the car's
heading and the polyline's neighbours, which is a MIRROR-SENSITIVE
inference made at a moment when the car may be facing any direction at all.**

**⇒ AND THIS IS THE SAME RULE THE PROJECT KEEPS LEARNING, IN A NEW PLACE:**
the direction round a closed track cannot be told from a residual, and it
cannot be told from a single sample of a car that is out of control.  It has
to come from the thing that knows, and that thing is the guest.
"""

if "### 9.12 What the dir= and mot= fields say" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.12")
