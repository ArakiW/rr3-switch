import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.11 The zero-code fix: hold the accelerator, and let the port navigate BY NAME

Two facts, measured on 2026-11-09, and together they are the whole fix.

**FACT ONE: A HELD BUTTON NEVER WALKS THE MAP; A PULSE DOES.**  The event
map is a scrolling world map ("EventMapScreen" is called "Stream Selection"
in the guest's own screen registry), so a button-down TRANSITION moves the
selection from one championship group to the next.

    m3, m1   the confirm button HELD      no transition, no walking
                                            the guest stays on the group it
                                            started on -- the streamable
                                            AMATEUR one -- and RACES
    v4       the confirm button PULSED    a transition, so the selection
                                            WALKED to the NASCAR card, whose
                                            assets are not on the card, and the
                                            guest sat on a 0 % progress bar

**FACT TWO: THE PORT CAN ALREADY PRESS A CONTROL BY NAME, AND NOTHING WAS
ASKING IT TO.**  `rr3-ui.txt` is a list of the guest's own control ids and
the port taps each one it finds on the current screen
(`ui_control_find` / `ui_control_next_request` in ui_control_probe.cpp,
called from guest_view_probe.cpp).  A request whose control is not on the
screen is SKIPPED, so one wrong id cannot block the others.  And the file
on the card asked for exactly one id:

    BTN_RACE

which the guest's own .gui files put on `MainMenu` and `RaceStartScreen`
and NOWHERE on the event map.  So the request was pressing nothing, and
the event map was navigated by the script's blind button count instead.

**⇒ THE FIX IS ONE CARD FILE, NOT A NEW BUILD:**

    rr3-ui.txt:
        MAP_EVENT_CARD_BTN      the pressable event card, on EVERY group
        BTN_RACE                the race start button
        BTN_OK, BTN_CONTINUE    the popup screens

`.build/deploy-ui-requests.ps1` writes it and installs the HELD pad script
beside it.

**⇒ AND ROUND v5, WITH NOTHING ELSE CHANGED, IS THE PROOF:**

    v5   the held script + the semantic requests, census off, the same build

        metrics live       149 samples, first at frame 1950
        start lights 7, race screen 10      (the normal counts)
        flips 1, stalls 0                   (v3 was flips 7, stalls 5)
        rres median 6.27 m                  (v3 was 9.60 m)

**⇒ TWO FIXES ARE IN THAT ONE NUMBER.**  The semantic requests are what put
the guest in a race that the pursuit can be measured in, and the flip fix --
which inverts the direction lock together with the steering sign, because a
car going the wrong way round the track means the track's direction is the
other way -- is what took the stalls from 5 to 0 and the flips from 7 to 1.

**⇒ AND WHAT IS STILL WRONG IS NOW MEASURABLE RATHER THAN MYSTERIOUS.**  The
lap progress still ends negative (the car still turns round at the end), and
the `dir=` and `mot=` fields are in the log, so the next question is a
measurement and not a theory.
"""

if "### 9.11 The zero-code fix" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.11")
