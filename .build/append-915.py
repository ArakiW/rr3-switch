import pathlib

p = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
t = p.read_text(encoding="utf-8")

tail = """
### 9.15 The port's own navigation is the variable, and the instrument lied AGAIN

**⇒ THE INSTRUMENT, THIRD TIME.**  `total` is SIGNED: the guest publishes the
car's position along the track and it goes negative when the car drives
backwards, so the raw uint32 WRAPS.  The comparison table was taking the max
of the UNSIGNED value, so a wrapped negative -- the car going backwards --
looked like the best result of the round.  It is now signed.

**⇒ AND THE ROUND THAT SETTLES THE NAVIGATION.**  The cheapest possible
discriminator: take MAP_EVENT_CARD_BTN out of rr3-ui.txt, so the port taps
NOTHING on the event map and the guest is navigated by the pad script's blind
confirm press, exactly as in q145.  Same build, same card, the guest's save
restored, same configuration (centre line, constant 20 m).

    q145  the reference                        race starts at frame  540
                                                  best total 186,921 = 10.35 %
    m7    blind navigation, lock inversion off   race starts at frame  540
                                                  best total     -2,728  (backwards)
    m6    the port taps MAP_EVENT_CARD_BTN       race starts at frame 1440
                                                  best total    75,141 = 4.16 %

**⇒ SO THE PORT'S OWN NAVIGATION IS A VARIABLE, AND IT CHANGED WHEN THE
RACE STARTED.**  With the blind navigation the race starts on the SAME
frame as q145 -- 540 -- and with the port tapping the card it starts 900
frames later.  The port's tap selects whatever card is under the guest's
red glove, which is not necessarily the card the blind press used.

**⇒ AND THE PURSUIT'S OWN FAULT IS NOW ISOLATED FROM THE NAVIGATION.**
In m7 the race starts exactly where q145's did, and the car still drove
backwards.  So the remaining distance from 10.35 % is NOT the navigation and
NOT the lock inversion (9.14).  What is left is the pursuit's own behaviour,
the census, the cond-wait change and the clean loader.

**⇒ ⇒ AND THE NEXT ROUND IS THE CENSUS, WITH THE RACE NOW STARTING ON THE
SAME FRAME AS q145:** the same blind navigation with the census ON.  If the
distance comes back, the census is the variable -- and that would be the
third time the census has been blamed and the second time it has been
wrong, so it will be worth a repeat round before it is believed.
"""

if "### 9.15 The port's own navigation is the variable" in t:
    print("already present")
else:
    p.write_text(t.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.15")
