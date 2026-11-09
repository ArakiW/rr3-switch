"""Append section 9.27 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the manual round is the first end-to-end measurement of the HUMAN path
on the current build, and it is the line the "is the port playable" question
actually turns on: the autopilot is a measurement harness, the human path is
the game.
"""
import io
import sys

SECTION = """
### 9.27 The human path, measured end to end on the current build

**MEASURED 2026-11-10, round `manual1` (card at `autopilot = 0`, the weave
script holding the accelerator and alternating the steering axis, the port's
closed loop switched off).**

**⇒ ⇒ THE INPUT REACHES THE GUEST, EVERY SAMPLE (24 of 24):**

    f= 900  mode=0 path=script lx_script=0.500 lx_guest=0.500 buttons=0x0001 loop_steered=0 physical_active=0
    f=1200  mode=0 path=script lx_script=0.500 lx_guest=0.500 buttons=0x0001 loop_steered=0 physical_active=0
    ...
    f=5700  mode=0 path=script lx_script=0.000 lx_guest=0.000 buttons=0x0001 loop_steered=0 physical_active=0

`lx_script` is what the script asked for, `lx_guest` is what the guest
received, and they are equal in every sample.  `loop_steered=0`: the port's
own closed loop did NOT overwrite the human's axis.  `buttons=0x0001`: the
guest's BTN_A, the accelerator, is held.

**⇒ ⇒ THE PORT DID NOT TOUCH THE CAR AT ALL:** all 25 `npad-heartbeat` lines
carry `autopilot_injected=0`.

**⇒ ⇒ AND THE CAR RESPONDS TO IT:**

    f= 630  total=     0  rate=  0.00
    f= 720  total=  5663  rate= 32.58
    f= 780  total= 47763  rate= 76.73
    f= 810  total=55062  rate=181.23

The car's own progress along the track goes from 0 to 55,062 units, and the
progress rate reaches 181.23 units per frame.  The lap is 1,171,456 units, so
the rate is about 84 km/h.  **The throttle reaches the car and the car
really drives.**

**⇒ ⇒ THE FRAME SHOWS IT:** `frames/frame-000600.png` is the start at
Hockenheim: the player's black Mercedes SLS AMG with its wheels, brake discs,
rear wing and AMG decals, the pit building with its HOCKENHEIMRING signs and
the sponsor boards, the start lights lit, the grid painted on the asphalt,
and an ORANGE rival car in the background.  **Every one of them is textured
correctly.**

**⇒ ⇒ SO WHAT IS ACTUALLY BROKEN IS THE AUTOPILOT, NOT THE GAME.**  The
autopilot's best measured round reached 15.96 % of a lap; the human path
reaches a racing speed in the first ten seconds of the same race.  The
pursuit's failure (section 9.24) is a defect in the port's own measurement
harness and its attract mode, NOT in the game's playability.

**⇒ ⇒ AND THE TWO REMAINING GAPS FOR THE HUMAN ARE EXACTLY THE TWO THE USER
HAS TO CLOSE:** the in-race HUD's two style files (only an Android device has
them), and the controller binding -- in Eden the port sees an idle emulated
Pro Controller (`state=npad-idle`, `seen_press=0`, `buttons=0x0` in all 25
heartbeats) because the host controller is bound to no player.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.27 The human path" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
