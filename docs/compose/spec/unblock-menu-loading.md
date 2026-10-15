---
feature: unblock-menu-loading
status: in-progress
updated: 2026-10-16
branch: master
commits: 33fdbcfc, 065402e5, 568133f5
---

# Unblock Menu Loading

## Report

**Device round 1 (NRO 0DBBD6C2)**: Splash→menu transition WORKS. `hideSplash` called at frame ~4. Guest issues `draw-arrays` (count=2). `frameState=0`, `splashExit=1`, `force-splash-flags=0x01010101` all confirmed in log. After hideSplash guest enters cloudcell init (CC_SeedData, getComponent, Constructor, init, post). No `MainMenu.gui`/`sprites_*` opens yet — menu load appears gated on cloudcell completion.

**Mesa round 2026-10-11 to 2026-10-12**: the guest now renders. A real GPU backend (`backend=mesa`), the console framebuffer disabled (`nofb=1`), and a host-driven frame loop (the guest has no Choreographer — Looper/Handler/MessageQueue/SurfaceHolder/GLSurfaceView/onDrawFrame/postDelayed all score zero hits across the guest) put **46,630 frames** on screen and the first pixels on the GPU. All seven render stages verify correct:

```
tex=2 texWH=16x16 mode=0x5(GL_TRIANGLE_STRIP) count=4
vp=0,0,1280,720  sc=0,0,1280,720  fbo=0  glerr=0x0   frameState=0
```

The guest binds a 16×16 solid-colour texture and stretches it full-screen — the loading background, RGB(13,31,89), read back over the full 921,600 pixels. The host side has nothing left to check.

**Emulator round 2026-10-16 — the wedge is located.** See [S4](#s4-the-wedge-is-a-host-side-circular-wait-not-a-guest-mystery).

### 2026-10-16: four claims this project had been carrying are wrong

They were written into the source as settled facts. Each was re-derived from the binary and the archived logs, and each failed:

| Recorded claim | Measured reality |
|---|---|
| "the frame loop never runs" | `loop-top` fires once (it only logs for `frames < 3`). `guest-activity=0` only proves `frames < 30`, because it is emitted at `frames % 30 == 0` |
| "one byte, two consumers, opposite polarity" | `0xab2c94` also `cbz`s and also needs **non-zero**. Four sampled consumers, all non-zero. There is no opposite polarity |
| "the guest never writes this byte" | `0x1cbe218: strb w8,[x19,#0x4c1]` preceded by `mov w8,#1`, inside the guest's own Cloudcell constructor. The write that was cited as proof — `0x10f0990: strb wzr,[sp,#0x4c1]` — is a **frame zero-init**, and *undercuts* the claim it was cited for |
| "a single byte test gates the menu" | `0x12aefc0–0x12af010` is a four-way AND with **two host stub calls** (`vtable[6]`, `vtable[17]`) executed between the byte test and the `blr` |

### 2026-10-16: two measurements that settled the wedge

Both are two-run reproductions with identical numbers.

```
archive                  cc-slot   path-req   frames
20261012-025837-tex1         0       120     23280   gate closed (baseline)
20261012-041629-menu4a      47       120         1   gate open
20261012-042143-menu4b      47       120         1   gate open
```

1. **Every one of the 47 slot calls has a caller in `0xCD1xxx–0xCD3xxx`** — the guest's Cloudcell init. **None comes from `0x12aefc0`.** The guest never reaches the menu gate at all.
2. **`path-req` is identical (120 vs 120, zero difference).** The menu loader asks for no new file. "The menu load is missing a resource" is falsified.

## [S1] Problem

Guest opens loading-screen assets (LoadingScreen_Init.gui, GlobalStyles.gui, SpriteAnimation.gui) but never MainMenu.gui or sprite atlases. `drawElements` stays 0. The splash→menu transition never fires, leaving the device showing a host-drawn magenta/black test quad.

**Update (round 1)**: Splash→menu transition now fires. `hideSplash` is called. Guest draws via `draw-arrays`. Remaining gap: main menu assets (MainMenu.gui, sprites_*) not opened — cloudcell init appears to gate menu load.

## [S2] Design

Force the guest FSM past the splash state by writing known gate bytes after `onViewChangedJNI` and re-asserting them every frame:

| Offset | Value | Role |
|--------|-------|------|
| `activity+0x4d8` (frameState) | 0 | Skip warmup(1/2/3)/GL-reset(4)/splash(5); enter main-render path at `0x12aec2c` |
| `activity+0x4d0` (paused) | 0 | If paused≠0 and flag+0x4d2=0, render returns early |
| `renderer+0x1f5a9` | 1 | Splash→menu trigger; main-render path at `0x12aed14` skips menu layout when 0 |
| `renderer+0x1f5ab` | 1 | Secondary gate checked when splash-exit=1 |
| `renderer+0x1f5ac` | 1 | Secondary gate checked when splash-exit=1 |
| `renderer+0x120` | 1 | Secondary gate checked when splash-exit=1 |
| `engine+0xa` | 1 | Render early-out gate |
| `container+0xa` | 1 | Menu draw gate |
| `container+0x20` | 1 | Ready gate |

`renderer` = `*container` (containerQ0). Only write when pointer > 4 GiB (heap). Guest clears `renderer+0x1f5a9` after processing (`0x12aeddc`), so re-assert every frame.

Removed: `state+0x38c` force and per-frame `FUN_014bcbac` call — that function only drains an empty event queue.

## [S3] Out of Scope

- Audio, race gameplay, online services
- Decrypting `.gui`/`.vsh` files
- Modifying the seven data volumes

## [S4] The wedge is a host-side circular wait, not a guest mystery

Opening the Cloudcell availability byte (`menugate=1`) used to be recorded as "the guest hangs at frame 0, hang point unknown". It is neither unknown nor the guest's.

**The chain, every link measured:**

1. With `menugate=1` the guard at `0xab2c30` passes, so the guest issues the async request and hands a `std::function` continuation to slot 2 of the endpoint object. Measured: slot 2 fires twice, callers `0x1415EA0` and `0xCD1CFC`.
2. `cloudcellEndpointStubRequest` copies that continuation into `g_ccPending`, sets `g_ccRequestSeen`, and returns 0 **without ever invoking it**.
3. The only code that ever invokes it, `cloudcellPumpCompletion()`, has exactly one call site — inside `frameLoopDriver`, *after* the `renderTarget(...)` call.
4. The guest blocks **inside** that `renderTarget(...)` in pure guest code (`inbridge=0` — not parked in any host shim), so frame 1 is never reached and the completion is structurally unreachable.

> The thread that must deliver the answer is the thread waiting for the answer. The host's own call ordering produced the circular wait.

**Design (`ccmode`)**: move the delivery point *up* to the capture site, which is provably a running context.

| Value | Delivery site | Character |
|---|---|---|
| `off` (default, absent) | frame loop only | unchanged; the verified-good config keeps its exact behaviour |
| `now` | inline at the capture | removes the circularity; **re-entrant by construction** — diagnostic arm |
| `bg` | `cloudcell_defer()` → drained by the existing `alert-pump` tick | preserves "never run guest code inline inside a Cloudcell lock" |

The record stays **all-zero**. No authentication, no response body and no success bit is invented: the guest receives a denial it can decode, clears `pending`, and takes its own offline branch. Forging a success is the one thing this port must not do, and the all-zero record is what keeps it honest.

`g_ccPending` is now guarded by a libnx `Mutex`, not `std::mutex` — a `std::mutex` on this guest-visible path measured 13.7 s → 80.7 s of host CPU by dragging the guest's pthread shim into a host lock. Discipline is take → copy → drop, and the lock is **never held across the `blr`** into guest code.

**`bg` is currently unusable**: the alert-pump thread that would drain it is created lazily by `startAlertPump()` and lives inside an anonymous namespace in `jni_bridge.cpp`. If the wedge precedes the first alert answer, `bg` enqueues and is never drained. One line in `jni_bridge.cpp` fixes it; it has not been added.

### A separate landmine, found while running the A/B

`readConf()` read the file once into `char buffer[256]`. The live `rr3-gfx.conf` is 2.7 KB — almost all of it the documentation block recording why Mesa and `nofb` are load-bearing. **Every setting past byte 256 was discarded without a warning.**

```
parsed:        backend=mesa   nofb=1
never parsed:  ccmode=now     menugate=1      ← the variable under test
```

Two 5-minute emulator runs that were meant to differ came back with `frames=16080` and `15960`. They were the same configuration. A switch that silently does nothing is indistinguishable from a switch that does nothing useful, and nothing logged. Fixed in `568133f5`: buffer 4096, explicit `conf_trunc` alarm, and the `gfx-backend` summary line now echoes `ccmode` / `menugate` / `ccdone` / `conf_trunc` / `conf_bytes` — so the configuration of a run is readable from one line without going to the SD card.

### Acceptance, fixed before running

`ccmode=now` has **not yet been measured**: both attempts so far were invalidated by the buffer truncation above.

| Observation | Conclusion |
|---|---|
| `cc-deliver` **and** `cc-complete-returned` present, frames climbing | delivery works and returns; fix holds |
| `cc-deliver` present, `cc-complete-returned` absent | wedged *inside* the continuation ⇒ `now` re-entry broke; move to `bg` |
| no `cc-deliver` | the continuation was never captured; the capture-site reading is wrong |
| crash file not `armed` | not a hang — a fault, and `pc`/`lr` name the instruction outright |

Two runs of the same configuration are required before any of these is believed. `now` is the only arm currently testable.

## Tasks

- [x] T1: Force frameState=0 and paused=0 after Changed — acceptance: log shows `force-frameState-0` (covers: S2)
- [x] T2: Force renderer+0x1f5a9/0x1f5ab/0x1f5ac/0x120 — acceptance: log shows `force-splash-flags` (covers: S2)
- [x] T3: Re-assert all gates every frame — acceptance: log shows `reassert-*` lines for frames 0-4 (covers: S2)
- [x] T4: Remove dead state+0x38c / FUN_014bcbac path — acceptance: build clean, 11/11 tests (covers: S2)
- [x] T5: Device validation round 1 — acceptance: `hideSplash` called, `draw-arrays > 0` (covers: S2; depends: T1-T4)
- [x] T7: Switch host-quad fallback to drawArrays-aware — acceptance: host quad stops when guest draws (covers: S2; depends: T5)
- [x] T8: Locate the `menugate=1` wedge — acceptance: every call site of the 47 host stub calls has a classified caller, and the delivery site is proven unreachable (covers: S4)
- [x] T9: Make the captured continuation reachable (`ccmode`) — acceptance: build clean, 11/11, frozen hash intact (covers: S4)
- [x] T10: Make `readConf` self-reporting — acceptance: `gfx-backend` line carries `conf_trunc`/`conf_bytes` and the live conf's settings appear in it (covers: S4)
- [ ] T6: Unblock cloudcell→menu gate — acceptance: guest opens `MainMenu.gui` or `sprites_*` (covers: S2, S4; depends: T8-T10, and on T11)
- [ ] T11: Measure `ccmode=now`, twice — acceptance: two archives with identical discriminators (covers: S4; depends: T9, T10)
- [ ] T12: Make `ccmode=bg` usable — acceptance: `startAlertPump()` starts at init, gated on `ccmode=bg` (covers: S4; depends: T9)
- [ ] T13: Fix 70 `vehicles/shaders/*.pvr.z` `Z_DATA_ERROR` — acceptance: `Z_DATA_ERROR` count is 0 (independent of S4; identical in both arms, so it is not the wedge)

## [S5] Still open

- Guest PC sampling is structurally unavailable in the emulator: `svcGetThreadContext3` returns the *host* NRO program counter, so every `census-pc` line reads `pc_img=host-nro`. The census import-ring is the only working substitute, and an import ring cannot distinguish "spinning on a flag" from "walking a corrupt structure" — both are pure guest code with zero imports.
- `0x12aea7c` (the per-frame function) has **zero `bl` callers, zero `adrp` references, and appears as no constant in either the guest or the host ELF**. It is reached through a runtime-computed pointer, so the ordered boot chain to it is not recoverable statically. Do not spend time on a static caller search for it.
- `0x12E1600` (network/event thread) computes its `poll` timeout from a `fmov d9,#-1.0` sentinel, giving −1,000,000 ms, i.e. an infinite wait. That is by design, but it means the cloud state machine can never advance offline.

