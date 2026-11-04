<div align="center">

# Real Racing 3 — Nintendo Switch Native Port

**Reverse-engineering the Android ARM64 build of Real Racing 3 14.0.1 into a
native Horizon OS `.nro`.**

English | [简体中文](README.md)

![Status](https://img.shields.io/badge/status-In%20Development-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-MIT-blue)
![Compliance](https://img.shields.io/badge/EA%20assets-not%20redistributed-brightgreen)

</div>

---

## Compliance

**This repository ships the port's SOURCE only.** The game itself — its code,
its assets and its data — comes from **your own copy of the application**. The
port circumvents no protection, licence check or DRM. See
[NOTICE.md](NOTICE.md).

---

## Where it stands (2026-11-05)

**It boots, it reaches the front end, it gets into a race, and a controller
really drives the car.**

| Capability | State | Evidence |
|---|---|---|
| libnx NRO bootstrap + 4-DSO dynamic loader (TLS / relocations / RELRO) | ✅ | `multi_dso_linker.cpp` |
| JNI bridge (233-slot JNIEnv emulation) + 181 constructors | ✅ | `jni_bridge.cpp` |
| Guest View lifecycle (created / changed / render) + eGL present | ✅ | `guest_view_probe.cpp` |
| Filesystem path remapping (7 ROMFS data volumes) | ✅ | `guest_filesystem.cpp` |
| **Front end → car select → into a race** | ✅ | the port presses the race button from the guest's own control tree (`ui-ctl`) |
| **A human driving** | ✅ | MEASURED 2026-11-04: the axis asked for is the axis the guest got, the closed loop did not overwrite it, speed 1750 → 6464 |
| Controllers (Pro / Joy-Con native; DS4 / DualSense / Xbox need hidretro or dinput_redux) | ✅ | the 22 bindings in `rr3-gamepad.txt` |
| Voice / music / sound (FMOD) | ✅ | `fmod_shim.cpp` |
| **A full lap, and the lap counter** | ⚠️ | **fine when a human drives**; the port's own autopilot cannot lap yet (below) |

### The port's own autopilot (an optional attract mode)

`rr3-gamepad.txt` carries `autopilot = 0` (**you drive**, the default) or
`1` (the port drives). The autopilot follows the track's **centre line**,
while the track's own racing line (`ai_spline`) is 67 m shorter a lap and
2.92–8.25 m away from it ⇒ the car runs on the outside of every corner, hits
a barrier about ten times a race, and gets as far as **10.35 % of a lap**.
**That is a quality problem in the autopilot, not a porting defect** — when
you drive, the counter works.

### What this round changed (every item has a measured number)

* **The pursuit direction is no longer re-decided every frame.** It used to be
  recomputed from the instantaneous heading, so a single knock moved the
  look-ahead point to the other side of the car, the controller went to full
  lock the wrong way, the car drove backwards, and the heading flipped again —
  a closed loop. The direction is now **locked for the race**, decided once
  from the car's displacement over a 48-sample window. MEASURED: the samples
  with the target behind the car **45 % → 13 %**, lap progress
  **5.08 % → 8.65 %**.
* **A car pinned against a barrier can get free.** The stall test's "on the
  line, so not stuck" exemption let a genuinely stuck car through (its lateral
  residual was 2.07 m, inside the 3 m exemption). The test now uses the car's
  **world position**. MEASURED: race frames **4770 → 5760**, lap progress
  **8.65 % → 10.35 %**.
* **The player can actually take over.** The `autopilot = 0` switch only
  governed the *physical controller* path, and the package shipped an
  experiment pad script that — while active — **ignored the physical
  controller entirely**, so a console player's stick was dead. The mode now
  governs both paths, the script stands down the moment a controller is
  touched, and the scripted path has its own instrument (`scripted-state`).
* **The black screen has an account.** Every frame now logs `tt/tr/ts/tp`:
  the whole frame, the guest's render, the port's own work, and the present.
  MEASURED on Eden: the **port's own share is 0.1 % (menu) / 1.0 % (race)**;
  the rest is the guest's own ARM64 code being **emulated** ⇒ **the
  emulator's frame rate says nothing about the console**, and the console's
  black screen is far more likely to be the boot phase.
* Diagnostic log volume cut by **20 %** (both dedup tables were far too
  small); the guest-thread census is measurably **not** the frame-rate lever
  but costs 9 % of the CPU.

---

## Build

```powershell
# Build the NRO
powershell -File tools\Build-NativeSwitch.ps1

# The gate: build + input check + 39 contract tests + symbol resolution
powershell -File .build\gate.ps1
```

Gate status: **39/39 tests pass**; Cloudcell symbols **38 resolved / 16
confirmed absent / 0 unexpected misses**.

Output: `native-switch/rr3-native-rehost.nro` (with the embedded RomFS).
**The repository carries no EA material** — the guest binary, text, fonts and
FMOD come from your own copy of the application and are staged into the RomFS
at build time, so the packages on the Release carry them.

## Install on the console

The full procedure is in
**[tools/README-hardware-test.md](tools/README-hardware-test.md)** (Chinese:
volumes, app-only update, a small-card variant, and exactly which logs to
send back).

In short:

1. Unpack `dist/rr3-switch-app-*.zip` over the card root (45 MB: the
   application and the control scripts);
2. the game's own files come from your copy of the application — the loader
   finds them next to the NRO in `sdmc:/switch/rr3-native-rehost/`;
3. launch; **press A once** to dismiss the guest's in-race tutorial dialog;
4. right trigger / A is the throttle, left stick steers.

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
| [PORT_PROGRESS.md](PORT_PROGRESS.md) | the round-by-round timeline |
| [docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md) | the feature matrix |
| [tools/README-hardware-test.md](tools/README-hardware-test.md) | the device install and verification guide (Chinese) |
| [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) | method and results of the reverse engineering |
| [NOTICE.md](NOTICE.md) | third-party code and licences, and the **compliance boundary** |

## Licence

The port is released under the **MIT Licence** (see [LICENSE](LICENSE)); the
third-party components and their terms are listed in [NOTICE.md](NOTICE.md).
The game itself is © Electronic Arts; the repository contains none of its
content, and the guest runtime and assets in the published packages come from
the user's own copy of the application.

## Acknowledgements

- [devkitPro](https://devkitpro.org/) — the Switch toolchain
- [libnx](https://github.com/switchbrew/libnx) — the Switch homebrew library
- [Mesa](https://mesa3d.org/) — the GL implementation
- everyone who tested it and sent logs back
