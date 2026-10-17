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
| `now` | inline at the capture | removes the circularity; **re-entrant by construction** — and, see below, **the only arm that can work** |
| `bg` | `cloudcell_defer()` → drained by the existing `alert-pump` tick | **not viable while the game thread is wedged** |

The record stays **all-zero**. No authentication, no response body and no success bit is invented: the guest receives a denial it can decode, clears `pending`, and takes its own offline branch. Forging a success is the one thing this port must not do, and the all-zero record is what keeps it honest.

`g_ccPending` is guarded by a libnx `Mutex`, not `std::mutex` — a `std::mutex` on this guest-visible path measured 13.7 s → 80.7 s of host CPU by dragging the guest's pthread shim into a host lock. Discipline is take → copy → drop, and the lock is **never held across the `blr`** into guest code.

### `bg` cannot work, and it is not a one-line fix

The design premise was that the alert-pump thread — a raw `threadCreate` host thread that calls `cloudcell_pump("alert-pump", false)` every 80 ms — would take the deferred delivery while the game thread is stuck. It does not, and the reason was already measured on 2026-10-04 and written down at `jni_bridge.cpp`:

> MEASURED: the alert-pump thread reaches its wait and then stops before `reached-drain` — `cloudcell_pump`'s first act is the lock on `g_queue_lock`, which **the spinning guest main thread is holding**.

So the drain blocks on the very thread we are trying to rescue. Starting the pump earlier does not help; it would just block earlier. `bg` needs a delivery path that never touches `g_queue_lock` at all — for example a dedicated thread polling `g_ccPending` under its own lock. That is a design, not a patch, and it is not built, because it is only worth building if `now` fails first.

**`now` is therefore the only arm currently testable**, and it is not merely the "diagnostic" one.

### A wrong comment that would have caused a future break

`cloudcell_defer.cpp` carried this:

> Mutex IS a bare u32 handle in this libnx. Every `Mutex x{}` in this project is therefore the handle 0, and nothing in this project ever calls mutexCreate.

Both the premise and the conclusion were wrong. `libnx/kernel/mutex.h` says `typedef _LOCK_T Mutex;` and documents "A mutex can also be statically initialized by assigning 0 to it" — `_LOCK_T` is newlib's low-level lock holding the **owning thread's id**, so `0` means *unowned*, i.e. the free state. The decisive evidence was already in the tree: writing `.handle` does not compile and casting one to `unsigned` always has, and C++ will not cast a struct to an integral type. The runtime evidence agrees — the alert-pump thread measurably *blocks* on these locks, which a lock that does not exclude could not do.

All 15 `Mutex` declarations in the project are fine and must **not** be given a `mutexCreate`. The comment is corrected in `568133f5`'s successor so nobody "fixes" a working lock.

### A separate landmine, found while running the A/B

`readConf()` read the file once into `char buffer[256]`. The live `rr3-gfx.conf` is 2.7 KB — almost all of it the documentation block recording why Mesa and `nofb` are load-bearing. **Every setting past byte 256 was discarded without a warning.**

```
parsed:        backend=mesa   nofb=1
never parsed:  ccmode=now     menugate=1      ← the variable under test
```

Two 5-minute emulator runs that were meant to differ came back with `frames=16080` and `15960`. They were the same configuration. A switch that silently does nothing is indistinguishable from a switch that does nothing useful, and nothing logged. Fixed in `568133f5`: buffer 4096, explicit `conf_trunc` alarm, and the `gfx-backend` summary line now echoes `ccmode` / `menugate` / `ccdone` / `conf_trunc` / `conf_bytes` — so the configuration of a run is readable from one line without going to the SD card.

### The delivery fix was inert because the ABI was wrong — both now fixed

An adversarial review of `065402e5`, run before the first measurement, found that the fix had nothing to deliver. Every archived run that reaches the capture site logs:

```
cc-endpoint-request: captured continuation
    vtable=0x0 invoker=0x0 manager=0x0 payload=0x0 blob=0x0 w2=10795
cc-endpoint-request: fn was NULL
```

All five fields zero, in all five runs. `takePendingContinuation` fails its `vtable != nullptr` guard, `cloudcellPumpCompletion` returns before writing a line, and a run of `ccmode=now` would have produced one dead `cc-endpoint-request`, zero `cc-deliver`, and the identical wedge.

**The stub was reading the wrong register.** Recovered from the binary and verified byte-for-byte with capstone against a control (`0x12aefd8` reads back `ldrb w8,[x0,#0x4c1]` from bytes `08045339`):

```
0xcf1050  ldp  w8, w3, [x0, #8]   ; w2 = [obj+8], w3 = [obj+0xc], BOTH 32-bit
0xcf105c  str  x21, [sp, #0x20]   ; x21 = sp, saved across the call
0xcf1068  mov  x4, sp             ; <-- the continuation is in x4
0xcf107c  blr  x9
```

x3 cannot hold a pointer at **any** of the 25 call sites of `0xcf1018`: `ldp w8, w3` encodes bits[31:30]=00, so w3 is a 32-bit scalar. The stub treated it as the continuation pointer. That is why the capture was all zeros at one site and `NULL` at another.

The numbers confirm the reading rather than merely permitting it: call site `0xab2d1c` builds its object with `stur q0,[x0,#8]` from `.rodata:0x388280` = `2b2a0000 a01d0000 …`, so `w2 = 0x2a2b = 10795` — the logged `w2` exactly — and `w3 = 0x1da0`.

**Delivery stays on vtable slot 6**, which is correct and was not part of the bug. After the call the guest does `ldr x0,[sp,#0x20] ; cmp x21,x0 ; b.eq #0xcf10a0` — a **frame-integrity dispatch**, not a self-comparison: `[sp+0x20]` held `sp` before the call, so equality means the callee left it alone and the guest takes `vptr[4]` (`0xcf11d0`, a bare `ret`, because the response has not arrived), while inequality means it was clobbered and the guest takes `vptr[5]` (`0xcf11d4` = `operator delete`). Neither delivers anything. `vptr[6]` is `0xcf11d8`, a real prologue, and the only entry with code. With the ABI fixed the stored `payload` is now the real object pointer, which is what that function needs to derive its callee.

### Why the outcome table below was ambiguous, and what replaced it

The row "no `cc-deliver` → nothing was captured" was wrong. Two completely different worlds produce byte-identical logs:

- the capture site is **never reached** — the 2026-10-16 runs, `cc-slot=0`, 15,960 clean frames
- the capture site is reached and the continuation is **degenerate** — `menu4b`, `vtable=0x0`

An analyst following the old table would start auditing vtable wiring instead of the argument ABI. The discriminator is `cc-endpoint-request` itself, and its `vtable=` field:

| `cc-endpoint-request` | Meaning |
|---|---|
| absent | capture site never reached — nothing to fix here yet |
| present with `vtable=0x0` | reached, but the argument mapping is wrong — **fix the ABI** |
| present with a real `vtable` | the fix can work; fall back to the `cc-deliver` / `cc-complete-returned` pair |

Only the last row makes the delivery pair meaningful. That is what T15 is for.

### ccdeliver3 (2026-10-17): both fixes confirmed, and a real stack

First run with a working config parser and the corrected ABI. The run states its own configuration:

```
gfx-backend ... ccmode=now menugate=1 ccdone=0 conf_trunc=0 conf_bytes=2542
```

**The ABI fix is confirmed by the numbers the guest produced**, not just by our reasoning:

```
cc-endpoint-request: captured vtable=0x21072a8448 invoker=0x210613e0d4
    manager=0x0 payload=0x210f970060 blob=0x1082cfbdb0 w2=10795 w3=7584
```

`w3=7584` is `0x1da0` and `w2=10795` is `0x2a2b` — exactly the two values the binary predicted from `.rodata:0x388280`. A second capture at a different call site shows `w2=10765 w3=0 manager=0x20`.

| metric | before | after |
|---|---|---|
| `cc-deliver` | 0 (inert) | **2** |
| `cc-complete-returned` | 0 | **2** |
| `Z_DATA_ERROR` | 70 | **0** |
| `frames` | 1 | **1** |

So the continuation is delivered, it returns, and the shader decompression failures are gone — **and the guest still wedges.** The wedge is therefore *not* the pending/response wait. Two blockers removed, blocker remains.

### The stack at the wedge

The host's frame-pointer walk captured it. Innermost first:

```
0x009f28e0 → 0x00cd33a4 → 0x0138de68 → 0x013c3bf4 → 0x014046e8
          → 0x01404998 → 0x01405898 → 0x012aeebc → host
```

`0x12aeebc` is inside the per-frame function `0x12aea7c`; `0xcd33a4` is in the Cloudcell region, and matches the caller addresses seen in `cc-slot` lines (`0xcd32b0`, `0xcd32c8`, `0xcd32d8`). Timeline in the same run: `view-render-ret` → `create-cloudcell` → `render-call-pre` → `cc-endpoint-request` ×2 → `cc-deliver` ×2 → silence.

**Why the import ring could never have found this.** The main thread is `inbridge=0` with `delta=0` — pure guest code making no import calls at all. Classifying the ring this run: 6 of 8 entries are host import-stub bridges, and the 2 that are inside the guest image (`0x15aa19c` = `FMCryptFile::openRead`, `0xba8474` = a stack-canary check followed by a function epilogue) are both **functions that had already returned**. A spin makes no imports, so the ring's guest entries are always stale. The frame-pointer walk is the instrument that works, because it does not depend on the guest calling out.

`ccmode=now` has **not yet been measured**, and until T15 lands it cannot answer its own question.

| Observation | Conclusion |
|---|---|
| `cc-endpoint-request` absent | capture site never reached |
| `cc-endpoint-request` with `vtable=0x0` | ABI is wrong; no delivery is possible — fix that first |
| `vtable` real, then `cc-deliver` **and** `cc-complete-returned` | delivery works and returns; fix holds |
| `cc-deliver` present, `cc-complete-returned` absent | wedged *inside* the continuation |
| crash file not `armed` | not a hang — a fault, and `pc`/`lr` name the instruction |

Two runs of the same configuration are required before any of this is believed.

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
- [ ] T6: Unblock cloudcell→menu gate — acceptance: guest opens `MainMenu.gui` or `sprites_*` (covers: S2, S4; depends: T8-T10, T11, T15)
- [ ] T11: Measure `ccmode=now`, twice — acceptance: two archives with identical discriminators. The ABI it depends on is fixed as of the commit below, so this is now runnable (covers: S4; depends: T9, T10, T15)
- [ ] T12: Give `ccmode=bg` a drain that never touches `g_queue_lock` — a dedicated thread polling `g_ccPending` under its own lock, because the alert-pump thread measurably blocks on the lock the wedged game thread holds. Only worth building if T11 shows `now` fails (covers: S4; depends: T9, T11)
- [x] T13: Fix 70 `vehicles/shaders/*.pvr.z` `Z_DATA_ERROR` — the `.z`-strip fallback served the uncompressed file to an unconditional `inflate()`. Removed; ENOENT now propagates and the guest walks its own format list (covers: S4)
- [x] T14: Correct the `Mutex` is-handle-0 comment in `cloudcell_defer.cpp` — `_LOCK_T` holds the owning thread id, 0 means unowned, and `mutexCreate` must NOT be added (covers: S4)
- [x] T15: Recover the guest's slot-2 continuation ABI — the stub read x3, a 32-bit scalar, as the continuation pointer; the record is in x4. Verified against a capstone control. `ccmode=now` is now runnable (covers: S4)

## [S5] Still open

- Guest PC sampling is structurally unavailable in the emulator: `svcGetThreadContext3` returns the *host* NRO program counter, so every `census-pc` line reads `pc_img=host-nro`. The census import-ring is the only working substitute, and an import ring cannot distinguish "spinning on a flag" from "walking a corrupt structure" — both are pure guest code with zero imports.
- `0x12aea7c` (the per-frame function) has **zero `bl` callers, zero `adrp` references, and appears as no constant in either the guest or the host ELF**. It is reached through a runtime-computed pointer, so the ordered boot chain to it is not recoverable statically. Do not spend time on a static caller search for it.
- `0x12E1600` (network/event thread) computes its `poll` timeout from a `fmov d9,#-1.0` sentinel, giving −1,000,000 ms, i.e. an infinite wait. That is by design, but it means the cloud state machine can never advance offline.

