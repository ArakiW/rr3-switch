import pathlib

path = pathlib.Path(r"E:\AI\RealRacing3-switch\docs\OPEN-ISSUES.md")
text = path.read_text(encoding="utf-8")

tail = """
### 9.10 The control ids the front-end driver needs, grounded in the guest's own files

The port can already read the guest's screen tree by control id
(`ui_control_probe.cpp`: `uiControlFind`, `uiScreenOpen`, and the current
screen's NAME in the sample).  What was missing was the TABLE, and the table
has to come from the guest's own .gui files, not from a screenshot.

`tools/gui_index.py` decrypts all 939 .gui files and indexes the 4,729
distinct `id_string`s.  Asked the right question, it answers exactly what
the driver needs:

    python tools\\gui_index.py --out .build\\gui_index.json
    python .build\\gui-ids.py <id> [...]
    python .build\\gui-ids.py --screens <name>

**THE EVENT MAP'S CARD IS `EventMapScreen_STREAM_GROUP_<GROUP>_card`**, one
per championship group, and every one of them carries the SAME pressable:

    MAP_EVENT_CARD_BTN              the pressable card, on EVERY group
    MAP_EVENT_CARD_STREAM           present only on the STREAMABLE groups
    MAP_EVENT_CARD_LOCKED           the event is locked
    MAP_EVENT_CARD_MANUFACTURER_FRAME
    MAP_EVENT_CARD_RACETEAM_FRAME

**AND THE STREAM FLAG IS THE DISCRIMINATOR.**  Measured from the index:

    stream=True   AMATEUR, ELITE, EXPERT, LEGEND, MASTER, PRO, PRO_AM,
                  career_group_card
    stream=False  NASCAR, ENDURANCE, ENDURANCE_PROTOTYPES,
                  ENDURANCE_GRAND_TOURING, EURO_TRACK_MASTERS, FORMULA_E,
                  SUPERCARS, SUPERTROFEO

**⇒ THE MANUFACTURER CUPS ARE THE ONES THAT WEDGE.**  They have no
streamable card, and they are exactly the groups whose assets are not on the
card: the guest's own log says the assets are fine ("Asset download skipped -
all files up to date!"), and the stuck screen's progress bar reads 0 %.

**⇒ SO THE TABLE IS SHORT AND IT IS EVIDENCE, NOT A GUESS:**

    on EventMapScreen, if MAP_EVENT_CARD_STREAM is present:
        press MAP_EVENT_CARD_BTN          -> a raceable event
    on EventMapScreen, if it is NOT present:
        the guest is on a manufacturer cup; go back and pick AMATEUR
    on Event_item, if EVENT_LOCKED is present:
        the event is locked; EVENT_LOCKED_BTN and
        EVENT_LOCKED_BTN_COST_CURRENCY are its cost, so do not press it

**⇒ AND THE ARCHIVES CONFIRM THE IDS.**  The screen name is in the probe's
own line, so an archive says which group the guest was on:

    v3 (raced)   ui-ctl-br screen=EventMapScreen ... MAP_EVENT_CARD_STREAM
                 ... MAP_EVENT_CARD_LOCKED ... ACTION_BTN
                 then screen=CarSelectScreen ... BTN_RIGHT_CAR
    v4 (wedged)  ui-ctl-br screen=EventMapScreen ... MAP_EVENT_CARD_MANUFACTURER_FRAME
                 ... MAP_EVENT_CARD_RACETEAM_FRAME ... ICON_GOLD
                 and NO MAP_EVENT_CARD_STREAM

**⇒ THE LESSON, WHICH IS THE SAME ONE THE WHOLE PROJECT KEEPS PAYING FOR: a
frame-based input script cannot express "press the button that means go",
and it cannot express "do not press anything on this screen".  It can only
press A a number of times, and the number of times that works is a
function of the frame rate and of how long the script runs.  The port
already has the guest's own vocabulary; the driver has to use it.**
"""

if "### 9.10 The control ids the front-end driver needs" in text:
    print("already present")
else:
    path.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("appended 9.10")
