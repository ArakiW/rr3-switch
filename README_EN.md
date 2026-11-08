<div align="center">

# Real Racing 3 — Nintendo Switch Native Port

Reverse-engineering the Android build of Real Racing 3 into a native Horizon
OS program.

</div>

> This repository ships the port's source only. The game itself — its code and
> its data — comes from **your own copy of the application**. See
> [NOTICE.md](NOTICE.md).

English | [简体中文](README.md)

![Status](https://img.shields.io/badge/status-In%20Development-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-MIT-blue)
![Repo](https://img.shields.io/badge/repo-source%20only-brightgreen)

---

## What works right now

**It boots, it reaches the front end, you can pick a car and get into a race,
and a controller really drives the car.**

| | |
|---|---|
| Boots to the game's front end | ✅ |
| Car select → into a race | ✅ |
| Driving with a controller | ✅ |
| Music and sound | ✅ |
| A full lap driven by you, with a working lap counter | ✅ |
| The port's own attract-mode autopilot completing a lap | ⚠️ not yet |
| Occasional black screen on the console | ⚠️ cause found, fix in the next build |

The full status of every item is in
[docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md).

## Installing it

1. Download the latest zip (**about 6.5 MB**) from
   [Releases](https://github.com/ArakiW/rr3-switch/releases/latest)
2. Unpack it and copy the contents of `switch\` over the root of your card
3. **The package carries no game content.** Take the guest runtime, the UI
   faces and the `.depot` text out of your own copy of the application and put
   them next to the NRO, in `sdmc:/switch/rr3-native-rehost/` -- that is where
   the port looks for them.
4. Launch it from Homebrew Launcher
5. Once you are in a race, **press A once** to dismiss the guest's tutorial dialog
6. Right trigger / A is the throttle, left stick steers

The full procedure — which files to place, the split volumes, the small-card
variant, and which logs to send back — is in
**[tools/README-hardware-test.md](tools/README-hardware-test.md)**.

By default **you drive**. To watch the port drive instead, change
`autopilot = 0` to `1` in `rr3-gamepad.txt`.

## Building

```powershell
powershell -File tools\Build-NativeSwitch.ps1              # build (full)
powershell -File tools\Build-NativeSwitch.ps1 -CleanLoader  # build (clean: no EA material in the RomFS)
powershell -File .build\gate.ps1                           # build + self-checks
```

Needs devkitPro's devkitA64 and libnx.

**The clean build** (`-CleanLoader`) is the one to publish: its RomFS holds
only what this project owns (libc++, the OFL Chinese bootstrap face and that
face's licence), and the game's own files go on the card, put there by the
user. The default build stages them into the RomFS instead, so nothing from
the game has to live in this repository either way.

## What was fixed recently

- **The published package can carry no EA material at all**: 6.5 MiB, and the
  NRO is measured to contain no EA file's bytes.
- **The car belongs to the player now.** The switch used to govern only the
  controller path, so a console player's stick was dead.
- **The pursuit direction is no longer re-decided every frame**, so the car no
  longer drives itself backwards.
- **A car pinned against a barrier can get free**, instead of staying stuck.
- **The console freeze is now explained.** The guest was waiting on a condition
  variable whose signal was dropped -- a host condition variable has no memory
  of its own. The wait now keeps its own ledger, never waits forever, and the
  simulator measurement went from **532 frames (stuck) to 3762 frames
  (rendering)**.

<details>
<summary>The measured numbers from this round</summary>

- The player's input reaches the car end to end: across 24 consecutive samples
  the axis the script asked for is the axis the guest got, and the closed loop
  did not overwrite it.
- After locking the direction: the samples with the target behind the car went
  45 % → 13 %, lap progress 5.08 % → 8.65 %.
- After the unstuck fix: race frames 4770 → 5760, lap progress 8.65 % → 10.35 %.
- The freeze fix, measured on the same simulator, the same card and with the
  census switched off: rendered frames **532 → 3762**, screenshots **5 → 84**,
  diagnostic log **186 MB → 24 MB**.
- On Eden the port's own share of a frame is 0.1 % (menu) / 1.0 % (race); the
  rest is the guest's ARM64 code being **emulated** ⇒ **the emulator's frame
  rate says nothing about the console**.
- Diagnostic log volume cut by 20 %.

The derivation and the raw data for each of these are in
[docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md).

</details>

## Layout

```
native-switch/          the native port (libnx + Mesa)
  source/               C++ sources
  include/              headers
  tests/                contract tests (host self-tests + Python)
  card/                 the control scripts that ship on the card
tools/                  build and deployment scripts, the device guide
docs/                  documentation (see below)
reverse/                reverse-engineering material (not a release artefact)
dist/                   packaged deliverables
```

## Documentation

| Document | What it holds |
|---|---|
| [docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md) | **the main handoff**: every located issue, the measured numbers, what is still open |
| [docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md) | the feature matrix |
| [tools/README-hardware-test.md](tools/README-hardware-test.md) | the device install and verification guide (Chinese) |
| [PORT_PROGRESS.md](PORT_PROGRESS.md) | the round-by-round timeline |
| [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) | method and results of the reverse engineering |
| [NOTICE.md](NOTICE.md) | third-party code and licences, and the compliance boundary |

## Licence

The port is released under the **MIT Licence** (see [LICENSE](LICENSE)); the
third-party components and their terms are listed in [NOTICE.md](NOTICE.md).
The game itself is © Electronic Arts; the repository contains none of its
content and **neither does the published package** -- the game's files go on
the card, taken by the user from their own copy of the application.

## Acknowledgements

- [devkitPro](https://devkitpro.org/) — the Switch toolchain
- [libnx](https://github.com/switchbrew/libnx) — the Switch homebrew library
- [Mesa](https://mesa3.org/) — the GL implementation
- everyone who tested it and sent logs back
