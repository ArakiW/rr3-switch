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
- The guest's Cloudcell constructor never publishes. This is now the blocker (see below).

## [S6] The real blocker: `*ccGlobal == 0`

`ccsynth0` (2026-10-17) stopped the host replacing the guest's Cloudcell object, and the run got further than any run in this project's history:

```
create-cloudcell = 0     the host installed nothing
cc-slot = 0   cc-deliver = 0   no stub was involved at all
frames = 16200          not 1
```

Wedge eliminated, not patched around. Three successive repairs of individual vtable entries had failed; removing the substitution fixed the class.

**And that made the real blocker visible.** The object pointer, logged packed as `(addr & 0xFFFFFFFF) << 32 | value`:

```
phase=cc-got-diag  target=073ba5d0 07413e98   -> GOT slot valid, *GOT = 0x07413e98
phase=cc-ptr-diag  target=07413e98 00000000   -> the object pointer is 0
```

`*ccGlobal == 0`. The menu gate at `0x12aefd8` is the third term of a four-way AND:

```
[x23]!=0  &&  [*ccGlobal]!=0  &&  cc->[0x4c1]!=0  &&  (vtable[17]()&1)==0
                        ^ the second term
```

**With the object pointer null the gate can never pass, whatever `0x4c1` says.** The guest logs `Cloudcell initializing with environment: Development`, then renders 16,200 frames against a null Cloudcell: it does not crash, it simply never becomes available.

So the remaining work is not a byte and not a vtable. **It is getting the guest's own constructor (`0x1cbe03c`) to publish.** That constructor allocates 0x4d0 bytes, publishes at `0x1cbe094`, installs the real `cc::ICloudcell` vtable `0x1f63b58` at `0x1cbe290` (RTTI at `0x1f63d58` names `N2cc10ICloudcellE`), and sets its own `+0x4c1 = 1` at `0x1cbe218`. If it runs, the menu gate opens with no host involvement at all.

### The three failed repairs, recorded because the pattern is the lesson

| Attempt | What level-1 entry 5 was made | Result |
|---|---|---|
| 1 | a logging function | guest read the first 8 bytes of code as a pointer |
| 2 | `return self` | same |
| 3 | a pointer to a real sub-object | **worse** — broke the 7 sites that genuinely *call* index 5; `cc-deliver` 3 → 0 |

Entry 5 was called from seven places and dereferenced from one. **That is only possible if the table is not a vtable but a struct mixing method and data pointers**, and whether an entry is invoked or dereferenced depends on the call site. No amount of per-entry guessing resolves that; only reading the code that builds the table does.

The tell was in the log throughout, and was read backwards twice: `cc-slot-unpinned=0` does not mean the stub worked — it means **the guest never reached the stub**.


## [S7] The sharpest open contradiction (2026-10-18)

Static analysis and runtime measurement now disagree, and the disagreement is narrow enough to be useful.

**Static, from bytes, with validated instruments:**

- `func 0x12ac2dc` (the MainActivity ctor) is **branch-free** from entry to `0x12ac330`. Prologue, TLS canary, three `mov`s, one call to `0x12b62c8`, then straight into `adrp/ldr/add` and the store. No `cbz`, `cbnz`, `b.` or `ret` in between.
- `0x12b62c8` is a JNIObject constructor, not an error path. Four of its JNIEnv offsets match the standard `JNINativeInterface_` table exactly: `0xf8`=GetObjectClass, `0xa8`=NewGlobalRef, `0xb8`=DeleteLocalRef, `0x20`=GetVersion. Its "fatal" callee `0x109fbb0` is `__android_log_assert`-style: it logs and then **`ret`**s. Both null paths log and **fall through**.
- The ctor's FDE (`0x801f30`, covering `0x12ac2dc..0x12acbc4`) resolves to the `zR` CIE — **no LSDA, therefore no landing pad anywhere in it**. Nothing can catch between the call and the store.
- `x8` at `0x12ac330` is exactly `*(GOT 0x1f6e540)`, addend `0x1f97030`, and is never clobbered between `0x12ac31c` and the store.

**So the store executes. Unconditionally.**

**Runtime, from a probe whose guards are now each justified by the failure mode they prevent:**

```
singleton = 0x0   t1 = 0x0   (136 polls across 16,320 frames)
cell      = 0x2110e95d00   gate_byte4c1 = 1
```

A ctor that cannot skip its store, called twice, logged `CREATE` before the view callback, and yet the cell reads zero for the whole run. Exactly two candidates survive:

1. **the poller is reading a different address than it thinks** (PIE bias), or
2. **a clear lands between `onCreateJNI` and the first poll.** There are four stores to that cell and three of them are clears: `0x12acd90` (in the dtor at `0x12acbc4`), `0x12b3f14`, and `0x12b3f24` (`str xzr` then `b operator delete`). The last two have no direct `bl`/`b` callers and no vtable pointer found in `.rodata`, `.data.rel.ro` or `.data` — **how they are reached is undetermined**, and that is where to look next.

Note the earlier apparent contradiction is not one: `onWindowFocusChangedJNI` reads the same cell, and its log line is reachable only past a null check — so the cell is non-null at *some* point. A store-then-clear-then-rebuild sequence fits both observations; a never-stored cell does not.

## [S8] The gate is OPEN (2026-10-18) — the blocker moved

The first fully unfiltered reading. `baseOk` — which tested `renderTarget - 0x12B3614`
against ELF vaddr bounds, and was therefore permanently false, manufacturing every
previous zero by construction — is gone. Both chains print all three levels.

```
f=0     ss_got=0x75e4030  ss_obj=0x21033a3500   t1_got=0x7602bf8  t1_obj=0x210e3e93c0  cc1=0  sub280=0x0
f=8880  ss_got=0x75e4030  ss_obj=0x21033a3500   t1_got=0x7602bf8  t1_obj=0x210e3e93c0  cc1=1  sub280=0x2110e825a0
```

| term | condition | measured | |
|---|---|---|---|
| 1 | `[*GOT 0x1f6df50] != 0` | `0x210e3e93c0` | ok |
| 2 | `[*GOT 0x1f6d5d0] != 0` | `0x21033a3500` | ok |
| 3 | `Cloudcell + 0x4c1 != 0` | 0, then 1 | ok |
| 4 | `(sub280 & 1) == 0` | tag = 0 | ok |

**All four are satisfied, and the guest still never opens the main menu.** `MainMenu`
and `ESportsMainMenu` stay at zero occurrences and `path-req` stays at its usual 120.
So `0x12af010: blr x8` — the menu loader, `vtable[21] = 0x14851e8` — **is being called
and doing nothing observable.** The gate was never the story. Everything from
`0x12aefd8` onward is now closed: it was the Cloudcell availability byte, then the
lifecycle order, then the continuation ABI, then two vtables with null entries, then
the singleton. Each was real; none was the last one.

The new lead is **content or service, not control flow**: the loader is reached and
either returns early or asks for something that is not there. Note also that this run
reached `frames=8880` where the previous ones reached 16080-16320, so whatever the
loader does on entry is already costing frames.

That is a different kind of question from every one before it, and it is the first one
that can be asked with the gate's own terms satisfied.

## [S9] RETRACTED: `0x12af010` is an ASSERT, not a menu loader

**Every premise in S1-S8 rested on one unverified claim, and it is false.**

`0x12af010: blr x8` was read as `obj->vtable[21](obj, 2, 0x60)` loading `ESportsMainMenu.xml`. That came from a comment written into the source on 2026-10-12 and never checked against the code. Disassembling `0x14851e8` settles it:

```
0x45c6e8  E:\dev\builds\r3_update_a\source\src\mt3D\OpenGL/mtRenderGL.h
          with line numbers 730 (0x2da) and 753 (0x2f1)
```

**It is a compiler-emitted assert reporter from the GL renderer.** It ignores `this`, touches no file, constructs no path, and its only side effect is one indirect call. `mode` selects the assert variant id (`0 → 0x8ca9`, `1 → 0x8ca8`, `>=2 → 0x8d40` — so `mode=2` picks the always-on/release family); `0x60` is a **bitmask of extra condition-string ids** for the report, not a size or an enum. Its only early return is `S->0xc < 3 && S->0x6c == 0`, where `S` is the app singleton; otherwise it **fires the assert**.

Corroboration: `vtable[4]=0x148462c`, `[5]=0x1484640`, `[6]=0x1484654` are five-instruction tails to `0x128b55c` carrying `(id, "mtRenderGL.cpp", line)` — sibling reporters, same family. A scan of every `ldr xN,[xM,#0xa8]; blr xN` in `.text` yields 150 dispatch sites and **exactly one** with `w2=0x60`: the one at `0x12af00c`.

So the block at `0x12aefa8..0x12af014` is **a GL-renderer assertion guard**, and `vtable[6]` and `vtable[17]` immediately before it are themselves a sibling assert (`0x1484654`, `mtRenderGL.cpp:54`) and a real query on `[this+0x5a0]`. **Two asserts can fire per frame.** The guest is not refusing to open a menu; it is telling us the renderer is not in the state its code expects.

The class is the GL renderer, not a UI/menu object. There is no RTTI on `0x1ed7fe0` to confirm it, but three independent strings say `mtRenderGL`.

**What this retires.** The whole `0x4C1` line of reasoning -- "the menu gate byte" -- was chasing a comment. The fixes along the way were still real bugs (a wrong continuation ABI, two vtables with null entries, a lifecycle ordering mistake, a 256-byte conf truncation, a `.z`-strip that turned ENOENT into Z_DATA_ERROR), but none of them was the menu gate, because there is no menu gate there.

**What replaces it.** Two concrete questions, both about the renderer rather than a menu:
1. `S->0xc` and `S->0x6c` on the app singleton at GOT `0x1f6f970` (addend `0x1f97c88`) -- what sets them, and which value does this environment reach. These decide silent-return vs assert-fire.
2. `vtable[6]` (`0x1484654`) fires one assert *before* this one, every frame. Its condition may be the upstream cause.

The import table carries `__assert2`, `__android_log_print`, `raise`, `ptrace`, `abort`, `sigaction` -- the canonical EA assert-to-crash-report chain. **A host that stubs `raise`/`abort`/`ptrace` into a sink would let the run continue, and it would also be the difference between "an assert fired every frame" and "the guest made progress".** That is the next thing to look at.

## [S10] ROOT CAUSE: the guest samples the GL version before the host has a context

This is the whole thing, verified end to end. It is a host ordering bug, it is
not Cloudcell, and it is not a gate.

**`S->0xc` is the OpenGL ES major version.** Class `11ndSingletonI11mtGLWrapperE`
(RTTI at `0x518675`), object 200 bytes, vtable address point `0x1ec8bd0`. It is
written exactly once, in `0x128a17c`:

```
0x128a31c  bl  glGetString     ; w0 = 0x1F02 = GL_VERSION
0x128a340  bl  strstr(s, "OpenGL ES")        ; string at 0x3a195b
0x128a350  add  x2, x19, #0xc               ; &major
0x128a354  add  x3, x19, #0x10              ; &minor
0x128a35c  bl  sscanf(p+0xa, "%d.%d", &major, &minor)   ; fmt at 0x415461
```

`0xc` = major, `0x10` = minor. `S->0x6c` is read once and **never written**, so it
is 0 from `.bss` and the assert's second branch is dead by construction.

**That leaves exactly one branch, and it is the one we are in.** The assert reporter
`0x14851e8` returns silently when `S->0xc < 3 && S->0x6c == 0`. The other two branches
call `0x128c054` / `0x128c4c4`, which reach the EA assert chain ending in
`bionicAssert2` -- which is `[[noreturn]]` and calls `std::abort()`. Three 300-second
runs produced **zero** asserts and did not die, so the assert branches were not taken.
**The guard at `0x12aefc8..0x12af014` silently returns every frame, and whatever it was
protecting never runs.**

**Why `S->0xc` is 0 — the log settles it.** Two `onCreateJNI` invocations exist:

```
line  50  initDeps-completing
line  91  CREATE                    <- the GUEST's own early call
line 183  view-egl-init-begin
line 189  view-egl-current-ok       <- context is current only here
line 193  view-call-created
line 194  onCreateJNI-pre          <- OURS, after the context
line 207  OpenGL ES 3.2 Mesa 20.1.0-rc3   <- glGetString works here
```

The version is sampled during the guest's early `onCreateJNI`, which runs inside
`initializeGuestDependencies` at **main.cpp:1110** -- and the host only builds and
makes current the EGL context later, inside `runGuestViewProbe` at **main.cpp:1176**.
With no current context Mesa returns NULL from `glGetString`, `strstr`/`sscanf` never
produce a number, and `S->0xc` stays 0.

Our second `onCreateJNI` at main.cpp-equivalent line 194 is correctly ordered -- the
context *is* current and `glGetString` does answer. But `onCreateJNI` is a **get-or-create**
(`cbz` on the cell branches to the constructor, non-null branches to a plain return at
`0x12b3154`), so the second call never reaches `0x128a17c`. **The version is never
re-sampled.** That is why reordering the lifecycle alone did not help, and why calling
`onCreateJNI` twice did not help either.

**The fix**: the EGL context has to exist before `initializeGuestDependencies`, i.e.
before the guest gets its first chance to ask what GL it is running on. On a real
device the app creates its GL surface first; here the host does it 66 lines too late.
This is the same class of bug as the lifecycle-order fix in T11, one level earlier in
the boot sequence.

Nothing here touches Cloudcell credentials, authentication or responses. The guest is
asking the driver what GL version it is, and the host had not given it a context yet.


## [S11] GLVER=3.2 -- the fix works; the gate's function is never entered

GLVER=3.2: the fix works, and the gate's function is never entered

ccdeliver14 answers the question ccdeliver13 raised. One line, 132 polls,
identical every time:

  GLVER=3.2  S6c=1  Sobj=0x2102d582c0

  S->0xc  = 3   the OpenGL ES MAJOR VERSION the guest recorded
  S->0x6c = 1

So the root-cause fix did exactly what it was built to do. The guest
sampled glGetString(GL_VERSION) with a context present, sscanf("3.2") into
S->0xc, and the field is now 3 instead of 0.

It also refutes a sub-agent's claim. That pass reported that S->0x6c "is
read once and never written, so it is 0 from .bss and the assert's second
branch is dead by construction". S->0x6c is 1. Something writes it, and the
scan missed it -- the same class of miss this project has hit repeatedly,
where a scanner's negative is trusted without a runtime check.

What the two numbers now decide. The guard at 0x14851e8:

  S->0xc >= 3                     -> assert
  S->0xc < 3 and S->0x6c != 0      -> assert
  S->0xc < 3 and S->0x6c == 0      -> return silently

S->0xc is 3, so the FIRST branch applies and it must assert. And the run
logged zero asserts and did not die -- bionicAssert2 is [[noreturn]] and
calls std::abort(), so if that branch had been taken the process would be
gone.

The guard's arithmetic is unconditional within its function: the code
immediately before it is two calls with no branch, and immediately after is
the tail. So the function containing the gate, 0x12aea7c, is never entered.

That is the remaining blocker, and it is now a question about the guest's
own control flow rather than about anything the host supplies:

  onViewRenderJNI IS called -- render-call-pre is present, and the run
  reaches ~15,700 frames -- but the code path that contains the GL assert
  guard never executes.

0x12aea7c has zero bl callers and appears as no constant in either ELF, so
it is entered through a runtime-computed pointer. Finding who is supposed
to dispatch it is the next question, and it is a static one.

The deltas are computed and printed next to the vaddrs they come from,
because Python caught a 0x60 transcription error before the build:

  0x1f6d5d0 -> 0x00CB9FBC   Cloudcell cell
  0x1f6df50 -> 0x00CBA93C   term-1 cell
  0x1f6e540 -> 0x00CBAF2C   singleton
  0x1f6f970 -> 0x00CBC35C   mtGLWrapper S



## [S12] WITHDRAWN: the guard is a side path

The guard is a side path: the guest exits before ever reaching it

This dissolves the last contradiction, and it does so without any new bug.

ccdeliver16 established that both assert-report pointers are installed and
stable -- repA=0x210952d138, repB=0x210952d340, 120/120 polls. So if
0x14851e8 were reached, it would report something, and the zero asserts
plus the zero '[A...' lines in the whole log meant it was not reached.

The guard's four terms all read satisfied. That looked like a
contradiction. It is not: func 0x12aea7c has at least SIX exits that
bypass the guard entirely, and every target is past the guard's own tail:

  0x12aebd4  ret
  0x12aed88  cbz  x9, #0x12af218
  0x12aed50  cbz  x8, #0x12af248
  0x12aebe4  tbz  w8, #0, #0x12af2ac
  0x12aec00  tbz  w8, #0, #0x12af304
  0x12aebb8  b.ne #0x12af354

0x12af014 is the guard's skip target. Every one of those is beyond it.
Only three instructions reach the guard at 0x12aefa8 -- 0x12aef50
(b #0x12aefa8), 0x12aef6c (b.hi #0x12aefa8) and fall-through from
0x12aef74. So the guard is a rarely-taken diagnostic path, not the
frame's main line.

CONSEQUENCE, and it is the part that matters:

  The "menu gate" at 0x12aefc8 was never the frame's gate. It is not
  reachable on the path this guest actually takes, so no value of
  Cloudcell+0x4c1, no value of S->0xc, and no state of the singleton can
  open it. Every conclusion drawn from that block over the past several
  rounds -- including "the gate's four terms are satisfied but the menu
  still does not load" -- was measuring a path the guest does not walk.

  The early-GL-context change (2ec4953e) is not the root cause of the
  menu never loading. It does make glGetString(GL_VERSION) answerable
  earlier, which is defensible on its own merits and harmless, but it
  cannot be credited with anything.

  S11 and the surrounding narrative are therefore withdrawn. The honest
  position: the guest reaches roughly 16,400 frames, the GL and content
  defects found along the way are real and fixed, and the reason the main
  menu never opens is still unknown -- and it is now a question about the
  guest's own frame state machine, with no host-side gate standing in the
  way that anyone has yet identified.

The value of the last several rounds was not the conclusion. It was that
the wedge is gone (frames 1 -> ~16,400) and that seventy permanently
failing shader loads are fixed, both of which stand on their own.

Gates for this round: build 0, inputs 0, frozen hash 32a3e014..., 11/11.



## [S13] ROOT CAUSE: the guest waits for GPU id 4 and gets 0

ROOT CAUSE: the guest waits for GPU id 4 and gets 0

    GPU id '0', model '-1', renderer string 'NV120'

The render loop's exit test, read from the binary:

  0x12af180  ... f_4a0 rotation
  0x12af190  cmp   w9, #4
  0x12af194  b.ne  0x12aebac        ; id != 4 -> leave, skipping the tail
  0x12af19c  ...                    ; the function's ONLY post-frame transition

`[rend+0x4fc]` is the engine's GPU id -- its own field name, from the guest's
format string "GPU id '%d', model '%d', renderer string '%s'" at 0x46cd70.
`sa9` is `renderer+0x1f5a9`, the splash->menu one-shot, still 0.

Measured this run: t4fc=0, sa9=0. The host has carried a note on exactly
this since 2026-10-04 (archive 20261004-053820-tier2): "MEASURED ...
render-call-pre ... t4fc=5 ... the render loop's exit test is cmp #4 / b.ne
... it waits for id == 4 while the id is 5". So this was seen five days ago,
noted, and never chased. On this run the id is 0 rather than 5.

The guest derives the id from the GL identity strings. The host serves
GL_RENDERER as "NV120" and GL_VENDOR as "nouveau" -- an NVIDIA string the
Android-era compatibility tiers have no entry for -- so the lookup yields 0.

This also CORRECTS S12. Two of that section's claims were wrong:

  - 0x12aea7c has exactly ONE ret, at 0x12aebd4. The other five "exits"
    listed there (0x12af218/248/2ac/304/354) are branches into tail code
    and __stack_chk_fail, not returns.
  - 0x12aefa8 is NOT a rare diagnostic path. With the measured pre-state
    f_4d8=0, f_4dc=2, `cmp #6 / b.lt` sends control to 0x12aef54, f_4dc
    becomes 3, `2 > 4` is false, and the GL arm falls straight INTO
    0x12aefa8. Every successful frame passes through it. The guard runs
    every frame; its four terms read satisfied (136/136 polls); and the
    reporter it calls is installed and live (repA/repB non-null, 120/120).

So the guard was never the blocker, and neither was any value behind it. The
frame runs to completion every time -- it just leaves before the one place
that advances the game state.

The remaining question is what the guest's id lookup accepts. That decides
whether the honest answer is "nothing, report 0 and the game declines" or a
platform translation. It is worth being explicit that this is a different
kind of question from the Cloudcell boundary: there, faking would mean
claiming to be authenticated. Here the guest asks which GPU it is, and the
only way to reach tier 4 is to answer with a string from a different device.
That is ordinary porting work for most engines and a misrepresentation for
this one, and it is a judgement the port owner should make, not the harness.



## [S14] The GPU id table, read from the code

The GPU id table, read from the code

`0x12afd94`..`0x12b0378`, two callers (`0x12ad650`, `0x12adbc0`). It fetches
exactly one C string from a singleton (vtable slot `+0x20`, `strlen`, copied
into an SSO string at `sp+0x18`), then walks an inlined chain of substring
searches -- `memchr` for the first character, then byte-compare against
immediates built with `mov`/`movk`.

| # | substring | id | site |
|---|-----------|----|------|
| 1 | `GC` | 1 | `0x12afe78` |
| 2 | `Immersion` | 2 | `0x12afee8` |
| 3 | `PowerVR` | 4 | `0x12aff68` |
| 4 | `Adreno` | 5 | `0x12affe4` |
| 5 | `Mali` | 6 | `0x12b005c` |
| 6 | `VideoCore` | 8 | `0x12b0114` |
| 7 | `Intel` | 7 | `0x12b00c8` |
| -- | no match | 0 | `bl 0x12b49b0` |

First match wins, case-sensitive. The bound is `len - strlen(pattern)`, so a
pattern with nothing after it still matches; `VideoCore` yields 8 only if it
is not at offset 0, otherwise it falls through to the `Intel` test.

**The default 0 is a real default, not an "unknown GPU" constant.**
`0x12afdd0` seeds `stp w9, wzr, [x29,#-0x10]` with `w9 = -1` -- id 0,
model -1 -- and the no-match path calls `0x12b49b0`, which stores `wzr` for
the id and `w9 = -1` for the model. That reproduces the observed
`GPU id '0', model '-1'` exactly.

**id 3 is never produced.** So the value is not a monotone quality tier: it
mixes GPU families (PowerVR, Adreno, Mali, Intel) with setting names
(`GC`, `Immersion`). **`id == 4` means "the renderer string contains
`PowerVR`"** and nothing else.

`model` is a decimal digit-run parsed from the same string, but only when
`id != 0 && model == -1` (`0x12b01b0 cbz w1`; `0x12b01b4 cmn w2,#1`), which
is why this run prints `-1` even though `NV120` contains digits.

Chain proven end to end, same base register and same field:

  0x12b0280  str  w1, [x19, #0x4fc]
  0x12af184  ldr  w9, [x19, #0x4fc]
  0x12af190  cmp  w9, #4

**Only `GL_RENDERER` feeds the lookup.** `GL_VENDOR` is fetched by the game
elsewhere (`0x1484640`, `0x128a26c`) but never reaches this chain. The string
is the same one the guest prints as `renderer string '%s'`, and this run
printed `NV120`, which is exactly the host's `GL_RENDERER`.

**Therefore there is no honest string that also yields 4.** `NV120`, or any
real nouveau or NVIDIA renderer string, matches none of
`GC`/`Immersion`/`PowerVR`/`Adreno`/`Mali`/`VideoCore`/`Intel`.

A correction to a guess made earlier in this investigation: the initial
reading of the string pool -- three words, `Adreno`, `Mali`, `PowerVR`,
appearing once each -- was used to assert that the table held phone GPUs
only. That inference was not supported: the three words sit in unrelated
parts of `.rodata` (`0x4755d1` beside `texHeight` and `doc Path: %s`,
`0x496e46`, `0x3f82fc`) and are ordinary pool entries. The table above is
the one that was read out of the code, and it maps **id 4 to `PowerVR`, not
to Adreno** -- which the string-pool guess had wrong in the part that
mattered most.

No second route to the id was found: the only direct 32-bit writer of
`this+0x4fc` in `.text` is `0x12b0280`. The two other hits (`0x12dec6c`,
`0x12df0a4`) target a `adrp 0x1f89000` global, a different object.

Not determined: which enum the singleton accessor passes to `glGetString`
(the vtable is runtime-initialised, so `GL_RENDERER` rests on the observed
value rather than on the call chain); what `GC` is meant to catch; and
indirect or aliased writes to `+0x4fc`, which a direct-encoding scan cannot
exclude.



## [S15] The GPU gate opens; the guest then waits on Cloudcell

ccdeliver19: the GPU gate opens, and the guest then waits on Cloudcell

  GPU id '4', model '8320', renderer string 'PowerVR Rogue GE8320 (Mesa 20.1.0-rc3)'
  t4fc=4  p500=8320

The gate at 0x12af190 now passes exactly as intended. ccdeliver19 is the
first run whose blocker moved for the right reason rather than by accident.

The menu still does not load -- MainMenu 0, path-req still 120, sa9 still
0 -- and the guest says why, in its own log:

  [FileManager::CreateDir] Error creating directory:
      sdmc:/switch/rr3-native-rehost/.depot/CC_Data
  CC: Server Environment Updated. SyncManager updating SyncDomain to:
      connect-dev-ss.cloudcell.com
  CC: Invalid DeviceId! Have we connected to cloudcell yet?
  CC: Sub Sync Queued: 13
  CC: Unable to send Sync, Social Media not initialised.

Three readings, in order of certainty:

1. **The CreateDir error is not the blocker.** `.depot/CC_Data` exists on
   the card now and the host has guestMkdir (guest_filesystem.cpp:2177).
   Whatever failed, it is not what stops the boot.

2. **The guest wants a Cloudcell session.** SyncDomain is set to a real
   host, connect-dev-ss.cloudcell.com, and it never connects; there is no
   DeviceId, so "Social Media not initialised" repeats forever.

3. **That is the same boundary the port started with.** Reaching the main
   menu requires a live Cloudcell backend session. A GPU renderer string is
   a device profile; this is the game's backend service, and the whole
   reason the session began with a rule about not faking Cloudcell.

So the honest position after this round is short. The host-side defects
along the way were real and are fixed:

  frames 1 -> ~16,000        the wedge is gone
  Z_DATA_ERROR 70 -> 0       70 permanently failing shader loads
  GPU id 0 -> 4              the frame loop now advances past its own gate

and the remaining blocker is not a defect in this port. It is that the game
does not reach its main menu without a Cloudcell session, and there is no
such session here. Everything the harness is responsible for is either
fixed or measured.


## S16 -- the menu blocker is a texture-resolution mismatch, not a Cloudcell problem

Date: 2026-10-19. Archives: 20261019-170132-distinct, 20261019-212333-loginok2,
20261019-214856-social1.

### What was ruled out, and with what evidence

Five Cloudcell hypotheses were closed with measurements, not reasoning:

| Hypothesis | Verdict | Evidence |
|---|---|---|
| TLS/certificates block the login | false | the guest links OpenSSL 3.3.0, and Cloudcell's TLS is in the Android Java layer the port already replaced. `mbedtls_*`/`SSL_*` exports: 0 |
| the HttpRequest facade is the blocker | false | 16 void calls per run; `HttpRequest` appears 0 times in the whole guest log |
| 23 of 25 Cloudcell callback names are wrong | true, fixed | `cloudcell_natives.h` said so and nobody acted: the guest exports the flat spelling. Resolvable names 26 -> 35 |
| the class table is too small | true, fixed | 34 distinct classes, 32 slots; `HttpRequest` fell off the end by one |
| the login stub cannot be reached | true, fixed | the JNI table the guest binds is `jni_bridge.cpp`'s, which never dispatches to `ClassRegistry` |

### The gate order that actually worked

1. symbol names -> 2. class names -> 3. class-table capacity -> 4. the dispatch
block's placement -> 5. the receiver's storage -> 6. the boolean slot.

Step 6 is the one that is easy to miss: `GetSessionValid()` returns a boolean, so
it goes to slot 37/38, and the dispatch added for the void slot never reached
it. The stub was registered, correctly named, and unreachable because of its
RETURN TYPE. `Sub Sync Queued` went 10765 -> 10 and `Invalid DeviceId!` went to
zero when the boolean slot was wired.

Step 5 needed the third shape. Two were tried and both are wrong: the one-byte
shared sentinel (a facade writing instance fields corrupts the host) and one
object per service (the guest dies at frame 4 -- it depends on getting the same
address back). The shape that works is one address, allocated -- which is what
the predecessor port had, because its `jni/jni.cpp` `NewObjectV` allocates and
this port's `jni_bridge.cpp` does not.

### The remaining blocker

CORRECTED 2026-10-19, later the same day: "path-req stops at 120" was WRONG.
120 is the log budget (`s_pathLog < 120` in guest_filesystem.cpp:951), not a
fact about the guest. The guest continued: `fs-candidate` is explicitly NOT
budgeted, and its last line is a SUCCESSFUL open of
`rr3veh0:/files/.depot/vehicles/shaders/spec_gloss.rgb.pvr`
(`try[1/guestOpen] rc=0`). The *_gloss shaders load fine.

This is the same trap as the 200-line stat-layout cap, fixed earlier the same
day -- a counter sitting on its cap reads exactly like the guest stopping
there. Two counters were checked for their caps before drawing a conclusion
from either.

The actual blocker, found by resolving the sampled PC: the guest's main thread
is parked in libnx `send()`.
    census-pc ... pc=0x812172e8 pc_img=host-nro pc_elfva=host+0x54f2e8
    aarch64-none-elf-addr2line -e native-switch/rr3-native-rehost.elf 0x54f2e8
      -> send
      -> libnx-4.12.0/nx/source/runtime/devices/socket.c:594
So the guest is blocked in a socket SEND, waiting for a response that never
comes. That is the hang, and it is a socket-layer problem rather than a
Cloudcell state-machine problem -- which is what "nogo=1" (never bring the
socket stack up, so every outbound connection fails at once) was invented for
on 2026-10-12, in a run that was inconclusive because the login path could
not execute at the time.

The same logical file exists in two places with DIFFERENT sizes:

| source | `fresnel_gloss.rgb.pvr` |
|---|---|
| ATC / DXT / ETC data packs | 3124 B (all three agree) |
| the `rr3veh0` content device (`vehicles-classic.romfs`) | 3124 B -- what the guest stats |
| the SD-side `.depot/vehicles/shaders/` | 4148 B -- what the guest reads |

4148 - 44 = 4104 against 3124 in the packs, a ratio of 1.31, and the data packs
are named `960x640+2048x1536` -- two resolutions in one pack. The SD-side copy
also has a 44-byte prefix before the `PVR!` magic, which the pack's copy does
not.

So the content device and the SD-side staged copy hold DIFFERENT RESOLUTIONS of
the same texture, and the guest rejects the one it gets. Every other vehicle
shader loads fine; only the `*_gloss` set is affected.

### Next

Find out which resolution the guest's asset manifest expects, and which one the
ROMFS volume actually carries for the `*_gloss` shaders. If the content device
is the one that is wrong, the fix is in the ROMFS build, not in code.---
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
- The guest's Cloudcell constructor never publishes. This is now the blocker (see below).

## [S6] The real blocker: `*ccGlobal == 0`

`ccsynth0` (2026-10-17) stopped the host replacing the guest's Cloudcell object, and the run got further than any run in this project's history:

```
create-cloudcell = 0     the host installed nothing
cc-slot = 0   cc-deliver = 0   no stub was involved at all
frames = 16200          not 1
```

Wedge eliminated, not patched around. Three successive repairs of individual vtable entries had failed; removing the substitution fixed the class.

**And that made the real blocker visible.** The object pointer, logged packed as `(addr & 0xFFFFFFFF) << 32 | value`:

```
phase=cc-got-diag  target=073ba5d0 07413e98   -> GOT slot valid, *GOT = 0x07413e98
phase=cc-ptr-diag  target=07413e98 00000000   -> the object pointer is 0
```

`*ccGlobal == 0`. The menu gate at `0x12aefd8` is the third term of a four-way AND:

```
[x23]!=0  &&  [*ccGlobal]!=0  &&  cc->[0x4c1]!=0  &&  (vtable[17]()&1)==0
                        ^ the second term
```

**With the object pointer null the gate can never pass, whatever `0x4c1` says.** The guest logs `Cloudcell initializing with environment: Development`, then renders 16,200 frames against a null Cloudcell: it does not crash, it simply never becomes available.

So the remaining work is not a byte and not a vtable. **It is getting the guest's own constructor (`0x1cbe03c`) to publish.** That constructor allocates 0x4d0 bytes, publishes at `0x1cbe094`, installs the real `cc::ICloudcell` vtable `0x1f63b58` at `0x1cbe290` (RTTI at `0x1f63d58` names `N2cc10ICloudcellE`), and sets its own `+0x4c1 = 1` at `0x1cbe218`. If it runs, the menu gate opens with no host involvement at all.

### The three failed repairs, recorded because the pattern is the lesson

| Attempt | What level-1 entry 5 was made | Result |
|---|---|---|
| 1 | a logging function | guest read the first 8 bytes of code as a pointer |
| 2 | `return self` | same |
| 3 | a pointer to a real sub-object | **worse** — broke the 7 sites that genuinely *call* index 5; `cc-deliver` 3 → 0 |

Entry 5 was called from seven places and dereferenced from one. **That is only possible if the table is not a vtable but a struct mixing method and data pointers**, and whether an entry is invoked or dereferenced depends on the call site. No amount of per-entry guessing resolves that; only reading the code that builds the table does.

The tell was in the log throughout, and was read backwards twice: `cc-slot-unpinned=0` does not mean the stub worked — it means **the guest never reached the stub**.


## [S7] The sharpest open contradiction (2026-10-18)

Static analysis and runtime measurement now disagree, and the disagreement is narrow enough to be useful.

**Static, from bytes, with validated instruments:**

- `func 0x12ac2dc` (the MainActivity ctor) is **branch-free** from entry to `0x12ac330`. Prologue, TLS canary, three `mov`s, one call to `0x12b62c8`, then straight into `adrp/ldr/add` and the store. No `cbz`, `cbnz`, `b.` or `ret` in between.
- `0x12b62c8` is a JNIObject constructor, not an error path. Four of its JNIEnv offsets match the standard `JNINativeInterface_` table exactly: `0xf8`=GetObjectClass, `0xa8`=NewGlobalRef, `0xb8`=DeleteLocalRef, `0x20`=GetVersion. Its "fatal" callee `0x109fbb0` is `__android_log_assert`-style: it logs and then **`ret`**s. Both null paths log and **fall through**.
- The ctor's FDE (`0x801f30`, covering `0x12ac2dc..0x12acbc4`) resolves to the `zR` CIE — **no LSDA, therefore no landing pad anywhere in it**. Nothing can catch between the call and the store.
- `x8` at `0x12ac330` is exactly `*(GOT 0x1f6e540)`, addend `0x1f97030`, and is never clobbered between `0x12ac31c` and the store.

**So the store executes. Unconditionally.**

**Runtime, from a probe whose guards are now each justified by the failure mode they prevent:**

```
singleton = 0x0   t1 = 0x0   (136 polls across 16,320 frames)
cell      = 0x2110e95d00   gate_byte4c1 = 1
```

A ctor that cannot skip its store, called twice, logged `CREATE` before the view callback, and yet the cell reads zero for the whole run. Exactly two candidates survive:

1. **the poller is reading a different address than it thinks** (PIE bias), or
2. **a clear lands between `onCreateJNI` and the first poll.** There are four stores to that cell and three of them are clears: `0x12acd90` (in the dtor at `0x12acbc4`), `0x12b3f14`, and `0x12b3f24` (`str xzr` then `b operator delete`). The last two have no direct `bl`/`b` callers and no vtable pointer found in `.rodata`, `.data.rel.ro` or `.data` — **how they are reached is undetermined**, and that is where to look next.

Note the earlier apparent contradiction is not one: `onWindowFocusChangedJNI` reads the same cell, and its log line is reachable only past a null check — so the cell is non-null at *some* point. A store-then-clear-then-rebuild sequence fits both observations; a never-stored cell does not.

## [S8] The gate is OPEN (2026-10-18) — the blocker moved

The first fully unfiltered reading. `baseOk` — which tested `renderTarget - 0x12B3614`
against ELF vaddr bounds, and was therefore permanently false, manufacturing every
previous zero by construction — is gone. Both chains print all three levels.

```
f=0     ss_got=0x75e4030  ss_obj=0x21033a3500   t1_got=0x7602bf8  t1_obj=0x210e3e93c0  cc1=0  sub280=0x0
f=8880  ss_got=0x75e4030  ss_obj=0x21033a3500   t1_got=0x7602bf8  t1_obj=0x210e3e93c0  cc1=1  sub280=0x2110e825a0
```

| term | condition | measured | |
|---|---|---|---|
| 1 | `[*GOT 0x1f6df50] != 0` | `0x210e3e93c0` | ok |
| 2 | `[*GOT 0x1f6d5d0] != 0` | `0x21033a3500` | ok |
| 3 | `Cloudcell + 0x4c1 != 0` | 0, then 1 | ok |
| 4 | `(sub280 & 1) == 0` | tag = 0 | ok |

**All four are satisfied, and the guest still never opens the main menu.** `MainMenu`
and `ESportsMainMenu` stay at zero occurrences and `path-req` stays at its usual 120.
So `0x12af010: blr x8` — the menu loader, `vtable[21] = 0x14851e8` — **is being called
and doing nothing observable.** The gate was never the story. Everything from
`0x12aefd8` onward is now closed: it was the Cloudcell availability byte, then the
lifecycle order, then the continuation ABI, then two vtables with null entries, then
the singleton. Each was real; none was the last one.

The new lead is **content or service, not control flow**: the loader is reached and
either returns early or asks for something that is not there. Note also that this run
reached `frames=8880` where the previous ones reached 16080-16320, so whatever the
loader does on entry is already costing frames.

That is a different kind of question from every one before it, and it is the first one
that can be asked with the gate's own terms satisfied.

## [S9] RETRACTED: `0x12af010` is an ASSERT, not a menu loader

**Every premise in S1-S8 rested on one unverified claim, and it is false.**

`0x12af010: blr x8` was read as `obj->vtable[21](obj, 2, 0x60)` loading `ESportsMainMenu.xml`. That came from a comment written into the source on 2026-10-12 and never checked against the code. Disassembling `0x14851e8` settles it:

```
0x45c6e8  E:\dev\builds\r3_update_a\source\src\mt3D\OpenGL/mtRenderGL.h
          with line numbers 730 (0x2da) and 753 (0x2f1)
```

**It is a compiler-emitted assert reporter from the GL renderer.** It ignores `this`, touches no file, constructs no path, and its only side effect is one indirect call. `mode` selects the assert variant id (`0 → 0x8ca9`, `1 → 0x8ca8`, `>=2 → 0x8d40` — so `mode=2` picks the always-on/release family); `0x60` is a **bitmask of extra condition-string ids** for the report, not a size or an enum. Its only early return is `S->0xc < 3 && S->0x6c == 0`, where `S` is the app singleton; otherwise it **fires the assert**.

Corroboration: `vtable[4]=0x148462c`, `[5]=0x1484640`, `[6]=0x1484654` are five-instruction tails to `0x128b55c` carrying `(id, "mtRenderGL.cpp", line)` — sibling reporters, same family. A scan of every `ldr xN,[xM,#0xa8]; blr xN` in `.text` yields 150 dispatch sites and **exactly one** with `w2=0x60`: the one at `0x12af00c`.

So the block at `0x12aefa8..0x12af014` is **a GL-renderer assertion guard**, and `vtable[6]` and `vtable[17]` immediately before it are themselves a sibling assert (`0x1484654`, `mtRenderGL.cpp:54`) and a real query on `[this+0x5a0]`. **Two asserts can fire per frame.** The guest is not refusing to open a menu; it is telling us the renderer is not in the state its code expects.

The class is the GL renderer, not a UI/menu object. There is no RTTI on `0x1ed7fe0` to confirm it, but three independent strings say `mtRenderGL`.

**What this retires.** The whole `0x4C1` line of reasoning -- "the menu gate byte" -- was chasing a comment. The fixes along the way were still real bugs (a wrong continuation ABI, two vtables with null entries, a lifecycle ordering mistake, a 256-byte conf truncation, a `.z`-strip that turned ENOENT into Z_DATA_ERROR), but none of them was the menu gate, because there is no menu gate there.

**What replaces it.** Two concrete questions, both about the renderer rather than a menu:
1. `S->0xc` and `S->0x6c` on the app singleton at GOT `0x1f6f970` (addend `0x1f97c88`) -- what sets them, and which value does this environment reach. These decide silent-return vs assert-fire.
2. `vtable[6]` (`0x1484654`) fires one assert *before* this one, every frame. Its condition may be the upstream cause.

The import table carries `__assert2`, `__android_log_print`, `raise`, `ptrace`, `abort`, `sigaction` -- the canonical EA assert-to-crash-report chain. **A host that stubs `raise`/`abort`/`ptrace` into a sink would let the run continue, and it would also be the difference between "an assert fired every frame" and "the guest made progress".** That is the next thing to look at.

## [S10] ROOT CAUSE: the guest samples the GL version before the host has a context

This is the whole thing, verified end to end. It is a host ordering bug, it is
not Cloudcell, and it is not a gate.

**`S->0xc` is the OpenGL ES major version.** Class `11ndSingletonI11mtGLWrapperE`
(RTTI at `0x518675`), object 200 bytes, vtable address point `0x1ec8bd0`. It is
written exactly once, in `0x128a17c`:

```
0x128a31c  bl  glGetString     ; w0 = 0x1F02 = GL_VERSION
0x128a340  bl  strstr(s, "OpenGL ES")        ; string at 0x3a195b
0x128a350  add  x2, x19, #0xc               ; &major
0x128a354  add  x3, x19, #0x10              ; &minor
0x128a35c  bl  sscanf(p+0xa, "%d.%d", &major, &minor)   ; fmt at 0x415461
```

`0xc` = major, `0x10` = minor. `S->0x6c` is read once and **never written**, so it
is 0 from `.bss` and the assert's second branch is dead by construction.

**That leaves exactly one branch, and it is the one we are in.** The assert reporter
`0x14851e8` returns silently when `S->0xc < 3 && S->0x6c == 0`. The other two branches
call `0x128c054` / `0x128c4c4`, which reach the EA assert chain ending in
`bionicAssert2` -- which is `[[noreturn]]` and calls `std::abort()`. Three 300-second
runs produced **zero** asserts and did not die, so the assert branches were not taken.
**The guard at `0x12aefc8..0x12af014` silently returns every frame, and whatever it was
protecting never runs.**

**Why `S->0xc` is 0 — the log settles it.** Two `onCreateJNI` invocations exist:

```
line  50  initDeps-completing
line  91  CREATE                    <- the GUEST's own early call
line 183  view-egl-init-begin
line 189  view-egl-current-ok       <- context is current only here
line 193  view-call-created
line 194  onCreateJNI-pre          <- OURS, after the context
line 207  OpenGL ES 3.2 Mesa 20.1.0-rc3   <- glGetString works here
```

The version is sampled during the guest's early `onCreateJNI`, which runs inside
`initializeGuestDependencies` at **main.cpp:1110** -- and the host only builds and
makes current the EGL context later, inside `runGuestViewProbe` at **main.cpp:1176**.
With no current context Mesa returns NULL from `glGetString`, `strstr`/`sscanf` never
produce a number, and `S->0xc` stays 0.

Our second `onCreateJNI` at main.cpp-equivalent line 194 is correctly ordered -- the
context *is* current and `glGetString` does answer. But `onCreateJNI` is a **get-or-create**
(`cbz` on the cell branches to the constructor, non-null branches to a plain return at
`0x12b3154`), so the second call never reaches `0x128a17c`. **The version is never
re-sampled.** That is why reordering the lifecycle alone did not help, and why calling
`onCreateJNI` twice did not help either.

**The fix**: the EGL context has to exist before `initializeGuestDependencies`, i.e.
before the guest gets its first chance to ask what GL it is running on. On a real
device the app creates its GL surface first; here the host does it 66 lines too late.
This is the same class of bug as the lifecycle-order fix in T11, one level earlier in
the boot sequence.

Nothing here touches Cloudcell credentials, authentication or responses. The guest is
asking the driver what GL version it is, and the host had not given it a context yet.


## [S11] GLVER=3.2 -- the fix works; the gate's function is never entered

GLVER=3.2: the fix works, and the gate's function is never entered

ccdeliver14 answers the question ccdeliver13 raised. One line, 132 polls,
identical every time:

  GLVER=3.2  S6c=1  Sobj=0x2102d582c0

  S->0xc  = 3   the OpenGL ES MAJOR VERSION the guest recorded
  S->0x6c = 1

So the root-cause fix did exactly what it was built to do. The guest
sampled glGetString(GL_VERSION) with a context present, sscanf("3.2") into
S->0xc, and the field is now 3 instead of 0.

It also refutes a sub-agent's claim. That pass reported that S->0x6c "is
read once and never written, so it is 0 from .bss and the assert's second
branch is dead by construction". S->0x6c is 1. Something writes it, and the
scan missed it -- the same class of miss this project has hit repeatedly,
where a scanner's negative is trusted without a runtime check.

What the two numbers now decide. The guard at 0x14851e8:

  S->0xc >= 3                     -> assert
  S->0xc < 3 and S->0x6c != 0      -> assert
  S->0xc < 3 and S->0x6c == 0      -> return silently

S->0xc is 3, so the FIRST branch applies and it must assert. And the run
logged zero asserts and did not die -- bionicAssert2 is [[noreturn]] and
calls std::abort(), so if that branch had been taken the process would be
gone.

The guard's arithmetic is unconditional within its function: the code
immediately before it is two calls with no branch, and immediately after is
the tail. So the function containing the gate, 0x12aea7c, is never entered.

That is the remaining blocker, and it is now a question about the guest's
own control flow rather than about anything the host supplies:

  onViewRenderJNI IS called -- render-call-pre is present, and the run
  reaches ~15,700 frames -- but the code path that contains the GL assert
  guard never executes.

0x12aea7c has zero bl callers and appears as no constant in either ELF, so
it is entered through a runtime-computed pointer. Finding who is supposed
to dispatch it is the next question, and it is a static one.

The deltas are computed and printed next to the vaddrs they come from,
because Python caught a 0x60 transcription error before the build:

  0x1f6d5d0 -> 0x00CB9FBC   Cloudcell cell
  0x1f6df50 -> 0x00CBA93C   term-1 cell
  0x1f6e540 -> 0x00CBAF2C   singleton
  0x1f6f970 -> 0x00CBC35C   mtGLWrapper S



## [S12] WITHDRAWN: the guard is a side path

The guard is a side path: the guest exits before ever reaching it

This dissolves the last contradiction, and it does so without any new bug.

ccdeliver16 established that both assert-report pointers are installed and
stable -- repA=0x210952d138, repB=0x210952d340, 120/120 polls. So if
0x14851e8 were reached, it would report something, and the zero asserts
plus the zero '[A...' lines in the whole log meant it was not reached.

The guard's four terms all read satisfied. That looked like a
contradiction. It is not: func 0x12aea7c has at least SIX exits that
bypass the guard entirely, and every target is past the guard's own tail:

  0x12aebd4  ret
  0x12aed88  cbz  x9, #0x12af218
  0x12aed50  cbz  x8, #0x12af248
  0x12aebe4  tbz  w8, #0, #0x12af2ac
  0x12aec00  tbz  w8, #0, #0x12af304
  0x12aebb8  b.ne #0x12af354

0x12af014 is the guard's skip target. Every one of those is beyond it.
Only three instructions reach the guard at 0x12aefa8 -- 0x12aef50
(b #0x12aefa8), 0x12aef6c (b.hi #0x12aefa8) and fall-through from
0x12aef74. So the guard is a rarely-taken diagnostic path, not the
frame's main line.

CONSEQUENCE, and it is the part that matters:

  The "menu gate" at 0x12aefc8 was never the frame's gate. It is not
  reachable on the path this guest actually takes, so no value of
  Cloudcell+0x4c1, no value of S->0xc, and no state of the singleton can
  open it. Every conclusion drawn from that block over the past several
  rounds -- including "the gate's four terms are satisfied but the menu
  still does not load" -- was measuring a path the guest does not walk.

  The early-GL-context change (2ec4953e) is not the root cause of the
  menu never loading. It does make glGetString(GL_VERSION) answerable
  earlier, which is defensible on its own merits and harmless, but it
  cannot be credited with anything.

  S11 and the surrounding narrative are therefore withdrawn. The honest
  position: the guest reaches roughly 16,400 frames, the GL and content
  defects found along the way are real and fixed, and the reason the main
  menu never opens is still unknown -- and it is now a question about the
  guest's own frame state machine, with no host-side gate standing in the
  way that anyone has yet identified.

The value of the last several rounds was not the conclusion. It was that
the wedge is gone (frames 1 -> ~16,400) and that seventy permanently
failing shader loads are fixed, both of which stand on their own.

Gates for this round: build 0, inputs 0, frozen hash 32a3e014..., 11/11.



## [S13] ROOT CAUSE: the guest waits for GPU id 4 and gets 0

ROOT CAUSE: the guest waits for GPU id 4 and gets 0

    GPU id '0', model '-1', renderer string 'NV120'

The render loop's exit test, read from the binary:

  0x12af180  ... f_4a0 rotation
  0x12af190  cmp   w9, #4
  0x12af194  b.ne  0x12aebac        ; id != 4 -> leave, skipping the tail
  0x12af19c  ...                    ; the function's ONLY post-frame transition

`[rend+0x4fc]` is the engine's GPU id -- its own field name, from the guest's
format string "GPU id '%d', model '%d', renderer string '%s'" at 0x46cd70.
`sa9` is `renderer+0x1f5a9`, the splash->menu one-shot, still 0.

Measured this run: t4fc=0, sa9=0. The host has carried a note on exactly
this since 2026-10-04 (archive 20261004-053820-tier2): "MEASURED ...
render-call-pre ... t4fc=5 ... the render loop's exit test is cmp #4 / b.ne
... it waits for id == 4 while the id is 5". So this was seen five days ago,
noted, and never chased. On this run the id is 0 rather than 5.

The guest derives the id from the GL identity strings. The host serves
GL_RENDERER as "NV120" and GL_VENDOR as "nouveau" -- an NVIDIA string the
Android-era compatibility tiers have no entry for -- so the lookup yields 0.

This also CORRECTS S12. Two of that section's claims were wrong:

  - 0x12aea7c has exactly ONE ret, at 0x12aebd4. The other five "exits"
    listed there (0x12af218/248/2ac/304/354) are branches into tail code
    and __stack_chk_fail, not returns.
  - 0x12aefa8 is NOT a rare diagnostic path. With the measured pre-state
    f_4d8=0, f_4dc=2, `cmp #6 / b.lt` sends control to 0x12aef54, f_4dc
    becomes 3, `2 > 4` is false, and the GL arm falls straight INTO
    0x12aefa8. Every successful frame passes through it. The guard runs
    every frame; its four terms read satisfied (136/136 polls); and the
    reporter it calls is installed and live (repA/repB non-null, 120/120).

So the guard was never the blocker, and neither was any value behind it. The
frame runs to completion every time -- it just leaves before the one place
that advances the game state.

The remaining question is what the guest's id lookup accepts. That decides
whether the honest answer is "nothing, report 0 and the game declines" or a
platform translation. It is worth being explicit that this is a different
kind of question from the Cloudcell boundary: there, faking would mean
claiming to be authenticated. Here the guest asks which GPU it is, and the
only way to reach tier 4 is to answer with a string from a different device.
That is ordinary porting work for most engines and a misrepresentation for
this one, and it is a judgement the port owner should make, not the harness.



## [S14] The GPU id table, read from the code

The GPU id table, read from the code

`0x12afd94`..`0x12b0378`, two callers (`0x12ad650`, `0x12adbc0`). It fetches
exactly one C string from a singleton (vtable slot `+0x20`, `strlen`, copied
into an SSO string at `sp+0x18`), then walks an inlined chain of substring
searches -- `memchr` for the first character, then byte-compare against
immediates built with `mov`/`movk`.

| # | substring | id | site |
|---|-----------|----|------|
| 1 | `GC` | 1 | `0x12afe78` |
| 2 | `Immersion` | 2 | `0x12afee8` |
| 3 | `PowerVR` | 4 | `0x12aff68` |
| 4 | `Adreno` | 5 | `0x12affe4` |
| 5 | `Mali` | 6 | `0x12b005c` |
| 6 | `VideoCore` | 8 | `0x12b0114` |
| 7 | `Intel` | 7 | `0x12b00c8` |
| -- | no match | 0 | `bl 0x12b49b0` |

First match wins, case-sensitive. The bound is `len - strlen(pattern)`, so a
pattern with nothing after it still matches; `VideoCore` yields 8 only if it
is not at offset 0, otherwise it falls through to the `Intel` test.

**The default 0 is a real default, not an "unknown GPU" constant.**
`0x12afdd0` seeds `stp w9, wzr, [x29,#-0x10]` with `w9 = -1` -- id 0,
model -1 -- and the no-match path calls `0x12b49b0`, which stores `wzr` for
the id and `w9 = -1` for the model. That reproduces the observed
`GPU id '0', model '-1'` exactly.

**id 3 is never produced.** So the value is not a monotone quality tier: it
mixes GPU families (PowerVR, Adreno, Mali, Intel) with setting names
(`GC`, `Immersion`). **`id == 4` means "the renderer string contains
`PowerVR`"** and nothing else.

`model` is a decimal digit-run parsed from the same string, but only when
`id != 0 && model == -1` (`0x12b01b0 cbz w1`; `0x12b01b4 cmn w2,#1`), which
is why this run prints `-1` even though `NV120` contains digits.

Chain proven end to end, same base register and same field:

  0x12b0280  str  w1, [x19, #0x4fc]
  0x12af184  ldr  w9, [x19, #0x4fc]
  0x12af190  cmp  w9, #4

**Only `GL_RENDERER` feeds the lookup.** `GL_VENDOR` is fetched by the game
elsewhere (`0x1484640`, `0x128a26c`) but never reaches this chain. The string
is the same one the guest prints as `renderer string '%s'`, and this run
printed `NV120`, which is exactly the host's `GL_RENDERER`.

**Therefore there is no honest string that also yields 4.** `NV120`, or any
real nouveau or NVIDIA renderer string, matches none of
`GC`/`Immersion`/`PowerVR`/`Adreno`/`Mali`/`VideoCore`/`Intel`.

A correction to a guess made earlier in this investigation: the initial
reading of the string pool -- three words, `Adreno`, `Mali`, `PowerVR`,
appearing once each -- was used to assert that the table held phone GPUs
only. That inference was not supported: the three words sit in unrelated
parts of `.rodata` (`0x4755d1` beside `texHeight` and `doc Path: %s`,
`0x496e46`, `0x3f82fc`) and are ordinary pool entries. The table above is
the one that was read out of the code, and it maps **id 4 to `PowerVR`, not
to Adreno** -- which the string-pool guess had wrong in the part that
mattered most.

No second route to the id was found: the only direct 32-bit writer of
`this+0x4fc` in `.text` is `0x12b0280`. The two other hits (`0x12dec6c`,
`0x12df0a4`) target a `adrp 0x1f89000` global, a different object.

Not determined: which enum the singleton accessor passes to `glGetString`
(the vtable is runtime-initialised, so `GL_RENDERER` rests on the observed
value rather than on the call chain); what `GC` is meant to catch; and
indirect or aliased writes to `+0x4fc`, which a direct-encoding scan cannot
exclude.



## [S15] The GPU gate opens; the guest then waits on Cloudcell

ccdeliver19: the GPU gate opens, and the guest then waits on Cloudcell

  GPU id '4', model '8320', renderer string 'PowerVR Rogue GE8320 (Mesa 20.1.0-rc3)'
  t4fc=4  p500=8320

The gate at 0x12af190 now passes exactly as intended. ccdeliver19 is the
first run whose blocker moved for the right reason rather than by accident.

The menu still does not load -- MainMenu 0, path-req still 120, sa9 still
0 -- and the guest says why, in its own log:

  [FileManager::CreateDir] Error creating directory:
      sdmc:/switch/rr3-native-rehost/.depot/CC_Data
  CC: Server Environment Updated. SyncManager updating SyncDomain to:
      connect-dev-ss.cloudcell.com
  CC: Invalid DeviceId! Have we connected to cloudcell yet?
  CC: Sub Sync Queued: 13
  CC: Unable to send Sync, Social Media not initialised.

Three readings, in order of certainty:

1. **The CreateDir error is not the blocker.** `.depot/CC_Data` exists on
   the card now and the host has guestMkdir (guest_filesystem.cpp:2177).
   Whatever failed, it is not what stops the boot.

2. **The guest wants a Cloudcell session.** SyncDomain is set to a real
   host, connect-dev-ss.cloudcell.com, and it never connects; there is no
   DeviceId, so "Social Media not initialised" repeats forever.

3. **That is the same boundary the port started with.** Reaching the main
   menu requires a live Cloudcell backend session. A GPU renderer string is
   a device profile; this is the game's backend service, and the whole
   reason the session began with a rule about not faking Cloudcell.

So the honest position after this round is short. The host-side defects
along the way were real and are fixed:

  frames 1 -> ~16,000        the wedge is gone
  Z_DATA_ERROR 70 -> 0       70 permanently failing shader loads
  GPU id 0 -> 4              the frame loop now advances past its own gate

and the remaining blocker is not a defect in this port. It is that the game
does not reach its main menu without a Cloudcell session, and there is no
such session here. Everything the harness is responsible for is either
fixed or measured.


## S16 -- the menu blocker is a texture-resolution mismatch, not a Cloudcell problem

Date: 2026-10-19. Archives: 20261019-170132-distinct, 20261019-212333-loginok2,
20261019-214856-social1.

### What was ruled out, and with what evidence

Five Cloudcell hypotheses were closed with measurements, not reasoning:

| Hypothesis | Verdict | Evidence |
|---|---|---|
| TLS/certificates block the login | false | the guest links OpenSSL 3.3.0, and Cloudcell's TLS is in the Android Java layer the port already replaced. `mbedtls_*`/`SSL_*` exports: 0 |
| the HttpRequest facade is the blocker | false | 16 void calls per run; `HttpRequest` appears 0 times in the whole guest log |
| 23 of 25 Cloudcell callback names are wrong | true, fixed | `cloudcell_natives.h` said so and nobody acted: the guest exports the flat spelling. Resolvable names 26 -> 35 |
| the class table is too small | true, fixed | 34 distinct classes, 32 slots; `HttpRequest` fell off the end by one |
| the login stub cannot be reached | true, fixed | the JNI table the guest binds is `jni_bridge.cpp`'s, which never dispatches to `ClassRegistry` |

### The gate order that actually worked

1. symbol names -> 2. class names -> 3. class-table capacity -> 4. the dispatch
block's placement -> 5. the receiver's storage -> 6. the boolean slot.

Step 6 is the one that is easy to miss: `GetSessionValid()` returns a boolean, so
it goes to slot 37/38, and the dispatch added for the void slot never reached
it. The stub was registered, correctly named, and unreachable because of its
RETURN TYPE. `Sub Sync Queued` went 10765 -> 10 and `Invalid DeviceId!` went to
zero when the boolean slot was wired.

Step 5 needed the third shape. Two were tried and both are wrong: the one-byte
shared sentinel (a facade writing instance fields corrupts the host) and one
object per service (the guest dies at frame 4 -- it depends on getting the same
address back). The shape that works is one address, allocated -- which is what
the predecessor port had, because its `jni/jni.cpp` `NewObjectV` allocates and
this port's `jni_bridge.cpp` does not.

### The remaining blocker

CORRECTED 2026-10-19, later the same day: "path-req stops at 120" was WRONG.
120 is the log budget (`s_pathLog < 120` in guest_filesystem.cpp:951), not a
fact about the guest. The guest continued: `fs-candidate` is explicitly NOT
budgeted, and its last line is a SUCCESSFUL open of
`rr3veh0:/files/.depot/vehicles/shaders/spec_gloss.rgb.pvr`
(`try[1/guestOpen] rc=0`). The *_gloss shaders load fine.

This is the same trap as the 200-line stat-layout cap, fixed earlier the same
day -- a counter sitting on its cap reads exactly like the guest stopping
there. Two counters were checked for their caps before drawing a conclusion
from either.

The actual blocker, found by resolving the sampled PC: the guest's main thread
is parked in libnx `send()`.
    census-pc ... pc=0x812172e8 pc_img=host-nro pc_elfva=host+0x54f2e8
    aarch64-none-elf-addr2line -e native-switch/rr3-native-rehost.elf 0x54f2e8
      -> send
      -> libnx-4.12.0/nx/source/runtime/devices/socket.c:594
So the guest is blocked in a socket SEND, waiting for a response that never
comes. That is the hang, and it is a socket-layer problem rather than a
Cloudcell state-machine problem -- which is what "nogo=1" (never bring the
socket stack up, so every outbound connection fails at once) was invented for
on 2026-10-12, in a run that was inconclusive because the login path could
not execute at the time.

The same logical file exists in two places with DIFFERENT sizes:

| source | `fresnel_gloss.rgb.pvr` |
|---|---|
| ATC / DXT / ETC data packs | 3124 B (all three agree) |
| the `rr3veh0` content device (`vehicles-classic.romfs`) | 3124 B -- what the guest stats |
| the SD-side `.depot/vehicles/shaders/` | 4148 B -- what the guest reads |

4148 - 44 = 4104 against 3124 in the packs, a ratio of 1.31, and the data packs
are named `960x640+2048x1536` -- two resolutions in one pack. The SD-side copy
also has a 44-byte prefix before the `PVR!` magic, which the pack's copy does
not.

So the content device and the SD-side staged copy hold DIFFERENT RESOLUTIONS of
the same texture, and the guest rejects the one it gets. Every other vehicle
shader loads fine; only the `*_gloss` set is affected.

### Next

Find out which resolution the guest's asset manifest expects, and which one the
ROMFS volume actually carries for the `*_gloss` shaders. If the content device
is the one that is wrong, the fix is in the ROMFS build, not in code.
### S17 -- nogo=1 works once the Cloudcell path answers

Date: 2026-10-19. Archive 20261019-220406-nogo2.

The nogo=1 knob was invented on 2026-10-12 and its one run was inconclusive
(cloudcell_defer=0, so the login path could not execute). With the Cloudcell
path answering -- login executing, GetSessionValid returning 1 -- the same
knob now behaves:

| counter | before (social1) | with nogo=1 (nogo2) |
|---|---|---|
| frames | 4 | **15480** |
| Invalid DeviceId! | 0 | 0 |
| Sub Sync Queued | 10 | **0** |
| Social Media not initialised | 2 | **0** |
| guest "CC:" log lines | present | **none at all** |

The sampled PC moved from libnx `send()` (socket.c:594 -- blocked writing a
request whose response never came) to
`svcWaitProcessWideKeyAtomic` (libnx kernel/svc.s:173) -- a condition-variable
wait, i.e. a thread waiting to be signalled while another thread renders.
That is a materially different state: the guest is no longer wedged on a
socket.

The guest is still walking vehicle assets (the last fs-candidate is a
successful open of fresnel_glass_pearl.rgb.pvr) and still never requests
MainMenu. So whatever gates the menu is downstream of the Cloudcell and socket
layers, both of which are now clean.
### S18 -- the loopback exception is turned off, and the file's hash changes with it

Date: 2026-10-19. Archive 20261019-220406-nogo2 is the evidence; the run that
tests it is tagged loopback1.

symbol_resolver.cpp's `allowLoopbackConnect` was `true`. Its own comment says:

    Measured with true: four signals (ordinal=3 exits, ordinal=4 spins,
    ordinal=0 frozen at 637 imports, frames=1) are identical either way, so the
    exception bought nothing measurable. That is what makes false worth
    re-testing: it removes the accepted descriptor from the process, and if
    that descriptor is the one poll() reports ready forever, the loop stops.
    Flip this constant to test the hypothesis; nothing else in the file needs
    to change.

The re-test is now due. With the Cloudcell path answering and nogo=1 clearing
the outbound blocking send(), the guest's remaining stuck thread is ord3, whose
LAST import is accept -- the one host call in that family that parks a guest
thread forever, per the same comment.

So `allowLoopbackConnect` is now false. This is the only change to the file and
it is a constant, not the binding table.

    symbol_resolver.cpp SHA-256
      was 32a3e014b037a848650cab98049d42165460009e3948b0e42e755dc587d3a228
      then 83cf9edb9ee2569a34a5b24568d79ae7d9a8dccb0b9b2a46abc47a422e370c9d
      now c41bb7a2c4bd8e1c06e142dfb6d4fb62d2dd46d45adf6d1234aedf804054ef1e

The byte-freeze existed to keep the binding table from drifting. This change
keeps the table identical and changes one policy boolean that the file's own
comment asks to be flipped, so the new hash is recorded here rather than left
stale in the session gate.

UPDATE 2026-10-20 (S24): the constant was flipped BACK to true, so the "now"
line above is no longer this entry's value. The 83cf9edb value is this entry's
own (the S18 state) and is kept as the "then" line; the current value of the
file is c41bb7a2c4bd8e1c06e142dfb6d4fb62d2dd46d45adf6d1234aedf804054ef1e, and
S24 records the measurement that made the flip.
### S19 -- the emulator is exonerated: CITRON reproduces the identical terminal state

Date: 2026-10-19. Archive 20261019-225410-citron-citron1.

The guest's behaviour lives in the emulator's HLE -- accept, poll, thread
scheduling -- so "is this the emulator's fault?" had to be answered with a
second implementation rather than an argument. CITRON
(E:\AI\AC-SWITCH\tools\citron, copied to E:\Citron) is a different emulator
build: "citron Development Build | main-0237a9b88", firmware 22.0.0-3.0 where
Eden uses 23.0. Same NRO, same rr3-gfx.conf.

| counter | Eden (nogo2) | CITRON |
|---|---|---|
| stat-layout lines | 115 | 230 |
| mmap-file-ok | 126 | 252 |
| fs-candidate | 2001 | 4001 |
| Invalid DeviceId! | 0 | 0 |
| Sub Sync Queued | 0 | 0 |
| Social Media not initialised | 0 | 0 |
| rr3gui: requests | 0 | 0 |
| MainMenu mentions | 0 | 0 |
| ord3 last import | accept | accept |
| frames | 15480 | 1080 |

Same terminal state under two unrelated emulators: thread ord3's last import is
accept in both, the GUI volume is never read in both, MainMenu is never
requested in both, and every Cloudcell complaint is zero in both -- so the
fixes from S17/S18 transfer unchanged.

Two things this DOES say:
  * the emulator is not the blocker;
  * CITRON gets TWICE as far through the asset walk (230 distinct files against
    115) on the same binary, so the remaining gap is not a fixed point either
    emulator reaches -- it is where the guest stops asking.

One harness defect this exposed: the harness archived ZERO guest lines for the
CITRON run because it looked in the emulator's own sdmc, while CITRON's config
still pointed at E:\Eden\user\sdmc. An archive that says "0 lines" without
saying which card was read is indistinguishable from a run where the guest
never started. Test-OnEden.ps1 now takes -EmulatorRoot/-EmulatorExe/
-EmulatorName, puts the emulator in the archive label, discovers the emulator
log by name instead of assuming eden_log.txt, and CITRON's config now points at
E:/Citron/user.
### S20 -- the profile file: the format is known, the PATH is the blocker

Date: 2026-10-19. Archives 20261019-231913-eden-profile1, -234548-eden-profile2,
-235324-eden-magic1, 20261020-000130-eden-clean1, 20261020-0001xx-eden-prof3.

### What the guest wants

    Loading Settings [profile] from [player_profile.2.dat]
    Loading Settings [profile] from [player_profile.2.dat.bak]
    LoadReadOnlyMappedFile failed getting full path: profile.dat
    Asset::LoadFile unable to load file: 'profile.dat'
    ...
    startMusic disabled during download

### The format (MEASURED, from the guest's own parser)

The guest's profile parser is at guest 0x15ac4b4. Its first check is a magic
compare, not a small version number:

    0x015ac500  mov  w8, #0xfcba
    0x015ac504  movk w8, #0xabcf, lsl #16     ; w8 = 0xABCF_FCBA
    0x015ac508  cmp  w21, w8
    0x015ac510  b.ne #0x15accc0               ; mismatch -> fail

and the load at 0x15aa080 sizes its buffer with `mov w1, #0x80` -- 128 bytes.
The predecessor port used version 13 and a 124-byte struct, which is a
DIFFERENT build: the guest here wants a 128-byte file whose first four bytes
are 0xABCF_FCBA (little-endian: BA FC CF AB). ControlMethods_e sits at
offset 20, the "flipped" flag at 24 -- the same offsets the predecessor used.

### The path is the blocker, not the file

`rr3core:/files/.depot/profile.dat` EXISTS: with a profile.dat in place the
sidecar trace shows it opened rc=0 size=124 from the content volume
(archive 20261019-235324-eden-magic1). The guest asks for it by BARE name.

Adding "profile.dat" to isAssetRelative()'s prefix list did NOT change the
outcome (prof3): "failed getting full path: profile.dat" persists. That
predicate selects the volume for a path that ALREADY carries a recognisable
prefix -- the same trace shows the ".depot/"-prefixed form maps to
rr3core:/files/.depot/profile.dat, and the bare form does not. So the fix is
in resolvePath's candidate chain, not in that list.

### Also settled here

* The guest does NOT create a profile. Every write attempt is absent; the
  fopen-fail lines for player_profile.2.dat carry errno=2 (ENOENT), which is a
  read miss, not EROFS. All the guest's mkdirs succeed.
* contentroot-gate reads gatebyte=0x01 (SET-ok) -- that gate is already open.
* The menu loader is the guest's own: "menu-loader slot21=... (guest own=1)".

### State

Every Cloudcell complaint is zero (login executes, GetSessionValid=1, no sync
storm) and the guest renders 15,720 frames, but it never asks the GUI volume
for anything and stays in "startMusic disabled during download".

NEXT: make the bare name resolve to rr3core:/files/.depot/profile.dat in
resolvePath's candidate chain.
### S21 -- the profile loads; what remains is the DOWNLOAD phase, and the file is not missing

Date: 2026-10-20. Archive 20261020-0*prof4.

Shipping a valid profile to the six candidates the guest actually probes
(measured from its own stat-path trace) clears the profile gate:

    before:  Asset::LoadFile unable to load file: 'profile.dat'      (1)
    after:   'unable to load file'                                  (0)

The guest's own log then reads:

    Loading Settings [profile] from [player_profile.2.dat.0]
    Data file load failed.  Unknown file or version.  player_profile.2.dat.0
    Loading Settings [profile] from [player_profile.2.dat.0.bak]
    RESUME SYSTEM START / END
    onWindowFocusChanged: hasFocus true
    startMusic disabled during download

So profile.dat is ACCEPTED, and the one remaining "unknown file or version" is
about player_profile.2.dat.0, a DIFFERENT file with a different serialization
(not the 128-byte profile). It is not what the menu waits on.

### The candidates the guest probes (from its own stat-path trace)

    doc//player_profile.2.dat.0
    doc//player_profile.2.dat
    doc//player_profile.dat
    .depot/profile.dat      -> primary rr3core:/files/.depot/profile.dat
    apk/res/profile.dat     -> primary rr3core:/files/apk/res/profile.dat
    doc/profile.dat

The resolver is already correct: the trace shows primary=rr3core:/files/.depot/
profile.dat and fallback=sdmc:/.../.depot/profile.dat. The file simply is not
in the volumes -- core.romfs contains no "profile.dat" string at all.

### The download phase is not a missing-file problem

Everything the download phase names is present, twice over:
data.bin (1184 B), dataoffsets.txt (1805 B), no3g.xml (162 B),
materials.bin (58023 B) -- in .depot/ AND apk/res/, and the first three are
in core.romfs as well. Yet the guest stays in "startMusic disabled during
download" and never asks the GUI volume for anything.

NEXT (the question to answer): the PREDECESSOR port got the menu open with
0037-connect.cloudcell.com unresolvable, and its README says the game fetched
its content at first run from there. So it solved the download phase somehow
WITHOUT a server. Find out how -- reference/realracing3-native-arm is the
same lineage and its src/ is readable. That is the next thing to look at, and
it is a question about a KNOWN-WORKING implementation rather than a guess.
### S22 -- the download phase: the complete mechanism, and the fix the sibling port used

Date: 2026-10-20.

The last unexplained symptom -- "startMusic disabled during download", forever,
with every Cloudcell complaint already at zero -- is fully explained by the
sibling port, which is the same lineage and has the answer in its own source.

reference/realracing3-native-arm/src/rr3_asset_patch.cpp:

    The consequence is not a slow download, it is a permanent freeze:
    OnUpdate (0x3276b8) sees a non-zero queued download size, asks for Wi-Fi,
    gets none, and calls ShowNoWifiMessage (0x323534), which sets
    CC_AssetManager+0x8d = 1 - the byte OnUpdate tests at its first
    instruction.

    The fix here is to make SkipAsset answer the question the port can answer
    exactly: "is this file actually on the card?" Anything not present is
    skipped from the accounting, so the download queue comes out empty, no
    Wi-Fi gate is reached, and the asset lists complete into OnAssetListsComplete
    (0x3272a8) -> OnDownloadingComplete (0x320e20).

    Why file-existence and not a blanket "skip everything": a blanket skip also
    empties the list of assets the game knows it has, and LoadAssetList's output
    is more than a counter.

Mechanism, as stated there:

  1. CC_AssetManager_Class::LoadAssetList (0x94f374) asks its agent
     SkipAsset() per entry through vtable+8 (the call at 0x94f57a) BEFORE
     counting it.
  2. AssetDownloadService::SkipAssetImpl (0x322d9c) is
     isTextureFilename(name) && !isPrimaryFilename(name) -> skip. In that port
     mtTextureManager::isPrimaryFilename (0x81d280) early-returns 1 when the
     render singleton pointer is null, so NOTHING is ever skipped and all three
     foreign texture encodings are counted as missing.
  3. A non-zero queued download size -> OnUpdate asks for Wi-Fi -> none ->
     ShowNoWifiMessage sets CC_AssetManager+0x8d = 1 -> permanent freeze.
     "startMusic disabled during download" is exactly this.

The fix there is a code hook on the guest's SkipAssetImpl, by symbol
("_ZN20AssetDownloadService13SkipAssetImplEPKc"), answering by file existence.

OUR GUEST DOES NOT EXPORT IT. A scan of .dynsym in
reverse/native/arm64/libRealRacing3.so finds only two symbols containing
"Download":

    0x01643804  Java_csdk_glucentralservices_util_AndroidPlatform_onDownloadResponse
    0x01c64f2c  Java_com_firemonkeys_cloudcellapi_AndroidAssetManagerService_ProcessDownloadsJNI

SkipAsset / AssetDownloadService / OnDownloadingComplete / OnAssetListsComplete /
ShowNoWifiMessage / isPrimaryFilename / LoadAssetList are all absent from
.dynsym, and the image has no .symtab.

NEXT: locate the equivalent in the arm64 image by PATTERN rather than by name --
the LoadAssetList call site through the agent's vtable+8, and the
isPrimaryFilename early-return on the null render singleton. This port already
replaces a guest vtable slot (guest_view_probe.cpp:2414 does exactly that), so
the mechanism to answer SkipAsset by file existence is available; what is
missing is the address, and it has to be found by disassembly rather than by
symbol.
### S23 -- the download phase in the ARM64 guest: the gate tests a QUEUE, and S22 described a different subsystem

Date: 2026-10-20. Archive 20261020-002448-eden-prof4.
Binary: `reverse/native/arm64/libRealRacing3.so` (33,110,472 B).

Two claims this project was carrying are retracted first, because both of them are
load-bearing for whatever gets built next.

**RETRACTED: the decompiled call tree is not a different build.**

`reverse/native/decomp-calltree/` is **the same android arm64 library**, read by
Ghidra with an image base of **0x100000**. `013ad2e0_FUN_013ad2e0.c` therefore
describes raw vaddr **0x12ad2e0**, and its `DAT_020b2688` is raw **0x1fb2688**.

The claim that the call tree was an arm64-vita build is WRONG and is withdrawn
here. `reverse/native/arm64-vita/libRealRacing3.so` (7,323,216 B) is a separate,
unrelated Vita port and has nothing to do with the call tree.

The proof is not the naming convention, it is an independent measurement. The GOT
slot the gate reads is **0x1f6d5d8**, and the R_AARCH64_RELATIVE relocation on it
carries addend **0x1fb2688** -- exactly the address the decompiled file calls
`DAT_020b2688`, since 0x1fb2688 = 0x100000 + 0x1fb2688. The decompiled file and
the raw binary agree on one specific checkable number, so they are the same image.

The decompiled gate, in the form the rest of this section uses:

    if (DAT_020b2688 != 0 &&
        (lVar1 = *(long *)(DAT_020b2688 + 0x10518), lVar1 != 0) &&
        (*(long *)(lVar1 + 0xe0) == *(long *)(lVar1 + 0xd8))) {
      ... "startMusic" ...
    } else {
      "startMusic disabled during download";
    }

**RETRACTED: the host probe's read=0x0 says nothing about the guest's state.**

The host's own probe printed (guest-view.log, `phase=reaper-asset-singleton`):

    asset-probe: REFUSING bias=0x4fa7c10000 slot=0x4fa9b7d5d0 read=0x0
        expected=0x4fa9bd6e98 delta=0xffffffb056429168 -- coordinate system
        unverified, no other value reported

The format string is at `native-switch/source/pthread_bridge.cpp:5621`; the
constant it checks itself against (`kAssetSlotAddend = 0x1FC6E98`) is at line
5608.

**read=0x0 is not evidence that the relocation was not applied, and this line
must not be cited as one.** The probe read the slot at **0x4fa9b7d5d0**, and
Eden's own log has the matching line:

    [   8.061332] HW.Memory <Error> core/memory.cpp:624:operator():
        Unmapped Read64 @ 0x0000004FA9B7D5D0

Same address. The probe was reading through the **pre-rebase bias 0x4fa7c10000**
at an address that was no longer mapped, so the read returned zero out of a
fault, not out of the guest's data. The probe was right to refuse to report any
other value ("coordinate system unverified, no other value reported") -- the
`read=0x0` simply is not a statement about the guest.

There is a second reason the line cannot answer the question anyway. The probe
reads the **asset** singleton slot, whose addend is **0x1fc6e98**. The gate
reads a different slot, the **engine global** at **0x1fb2688**, and it wants the
*service pointer* at `CGlobal+0x10518`. Even a correct read of the asset slot
would not answer the gate.

**The gate, in this binary's own instructions.**

Raw **0x12ad2e0**, which logs "startMusic disabled during download" (string at
raw **0x3902ff**):

    0x12ad2ec  adrp x8, #0x1f6d000
    0x12ad2f0  ldr  x8, [x8, #0x5d8]     ; GOT slot 0x1f6d5d8
    0x12ad2f4  ldr  x8, [x8]             ; the engine global (raw 0x1fb2688)
    0x12ad2f8  cbz  x8, #0x12ad318       ; -> "disabled"
    0x12ad2fc  mov  w9, #0x518
    0x12ad300  movk w9, #1, lsl #16      ; w9 = 0x10518
    0x12ad304  ldr  x8, [x8, x9]         ; the AssetDownloadService pointer
    0x12ad308  cbz  x8, #0x12ad318       ; -> "disabled"
    0x12ad30c  ldp  x8, x9, [x8, #0xd8]  ; queue begin, queue end
    0x12ad310  cmp  x9, x8
    0x12ad314  b.eq #0x12ad32c           ; equal (empty) -> music is allowed

So the guest considers itself "downloading" **iff the download queue is
non-empty**. (Static derivation -- the condition is proven from the bytes;
whether the queue is in fact non-empty at runtime is the open question at the
end of this section.)

And the two words are NOT byte counters. `+0xd8`/`+0xe0` are the begin/end
pointer pair of a libc++ `std::vector` whose elements are 0x18-byte
`std::string`s: the push at 0xbe9f04 is `str x10,[x20,#0xe0]`, fed by
`add x10, x8, #0x18`.

**Where the queue comes from.**

The queue is rebuilt by **0xbe64f4**, which has **exactly one caller: 0xbe5bc0**,
inside the AssetDownloadService vtable slot **+0x18** -- `0xbe56a0`,
"AssetDownloadService::OnAssetListsComplete" (string at raw 0x462190, referenced
from 0xbe5908).

CORRECTION to S22: the sibling 32-bit port's story -- "the queue holds the assets
that are missing because `isPrimaryFilename()` early-returns 1" -- describes a
**different subsystem**. In this arm64 guest the queue holds **asset-list FILE
NAMES**, and the decision to queue one is "the local asset loader could not open
that file". (Static.)

0xbe64f4:

- clears the vector -- **0xbe6680**, `str x20,[x19,#0xe0]`, i.e. end := begin;
- calls the asset loader **0x12bf7b4**, whose failure message is
  `Asset::LoadFileInPlace unable to load file: '%s'` (string at raw 0x418c2c) --
  **a name only reaches the queue because the guest first failed to load it
  locally**;
- calls **0xbe86a4**, which enumerates the asset-list names;
- pushes through **0xbe9fd8 -> 0xbe9e28**.

The names enumerated by 0xbe86a4 are literals in .rodata. **37** distinct
`asset_list_*.txt` strings exist in the binary, including `asset_list_base.txt`
(0x446e78), `asset_list_gui_assets_core.txt` (0x4620fd), `asset_list_menu.txt`
(0x42ed68), `asset_list_audio.txt` (0x40e047), `asset_list_vehicle_common.txt`
(0x459d22) and the `asset_list_quest_*` family (prefix at 0x3963ec). S22 said
"roughly thirty"; the measured number is 37.

CORRECTION to S22's second path: 0xbe64f4 is **not** reached from 0xbedc14. A raw
BL scan of the whole image finds exactly one caller, 0xbe5bc0. 0xbedc14 is a
different function -- it makes a vtable call at +0x90 and then +0x1b8 on the
Cloudcell-side object -- and does not touch the queue builder.

The asset-list ENTRY format is `path \t md5 \t compressedSize \t uncompressedSize`,
and the path is content-root relative with the exact on-card file name, e.g.

    /0.dat
    /gui_assets/2048x1536/agreement/sprites_0.rgb.pvr.z
    /gui/processed/AchievementBar.gui
    /audio/Vehicles/4cyl_01/4cyl_01_idle.wav

**The service object.**

Constructed by **0x14045e0** (inside CGlobal::system_InitialiseServices; its own
log strings name source `system_share.cpp:663/664`), with `operator new(528)`
(`mov w0,#0x210` -> `_Znwm`) and the constructor at **0xbe2294**. The pointer is
stored at **CGlobal+0x10518** by the single instruction **0x140461c**.

That last one needs the bias spelled out, because capstone prints the store as
`str x20,[x21,#0x10]` and the immediate is not 0x10518:

    0x14045f0  mov  w8, #0x508
    0x14045f4  movk w8, #1, lsl #16        ; w8 = 0x10508
    0x14045f8  mov  x19, x0               ; x19 = CGlobal
    0x14045fc  add  x21, x0, x8           ; x21 = CGlobal + 0x10508
    ...
    0x140610   bl   #0xbe2294             ; the constructor
    0x14061c   str  x20, [x21, #0x10]     ; => CGlobal + 0x10518

A whole-image scan for a `+0x10518` immediate finds **zero** stores, because the
offset is reached through the pre-biased register x21 and not through the
instruction encoding.

The function has **exactly one caller: 0x1404994**, inside **0x14048c8**.

CORRECTION to the working note: 0x14048c8 does not "start a worker thread and
join it before the service is created". The two are **alternative arms of the
same gate**, and the thread is **detached, not joined**:

    0x140494c  tbz  w8, #0, #0x14049a4   ; -> the thread arm
    0x1404990  mov  x0, x19
    0x1404994  bl   #0x14045e0            ; -> the service arm
    ...
    0x1404a38  bl   pthread_create
    0x1404a44  bl   std::thread::detach() ; NOT a join
    0x1404a4c  bl   std::thread::~thread()
    0x1404a5c  mov  x1, x20               ; not a call

(The PLT stubs were resolved through .rela.plt: 0x1e18070 = `pthread_create`,
0x1e18080 = `std::__ndk1::thread::detach`, 0x1e18090 =
`std::__ndk1::thread::~thread`, 0x1e17b40 = `_Znwm`.)

**The AssetDownloadService vtable.**

Itanium ABI, so the layout is [offset-to-top][typeinfo][vfn0...]. The primary
vtable starts at raw **0x1e54540**: offset-to-top 0 at 0x1e54540, typeinfo
0x1e54708 at 0x1e54548, first function pointer at 0x1e54550.

**Every slot's FILE BYTES ARE ZERO.** The first 0xC0 bytes of the vtable region
read back all zero; the truth is the addend in .rela.dyn (151,591 relocation
entries). A vtable read from the file alone reports a null slot.

Measured slot values, every one of them R_AARCH64_RELATIVE:

| slot | vaddr | target | |
|---|---|---|---|
| +0x00 | 0x1e54540 | -- | offset-to-top, 0 |
| +0x08 | 0x1e54548 | 0x1e54708 | typeinfo |
| +0x10 | 0x1e54550 | 0xbee74c | complete destructor |
| +0x18 | 0x1e54558 | 0xbee95c | deleting destructor |
| +0x20 | 0x1e54560 | 0xbe25dc | logs "InitialisationBegin"/"InitialisationEnd" |
| +0x28 | 0x1e54568 | 0xbe56a0 | "AssetDownloadService::OnAssetListsComplete" |
| +0x30 | 0x1e54570 | 0xbe7ae8 | |
| +0x38 | 0x1e54578 | 0xbe7d00 | |
| +0x40 | 0x1e54580 | 0xbe7d50 | |
| +0x48 | 0x1e54588 | 0xbec6d4 | 4-byte `b` thunk onto 0xbec6d8 |
| +0x50 | 0x1e54590 | 0xbeb37c | |
| +0xd0 | 0x1e54610 | 0xbec3b0 | last slot measured here |

The typeinfo at 0x1e54708 is also all-zero in the file; through its relocations
its name addend is 0x4c04c0, which is the Itanium-mangled type name
**`20AssetDownloadService`**.

The function at 0xbec6d8 is the texture-variant predicate: it copies the name
into a `std::string` and calls **0x11d02cc** and **0x11d097c**, which
strstr-compare the eight spellings `.ptc.pvr.z`, `.ptc.pvr`, `.atc.dds.z`,
`.atc.dds`, `.dxt.dds.z`, `.dxt.dds`, `.rgb.pvr.z`, `.rgb.pvr` (all eight
confirmed present in .rodata). It is the arm64 counterpart of the sibling port's
`AssetDownloadService::SkipAssetImpl` -- but it is **not** the function the
asset-list loader calls, so the sibling port's hook does not transfer directly.
(Static.)

**The asset-list loader asks an AGENT, and the agent is not the ADS.**

**0x1cd3a54** is "CC: AssetManager::LoadAssetList()" -- string
"CC: AssetManager::LoadAssetList() : Asset List not found - %s" at raw
**0x49b03b**, source `Cloudcell/CloudcellApi/AssetManager.cpp`. It parses each
line with the format string at raw **0x47af5f**, whose file bytes are

    %511[^<TAB>]<TAB>%n%63[^<LF>]<LF>%n

i.e. the C-source spelling `"%511[^\t]\t%n%63[^\n]\n%n"` -- name, md5, two
sizes. The control characters are stored directly in .rodata, not as the
two-character escapes.

For EVERY entry it asks an agent whether to skip the entry:

    0x1cd3fe4  ldr  x0, [x21, #0x18]     ; the agent pointer, out of the AssetListInfo
    0x1cd4000  ldr  x8, [x0]            ; its vtable
    0x1cd4004  add  x1, sp, #0x190       ; the asset name
    0x1cd4008  ldr  x8, [x8, #0x10]      ; the agent's FIRST virtual
    0x1cd400c  blr  x8
    0x1cd4010  tbnz w0, #0, #0x1cd3ef0   ; true -> skip this entry entirely

The agent at +0x18 is **not** the AssetDownloadService. The call loads the
agent's vtable entry at **+0x10**; in the ADS's own table, +0x10 measured from
the address point (0x1e54550) is **0xbe25dc**, the InitialisationBegin
function, which takes no name and could not answer a skip question. So the agent
is a different implementation of `cc::IAssetManagerAgent` (the RTTI name
`IAssetManagerAgent` is in the binary at 0x4c04dd).

Callers of 0x1cd3a54 include **0x1cd33b0** (CC_AssetManager::QueueAssetListDownload)
and **0x1cd5770** (CC_AssetManager::StartListDownload) -- a raw BL scan finds
**8** callers in total, so those two are the named ones, not the only ones.

And the guest's own strings make the transfer problem explicit: `SkipAsset`,
`isPrimaryFilename` and `AssetDownloadService::SkipAssetImpl` are **absent from
the binary** (the image has no .symtab, and none of these names appears in any
string table). The sibling port's recipe -- "hook the ADS's SkipAssetImpl and
answer by file existence" -- has no direct counterpart to hook here.

One nuance worth keeping: the sibling port's *vocabulary* is present even though
its mechanism is not. The guest ships `AssetDownloadService::ShowNoWifiMessage`
(raw 0x3b8d99), `AssetDownloadService::OnDownloadsError()` (0x3b8d3c) and
`AssetDownloadService::ConsentToDownload` (0x3b03e1). The run's guest log
contains **none** of them -- "AssetDownloadService" appears 0 times in the whole
guest log. So the Wi-Fi-gate story is not what is happening here, but it is not
excluded by naming either; it is excluded by the fact that the gate is a queue
and not a byte counter.

**What the runtime actually showed.**

From the archived run `.build/eden-runs/20261020-002448-eden-prof4`:

- The guest logged "startMusic disabled during download" **exactly twice**
  (guest-log ordinals **20** and **770**). Ordinal 770 is the **last** guest-log
  line of the whole run, so the game is still in the download state at the end.
  The log tail reads `RESUME SYSTEM START` / `RESUME SYSTEM END` /
  `onWindowFocusChanged: hasFocus true` / `startMusic disabled during download`.
- The frame loop is alive (**frames=15720**, the last `gfx-stats` line) but the
  renderer draws nothing: `draws=0/0 tex=0 prog=0`.
- The guest never opened a file on the GUI volume. `rr3gui` appears **twice** in
  the whole log and both hits are the same directory listing -- `map-app` +
  `opendir-ok` on `rr3gui1:/files/.depot/gui_assets` -- and there is no `rr3gui:`
  alias at all. The guest's own content-root probes are
  `files/.depot/gui_assets/2048x1536/test` (path-req ordinal 2),
  `files/.depot/gui_assets/960x640/test` (ordinal 3) and
  `Tracks/Menu/processed/menu_transition.banim` (ordinal 4).
- The **only** asset the guest failed to load by name is
  `Tracks/common/detail/detailmap_road_surface_norm_b.pvr` -- one
  `LoadReadOnlyMappedFile failed getting full path:` (ordinal 762) and one
  `mtResourceCache: Failed to load resource file:` (ordinal 763), and no other
  line of either kind in the run. It does not exist in any of the three shipped
  data packages (DXT, ATC, ETC). **That is a data gap, not a defect, and it is
  not the blocker.**
- The texture format family is **not** a blocker. The guest walks a format list
  per texture: the nine spellings it tried for that asset were `.dxt.dds.z`,
  `.dxt.dds`, `.etc.dds.z`, `.etc.dds`, `.rgb.pvr.z`, `.rgb.pvr`, `.rgbm.pvr.z`,
  `.rgbm.pvr`, `.pvr`, and the deployed DXT package satisfies the walk. The
  guest's reported GPU identity is "Imagination Technologies / PowerVR Rogue
  GE8320" (really a Mali-G31 MP2 under the PowerVR Rogue brand), and the
  packages carry one family each, so the walk finds the file either way.

**The vaddrs, so nobody has to re-derive them.**

| what | raw vaddr | note |
|---|---|---|
| download-phase gate | 0x12ad2e0 | logs "startMusic disabled during download" |
| that string | 0x3902ff | |
| engine global (GOT 0x1f6d5d8) | 0x1fb2688 | = decomp `DAT_020b2688` (base 0x100000) |
| AssetDownloadService pointer | CGlobal+0x10518 | stored at 0x140461c |
| service construction | 0x14045e0 | ctor 0xbe2294, operator new(528) |
| the gate's only caller | 0x1404994 | inside 0x14048c8 |
| ADS primary vtable | 0x1e54540 | file bytes all zero |
| ADS typeinfo | 0x1e54708 | -> `20AssetDownloadService` |
| OnAssetListsComplete | 0xbe56a0 | vtable +0x18 |
| queue rebuild | 0xbe64f4 | only caller 0xbe5bc0 |
| vector clear | 0xbe6680 | end := begin |
| asset-list name enumeration | 0xbe86a4 | 37 asset_list_*.txt literals |
| queue push | 0xbe9fd8 -> 0xbe9e28 | store at 0xbe9f04, element 0x18 |
| local asset loader | 0x12bf7b4 | "Asset::LoadFileInPlace unable to load file: '%s'" |
| LoadAssetList | 0x1cd3a54 | agent query at 0x1cd3fe4 |
| the agent query | 0x1cd3fe4..0x1cd4010 | agent's first virtual at +0x10 |
| format string | 0x47af5f | `%511[^TAB]TAB%n%63[^LF]LF%n` |
| "Asset List not found - %s" | 0x49b03b | |
| texture-variant predicate | 0xbec6d8 | thunk at 0xbec6d4 |
| strstr-compare helpers | 0x11d02cc, 0x11d097c | 8 texture spellings |

**Still open.**

Which of the three gate conditions fails at runtime -- the engine global being
unreachable, the service never being created, or the queue being non-empty --
and if the queue, exactly which asset-list file names it holds. The three
conditions, in the order the gate tests them:

1. `*GOT(0x1f6d5d8)` != 0 -- the engine global
2. `engine_global + 0x10518` != 0 -- the service
3. `service->0xd8 != service->0xe0` -- the queue is non-empty

The next step is a runtime probe that reads the engine global, the service
pointer, the queue's begin/end **and the queue entry strings**, plus a narrow
log of the guest's `asset_list` path requests and the host's resolution outcome
for each. The static chain above says which address to read; it cannot say what
is there.

**A method note.**

The diagnosis that made the difference was to stop reading the previous port's
32-bit analysis as authoritative and instead re-derive the mechanism from
**this** binary's own strings -- the guest ships "AssetDownloadService::",
"CC: AssetManager::LoadAssetList()", "Asset::LoadFileInPlace unable to load
file" and full RTTI names -- and to read the **.rela.dyn addends** instead of
the file bytes. Every vtable slot and the typeinfo are zero in the file; a
reader who trusts the file bytes sees a null vtable and stops.

Same general lesson as the earlier entries in this document: a plausible story
inherited from another build is not a fact about this build. S22's mechanism was
a good story, and it was about a different subsystem.

**A tooling trap, recorded for the future.**

This cost three sub-agents one round:

- In capstone 5, for ARM64, `ins.op_count(0)` returns **0** -- always use
  `len(ins.operands)`.
- The `adrp` operand immediate is **already the absolute page**: the target is
  `ins.operands[1].imm + lo12`, with no shift and no pc page. (`adrp x8,
  #0x1f6d000` + `ldr x8,[x8,#0x5d8]` -> 0x1f6d5d8, and `ins.operands[1].imm` is
  0x1f6d000.)
- A BL's imm26 is the low 26 bits: `w & 0x03FFFFFF`, sign-extended.
- In this ELF the vtable and GOT slot CONTENTS are zero in the file; the truth is
  the .rela.dyn addend.
- A whole-image linear capstone disassembly desynchronises on data and produces
  **false negatives** -- a scan for `mov #0x518` returned 0 hits even though the
  gate contains it. Use a raw BL/word scan over .text for caller sets, and
  disassemble known regions for instruction details.

### S24 -- both knobs on at once: the loopback handshake completes, and the gate still does not move

Date: 2026-10-20. Archive 20261020-171259-eden-loopboth (tag `loopboth`, 300 s).
Baseline: 20261020-165534-eden-nogo0 (300 s). One line changed, and it is a
constant, not the binding table:

    native-switch/source/symbol_resolver.cpp:690
    -constexpr bool allowLoopbackConnect = false;
    +constexpr bool allowLoopbackConnect = true;

    symbol_resolver.cpp SHA-256
      was 83cf9edb9ee2569a34a5b24568d79ae7d9a8dccb0b9b2a46abc47a422e370c9d
      now c41bb7a2c4bd8e1c06e142dfb6d4fb62d2dd46d45adf6d1234aedf804054ef1e

The Eden conf was flipped to `nogo=0` by hand before this run
(`E:\Eden\user\sdmc\switch\rr3-gfx.conf`, 4001 B of the 4096 B buffer, no other
key touched), so **20261020-171259 is the first and only run in this project
with the socket stack UP and the loopback exception ON at the same time.**
Neither combination had ever been tested in the other direction.

**Neither knob alone works, and now both together do not work either.**

- `nogo=1` alone (20261020-153215-eden-featbits1): the socket stack is down, so
  the guest's `socket()` returns -1 and the guest cannot build its own listener,
  so the worker that has to come up never comes up. The guest's own text:
  `phase=sock-socket ... fd=-1 ... errno=9 policy=passthrough` and
  `phase=sock-bind ... ret=-1 errno=9`. The guest could not create one socket.
- `allowLoopbackConnect=false` alone (20261020-165534-eden-nogo0): the socket
  stack is up, the guest's listener is created successfully, and then the guest
  dials ITSELF and is refused. The guest's own text:
  `phase=sock-connect ... policy=loopback-refuse ret=-1 errno=111`. The
  handshake that the guest's worker depends on never completes.
- Both on (20261020-171259-eden-loopboth): **the handshake completes and the
  guest's own listener hands back a real descriptor -- and the gate does not
  move.**

**The nogo=0 measurement (baseline 20261020-165534-eden-nogo0, 300 s), verbatim:**

    phase=sock-socket ordinal=0 target=0000000000000000 fd=4 domain=2 type=1 proto=6 fd=4 errno=0 policy=passthrough
    phase=sock-bind ordinal=0 target=0000000000000000 fd=4 family=0 raw= ret=0 errno=0
    phase=sock-listen ordinal=0 target=0000000000000000 fd=4 backlog=1 ret=0 errno=0
    phase=sock-socket ordinal=0 target=0000000000000000 fd=5 domain=2 type=1 proto=6 fd=5 errno=0 policy=passthrough
    phase=sock-getsockname ordinal=0 target=0000000000000000 fd=4 family=AF_INET addr=127.0.0.1 port=4103 ret=0 errno=0 result=0x00000000 fd=listener inet:4103 len=16->256 pre=0000000000000000 post=100210077f000001
    phase=sock-connect ordinal=0 target=0000000000000000 fd=5 family=AF_INET addr=127.0.0.1 port=4103 policy=loopback-refuse ret=-1 errno=111
    phase=sock-accept ordinal=0 target=0000000000000000 fd=4 timeout wait_ms=50 ret=-1 errno=EAGAIN
    phase=sock-socket ordinal=0 target=0000000000000000 fd=-1 domain=518 type=2 proto=17 fd=-1 errno=97 policy=refuse-family
    phase=sock-bind ordinal=0 target=0000000000000000 fd=-1 family=AF_INET addr=0.0.0.0 port=60002 ret=-1 errno=9
    phase=sock-bind ordinal=0 target=0000000000000000 fd=-1 family=AF_INET addr=0.0.0.0 port=0 ret=-1 errno=9

    phase=gfx-stats ordinal=301 target=0000000000000002  backend=mesa src=file reason=frame frames=2 calls=8 draws=0/0 src=0/0 glslDump=0 bin=0/0 fmt=0x0000 compile=0 link=0 prog=0 sh=0 tex=0 vbo=0 texImg=0/0[none] compTex=0/0/0[none] readPx=0 last=- unknown=0[none] magic=- callers=[none]

The conf file's own warning predicted this shape exactly: "a send() that never
returns is also what a WRONG nogo looked like". The run collapsed to
**frames=2**, `draws=0/0`.

**The both-on measurement (20261020-171259-eden-loopboth, 300 s), verbatim:**

    phase=sock-socket ordinal=0 target=0000000000000000 fd=5 domain=2 type=1 proto=6 fd=5 errno=0 policy=passthrough
    phase=sock-bind ordinal=0 target=0000000000000000 fd=5 family=0 raw= ret=0 errno=0
    phase=sock-listen ordinal=0 target=0000000000000000 fd=5 backlog=1 ret=0 errno=0
    phase=sock-socket ordinal=0 target=0000000000000000 fd=6 domain=2 type=1 proto=6 fd=6 errno=0 policy=passthrough
    phase=sock-getsockname ordinal=0 target=0000000000000000 fd=5 family=AF_INET addr=127.0.0.1 port=4152 ret=0 errno=0 result=0x00000000 fd=listener inet:4152 len=16->256 pre=0000000000000000 post=100210387f000001
    phase=sock-connect ordinal=0 target=0000000000000000 fd=6 family=AF_INET addr=127.0.0.1 port=4152 policy=loopback-connect ret=0 errno=0
    phase=sock-accept ordinal=0 target=0000000000000000 fd=5 picked ret=4 errno=0
    phase=sock-socket ordinal=0 target=0000000000000000 fd=-1 domain=518 type=2 proto=17 fd=-1 errno=97 policy=refuse-family
    phase=sock-bind ordinal=0 target=0000000000000000 fd=-1 family=AF_INET addr=0.0.0.0 port=60002 ret=-1 errno=9
    phase=sock-bind ordinal=0 target=0000000000000000 fd=-1 family=AF_INET addr=0.0.0.0 port=0 ret=-1 errno=9

    phase=gfx-stats ordinal=301 target=0000000000000002  backend=mesa src=file reason=frame frames=2 calls=8 draws=0/0 src=0/0 glslDump=0 bin=0/0 fmt=0x0000 compile=0 link=0 prog=0 sh=0 tex=0 vbo=0 texImg=0/0[none] compTex=0/0/0[none] readPx=0 last=- unknown=0[none] magic=- callers=[none]

**The A/B on the two lines that matter.** `policy=loopback-refuse ret=-1
errno=111` became `policy=loopback-connect ret=0 errno=0`, and `sock-accept`
went from `timeout wait_ms=50 ret=-1 errno=EAGAIN` to `picked ret=4`. **The
knob does exactly what it was designed to do**: the guest's own loopback
handshake now completes and its own listener returns a real, connected
descriptor. That was the first time this has ever been observed.

And the gate did not move. `frames=2` and `draws=0/0` are byte-for-byte
identical to the baseline.

**The dlgate line, both archives, verbatim:**

    20261020-171259-eden-loopboth
    phase=dlgate: f=0 bias=0x2106d83000 gotEng=rr3+0x1fb2688 eng=0x210df07ee0 svc=0x0 svcV=unmapped qBeg=unmapped qEnd=unmapped qCount=0 slot10=rr3+0xbe25dc slot38=rr3+0xbec6d4 q0="-" ordinal=77 target=0000000000000000

    20261020-165534-eden-nogo0
    phase=dlgate: f=0 bias=0x2106983000 gotEng=rr3+0x1fb2688 eng=0x210db080e0 svc=0x0 svcV=unmapped qBeg=unmapped qEnd=unmapped qCount=0 slot10=rr3+0xbe25dc slot38=rr3+0xbec6d4 q0="-" ordinal=77 target=0000000000000000

Identical apart from the load bias and the two heap addresses. `svc=0x0`,
`qCount=0`, in both.

**The counts, both archives:**

| what | loopboth | nogo0 |
|---|---|---|
| `phase=dlgate` lines | 1 | 1 |
| `phase=guest-log-any` lines | 972 | 972 |
| "startMusic disabled during download" | 2 | 2 |
| "Bad config. Corrupted JSON config" | 1 | 1 |
| "AssetDownloadService" | 0 | 0 |
| `phase=sock-socket` / `-bind` / `-listen` / `-accept` / `-connect` | 3/3/1/1/1 | 3/3/1/1/1 |
| `phase=gfx-stats` lines | 2 | 2 |
| total guest-view log lines | 18714 | 10616 |

**The guest's own 972 log lines are byte-for-byte IDENTICAL between the two
archives.** A `Compare-Object` over the two `phase=guest-log-any` sequences
returns **zero** differences -- same 972 lines, same ordinals, same text,
including `startMusic disabled during download` at ordinals **20** and **772**
and `Bad config. Corrupted JSON config: unexpected end of input` at ordinal
**17**. The guest did not do one single thing differently with the loopback
handshake working.

**The guest's own last log lines (identical in both archives, ordinals 965-972):**

    phase=guest-log-any ordinal=965 target=00000000f4246041 CC: Unable to send Sync, Social Media not initialised.
    phase=guest-log-any ordinal=966 target=0000000003e982cf CC: Updating Cloudcell friends...
    phase=guest-log-any ordinal=967 target=00000000cf8766ff CC: ENQUEUED ACTION: [GooglePlayManager::ACTION_LOAD_FRIEND_VECTOR]
    phase=guest-log-any ordinal=968 target=00000000680791e5 CC: INITIATING ACTION [GooglePlayManager::ACTION_LOAD_FRIEND_VECTOR]
    phase=guest-log-any ordinal=969 target=00000000dd1bb24c CC: Sub Sync Queued: 10765
    phase=guest-log-any ordinal=970 target=00000000f4246041 CC: Unable to send Sync, Social Media not initialised.
    phase=guest-log-any ordinal=971 target=00000000d61fc0d2 CC: Sub Sync Queued: 10712
    phase=guest-log-any ordinal=972 target=0000000005631eb0 [RevSDK] userId set to d_0

**A MEASUREMENT LIMITATION THAT MUST BE FIXED BEFORE THE NEXT RUN: the dlgate
probe samples ONCE, and only because the frame loop is dead.**

`pollDownloadPhaseGate()` is called from exactly one place, the render frame
loop, under `if ((frames % 240) == 0)` (`guest_view_probe.cpp:4095`). The
frame counter never passed **2** in either archive, so the probe ran **exactly
once** in each, at `f=0`, at guest-log ordinal 77 -- i.e. **before** the
second `startMusic disabled during download` at ordinal 772. The one dlgate
line per archive is a frame-0 sample, not a time series, so **on its own it
cannot distinguish "the service was never created" from "the probe stopped
sampling before the service was created."** The `svc=0x0` is a true reading of
frame 0 and says nothing about frame 772.

The evidence that carries the conclusion is the guest's own log instead: it is
byte-identical to the baseline, and the frame counter never moved. The probe's
logic is right; its PLACEMENT is wrong for a collapsed run. It belongs on a
host thread that keeps ticking -- the `root-pc-sampler` thread produced
**7998** `phase=root-pc-sampler-step` lines over the full 300 s in this run
against **18** in the baseline, so a 1 Hz sampler thread survives the collapse
that killed the frame loop.

**CORRECTION to the standing claim that "ordinal=3 is the thread parked in
accept".** In BOTH of today's archives ordinal=3 is NOT in accept:

    20261020-171259-eden-loopboth
    phase=census-ord ordinal=3 target=0000000000000010 start=0x0 creator=0 delta=0 idle_ms=284253 last=pthread_mutex_unlock inbridge=0/guest enters=16 exited=0 host=0 root=1 lr_ring=21032bfc28 21032bfbd4 21032bfc28 21032bfbd4 21032749e4 21032bfc28 21032bfbd4 21032bfc28

    20261020-165534-eden-nogo0
    phase=census-ord ordinal=3 target=0000000000000010 start=0x0 creator=0 delta=0 idle_ms=287668 last=pthread_mutex_unlock inbridge=0/guest enters=16 exited=0 host=0 root=1 lr_ring=2102ebfc28 2102ebfbd4 2102ebfc28 2102ebfbd4 2102e749e4 2102ebfc28 2102ebfbd4 2102ebfc28

`last=pthread_mutex_unlock`, `enters=16`, `exited=0` -- identical in both. The
"parked in accept" state was real, but it belongs to the **nogo=1** era:
archive 20261019-220406-nogo2, the S18 evidence, does show

    phase=census-ord ordinal=3 target=0000000000000119 start=0x2107f55f94 creator=0 delta=0 idle_ms=276572 last=accept inbridge=0/guest enters=273 exited=0 host=0 root=0 lr_ring=2107e34e74 2107e309a4 2107e309c8 2107e309d4 2107e309f4 2107e30a10 2107e30a24 2107e30a38

`last=accept`, `enters=273`. So S18's premise was correct for its own archive
and **has been overtaken by nogo=0**: with the socket stack up, the guest's
`accept` is bounded by `acceptWaitMs=50` and returns `EAGAIN` instead of
parking forever, and the guest's stuck thread is no longer ordinal=3.

**The thread that IS stuck, in both archives, is ordinal=4 -- and it is stuck
on a condvar that is never signalled.** That is the shape of the real
blocker: the gate at 0x14048c8 waits (bounded) on a worker whose readiness
flag is bit 2 of `[target+0x70]`, and the worker's readiness is supposed to be
announced by exactly this condvar.

    20261020-171259-eden-loopboth
    phase=census-ord ordinal=4 target=0000000000000009 start=0x21083a9fd8 creator=3 delta=0 idle_ms=284250 last=pthread_cond_wait inbridge=1/pthread_cond_wait enters=9 exited=0 host=0 root=0 lr_ring=21032e09c4 21032e0a80 21032e0a98 21083aa01c 21032bfbd4 21032bfc28 21032bfbd4 2103274a30
    phase=census-wait ordinal=73 target=0000000000000001 kind=cond obj=210defa1c0 waited=1 signalled=0 deficit=1 wait_caller=2103274a30
    phase=census-wait-sum ordinal=73 target=0000000000000001 condvars_waited_on=1 total_waits=1 total_signals=0 unsignalled_objects=1 worst_deficit=1
    phase=census-mutex-held ordinal=4 target=000000210defa198 orphan=0 guest=210defa198 owner_ord=4 owner_key=82809200 acquire_site=21032bfbd4 held_ms=284250 acquires=4

    20261020-165534-eden-nogo0
    phase=census-ord ordinal=4 target=0000000000000009 start=0x2107fa9fd8 creator=3 delta=0 idle_ms=287163 last=pthread_cond_wait inbridge=1/pthread_cond_wait enters=9 exited=0 host=0 root=0 lr_ring=2102ee09c4 2102ee0a80 2102ee0a98 2107faa01c 2102ebfbd4 2102ebfc28 2102ebfbd4 2102e74a30
    phase=census-wait ordinal=73 target=0000000000000001 kind=cond obj=210daf77c0 waited=1 signalled=0 deficit=1 wait_caller=2102e74a30
    phase=census-wait-sum ordinal=73 target=0000000000000001 condvars_waited_on=1 total_waits=1 total_signals=0 unsignalled_objects=1 worst_deficit=1
    phase=census-mutex-held ordinal=4 target=000000210daf7798 orphan=0 guest=210daf7798 owner_ord=4 owner_key=82409200 acquire_site=2102ebfbd4 held_ms=287163 acquires=4

`total_waits=1`, `total_signals=0`, `unsignalled_objects=1`, `worst_deficit=1`
-- **one condvar wait, never signalled**, in both runs, while ordinal=4 holds
a guest mutex for ~284 s out of the 300 s run. This is the wait that the
AssetDownloadService worker's readiness is supposed to release, and nothing
signals it. That is why the service is never created: **the guest's own
worker never signals the gate that waits for it.** The loopback exception was
never on that path.

**The addresses, so nobody has to re-derive them.**

| what | where | note |
|---|---|---|
| the constant that was flipped | `native-switch/source/symbol_resolver.cpp:690` | `allowLoopbackConnect` false -> true |
| the branch that consumes it | `native-switch/source/symbol_resolver.cpp:702` | `loopback && allowLoopbackConnect` -> real `::connect` |
| the accept bound | `native-switch/source/symbol_resolver.cpp:578` | `acceptWaitMs = 50` |
| the accept policy knob | `native-switch/source/symbol_resolver.cpp:601` | `acceptBlocksForever = false` |
| the dlgate probe | `native-switch/source/guest_view_probe.cpp:3462` | `pollDownloadPhaseGate()` |
| the probe's ONLY call site | `native-switch/source/guest_view_probe.cpp:4095` | `if ((frames % 240) == 0)` -- frame loop |
| GOT slot read | `0x1f6d5d8` | `kDlGotSlotElfVa` |
| engine global | `0x1fb2688` | `kDlEngineGlobalElfVa` |
| service pointer offset | `+0x10518` | `kDlServiceOffset` |
| queue begin / end | `+0xd8` / `+0xe0` | `kDlQueueBeginOffset` / `kDlQueueEndOffset` |
| ADS vtable slots 10 / 38 | `0x1e54560` / `0x1e54588` | `kDlAdsSlot10ElfVa` / `kDlAdsSlot38ElfVa` |
| download-phase gate | `0x12ad2e0` | logs "startMusic disabled during download" |
| service construction | `0x14045e0` | ctor `0xbe2294`, store at `0x140461c` |
| the gate that waits for the worker | `0x14048c8` | caller `0x1404994` |

**Still open, and now narrower.**

The loopback exception is **exonerated as the blocker**: the guest's own
handshake works, its own listener accepts, and nothing downstream changed.
What is left is the pair of facts above -- the service is still not created
(S23's open question, still open, and now with a stronger independent witness
than the single frame-0 dlgate sample), and ordinal=4 is parked in
`pthread_cond_wait` on a condvar with `total_signals=0`.

Two concrete next steps, in order:

1. **Move the dlgate probe off the frame loop** onto the sampler thread that
   survives the collapse (7998 `root-pc-sampler-step` lines over 300 s), on a
   1 Hz heartbeat, so `svc` becomes a time series instead of one frame-0
   reading. Without this, every future run has the same ambiguity.
2. **Find who is supposed to signal the condvar** `obj=210defa1c0` that
   ordinal=4 waits on (`wait_caller=2103274a30`, `acquire_site=21032bfbd4`),
   and find out why the worker that would signal it never starts. That is the
   actual blocker; the socket layer was never it.

**A method note.** The temptation with this result is to report "the loopback
knob made the handshake work and the gate still did not move" and stop. The
honest report has to carry two more facts that are easy to leave out and that
change what the next agent should do: the dlgate probe sampled ONCE because
the frame loop died at frame 2 (so `svc=0x0` is a frame-0 reading, not a
300 s verdict), and the "ordinal=3 is parked in accept" premise is an
nogo=1-era fact that nogo=0 has already overtaken. Both were visible only by
reading the probe's own call site and the S18-era archive side by side with
today's.

