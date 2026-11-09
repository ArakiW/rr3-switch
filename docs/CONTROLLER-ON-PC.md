# Using a non-Npad controller on the PC (Eden)

Measured on this machine on 2026-11-09. Every claim below has a measurement
behind it; the numbers are in the "WHY" lines of the code and in the archives.

## The short version

On the PC the port can never see your Xbox Elite Controller directly, and that
is NOT a port bug. The emulator owns the controller, and it only hands the
guest a Nintendo Pro Controller (Npad). The guest -- an Android game -- cannot
read anything else. So the work is: **make Eden forward the pad as a Pro
Controller**, and the port's existing Npad path does the rest.

## The three measurements that pin it down

1. **The pad is alive on the PC.** `.build/probe-xinput.ps1`:
   `XINPUT-SLOT-SCAN  slot0 CONNECTED` -- Windows sees the Xbox Elite
   Controller through XInput, so it is plugged in, powered and paired.

2. **Eden gives the guest nothing but a keyboard.** Eden's global config is
   `E:\Eden-rr3\user\config\qt-config.ini`. Every binding in its `[Controls]`
   section is a keyboard binding:
   `player_0_button_a="engine:keyboard,code:67"`, `player_0_button_b=...`, and
   so on for `player_0` .. `player_9`, plus `player_0_lstick`,
   `player_0_rstick` and `debug_pad_*`. **No host gamepad is bound to any
   player.** The same file also carries `enable_all_controllers=false`.

3. **The guest cannot see the host's HID stack either.** The port's own
   device inventory (`logRawHidInventory()` in `native-switch/source/raw_hid.cpp`)
   printed no raw HID device at all inside Eden. Eden emulates the HID layer;
   it does not pass the host's USB devices through. The raw-HID path added in
   the previous round is therefore a **real-Switch** feature (a non-Npad pad
   plugged into the dock/USB), and it cannot help on the PC.

## What to do in Eden

1. Open Eden.
2. `Settings` -> `Controllers` -> `Configure Current Game` (the binary carries
   the string `Configure Current Game`, `Pro Controller` and `Joy-Con`).
3. For **Player 1**, set the device to your **Xbox Elite Controller** and
   assign the buttons. Keep `Pro Controller` as the guest-facing type -- that
   is the only thing the guest can read.
4. Save, start the game, and drive.

The port does not need any change for this: `rr3-gamepad.txt` is the single
place that says which guest control means which key, and the Npad read path is
already proven by the port's own autopilot, which drives the car through it.

## How we will know it worked

One log line, so that "Eden never forwarded it" and "the port read it and did
nothing" stop looking the same:

- `npad-none`  -- no Npad slot is connected at all: Eden forwarded nothing.
- `npad-idle`  -- a slot is connected but the guest's input never changes:
  Eden is emulating a Pro Controller but no pad is bound to it.
- `npad-active` -- the guest's input changes: the pad is reaching the guest.

The heartbeat is emitted every 5 seconds and on every change of the connected
slot set or of the button/analog state, so a press is visible even if you only
hold it for a moment.

To check it after a run:

```
Select-String -LiteralPath <card>\rr3-native-rehost.guest-view.log -Pattern 'npad-(none|idle|active)'
```

## On the real Switch

The raw-HID path (`native-switch/source/raw_hid.cpp`) translates a non-Npad pad
into the same `rr3::gamepad::NpadState` shape the rest of the port uses, so
the mapping table stays the only place that decides what a button means. It
only takes over while **no Npad slot is active**, so a real Switch with a
Joy-Con or a Pro Controller keeps its exact current behaviour, and the
15-frame device rescan means a pad plugged in mid-run is still picked up
quickly.
