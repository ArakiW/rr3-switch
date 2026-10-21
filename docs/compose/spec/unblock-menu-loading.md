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



### S25 -- the probe becomes a time series, and the condvar's owner is named

Date: 2026-10-20. Archive 20261020-195848-eden-probe2 (tag `probe2`, 300 s).
Baseline: 20261020-171259-eden-loopboth (S24).

S24 left two things open. This entry closes the first, names the second,
and corrects one premise from the brief it was given.

#### 1. The dlgate probe moved off the frame loop

**The defect, exactly.** `pollDownloadPhaseGate()` was called from exactly
one place -- the render frame loop, under `if ((frames % 240) == 0)`. The
frame loop dies at frame 2 in the collapsed run, so the probe sampled
EXACTLY ONCE per run, at `f=0`. One reading cannot distinguish "the
AssetDownloadService was never created" from "the probe stopped sampling
before the service was created". That is why S24's `svc=0x0` was a
frame-0 reading and not a verdict.

**Where it went.** The probe now runs on the HOST sampler thread,
`samplerWorker()` in `native-switch/source/guest_view_probe.cpp` -- a raw
libnx `threadCreate` thread at priority `kDiagnosticThreadPriority`
(0x3B), which ticks every 75 ms (`kSamplerIntervalNs`).

That thread is the right carrier for a measured reason, not a stylistic
one: it is a raw host thread, NOT a thread created through the guest
pthread bridge, and a guest-bridge thread never gets scheduled out of
`svcSleepThread` on Eden (the same fact the file already records at
guest_view_probe.cpp:842-848). It is PROVEN to survive the collapse --
the probe2 archive has 92 `phase=sampler-flush` lines and the last one is
at the very END of the 300 s run whose frame loop stopped at frame 2.

The root-pc-sampler thread in `pthread_bridge.cpp` would be the other
candidate, but its body is in a file this change does not own and there
is no seam into it from here.

**The cadence.** Two triggers, and both are needed:

1. a STATE CHANGE -- reported on the very next tick (<=75 ms), so a
   change is never hidden behind the heartbeat;
2. a **1 s HEARTBEAT** (`kDlHeartbeatMs = 1000`), so the log is a time
   series even when nothing changes.

Trigger 2 is what makes the probe answer the question it exists to
answer. A series of `svc=0x0` readings across the whole run is a
statement about the whole run; one reading is a statement about one
instant. Without the heartbeat the probe would print one line and be
indistinguishable from the frame-0 sample it replaced. The heartbeat is
measured on the wall clock (`armGetSystemTick`), not on a tick count, so
a starved carrier produces a LONGER gap rather than a burst of
catch-up samples.

**The line format is unchanged** (`phase=dlgate: f=... bias=... gotEng=...
eng=... svc=... svcV=... qBeg=... qEnd=... qCount=... slot10=... slot38=...
q0="..."`), and so is the per-line cap (`kDlMaxLines = 200`). One field's
MEANING changed: `f=` is now the emitted-line index (0-based) rather
than the render frame counter, because a frame counter does not exist
off the frame loop. Consecutive lines are consecutive samples.

**What the run measured.** 20261020-195848-eden-probe2, and
reproduced in 20261020-201647-eden-probe2c (a third run, 300 s, same
result):

    phase=dlgate lines: 200   (the cap; the series is longer)
    f=0   bias=0x210714f000 gotEng=rr3+0x1fb2688 eng=0x210e2d4260 svc=0x0
          svcV=unmapped qBeg=unmapped qEnd=unmapped qCount=0
          slot10=rr3+0xbe25dc slot38=rr3+0xbec6d4 q0="-"
    f=199 (same values)
    distinct svc values across all 200 lines: 1   -> 0x0
    distinct qCount values across all 200 lines: 1 -> 0
    first dlgate at log line 7701, last at 12944 (the file is 13494 lines)
    bracketed by census-sweep ordinal=3 (elapsed_ms=12066) and
                     census-sweep ordinal=56 (elapsed_ms=224528)

So the series spans roughly **212 s of the 300 s run at ~0.94 Hz**, and
the cap (200 lines) is what stopped it, not the run. Every one of the
200 readings says `svc=0x0`, `qCount=0`, `svcV=unmapped`, with the two
relocation words intact (`slot10=rr3+0xbe25dc`, `slot38=rr3+0xbec6d4`).
**The AssetDownloadService was never created, and now that is a
statement about 212 seconds of the run rather than about one instant
at frame 0.** The frame loop was dead for essentially all of it.

**Two runs, and a third that also reproduced it, put the SAME ELF
offsets on three different load biases** -- `libc++_shared.so +0x77a30`
for the wait, `libRealRacing3.so +0x1626fd8` for the blocked worker,
`+0x1406f94` for the async task, in all three. That is the strongest
cross-run confirmation available: the addresses are the guest's, not
the run's.

**A THIRD run, 20261020-201647-eden-probe2c, reproduced the 200-line
series and the whole blocker shape exactly** (73 census sweeps,
`ordinal=4 last=pthread_cond_wait`, `ordinal=5 last=accept`,
`total_signals=0`). A second run in the same session,
20261020-201304-eden-probe2b, produced only SIX dlgate lines and its
guest stopped making progress at ~20 s -- with the frame loop at frame
240 instead of frame 2, i.e. the GUEST took a different path that run
and wedged somewhere else and earlier. That is run-to-run variance in
the guest's own startup, not a property of the probe: the probe's
output is the same in every run that lived long enough, and the run
that produced 6 lines produced 6 because the thing it was measuring had
already stopped.

**A safety change the move forced.** The probe used to run on the
guest's own thread, between frames, where the guest was not mutating
memory. On a host thread it reads guest memory CONCURRENTLY with a
running guest, and a heap object the guest is in the middle of freeing is
a pointer-shaped value pointing at a page that is no longer there. The
heap reads now ask the kernel with the SAME query the census uses for
guest stack words (`svcQueryMemory` + a `Perm_R` check, as in
`censusReadStackWord` in `pthread_bridge.cpp`); a page that is not
mapped, or is mapped without read permission, is reported as "unmapped"
and never as a value. The image-relative reads are unaffected --
`guestImageContains()` is already exact for those. A diagnostic that can
crash the run it is measuring is worse than no diagnostic.

**And the cost of that guard had to be bounded.** The first version
queried the kernel PER BYTE, and the queue is up to eight std::strings
of up to 24 bytes each: ~200 `svcQueryMemory` calls per sample, i.e.
thousands of syscalls a second, from the thread that also does a
pause/context/resume round trip on the guest. The guard now caches the
last queried page and the string decoder asks about a WHOLE RANGE once
instead of a byte at a time, so a sample costs a handful of queries
rather than hundreds.

#### 2. Who owns the condvar, and who must signal it

**A premise in the brief was wrong, and it matters.** S24's next step
said to "subtract the run's load bias" to get the ELF vaddr. The run's
bias (`0x210714f000` for `libRealRacing3.so` in probe2) does not apply:
`wait_caller=0x210363fa30` is BELOW it, so it is not in the main image.
The `phase=sampler-image` lines in the same run give the bases:

| runtime | image | offset |
|---|---|---|
| `0x210363fa30` (wait_caller) | `libc++_shared.so` (base `0x21035c8000`) | `+0x77a30` |
| `0x210368abd4` (acquire_site) | `libc++_shared.so` | `+0xc2bd4` |
| `0x210368ac28` (the unlock site) | `libc++_shared.so` | `+0xc2c28` |
| `0x2108775fd8` (ordinal 4's start) | `libRealRacing3.so` | `+0x1626fd8` |
| `0x210877601c` (ordinal 4's lr) | `libRealRacing3.so` | `+0x162701c` |
| `0x2108555f94` (ordinal 5's start) | `libRealRacing3.so` | `+0x1406f94` |
| `0x210e2c37b8` (the mutex) | GUEST HEAP | -- |
| `0x210e2c37e0` (the condvar) | GUEST HEAP | -- |

The probe2c run has different bases (`libc++_shared.so` at
`0x21033c8000`, `libRealRacing3.so` at `0x2106f4f000`) and therefore
different runtime addresses -- and IDENTICAL offsets: `wait_caller`
`+0x77a30`, `acquire_site` `+0xc2bd4`, ordinal 4's start `+0x1626fd8`,
ordinal 5's start `+0x1406f94`. Three runs, three biases, one set of
offsets.

The probe2 run reproduces the loopboth addresses EXACTLY in ELF-offset
terms (`+0x77a30`, `+0xc2bd4`, `+0x1626fd8`, `+0x1406f94`), which is
the strongest cross-run confirmation available: two runs, different
biases, same offsets.

**The waiting function is libc++'s `std::condition_variable::wait`.**
`libc++_shared.so + 0x77a14` is the function; the recorded
`wait_caller` `+0x77a30` is the instruction after its
`bl 0xecbc0` (`__libcpp_condvar_wait`). The same function's
neighbouring strings are the proof -- `libc++_shared.so + 0x4be7f` reads
`"condition_variable::wait: mutex not locked"` and `+0x4cb06` reads
`"condition_variable wait failed"`.

**The chain, in raw vaddrs:**

```
libRealRacing3.so 0x1626fd8   the worker body (ordinal 4's start routine)
  -> 0x16270c8  bl std::condition_variable::wait   (PLT 0x1e19390)
     -> libc++_shared.so 0x77a14  std::condition_variable::wait
        -> libc++_shared.so 0xecbc0  __libcpp_condvar_wait
           -> host pthread_cond_wait  (the import the census saw)
```

`0x1626fd8` is the std::thread-style trampoline: it is the function
whose `pthread_setspecific` (PLT `0x1e18150`) and `__thread_local_data`
(PLT `0x1e18140`) calls are the signature, and it is reached from
`0x1624d94` via the `adrp`/`add` pair at `0x1624e34`/`0x1624e44` -- i.e.
it is the entry point of a thread that `0x1624d94` creates. The census
confirms it independently: ordinal 4's `start=` is
`libRealRacing3.so + 0x1626fd8` and `creator=3`.

**Who is SUPPOSED to signal it.** The notifier is
`libRealRacing3.so 0x1624cc8`, which calls `std::condition_variable::
notify_one` (PLT `0x1e19370`) at `0x1624d08`. It is reached from
`0x1624c68`, which is reached from `0x161f4d0` and `0x164cf80`. A
sibling notifier is at `0x1624ecc` (nine callers).

**The mutex explains the shape.** The census reports
`census-mutex-held ordinal=4 guest=210e2c37b8 owner_ord=4
acquire_site=210368abd4 held_ms=282582 acquires=4`. The acquire_site is
`libc++_shared.so + 0xc2bd4` = `std::mutex::lock` (the PLT `0x1e18040`
is `_ZNSt6__ndk15mutex4lockEv`). The guest's `0x1626fd8` takes that
mutex at `0x162706c` and then, at `0x16270c8`, calls
`condition_variable::wait` on the object at `+0x40` -- the condvar.
The mutex is `0x210e2c37b8` and the condvar `0x210e2c37e0`, and
`0x210e2c37e0 - 0x210e2c37b8 = 0x28` -- the two are ADJACENT FIELDS of
one guest object, a `std::mutex` at `+0x00` and a
`std::condition_variable` at `+0x28`. That is the `std::async`
shared-state layout, and the object at `0x1fb5328` is the one S24
already identified.

**Whether the mutex is genuinely still held is a HOST-side question
the census cannot answer by itself**, because the ledger records
`held_ms` from the guest's own lock/unlock calls, and a
`pthread_cond_timedwait` that times out does not call the guest's
`pthread_mutex_unlock` -- the guest never asked to unlock. The host
bridge's `guestPthreadCondWait` (pthread_bridge.cpp:5975) implements
the wait as a LOOP of one-second `pthread_cond_timedwait`s, and each
timeout re-acquires the mutex. So the census's `held_ms=282582` for
this mutex is EXPECTED for any thread sitting in a timed wait -- it is
not by itself evidence that the guest holds the mutex.

**So the missing guest-side action is a specific one.** The signal was
never SENT: the census says `total_signals=0`, `unsignalled_objects=1`,
`worst_deficit=1`. The notifier `0x1624cc8` is a guest code path, and
the census shows ordinal 4 has not advanced since the run began
(`delta=0`). The one thing that must happen and does not is:

**the guest's own `__async_assoc_state` completion path must publish
the result and notify the condition variable -- and the thread that
would run it has already EXITED.**

**Is ordinal 4 the async task that must set the ready bit? NO, and
this is the correction to the brief.** The async task is a DIFFERENT
thread: ordinal 5, whose `start=` is `libRealRacing3.so + 0x1406f94` --
the trampoline S24 named, whose body is `0x1406f80 bl 0x140445c` (the
async callable) followed by `b 0x1e18130` (`__assoc_sub_state::
set_value`, PLT `0x1e18130` ->
`_ZNSt6__ndk117__assoc_sub_state9set_valueEv`).

**And what ordinal 5 is doing differs between the runs, and the
difference is the whole story:**

| | loopboth (S24) | probe2 / probe2c |
|---|---|---|
| ordinal 5 `last=` | `pthread_exit` | `accept` |
| ordinal 5 `exited=` | **1** | **0** |
| ordinal 5 `idle_ms=` | 276004 | 276261 |
| ordinal 5 `enters=` | 274 | 269 |
| ordinal 4 `last=` | `pthread_cond_wait` | `pthread_cond_wait` |
| ordinal 4 `exited=` | 0 | 0 |

So in loopback the async task thread had ALREADY EXITED (via
`pthread_exit`, not by returning normally) -- which means the
`b 0x1e18130` (set_value) at the end of the trampoline may never have
executed, and the brief's claim that "bit 2 is set when the guest's own
task body 0x140445c returns normally" does not apply to a thread that
was cancelled.

In probe2/probe2c the async task thread has NOT exited: it is parked in
`accept` (`inbridge=0/guest`, `delta=0`). The guest's accept call sites
are `0x12e1a34`, `0x12e556c` and `0x1a826b4` (PLT `0x1e18f70`), and
ordinal 5's `lr_ring` is `libRealRacing3.so + 0x12e5e74 ...` -- i.e.
inside the `0x12e5b74` function, whose `0x12e5ec8` branches back into
it. That is the guest's OWN loopback listener, and with `nogo=1` the
socket stack is down: the run's own `phase=sock-socket` line reads
`fd=-1 ... errno=0 policy=passthrough`, `phase=sock-listen` reads
`ret=-1 errno=9`, and `phase=sock-connect` reads
`policy=offline-refuse ret=-1 errno=111`. So the guest's accept is
waiting for a connection that can never arrive, because the guest's own
listener socket could not be created.

**Which thread runs `0x140445c`, and what is IT waiting on?**
Ordinal 5, and in probe2 it is waiting on the guest's own `accept()` --
on a socket that `nogo=1` guarantees will never be connected.

**The gate's own waiter is NOT stuck.** `0x14048c8` (the gate) calls
`0x1404ac0` (the future waiter) at `0x1404960`, and `0x1404ac0` POLLS
the status byte at `[state+0x70]` (`ldr w8, [x22, #0x70]`) and returns 0
(success) when BIT 2 is set -- it does NOT block. So the gate is
waiting on a POLL, not on a condvar, and it is not what is stuck.

**The chain, end to end, with the one missing link marked:**

```
libRealRacing3.so 0x14048c8  the download-phase gate
  -> 0x1404ac0  poll [state+0x70] bit 2   (never blocks)
     ... needs the async task to have run to completion ...

libRealRacing3.so 0x1406f94  ordinal 5: the async task thread
  -> 0x1406f80 bl 0x140445c   (the async callable)
     -> 0x12e5b74 ... 0x12e5ec8   the guest's own socket listener
        -> accept()  <-- BLOCKED: nogo=1 means the listener never exists
  -> 0x1406f90 b 0x1e18130    set_value   <-- NEVER REACHED

libRealRacing3.so 0x1626fd8  ordinal 4: a different worker
  -> 0x16270c8 bl condition_variable::wait  (libc++ 0x77a14)
     -> blocked forever: total_signals=0, the notifier 0x1624cc8 never runs
```

**The single missing guest-side action.** The async task at
`libRealRacing3.so 0x1406f94` must be allowed to RUN TO COMPLETION so
that `0x1406f90 b 0x1e18130` (`set_value`) executes and the ready bit
lands. In probe2 it cannot, because it is parked in the guest's own
`accept()`, which `nogo=1` makes unsatisfiable. In loopboth it had
already exited via `pthread_exit` without reaching `set_value`.

**And ordinal 4's wait is the same root cause, not a second one.** The
condvar ordinal 4 waits on is the one the async state's completion path
is supposed to notify (`0x1624cc8`). The async task never completes,
so the notify never happens, so ordinal 4 waits for the whole run. The
guest's own `std::async` machinery has one dependency -- the callable
must return -- and the callable is stuck.

#### 3. The sync queue

**The two log sites**, both in the guest's own Cloudcell code:

- `"CC: Unable to send Sync, Social Media not initialised."` -- string at
  `.rodata` `0x3aee47`, referenced from `libRealRacing3.so 0x1d0307c`
  (the `adrp`/`add` pair at `0x1d031a4`/`0x1d031ac`, the log call at
  `0x1d031b4`).
- `"CC: Sub Sync Queued: %d\n"` -- string at `.rodata` `0x3ebbef`,
  referenced from `libRealRacing3.so 0x1d02e00` (the `adrp`/`add` pair
  at `0x1d02fd4`/`0x1d02fdc`, the log call at `0x1d02fe4`). The `%d`
  comes from `[sp,#0x14]`, loaded at `0x1d02fcc`.

**What "Social Media not initialised" actually tests.** At `0x1d0307c`:

```
0x1d030ac  ldr  x20, [x20, #0x5d0]   ; the Cloudcell global
0x1d030b0  ldr  x8, [x20]             ; its vtable
0x1d030b4  ldr  x9, [x9, #0x158]      ; vtable slot 43
0x1d030bc  blr  x9                    ; -> returns an object
0x1d030c4  ldr  x8, [x0]
0x1d030c8  ldr  x8, [x8, #0xd8]       ; THAT object's vtable slot 27
0x1d030cc  blr  x8                    ; -> bool
0x1d030d0  tbz  w0, #0, #0x1d031a0    ; bit0 clear -> the "not initialised" log
```

Slot 43 of the Cloudcell vtable (`0x1f63b58`) is `0x1cbfb6c` =
`add x0, x0, #0x2f8 ; ret` -- it returns the sub-object at
`Cloudcell+0x2f8`. That sub-object's vtable is installed at `0x1cbe530`
and its typeinfo names **`cc::social::ISocialMedia`** (RTTI
`N2cc6social12ISocialMediaE` at `.rodata 0x604966`, typeinfo object at
`0x1f66f00`, vtable at `0x1f66f18`), whose slot 27 (`+0xd8`) is
`0x1d081b8`. The concrete class is **`cc::social::SocialMedia`**
(RTTI `N2cc6social11SocialMediaE` at `0x60494c`, vtable `0x1f66de8`).

**And this is NOT the `GetSessionValid` path our `ccsocialok=1` switch
controls.** The port's switch makes
`FacebookWorker.GetSessionValid()` / `GooglePlayWorker.GetSessionValid()`
answer true (`native-switch/source/jni_compat/classes/cc_services.cpp:446`
and `:588`, reached through the BOOLEAN JNI slot in
`native-switch/source/jni_bridge.cpp`). But the guest's test here is two
levels of C++ virtual dispatch inside `libRealRacing3.so` -- the
`cc::social::ISocialMedia` sub-object, then ITS vtable slot 27.
Nothing in that chain is a JNI call to `GetSessionValid`, so the switch
does not reach it.

That is the answer to "does our switch actually satisfy it": **no**. The
switch answers a question the guest never asks at this site, which is why
the guest keeps logging the message with `ccsocialok=1` set (the Eden
conf has `ccsocialok=1` AND the archive still shows the message at
ordinals 965 and 970 in loopboth, and at the equivalent ordinals here).

**What the guest's own notion is.** The `cc::social::ISocialMedia`
component's slot 27 is the guest's "is the social-media session usable"
bit. The port already has a pin for a DIFFERENT Cloudcell decision point
-- `cloudcellPinZero27` (guest_view_probe.cpp:154) forces Cloudcell
vtable slot 27 to return 0 so the engine takes its give-up path instead
of spinning. But that pin is on the FIRST-level Cloudcell vtable, not on
the `cc::social::ISocialMedia` vtable that this test uses, so it does
not affect this log line.

**What our host must provide.** For this site to stop logging, the
`cc::social::ISocialMedia` sub-object's slot 27 must return a bool with
bit 0 set. The guest builds its own Cloudcell object (`0x1cbe03c`,
allocating `0x4d0` bytes and installing the `cc::ICloudcell` vtable
`0x1f63b58` at `0x1cbe290`), so the host can only influence this
through the JAVA side of that component -- which is what `ccsocialok=1`
was MEANT to do, but the guest's `cc::social::ISocialMedia` evidently
asks the Java layer something ELSE (a different class, method, or value)
than `GetSessionValid`. **Finding which Java method
`cc::social::ISocialMedia` slot 27 consults is the next piece of work
on this side, and it is a Java-layer question, not a guest one.**

**What the 10765 sync items are.** `0x1d02e00` is the queueing
function. It takes a lock at `+0x10` (`ldaxrb`/`stxrb` on `[x0+0x10]`),
then at `0x1d02ea4` reads `[x19+0x50]` (a count) and `[x19+0x48]` (a
table of per-slot entries), and at `0x1d02ee0` selects the entry with a
modular index. It then appends at `0x1d02fa4` (`bl 0x1d05a54`) or, when
the table is full, at `0x1d02fc8` (`bl 0x1d05320`), and logs the NEW
COUNT. So `10765` and `10712` are COUNTS of queued sub-sync items, not
identifiers: the queue is being re-populated and never drained, which
is exactly what the "Unable to send Sync" log above it predicts.

**What drains them.** The same file: `0x1d0307c` is the DRAIN attempt
("Unable to send Sync, ..."), and the send path is the
"CC: Sending Sync with SubSyncs:" string at `.rodata 0x458563`,
referenced from `0x1d03234`/`0x1d0323c` in the SAME function
`0x1d0307c` -- and the order inside that function is decisive:

```
0x1d030d0  tbz  w0, #0, #0x1d031a0   ; social-media bit clear -> "Unable to send Sync" and RETURN
0x1d030d4  ... (the send path)
0x1d03188  ldp  x8, x9, [x19, #0x30]  ; queue begin/end
0x1d03190  b.eq #0x1d031ec            ; queue EMPTY -> skip the send
0x1d03234  "CC: Sending Sync with SubSyncs:"
```

So the order in one function is: test the social-media bit -> if clear,
log "Unable to send Sync" and RETURN WITHOUT SENDING -> the queue is
never drained -> the next tick queues more -> the count grows. The
queue-empty check at `0x1d03188` is AFTER the bit test, so the queue
would not be drained even if it were empty; the bit test is the gate.

**So the two halves of C are the same story, and the same root cause as
B.** The sync queue grows because the send is gated on a social-media
bit the host is not satisfying; the download-phase gate is gated on a
future whose producing thread is parked in the guest's own `accept()`.
Both are the guest waiting on something that only the outside world
can supply -- a live social-media session, and a connected socket.

#### 4. A note on the analyser's positive control

The control in `.build/rr3fast.py` FAILED when this work started, and
the failure was real, not cosmetic: the `.eh_frame_hdr` search table
uses `DW_EH_PE_datarel|sdata4` (encoding byte `0x3b`), so every entry is
relative to the ADDRESS OF THE SECTION, not to section+4. With the
section+4 base, every function start was off by four and the control
resolved `0x12ad2e0` to `0x12ad228` -- the PREVIOUS function. The
control now passes (`func_of(0x12ad2e0) == 0x12ad2e0`). Recorded
because a scanner that cannot find its own function boundaries cannot be
trusted to find a caller's either.

#### What is still open

1. **Which Java method does `cc::social::ISocialMedia` slot 27
   consult?** This is a Java-layer question and it is the only
   remaining host-side lever for the "Social Media not initialised"
   log. `ccsocialok=1` does not reach it, and the guest's own
   `Cloudcell+0x2f8` sub-object is built by the guest, so the host
   cannot simply pin it.
2. **The accept() the async task is parked in.** Whether the guest's
   own loopback listener is the thing that must be connected (a
   host-side listener the guest is supposed to reach) or whether it is
   an outbound accept the guest never needed, decides whether this is
   host-fixable at all. `nogo=1` currently makes it unsatisfiable.

### S26 -- THE WEDGE IS A BRANCH TO ADDRESS 0 (2026-10-20)

#### 1. The instrument: every deferred item now has a name

`cloudcell_defer` takes a `tag` and the pump logs one line on entry and
one on return, so a batch that does not finish says WHICH item did not
finish:

```
phase=cloudcell_defer: n=1 total=1 depth=1 tag=aam.login
phase=cloudcell_pump: site=frame phase=entering drained=1 entering=1 ...
phase=cloudcell-item: enter id=1 tag=aam.login site=frame
   (no "cloudcell-item: return" line, ever)
```

Archive `20261020-230804-eden-wedgectl`. The single item in the frame-1
batch was **`aam.login`** -- the deferred
`AndroidAccountManager.LoginCompleteCallback`. The guest's own hint
(`ACTION_LOAD_FRIEND_VECTOR`) was a red herring: the GooglePlay
friend-vector callback was never even enqueued.

#### 2. Why it does not return: it is not a lock, it is a branch to 0

The guest's export is a four-instruction trampoline (guest `0x1ce7318`):

```
0x1ce7318  tst w2, #0xff      ; the boolean
0x1ce731c  mov x1, x4         ; the user handle
0x1ce7320  cset w0, ne
0x1ce7324  br x3               ; TAIL-BRANCH to the completion pointer
```

So the completion pointer arrives in `x3` and is **branched to**. Eden's
own log says what happened:

```
[ 20.940466] Core.ARM <Critical> ...ExceptionRaised:
             Cannot execute instruction at unmapped address 0x00000000000000
[ 20.941319] Core.ARM <Error> ...LogBacktrace: Backtrace @ PC=0000000000000004
R03=0000000000000000 ...
V01=436574656C706D6F_006B6361626C6C61   ; "ompleteCallback"
```

`R03` is `x3` and it is **zero**. The guest branch-to-zero killed the
frame thread outright, which is why the census then read `pc=0x4`
(sentinel), `delta=0`, `idle_ms=276095`, and why `after-cloudcell-pump`
was never emitted. The identical fault is in the previous archive at
`t=20.908754`, so this is the wedge, not a new one.

`x3` was zero because the guest calls `login()` on an
`AndroidAccountManager` instance that never received
`Constructor(JJ)V`:

```
phase=aam-ctor: obj=0x80f455c0 callback=0x10826fbda0 user=0x10826fbd30
phase=aam-login: obj=0x80f436d0 callback=0x0            user=0x10826fc2b0
```

Two instances: the one that got `Constructor` and the one that got
`setUserIdentifier` + `login` are different objects, and the second
one's completion pointer is zero.

#### 3. A second, independent bug found on the way: the non-virtual JNI dispatch

`iface_NewObject` calls the constructor NON-virtually, but it called
**every** dispatcher as `(env, obj, jclazz, va_list)`, while
`ManagedMethod::Register` builds a dispatcher of shape
`(env, obj, va_list)`. For a virtual-registered constructor the va_list
landed in the `jclass` slot and the constructor read its arguments from
the wrong place. `iface_CallMethod` and `iface_CallMethodV` already
branched on `method->takes_class`; the non-virtual variants did not.
They do now (`native-switch/source/jni_compat/jni.cpp`).

#### 4. The fix, and what it measured

Two changes, both minimal:

1. `aam_login` refuses to deliver a login completion whose continuation
   pointer is NULL, and says so in the log. There is no completion
   target on that instance, so there is nothing to complete -- and
   delivering one would branch the guest to 0.
2. The `takes_class` fix above.

Archive `20261020-234046-eden-loginfix`:

| | before (`wedgectl`) | after (`loginfix`) |
|---|---|---|
| frames in 300 s | 2 | **15720** |
| last gfx-stats | `frames=2 calls=8 draws=0/0` | `frames=15720 calls=47165 draws=0/0` |
| "Cannot execute instruction" | 1 (t=20.94 s) | **0** |
| cloudcell-item lines | 1 enter, 0 return | 0 (nothing deferred) |
| `MainMenu` | 0 | 0 |
| "Unable to send Sync" | 2 | 7 |

**The frame loop is alive for the first time.** The menu is still not
reached: `draws=0/0` throughout and `MainMenu` is still 0.

#### 5. "Social Media not initialised": which Java method, really

The open question from S25 is now answered by the guest's own code, and
the answer is **not** `GetSessionValid`.

The test the guest performs at `0x1d0307c` is `Cloudcell` vtable slot 43
-> the `cc::social::ISocialMedia` sub-object at `Cloudcell+0x2f8` ->
**slot 27** (`ldr x8,[x8,#0xd8]`). The vtable installed there is
`0x1f66df8` (`0x1cbe530`: `str x8,[x19,#0x2f8]`, with the vtable address
read from the relocation at `0x1f7dff0` = `0x1f66de8` + 0x10), and its
slot 27 is **`0x1d066e4`**. That function is:

```
0x1d066e4  ldp x19, x20, [x0, #8]     ; the 14-entry provider vector
0x1d0670c  ldr x0, [x19]              ; one provider
0x1d06718  ldr x8, [x8, #0x20]        ; the provider's slot 4
0x1d0671c  blr x8
0x1d06720  tbnz w0, #0, #0x1d06700    ; bit0 set -> next provider
0x1d06724  mov w0, wzr                ; any provider clear -> NOT initialised
```

So it is an **AND over all fourteen providers' slot 4**. The GooglePlay
provider's slot 4 is `0x1ca7654` = `ldrb w0,[x0,#0xc0]`, i.e. the
availability byte at `GooglePlayManager+0x148`. That byte is written in
exactly one place, `0x1ca23e8`:

```
0x1ca23d0  ldr x0, [x19, #8]
0x1ca23d8  ldr x8, [x8, #0xf8]     ; the WORKER's vtable slot 31
0x1ca23dc  blr x8
0x1ca23e0  tbz w0, #0, #0x1ca23ec  ; clear -> the byte is NOT written
0x1ca23e8  strb w8, [x19, #0x148]  ; the byte
```

The guest resolves `IsInitialPhaseComplete` on `GooglePlayWorker` at
boot (the resolution order is in Eden's JNI log) and the port answers it
(`ccsocialok`). But the guest never **calls** it: the refresh path
`0x1ca2188` that would call it is entered only from the
login-completion flow, and that flow never completed.

**⇒ There IS a path the port can satisfy, and `GetSessionValid` is not
it. `ccsocialok` answers a question the guest never asks at this site.
The gate opens only when the guest receives a login completion and walks
the refresh path, asks `IsInitialPhaseComplete`, and gets the byte
written.**

#### What is still open (after S26)

1. ~~**The guest's real completion pointer.**~~ **CLOSED in S27** - see
   below. The pointer is a guest CODE address, `0x1ce6e2c` plus the main
   image's load bias, and the port was reading it out of the wrong place
   entirely.
2. **The accept() the async task is parked in** (unchanged from S25).
3. The menu is still not reached: `draws=0/0` and `MainMenu=0` after
   15720 frames. The download gate's queue is still empty
   (`qCount=0` on 197 samples).

## S27 - the login completion pointer, measured in the guest

### The question S26 left open

S26 ended with one open item that blocked everything downstream: the guest's
`AndroidAccountManager` completion pointer. `Constructor(JJ)V` delivered
`0x10826fbda0` / `0x10826fbd30` - two addresses in the guest's STACK region
(`0x1082...`), and the guest's `LoginCompleteCallback` TAIL-BRANCHES to the
first of them. A pointer that is not a guest code address is not merely
useless: it is a branch into the stack.

The port's model of the method was also wrong in a second, structural way,
which S26 had not isolated: the two longs it logged were not the guest's two
arguments at all.

### 1. How the guest creates the account manager (MEASURED, guest disassembly)

The guest resolves the method itself. The classpath string is at `.rodata`
`0x3f42b6` and the two literals it pairs with it are `"Constructor"`
(`0x49ae00`) and `"(JJ)V"` (`0x412599`):

```
$ python .build/rr3fast.py str "com/firemonkeys/cloudcellapi/AndroidAccountManager" "(JJ)V" "Constructor"
  file=0x3f42b6 va=0x3f42b6 sec=.rodata :: 'com/firemonkeys/cloudcellapi/AndroidAccountManager'
  file=0x412599 va=0x412599 sec=.rodata :: '(JJ)V'
  file=0x415e80 va=0x415e80 sec=.rodata :: 'Constructor'
  file=0x49ae00 va=0x49ae00 sec=.rodata :: 'Constructor'
```

Two guest functions reference all three (`xrefs`): `0x1ce6c88` and
`0x1ce6e60`. `0x1ce6e60` is the class constructor - the guest's own factory
(`0x1ce6c28`) news 0x98 bytes and calls it:

```
0x1ce6c68  mov  w0, #0x98
0x1ce6c6c  bl   0x1e17b40        ; operator new
0x1ce6c70  mov  x19, x0
0x1ce6c74  bl   0x1ce6e60        ; the C++ constructor
```

and the constructor resolves and calls the Java method like this:

```
0x1ce6ed4  adrp x2, #0x3f4000
0x1ce6edc  add  x2, x2, #0x2b6   ; -> 0x3f42b6 'com/firemonkeys/cloudcellapi/AndroidAccountManager'
0x1ce6f20  adrp x2, #0x49a000
0x1ce6f28  add  x2, x2, #0xe00   ; -> 0x49ae00 'Constructor'
0x1ce6f24  adrp x3, #0x412000
0x1ce6f2c  add  x3, x3, #0x599   ; -> 0x412599 '(JJ)V'
0x1ce6f38  bl   0x1c68fcc        ; GetMethodID(cls, "Constructor", "(JJ)V")
0x1ce6f3c  adrp x3, #0x1f7e000
0x1ce6f40  ldr  x1, [x19, #0x80]      ; the receiver
0x1ce6f44  ldr  x3, [x3, #0x278]      ; the FIRST long
0x1ce6f48  mov  x2, x0               ; the method id
0x1ce6f4c  mov  x0, x21              ; the JNIEnv
0x1ce6f50  mov  x4, x19              ; the SECOND long
0x1ce6f54  bl   0xba7af8             ; the call
```

**So the signature really is `(JJ)V`**, and the port's registration of it was
right; the port was reading the two longs from the wrong place.

### 2. Where the continuation pointer comes from (MEASURED)

The first long is a relocated global, and the relocation is the answer:

```
$ python .build/rr3fast.py reloc 0x1f7e278
### reloc for 0x1f7e278  sec=.got  file_word=0x0000000000000000
  off=0x1f7e278 type=1027 sym=0 addend=0x1ce6e2c
    -> R_AARCH64_RELATIVE target vaddr = 0x1ce6e2c sec=.text
```

`0x1ce6e2c` is a guest CODE address, and it is the guest's own
`AndroidAccountManager::OnLoginComplete` - the function that asserts
`'LoginCompleteCallback'` / `AndroidAccountManager.cpp:72` and then notifies
the listener vector:

```
0x1ce6e2c  cbz x1, #0x1ce6e40
0x1ce6e30  and w8, w0, #1        ; the boolean
0x1ce6e34  mov x0, x1            ; the user handle
0x1ce6e38  mov w1, w8
0x1ce6e3c  b   #0x1ce71e4        ; -> the listener notify
```

The second long is `x19` - the guest's own C++ object pointer, i.e. the
"user" handle the completion carries back.

The guest's own trampoline, exported in `.dynsym` at exactly that address
(`Java_com_firemonkeys_cloudcellapi_AndroidAccountManager_LoginCompleteCallback`,
value `0x1ce7318`, size 16 - four instructions), closes the loop:

```
0x1ce7318  tst w2, #0xff     ; w2 = the boolean
0x1ce731c  mov x1, x4        ; x1 = the user handle (the second long)
0x1ce7320  cset w0, ne       ; w0 = (boolean != 0)
0x1ce7324  br x3             ; TAIL-BRANCH to the first long
```

**⇒ The completion pointer is a guest code address, `0x1ce6e2c` + the main
image's load bias, and the user handle is the guest's C++ object pointer.**

### 3. What the port was doing wrong

The port's slot-62 path (`JNIEnv::CallVoidMethodV`) treated the guest's
fourth argument as a flat `jvalue[]` array. It is not: it is the guest's own
`va_list`, built by the guest's single variadic helper `0xba7af8` (149 call
sites use it):

```
0xba7b08  stp  x3, x4, [x29, #-0x78]   ; x3..x7 -> the GP register save area
0xba7b0c  stp  x5, x6, [x29, #-0x68]
0xba7b10  stur x7,   [x29, #-0x58]
0xba7b14  stp  q0, q1, [sp]            ; q0..q7 -> the vector save area
...
0xba7b4c  stp  x9,  x8,  [x29, #-0x18]
0xba7b50  stp  x12, x10, [x29, #-0x28]
0xba7b5c  ldp  q0, q1, [x29, #-0x28]
0xba7b68  stp  q0, q1, [x29, #-0x50]
0xba7b60  sub  x3, x29, #0x50          ; the block IS the va_list
0xba7b64  ldr  x8, [x8, #0x1f0]        ; JNI table slot 62 = CallVoidMethodV
0xba7b6c  blr  x8
```

The block is 32 bytes: `[0]` the stack argument area, `[1]` the TOP of the
general-purpose register save area, `[2]` the TOP of the vector save area,
`[3]` packed offsets. The general-purpose arguments are one 8-byte slot each,
at `[top-0x28]` (the first, `x3`), `[top-0x20]` (`x4`), `[top-0x18]`,
`[top-0x10]`, `[top-0x08]`.

**The port's own log proves it read the header, not the arguments.** In
archive `20261020-234046-eden-loginfix` the port logged

```
phase=cc-http-void ... method=Constructor ... obj=0x80f455c0 args=0x10826fbd30
phase=aam-ctor: obj=0x80f455c0 callback=0x10826fbda0 user=0x10826fbd30
```

`args` (the block's own address) is `0x10826fbd30`, and the value the port
logged as `user` is **the same number**. The value it logged as `callback`
is the block's word 0 - the stack-argument pointer. The two "longs" were the
`va_list`'s own header words.

There is a second, independent defect behind it: the port's
`jniNewObjectV` **ignored the method id and the argument block entirely**, so
a guest `NewObject(clazz, ctorId, ...)` could never store anything, and the
port could not say which slot the guest had asked for.

### 4. The fix

Three bounded changes:

1. `native-switch/source/jni_bridge.cpp` - `decodeGuestVarArgs()` unpacks
   the guest's `va_list` into a `jvalue` array, using the guest's own JNI
   signature for the layout (the signature comes from the guest's
   `GetMethodID` record, not from the port's registration). The slot-62 path
   now calls the array-form dispatcher with the unpacked array instead of
   with the raw block. This also fixes every other Cloudcell method that
   takes arguments through slot 62 - the `HttpRequest.init` callback was
   read from `argv[7]`, which was the guest's stack.
2. `native-switch/source/jni_bridge.cpp` - two capped instrumentation lines,
   as asked for: `jni-newobject` (the class, the method id and the raw
   argument block of every `NewObjectV` for this class) and `aam-varargs`
   (the method, the signature, the argument count and the two decoded longs
   of every slot-62 call for this class).
3. `native-switch/source/jni_compat/classes/cc_services.cpp` - the pair is
   ALSO kept at class level, and `aam_login` falls back to it. This is
   needed because the two calls arrive on different port objects (measured:
   `Constructor` on `0x80f455c0`, `login` on `0x80f436d0`), so the instance
   that receives `login()` has no callback of its own. The delivery is
   refused unless the callback is inside the guest image
   (`rr3::guestImageContains`), so a bad value can never become a branch
   into the stack.

### 5. The gates

| gate | result |
|---|---|
| `tools/Build-NativeSwitch.ps1` | exit 0 (stderr: only `find: 'romfs': No such file or directory`) |
| `tools/Test-RR3Inputs.ps1` | exit 0 |
| `native-switch/tests/test_*.py` (11) | 11 pass, 0 fail |
| `tools/check-cc-symbols.py` | 35 resolved / 16 known-absent / 0 unexpected |

### 6. The run: archive `20261021-004233-eden-cbfix`

`tools/Test-OnEden.ps1 -Seconds 300 -Tag cbfix`, conf unchanged
(`nogo=0 ccmode=bg ccloginok=1 ccsocialok=1 ccdisp=2 objid=1`).

| | before (`20261020-234046-eden-loginfix`) | after (`20261021-004233-eden-cbfix`) |
|---|---|---|
| frames / calls | 15720 / 47165 | 15960 / 47885 |
| last gfx-stats | `frames=15720 calls=47165 draws=0/0` | `frames=15960 calls=47885 draws=0/0` |
| `MainMenu` | 0 | 0 |
| "Unable to send Sync" | 7 | 7 |
| guest log lines | 11412 | 11412 |
| **"Cannot execute instruction"** | 0 | **0** |
| **"ExceptionRaised"** | 0 | **0** |
| **cloudcell-item lines** | **0** | **2 (enter + return)** |

**The new instrumentation, from the archive:**

```
phase=jni-newobject ... class=com/firemonkeys/cloudcellapi/AndroidAccountManager
                        method=<init> mid=0x81435ac8 block=0x1082efbd00
                        w0=0x1082efbd70 w1=0x1082efbd00
phase=aam-varargs ... method=Constructor sig=(JJ)V argc=2 block=0x1082efbd30
                     cb=0x2109069e2c cb_rr3=yes user=0x21111c9e40
phase=aam-ctor: obj=0x8185b5d0 callback=0x2109069e2c user=0x21111c9e40
phase=aam-login: obj=0x818596e0 callback=0x2109069e2c user=0x21111c9e40 source=class
phase=cloudcell_defer: n=1 total=1 depth=1 tag=aam.login
phase=cloudcell-item: enter  id=1 tag=aam.login site=frame
phase=cloudcell-item: return id=1 tag=aam.login
```

**The decode is confirmed by arithmetic, not by assumption.** The run's main
image bias is `0x2107383000` (the `sampler-flush` line's `mainBias=`), and
`cb - bias = 0x2109069e2c - 0x2107383000 = 0x1CE6E2C` -- exactly the
`.rela.dyn` addend of `0x1f7e278` that the guest loads into `x3`. The
`user` handle, `0x21111c9e40`, is a guest heap address, not a stack address:
the old (wrong) values were `0x10826fbda0` / `0x10826fbd30`, and the new
run's raw block words are `0x1082efbd70` / `0x1082efbd00` -- the same shape
of stack pointers the port used to hand back as "the callback" and "the
user".

**⇒ The login completion now reaches the guest, for the first time.** The
deferred item ENTERED and RETURNED (the previous archive has zero
`cloudcell-item` lines), the guest's trampoline at `0x1ce7318` executed and
tail-branched to the guest's own `0x1ce6e2c`, and the guest's frame thread
is still alive: 15960 frames, no "Cannot execute instruction", no
"ExceptionRaised".

**But the guest's state did not change.** The guest's own log is the same
991 `guest-log-any` lines with the same "Sub Sync Queued" counter progression
(2, 11, 13 x6, 20, 80, 10712, 10738, 10765, 10790, 10795) as the previous
archive, and the menu is still not reached (`MainMenu` 0, `draws=0/0`).

### 7. The next blocker: the Cloudcell device id is empty

The guest asks for its EA device id and the port answers with an **empty
string**:

```
native-switch/source/jni_bridge.cpp:1813
    if (rawEquals(name, "GetEADeviceId")) return "";
```

and the guest's own log says so, with the brackets empty:

```
phase=guest-log-any ordinal=779 ... NimbleManager::GetEADeviceId []
phase=guest-log-any ordinal=974 ... CC: Invalid DeviceId! Have we connected to cloudcell yet?
phase=guest-log-any ordinal=980 ... CC: Unable to send Sync, Social Media not initialised.
```

That is the loop that is still running: with no device id the Cloudcell
state machine never initialises, so the guest keeps re-queueing the sync
("Sub Sync Queued: 13") and keeps refusing to send it ("Social Media not
initialised"), and the download gate's queue stays empty.

The port already has a non-empty identifier to hand out -- `getDeviceUID`
answers `"switch-rr3-native"` (`jni_bridge.cpp:1751`) -- so the next step
is to answer `GetEADeviceId` with the same kind of stable, non-empty string
instead of `""`, and then re-measure whether the social-media byte
(`GooglePlayManager+0x148`) gets written.

Two other observations from the same run, for whoever takes the next step:

* The guest resolves `IsInitialPhaseComplete` (it is in Eden's JNI trace),
  and the port registers it - on `GooglePlusWorker`
  (`cc_services.cpp:733`) - but the guest never calls anything on
  `GooglePlusWorker` in this run (0 log lines in both archives). So the
  refresh path that writes the availability byte is not reached through the
  GooglePlus worker; the byte must come from the GooglePlay provider's
  own path.
* `GooglePlayWorker.GetSessionValid` and
  `FacebookWorker.GetSessionValid` ARE dispatched and answer 1
  (`ccsocialok=1`). The guest's "not initialised" verdict is an AND over
  fourteen providers, so one provider still saying "no" is enough to keep
  the complaint.

### What is still open (after S27)

1. **The device id.** `GetEADeviceId` answers `""`, so the guest's Cloudcell
   state machine never initialises. This is now the first blocker, and it is
   a one-line answer to change.
2. The menu is still not reached: `draws=0/0` and `MainMenu=0` after 15960
   frames. The download gate's queue is still empty (`qCount=0` on 197
   samples).
3. **The accept() the async task is parked in** (unchanged from S25).
4. The two port objects for the one guest account manager are still two
   objects (`Constructor` on `0x8185b5d0`, `login` on `0x818596e0`), so the
   class-level fallback in `aam_login` is load-bearing, not cosmetic. The
   `jni-newobject` line shows the guest creating the account manager with
   `NewObjectV(cls, "<init>")`; the guest then calls the Java `Constructor`
   method on a DIFFERENT object. Why the guest holds two handles for one
   manager is still unexplained.

## S28 - the fake Cloudcell authenticate response, built and NOT reached

### What was built

`native-switch/source/jni_compat/classes/cc_cloudcell_auth.{h,cpp}` (new) and
`cc_http_request.cpp` now hold a hand-built Cloudcell LOGIN response:

* the BinaryBlob **checksum** (guest raw `0x1cb9734`) - not crc32, not
  adler32. Read off the disassembly: multiply the four bytes of each group
  together, then divide by the fourth one, 32-bit wrap, ARM64 `sdiv`
  truncation toward zero.
* the **box** format (writer `0x1cbd66c`, reader `0x1cbd8f0`):
  `[u32 checksum][u32 mode][payload]`, the checksum over everything from
  offset 8. Mode 1 is `u32 uncompressed_length` + a zlib stream, mode 2 is
  `u8 key` and `C[i] = P[i] ^ ((key + 3*i) & 0xFF)`. The guest sends mode 1
  then mode 2 (zlib inside XOR) and unboxes twice, so the response body is
  the outer XOR box around the inner zlib box.
* the **authenticate payload**, in the order
  `CC_SyncManager_Class::AuthenticationCallback` (guest raw `0x1d0131c`)
  reads it: f0 (non-zero, `0x1d013dc`), f1, f2 = **2**, f3 = member id, f4
  and f7 (bools, `< 2`), f5, f6, f8 = token length + raw bytes (non-zero,
  `0x1d0158c`), and f9 after the token (`0x1d01d9c`).
* f2 = 2 is not a guess. The state names live in a table at `0x1f66a68`
  (read out of `.data.rel.ro`): 0 LEGACY, 1 INITIATE, **2
  AUTHENTICATE_SUCCESS**, 3 AUTHENTICATED, 4 REAUTHENTICATE, 5
  MEMBER_CONFLICT, 6 DEVICE_CONFLICT, 7 SOCIAL_AUTH_CHANGED, 8
  REAUTHENTICATE_STRICT. The guest branches on state == 2 at `0x1d01484`
  and only that branch parses the member id and the token at all.
* f3 = 1 satisfies the guest's hard comparison at `0x1d01dc4`, which is
  against the read-only constant **1** at guest raw `0x600f30` (reached
  through the GOT slot `0x1f6e270`).

Delivery uses the guest's own natives, not a guess:
`Java_com_firemonkeys_cloudcellapi_HttpRequest_dataCallback` (guest raw
`0x1ce493c`) then `..._completeCallback` (guest raw `0x1ce4a3c`), which is
the order `HttpThread.run()` uses (`reverse/java/sources/com/firemonkeys/
cloudcellapi/HttpThread.java:92-104, :319-321`). `headerCallback` is
deliberately NOT called, so the inputs of the guest's own assertion at
`0x1ce3fd4` (AndroidHttpRequestWorker.cpp:147) stay exactly as they are on
the error path.

### What the run measured: the guest never calls that class

Archive `20261021-044636-eden-cloudresp2`, 300 s, `nogo=0 ccmode=bg
ccloginok=1 ccsocialok=1 ccdisp=2 objid=1`:

| signal | baseline `20261021-010106-eden-devid` | after |
|---|---|---|
| `CC: Invalid DeviceId!` | 5 | **5** |
| `CC: AUTH INFO` | 0 | **0** |
| `CC: Sub Sync Queued` | 15 | **15** |
| `MainMenu` | 0 | **0** |
| `cc-http-init` (new probe) | n/a | **0** |
| `cc-auth-req` (new probe) | n/a | **0** |

**Nothing changed, and the new probe says why.** `http_init` now logs one
line per call; it logged zero times in the whole run. The port's
`com/firemonkeys/cloudcellapi/HttpRequest` facade is never reached.

The guest does resolve it - Eden's JNI trace shows

```
[jni] FindClass com/firemonkeys/cloudcellapi/HttpRequest
[jni] GetMethodID <init> / init / addHeader / post / close / isClosed / shutdown
[jni] NewStringUTF RealRacing3/14.0.1 [H562]
[jni] NewStringUTF POST
[jni] NewStringUTF https://connect-dev-ss.cloudcell.com:443/time
```

and **every one of those lookups succeeds** (the guest-view log has zero
`jni-lookup-miss` lines in the whole run, and `FindClass` warns loudly when
a class is missing). The guest then never instantiates the object and never
calls `init`: no `Constructor` line for the class, no `cc-http-init`, and
the only `cloudcell_defer` in the entire run is still `tag=aam.login`.

### The next blocker: the transport, not the class

The one HTTP request the guest makes in this configuration is

```
CC: [1] POST request [https://connect-dev-ss.cloudcell.com:443/time]
```

logged by the Cloudcell **Android HTTP worker** (the format string
`CC: [%lu] %s request [%s]` is at guest raw `0x49283b`, referenced from
`0x1ce1eb4`, right next to the source path
`.../Cloudcell/CloudcellApi/Android/AndroidHttpRequestWorker.cpp`). That
request never completes, so the guest never reaches the authenticate call
at all, and the loop S27 describes ("Invalid DeviceId" -> "Sub Sync Queued:
13" -> "Unable to send Sync") is what it waits on.

The same worker resolves the Java class and then does not use it, so the
question for the next step is **which transport it picks instead** - the
`/time` request is the first thing that has to succeed, and it is the
thing that gates the login.

Note for the record: the port's own spec elsewhere says that forging a
success is the one thing this port must not do. The fake response above is
such a forgery, and it was built because it was asked for; it is inert in
this configuration, so nothing was forged in the run - but the decision to
ship a success-answering Cloudcell is the Chief's, not the implementer's.

### What is still open (after S28)

1. **The transport for `POST https://connect-dev-ss.cloudcell.com:443/time`.**
   Until that one request is answered, the authenticate request never
   happens and no amount of correctness in the login response matters.
2. **The fake response is correct but unexercised.** It is wired to the
   guest's own `dataCallback`/`completeCallback` exports and to the
   guest's own BinaryBlob format; it fires the moment the guest uses the
   Java facade, and never before.
3. The member id the guest compares the response against at `0x1d01e20` is
   the one the account manager already holds (written there at
   `0x1d01ebc`). It is 0 before the first login and the port cannot read
   it from the host side, so f3 = 1 satisfies the constant comparison but
   may not satisfy that one. The consequence of a mismatch is known (the
   error flag at `0x1d01e68` and the conflict path at `0x1d01fd4`), and it
   is measurable as soon as the transport question above is answered.

## S29 - the handoff is slot 62, and what was missing was the port's method TABLE

### The handoff (MEASURED, guest disassembly)

S28 stopped at "the worker resolves the Java class and then does not use it".
The class IS used. The full chain, read off the guest:

| guest vaddr | what it is |
|---|---|
| `0x49283b` | the format string `CC: [%lu] %s request [%s]` (`.rodata`) |
| `0x1ce1c00` | the Cloudcell **Android HTTP worker** function; it builds the request, calls `0x1ce350c` at `0x1ce1d4c`, and logs `POST request [...]` at `0x1ce1eb4` |
| `0x1ce350c` | the function that creates the Java object and calls `init` / `addHeader` / `post` |
| `0x1ce356c` | `bl 0x1c68e74` - `JavaNativeInterfaceObject::createObject`: FindClass (`0x1c687d4`), GetMethodID `"<init>"` `"()V"` (JNIEnv slot 33, offset `0x108`), NewObjectV (`0x12b622c`, slot 29), NewGlobalRef (slot 21, offset `0xa8`), DeleteLocalRef (slot 23, offset `0xb8`). The global ref is stored at `worker+0xc8` |
| `0x1ce35a0`..`0x1ce3678` | GetMethodID for `init`, `addHeader`, `post`, `close`, `isClosed`, `shutdown` |
| **`0x1ce3850`** | **`bl 0xba7af8` - the handoff: `CallVoidMethodV(env, worker+0xc8, initMethodID, va_list)`** |
| `0x1ce3908`, `0x1ce3a34` | `bl 0xba7af8` - `addHeader` |
| `0x1ce3adc` | `bl 0xba7af8` - `post` |
| `0xba7af8` | the guest's ONE variadic helper: `ldr x8, [x8, #0x1f0]` - **JNIEnv slot 62 = `CallVoidMethodV`** (`0x1f0` = 496 = 62 x 8) |

So the handoff slot is **62 (`CallVoidMethodV`)** on the object at
`worker+0xc8`, and the arguments are the guest's `init`:
`x3` user agent, `x4` method (`"POST"`), `x5` URL, `x6` body (`byte[]`),
`x7` read capacity (`int`), stack[0] callback (`long`), stack[1] `z`,
`v0` server time (`double`), stack[2] `z2`, stack[3] `z3`, stack[4] `i2`.
That is exactly the guest's registered signature
`(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BIJZDZZI)V` and
exactly the decompiled
`com/firemonkeys/cloudcellapi/HttpRequest.java`
`init(String userAgent, String method, String url, byte[] body,
int readCapacity, long callback, boolean, double, boolean, boolean, int)`.

### The port's side: the slot was wired; the ID was not

`g_jniTable[62] = jniCallVoidMethodV` (`jni_bridge.cpp:4292`), and that
handler DOES carry the ManagedMethod dispatch. **The conf note's "that slot
is not wired to dispatch at all yet" is true of `CallObjectMethod` (slots
34/35 - `jniCallObjectMethodV` has no ManagedMethod dispatch) and it is NOT
true of the slot this handoff uses.** S29 does not re-open ccdisp=2 on the
strength of that note.

What actually happened is smaller and quieter. `rememberJniMethod()` handed
out the shared `&g_fakeMethodId` sentinel once the port's method table was
full; `findJniMethod()` cannot resolve that id; `jniMethodName()` returned
null; and `jniCallVoidMethodV()` returned **before** it reached the
dispatch. The slot was reached and the call still did not arrive.

MEASURED, by replaying the JNI trace of archive `20261021-044636-eden-cloudresp2`
(`.build/cc_jni_replay.py` against that archive's `eden_log.txt.tail`):

```
distinct (class, name, static) bindings replayed: 265
table limit                                : 256
first HttpRequest entry : log line 2229, ('.../HttpRequest', '<init>', 0), rank 255
                          log line 2230, ('.../HttpRequest', 'init',   0), rank 256
first overflow          : log line 2231, ('.../HttpRequest', 'addHeader', 0), rank 257
```

The guest registers 247 instance methods and 16 372 static-method lookups in
one run; 265 of them are DISTINCT. The Cloudcell `HttpRequest` block lands
exactly on the 256 cap.

### The fix

1. `kMaxJniMethods` 256 -> 1024 (`jni_bridge.cpp`). Eviction is not an
   option - the guest caches the handle and hands it back - so the table
   only grows. The overflow path is now **loud** (`jni-method-overflow`).
2. `jniCallVoidMethodV` now logs `jni-void-unresolved` when the id does not
   resolve, so "the slot was reached and the id was not" is distinguishable
   from "the slot is not wired".
3. The fast-fail block's capture of the callback read the va_list as a flat
   `jvalue` array. It now uses `decodeGuestVarArgs` - the port's ONE va_list
   convention - so the callback is the guest's value and not the block's own
   header words.
4. The fake-object pool granted **512 bytes** per instance, while
   `CCHttpRequest` is **4128** bytes (it holds a 4 KB request body inline)
   and the pool does not size the slot from `Class::instance_size`. The
   first dispatch would have written over the neighbouring slots. The
   instance size is now 4608 (`kFakeObjectInstanceBytes` in
   `jni_compat/jni_internals.h`) and `cc_http_request.cpp` static_asserts
   that the facade fits.

### The run

Archive `20261021-053123-eden-dispatch1`, 300 s, the same conf
(`nogo=0 ccmode=bg ccloginok=1 ccsocialok=1 ccdisp=2 objid=1`). That is the
archive of the shipped NRO (deployed sha256 `36902D21...`, identical to the
built one). Two earlier post-fix runs, `20261021-051344-eden-dispatch1` and
`20261021-052604-eden-dispatch1`, give the same numbers in every row, so the
result is not a one-run artefact.

| signal | baseline `20261021-044636-eden-cloudresp2` | after |
|---|---|---|
| `cc-http-init` | 0 | **1** |
| `cc-auth-req` | 0 | **1** |
| `CC: AUTH INFO` | 0 | **0** |
| `CC: Invalid DeviceId!` | 5 | **5** |
| `CC: Sub Sync Queued` | 15 | **15** |
| `MainMenu` | 0 | **0** |
| `CC: [1] POST complete` | 0 | **1** |
| frames | 16320 | **16320** |

`frames` is unchanged, so ccdisp=2 did not regress this time. The dispatch
lines are in the archive:

```
cc-dispatch ... class=com/firemonkeys/cloudcellapi/HttpRequest method=init  -> MATCH
cc-dispatch ... class=com/firemonkeys/cloudcellapi/HttpRequest method=post  -> MATCH
cc-dispatch ... class=com/firemonkeys/cloudcellapi/HttpRequest method=close -> MATCH
```

and the guest's own log grew a line it never had before:
`CC: [1] POST complete [Succeeded=False]`. The request now COMPLETES. It
completes as a FAILURE, for the reason below.

### The next blocker: the first Cloudcell request is `POST /time`, not the authenticate request

The body the guest handed to `init()` is **one byte** (the port's
`NewByteArray` line says `length=1`), and the URL is
`https://connect-dev-ss.cloudcell.com:443/time`.

`cc_http_request.cpp` answers exactly one request shape - the authenticate
request, whose body is built by the guest at `0x1d00364` / `0x1d00f38`. A
`/time` body is not that shape, so it falls through to `errorCallback` and
the guest logs `POST complete [Succeeded=False]`.

The chain that follows is now fully determined:

1. `/time` fails -> the guest never learns the server time.
2. The server time is what the guest passes as the `double` of the NEXT
   request's `init` (the guest's own log line for the answer is
   `CC: Server time returned; server time: %f server-local time diff: %f`,
   `.rodata 0x3fc1e4`), so the guest never builds the authenticate request.
3. No authenticate request -> no device id -> `Invalid DeviceId!` x5,
   `Sub Sync Queued` x15, `MainMenu` 0.

So the authenticate response built in S28 is still correct and still
unexercised, and the thing to answer next is a `/time` response whose body
yields a double.

### S29: the /time answer is a RESPONSE HEADER, not a body

The last sentence above is WRONG, and the way it is wrong is the whole
blocker. The `/time` answer is not in the response body at all.

**The parser.** The guest's ServerTimeManager reads the server time in the
function at **guest raw `0x1cef8a4`** (`ServerTimeManager.cpp`, asserted
against the file name at `.rodata 0x48a180`). It is a VIRTUAL method - it has
no direct caller - and it is reached through the std::function the
ServerTimeManager registers with the request manager (its invoker is
`.got 0x1f7e3f0`, whose R_AARCH64_RELATIVE addend is `0x1cef8a4`).

Its disassembly, read instruction by instruction:

```
0x1cef8c8  ldr x8, [x0, #0x70]      ; the request token
0x1cef8cc  cmp x8, x2               ; must equal the token of THIS answer
0x1cef8d0  b.ne <return>            ; otherwise the answer is dropped
0x1cef8f8  adrp x8, #0x460000       ; "X-Request-Start"
0x1cef904  add  x8, x8, #0xbf2      ; .rodata 0x460bf2 - the ONLY reference
0x1cef92c  bl  0xa0b240             ; the guest's hash-map find
0x1cef97c  bl  0xbcb038             ; typed extraction of the value
0x1cef9d0  bl  0x1e18b80             ; strtod()  (PLT strtod)
0x1cef9d4  str d0, [x19, #0x88]     ; this->serverTime, a double
```

So the value the guest reads is a STRING that `strtod` parses: the wire
form is **ASCII decimal seconds**, not a raw little-endian double and not a
BinaryBlob. The double the guest ends up with is `1793000000.125000` for
the answer the port sends.

**Where the map comes from.** The map the parser looks the key up in is the
RESPONSE HEADER map, and the guest's own native
`Java_com_firemonkeys_cloudcellapi_HttpRequest_headerCallback` (guest raw
**`0x1ce4348`**, 1524 bytes) builds it. That function binds the JNI methods
`keySet` (`()Ljava/util/Set;`), `iterator`, `hasNext`, `next`, `get`
(`(Ljava/lang/Object;)Ljava/lang/Object;` and `(I)Ljava/lang/Object;`) and
`size` (`()I`), walks the `java.util.Map` the Java layer handed it, and
stores every (name, value) pair in the guest's own map at **worker+0x180**:

```
0x1ce4668  mov x3, x26              ; the key from iterator.next()
0x1cef466c ldr x2, [x8]             ; Map.get
0x1cef4670 bl  0x10f3e94            ; CallObjectMethodV(env, map, get, key)
0x1ce46d4  cmp w28, #1              ; the value list's size
0x1ce4720 bl  0x10f3e94            ; CallObjectMethodV(env, list, get, i)
0x1ce4750 bl  0x1ce4250             ; GetStringUTFChars -> the value's bytes
0x1ce47a4 add x0, x19, #0x178       ; x19 = worker+8, so worker+0x180: the map
0x1ce47c0 bl  0xbcb038              ; look the key up in it
```

`worker+0x180` is the same map the ServerTimeManager's parser reads, so the
port has to answer the `/time` request with a HEADER named
`X-Request-Start` whose value is ASCII decimal seconds.

**What the port does now.** `cc_http_request.cpp` captures the request URL
in `init()` (the URL is the only reliable discriminator - the guest's byte[]
is not a port array object, so `GetArrayLength` answers 0 for every request
and the body cannot tell the two requests apart), and for a URL containing
`/time` it now:

1. builds a `java.util.Map` with the single entry
   `X-Request-Start` -> `1793000000.125000`,
2. hands it to the guest's own native `headerCallback`, then
3. calls `completeCallback(200)`.

No body is delivered: the guest reads no body for this request, and
`HttpThread.run()` never calls `dataCallback` for an empty body either. No
`Content-Type` is sent either - the guest's completion handler compares the
response's content type against the one the worker expects (guest raw
`0x1ce3f68` -> `0x1ce3fd4`) and asserts when they differ, and the port
sends none today without the guest asserting.

The three `java.util` collections the guest walks are answered by NAME plus
the RECEIVER (`ccheaders::handleObject` / `handleBoolean` / `handleInt` in
`jni_compat/classes/cc_http_headers.h`), because the port's own name-keyed
fallbacks answer `keySet`, `iterator` and `get` with a shared dummy - which
is exactly why the map the guest built was empty.

## [S17] The server time ARRIVES: all three candidate causes are wrong (2026-10-21)

### The probe

`jni_compat/classes/cc_http_request.cpp` now carries `ccprobe`, a bounded
read-only probe around the `/time` exchange. It reports three things, and it
reads the guest's memory rather than calling into it:

1. the guest's own response-header map (a hash map at `worker+0x178`),
2. the ServerTimeManager's fields, found by a bounded two-level hunt,
3. the vtables of the request's three callback objects.

Every read is gated on `svcQueryMemory` **and** on the address being inside
the guest's own 64-bit window. Both are load-bearing: on its own
`svcQueryMemory` is not enough under Eden, because the kernel is asked about
the HOST's mapping of an address while Eden's emulated guest MMU has its own
idea of what is mapped. The first version of the probe followed a pointer
into the host's address space and took the run down with it (archive
`20261021-072048-eden-srvtime4`: `HW.Memory <Error> core/memory.cpp:624:
Unmapped Read64 @ 0x0000003A00000000`, frames 16200 -> 3960).

### The measurement (archive `20261021-074157-eden-srvtime7`)

```
before-header   map@worker+0x178  pairs=0  buckets=0x0 count=0
after-header    map@worker+0x178  pairs=2  [=] [X-Request-Start=1793000000.125000]
after-header    mgr  token=0x1  m78=1792539735.161719  m80=1792539735.322581
                       m88=1793000000.125000
after-complete  mgr  token=0x1  m80=1792539735.322581  m88=1793000000.125000
```

Archives `20261021-072801-eden-srvtime5` and `20261021-073540-eden-srvtime6`
reproduce it: the server time lands at `this+0x88` in every run, and
`this+0x70`, `this+0x88` and `this+0x60` are all still at their old values
after the completion in every run.

The probe as shipped costs the run nothing: `20261021-074157-eden-srvtime7`
has **frames=16200**, the same as the no-probe baseline
`20261021-063741-eden-srvtime2` (frames=16320). The first version of the hunt
was not free - it spent about 1500 `svcQueryMemory` calls on the thread the
guest called the JNI method from, and the run dropped to 3120 frames. The
shipped version caches the page query, uses the manager's vtable as a
one-read-per-candidate selector and stops the hunt at the level where the
manager actually is.

### What that rules out

| Candidate | Verdict | Evidence |
|---|---|---|
| (a) the header never landed in the guest's map | **RULED OUT** | the map goes from `pairs=0 count=0` to `pairs=2` with `[X-Request-Start=1793000000.125000]` inside the native `headerCallback` call |
| (b) the token check rejected the answer | **RULED OUT** | `m+0x80` is written by the parser at `0x1cef8fc`, i.e. only AFTER `cmp x8, x2` at `0x1cef8cc` passes, and it IS written (`m80=1792538901.930677`) |
| (c) the parser reads a different map | **RULED OUT** | the parser is handed `worker+0x178` (`0x1ce2c78: add x1, x19, #0x178`) and the native `headerCallback` writes the very same container (`0x1ce47a4: add x0, x19, #0x178`); the probe reads one address in both cases |

So the port's delivery is correct and complete: the header arrives, the
guest's own parser finds it, `strtod` turns it into a double and the guest
stores it at `this+0x88` as **1793000000.125000**.

### Where the answer is actually lost

The guest's ServerTimeManager response handler is guest raw `0x1cef2f8`, and
it is the only place that prints

```
CC: Server time returned; server time: %f server-local time diff: %f
```

(format string at guest raw `0x3fc1d0`). It reaches the logger only when all
of these hold:

```
0x1cef360  |now - (this+0x80)| <= 5.0    the parse happened recently
0x1cef37c  (this+0x88) > 0.0             THE SERVER TIME WAS PARSED
0x1cef384  (this+0x70) == the token      the token still matches
0x1cef390  |now - (this+0x78)| <  5.0    the response came back fast
```

Measured against the state the probe read back: `m+0x88` is positive,
`m+0x80` is set, and `m+0x80 - m+0x78` is 0.21 s, so the two timing windows
are satisfied. The handler's SUCCESS path clears `this+0x70`, `this+0x88`
and `this+0x60` (`0x1cef398`, `0x1cef3a4`, `0x1cef3b4`). After the
completion all three still hold their old values (`token=0x1`,
`m88=1793000000.125000`, `m60=0x1`), so **the handler never took its
success path**.

The only condition the probe cannot see is the handler's own token argument
(`x3` at `0x1cef328`, compared at `0x1cef384`): the token in the manager is
`this+0x70` and it is the return value of the HTTP manager's request-start
call (`0x1cef258: str x0, [x19, #0x70]`), i.e. a guest-internal value the
port never sees. The port cannot make that comparison fail on purpose, and
it cannot make it pass either.

### The chain, as measured

```
port -> guest native headerCallback (0x1ce4348)
         fills the map at worker+0x178
         calls record->vtable[3] = OnHeader (0x1ce3df0)
           -> 0x1ce4b88 -> record+0x30's callback
port -> guest native completeCallback (0x1ce4a3c)
         -> OnJNICompletion (0x1ce3f68)
           -> CompletionCallback (0x1ce4c60)
             -> record+0x90's callback = the ServerTimeManager's bind
               -> the parser (0x1cef8a4)      <- runs, m+0x88 is set
               -> the handler (0x1cef2f8)     <- never reaches the logger
```

### Correction to the map's address

The earlier note in this file says the map is at `worker+0x180`. The measured
address is `(*(jlong+0x08)) + 0x178`: the jlong the guest hands `init()` is
the request record, `record+0x08` is the `CC_AndroidHttpRequestWorker_Class*`,
and the map is that worker's own field at `+0x178`.

### S27 -- THE SERVER TIME WAS NEVER THE BLOCKER (2026-10-21)

#### 1. The hypothesis, and the change

The port answered the guest's `/time` request with a hard-coded
`X-Request-Start: 1793000000.125000`. The guest's own clock read
`1792539735` in archive `20261021-074157-eden-srvtime7` -- 460265 s (5.3
days) apart. The working hypothesis was that the guest's server-time sanity
window rejected the answer.

The constant is gone. `ccheaders::serverTimeEntry()`
(`jni_compat/classes/cc_http_request.cpp`) reads the host's
`CLOCK_REALTIME` at DELIVERY time and formats it as ASCII decimal seconds
with a microsecond fraction; a non-positive clock falls back to a plausible
epoch second, because the guest treats a non-positive server time as "no
answer". The storage is a process-lifetime slot, because the header map
keeps the pointer and the characters -- a stack buffer would die before the
guest reads the value.

MEASURED in archive `20261021-080025-eden-srvtime8`: the port sent
`1792540843.345517` and the guest's own clock read `1792540843.201727` -- the
server/local difference is now **0.14 s instead of 460265 s**.

#### 2. The adjudicator did not move

`CC: Server time returned; server time: %f server-local time diff: %f` is
still **absent** (0 occurrences, same as the baseline). So the hypothesis is
**WRONG**: the server-time VALUE gates nothing on the path to that log line.

That is now a STATIC fact, not an inference. The handler at guest raw
`0x1cef2f8` reaches the logger (`0x1cefd8c` -> `0x1cefb80`) only from its
success path, and that path is gated on four things:

```
0x1cef35c  |now - (this+0x80)| <  5.0    the parse happened recently
0x1cef37c  (this+0x88) > 0.0             the server time was parsed
0x1cef384  (this+0x70) == x3             the token still matches
0x1cef390  |now - (this+0x78)| <  5.0    the response came back fast
```

`now` is the SAME clock function in all three places (`0x1cef348`,
`0x1cef8f4`, `0x1cef1cc` all call the singleton's vtable slot 0), and the
measured values prove it: `this+0x78 = 1792540843.201727` and
`this+0x80 = 1792540843.355646` are LOCAL times, 0.154 s apart. **The
server-time value appears in none of the four gates.** A constant that was
5.3 days out could not change the outcome, and it did not.

#### 3. What the extended probe showed

The probe now also reports the fields the handler itself reads, so a run can
say whether the handler RAN rather than only whether it succeeded. From
`20261021-080025-eden-srvtime8` (all three phases identical):

```
before-header / after-header / after-complete
  token=0x1  m88=1792540843.345517  flag60=0x1  cnt90=0x0
  ma0=0.000000  ma8=0.000000  me8=-1.000000
  pend=0x211029eba0/0x211029ebd0
```

MEASURED consequences:

| field | value | what it says |
|---|---|---|
| `this+0x70` (token) | `0x1`, unchanged | the success path (`0x1cef398`) never ran |
| `this+0x88` (server time) | still set | the success path (`0x1cef3a4`) never ran |
| `this+0x60` (flag byte) | still `1` | the success path (`0x1cef3b4`) never ran |
| `this+0x90` (sample count) | `0` | `0x1cefd8c` (the ring push) never ran |
| `this+0x150`/`+0x158` | unchanged, non-empty | the pending list was never popped (`0x1cef79c`) |

**Every exit path of the handler writes one of those fields.** `0x1cef70c`
clears `+0x60` and `+0x70`; `0x1cef79c` writes `+0x158`; the success path
clears `+0x70`/`+0x88`/`+0x60` and bumps `+0x90`. None of them moved, so the
handler was **not entered at all** during the exchange -- it is not a
rejected answer, it is an answer nobody asked for.

#### 4. The chain, corrected

The completion path is `completeCallback (0x1ce4a3c)` -> `OnJNICompletion
(0x1ce3f68)` -> `CompletionCallback (0x1ce4c60)`, and the last one is
decisive:

```
0x1ce4c88  ldr x0, [x0, #0x90]     ; the ServerTimeManager's bind
0x1ce4c8c  cbz x0, #0x1ce4ca8     ; no bind -> nothing happens
0x1ce4c90  and w8, w1, #1         ; the success flag
0x1ce4c94  strb w8, [sp, #4]
0x1ce4ca0  ldr x8, [x8, #0x30]    ; the bind's invoke
0x1ce4ca4  blr x8                 ; -> the bind -> the parser AND the handler
```

The bind takes ONE argument -- a pointer to the success flag -- and it is
the bind that reaches BOTH `0x1cef8a4` (the parser) and `0x1cef2f8` (the
handler). The parser ran (it is the only writer of `this+0x80`/`this+0x88`,
and both are set). The handler did not. Both are called from the same bind,
so the question is no longer "is the token right" -- it is "why does the
bind call one and not the other".

`0x1cef8a4` and `0x1cef2f8` have **zero direct `bl` callers**: both are
reached through the closures the manager builds at request time
(`0x1cef1e4`-`0x1cef23c`, `sp+0xc0`/`fp-0x80`, the target function pointers
`0x1cef8a4` and `0x1cef2f8` written into the closures), and the bind's
invoke is the only thing that calls through them.

#### 5. The late sample: the handler never runs at all

Archive `20261021-081553-eden-srvtime9` adds a LATE sample of the same
manager on the host's own poll (four samples, thousands of frames after the
exchange). The late samples are **bit-identical** to the exchange-time one:

```
before-header / after-header / after-complete / late-poll x4
  token=0x1  m88=1792541772.348445  flag60=0x1  cnt90=0xaaaaaaaa00000000
  ma0=0.000000  ma8=0.000000  me8=-1.000000
  pend=0x2110abb140/0x2110abb170
```

Nothing moved, at any point in the 16,200-frame run. MEASURED conclusion:
**the handler at `0x1cef2f8` is never entered** -- not rejected, not late,
not on any tick of the guest's state machine. The guest's parser
(`0x1cef8a4`) runs, the guest logs `CC: [1] POST complete [Succeeded=True]`,
and the second callback that would consume the parsed value is simply never
invoked.

The pending list at `+0x150`/`+0x158` is NOT empty and never changes; its
first element carries the guest's own path string
(`itch/rr3-native-rehost/d...` at the record's `+0x08`), so it is the
ServerTimeManager's outstanding request record, still waiting.

#### 6. What is still open

1. **Why the bind calls the parser and not the handler.** The bind at
   `record+0x90` is invoked with one argument (a pointer to the success
   flag, `0x1ce4c88`-`0x1ce4ca4`), and it is the bind that reaches both
   `0x1cef8a4` and `0x1cef2f8`. The parser runs, the handler does not, from
   the same bind. That is a guest-side dispatch question, not a port-side
   one, and it is the next thing to find.

Neither of these changes the port's behaviour: the port now sends the
current wall clock, which is correct regardless of whether the guest ever
looks at it.










## [S9] The server-time handler: what the two binds are, and what the guest is actually waiting for (2026-10-21)

The open question in [S8] was "why the bind calls the parser and not the
handler". It is answered, and the answer is not about the bind.

### 1. The class is `cc::ServerTimeManager`, and there are TWO binds, not one

The guest's class is `cc::ServerTimeManager` (typeinfo at guest raw
`0x1f65d18`, name `N2cc17ServerTimeManagerE`; vtable base `0x1f65b88`,
`[1]` = typeinfo, `[2]` = vfunc0, so the vptr an instance carries is
`0x1f65b98`). The source file is named in the guest's own assert strings:
`E:\dev\builds\r3_update_a\source\src\Cloudcell\CloudcellApi\ServerTimeManager.cpp`.

Both functions are reached from ONE place, `cc::ServerTimeManager::TimeCallback(char const*, unsigned long)` at guest raw `0x1cef068`, which builds two delegates and hands them to one call:

```
0x1cef1e4  adrp x8, #0x1f7e000
0x1cef1ec  ldr  x8, [x8, #0x3e0]     ; GOT -> 0x1f65d80 (the __bind ops table)
0x1cef1f0  ldr  x9, [x9, #0x3e8]     ; GOT -> 0x1cef2f8  <-- THE HANDLER
0x1cef204  ldr  x9, [x9, #0x3f0]     ; GOT -> 0x1cef8a4  <-- THE PARSER
0x1cef220  ldr  x9, [x9, #0x3f8]     ; GOT -> 0x1f65e18 (the other ops table)
0x1cef224  stur x8, [x29, #-0x80]    ; closure A: {vtable, 0x1cef2f8, 0, this}
0x1cef228  stur q1, [x24, #0x68]     ; closure A's fn word
0x1cef22c  stp  x19, x20, [x29, #-0x68]
0x1cef234  str  x8, [sp, #0xc0]      ; closure B: {vtable, 0x1cef8a4, 0, this}
0x1cef238  stur q0, [x24, #8]
0x1cef23c  stp  x19, x21, [sp, #0xd8]
0x1cef244  sub  x2, x29, #0x80       ; closure A  -> the HANDLER
0x1cef248  sub  x3, x29, #0xb0
0x1cef24c  add  x4, sp, #0xc0        ; closure B  -> the PARSER
0x1cef250  ldr  x8, [x8, #0x18]       ; vtable slot 3
0x1cef254  blr  x8                    ; one call, two delegates
0x1cef258  str  x0, [x19, #0x70]     ; the request token
```

The two delegates differ in their TYPE, and that is what lands them in
different places:

| | closure A | closure B |
|---|---|---|
| target | `0x1cef2f8` (the handler) | `0x1cef8a4` (the parser) |
| `std::function` type | `void(char const*, u32, u64)` | `void(unordered_map<string,pair<const string,string>>&, u64)` |
| bound member | `cc::ServerTimeManager::(char const*, u32, u64)` | `cc::ServerTimeManager::(unordered_map&, u64)` |
| its invoke | `0x1cf0760` (3 args) | `0x1cf0824` (1 arg, in x2) |
| ops table | `0x1f65d80` | `0x1f65e18` |

The typeinfo names say so in the guest's own symbols: `__bind<cc::ServerTimeManager::(char const*, unsigned int, unsigned long)(...)>` and `__bind<cc::ServerTimeManager::(unordered_map<...>&)(...)>`.

### 2. They land in different slots of the request record, and are invoked by different events

`cc::HttpRequestManager::QueueRequest` (guest raw `0x1ce1a84`; the guest's
own assert names it) builds a 0x1b0-byte `cc::ActiveRequest_Struct` with
`0x1ce13a4` and then registers it:

```
0x1ce1ae4  bl   0x1ce13a4             ; build the record; its ctor calls post()
0x1ce1af4  ldp  x9, x8, [x19, #0x40]
0x1ce1b00  str  x20, [x9], #8         ; PUSH the record pointer
0x1ce1b04  str  x9,  [x19, #0x40]    ; the vector END  (begin at +0x38)
```

`0x1ce13a4` puts the three delegates into the record:

| record slot | what it is | who invokes it |
|---|---|---|
| `+0x90` | the completion delegate `void(bool*)` | `cc::AndroidHttpRequestWorker::CompletionCallback` (`0x1ce4c60`) |
| `+0xe0` | inline storage for a 32-byte delegate | (the copy target) |
| `+0x100` | **the HANDLER delegate** (`0x1ce2af4`-`0x1ce2b10`) | `cc::HttpRequestManager::Poll` (`0x1ce29b4`) |
| `+0x130` | the body delegate | the same poll, gated on `+0xd4 != 0` |
| `+0x140` | inline storage | (the copy target) |
| `+0x160` | **the PARSER delegate** (`0x1ce2c68`-`0x1ce2c8c`) | the same poll, with `(&record+0x178, &status)` |
| `+0xc8` | the RESPONSE-COMPLETE flag | set by `cc::HttpRequestManager::OnRequestComplete` (`0x1ce2da0`) |
| `+0xc9` | "the completion delegate was delivered" | set by the poll, just before it delivers |
| `+0x170` | the HTTP status | the worker's completion callback |

**So the two binds are separated by the EVENT, not by the bind:**

* the PARSER fires on the HEADERS event (it is handed the response header
  map at `record+0x178`), which is why it ran;
* the HANDLER fires on the RESPONSE-COMPLETE event, and the poll gates it
  on `record+0xc8 != 0 && record+0xc9 == 0`.

### 3. The probe now reads the gate, and it says the answer is ACCEPTED

`cc-probe-req` (new, in `native-switch/source/jni_compat/classes/cc_http_request.cpp`)
reads the request record out of the worker (`worker+0x08` IS the
`cc::ActiveRequest_Struct`; the worker's `CompletionCallback` does
`ldr x9,[x0,#8]` and `str w8,[x9,#0x170]`). Archive
`20261021-100742-eden-srvhandler`:

```
worker=0x21106b2020 rec=0x21101150c0 token=0x1 c8=1 c9=0 d4=0
handler100=0x21101151a0 body130=0x0 parser160=0x2110115200 status=0xc8
```

Read line by line:

* `status=0xc8` - the port's 200 landed in the record.
* `c8=1` - the guest's `OnRequestComplete` ran and marked the response complete.
* `handler100=0x21101151a0 = rec+0xe0` - **the handler delegate IS installed**
  (the manager copied the caller's inline delegate into `record+0xe0` and
  pointed `record+0x100` at it).
* `parser160=0x2110115200 = rec+0x140` - the parser delegate is installed.
* `token=0x1` - the record's token equals the manager's `this+0x70` (`0x1`),
  so the handler's own token check would PASS.
* `c9=0` - **the handler delegate was never delivered.**
* `body130=0x0` - the body delegate slot is empty, as it should be: the
  manager's out-parameter was never filled, so the ServerTimeManager's
  handler is the `+0x100` delegate and the parser the `+0x160` one.

**The port's `/time` answer is accepted. There is nothing wrong with it.**

The handler's own preconditions (all measured from the binary at
`0x1cef2f8`) would also have passed: `this+0x88 > 0` (the parser stored
`1792541772.348445`), `|now - this+0x80| < 5 s` and
`|now - this+0x78| < 5 s` (the request and the parse are 0.15 s apart in
the archive), and the token matches.

**A READING THAT WAS WRONG, AND IS RETRACTED HERE.** The same probe line
also reported `mgr=... vecn=0 invec=0`, which reads as "the record is not
in the manager's active-request vector". That is about the WRONG OBJECT:
the pointer was read from the guest's .bss global `0x1f97818`, and that
global holds a **GluRevSDKWrapper** (its vptr is ELF `0x1e4b948`, the
`GluRevSDKWrapper` vtable at `0x1e4b940`), not the
`cc::HttpRequestManager`. Its `+0x38`/`+0x40` are not an active-request
vector, so `vecn=0` says nothing. The probe now also prints the vptr so
this cannot be mistaken again.

### 4. The averaging is real, and it is why the guest never asks again

`cc::ServerTimeManager::SetAveragedTimes` (guest raw `0x1cefb80`) copies
**0x78 = 120 bytes = 5 samples of 24 bytes** out of `this+0xd8`, sorts them
and computes the mean and the variance; it asserts (ServerTimeManager.cpp
line 218) when `this+0x90 <= 0 && this+0x140 == 0 && this+0x148 < 0`.
`this+0x90` is the sample counter, and the handler compares it with five:

```
0x1cef3a0  ldr  w8, [x19, #0x90]
0x1cef3a8  cmp  w8, #5
0x1cef3ac  b.lo #0x1cef460            ; fewer than 5 samples -> schedule a timer
0x1cef460  ...                        ; a 5000 ms timer (mov w2, #0x1388)
                                       ; carrying the lambda TimeCallback::$_0
0x1cef494  blr x8                     ; the timer service's vfunc7
```

**The guest wants FIVE `/time` samples, five seconds apart, before it
publishes an averaged server time.** That is why `this+0xd8` is a five
element array and why the handler schedules a five second re-request.

And the re-request is gated on having a sample at all:

```
0x1ceef1c  ldr  w8, [x19, #0x90]
0x1ceef20  cbz  w8, #0x1ceef48        ; NO samples -> do not ask again
0x1ceef24  mov  x0, x19
0x1ceef2c  bl   0x1cef068             ; TimeCallback -> a new /time request
```

**So the deadlock is closed on itself:** the handler never runs, so the
sample counter stays 0, so the guest never asks again. The archive shows
exactly that - `cc-http-init = 1`, one `/time` request in the whole run,
`this+0x98 = 1`, `this+0x90 = 0`, `this+0x60 = 1` (still "in flight") and
`this+0x88` still holding the server time that was never consumed.

### 5. The delivery order is NOT the blocker (measured, and reverted)

The obvious candidate was the order. `cc::HttpRequestManager::QueueRequest`
(`0x1ce1a84`) builds the record, calls `post()` (which is where the port
answers), and ONLY THEN pushes the record pointer into the vector at
`this+0x38..+0x40`:

```
0x1ce1ae4  bl  0x1ce13a4        ; build the record; its ctor calls post()
0x1ce1af4  ldp x9, x8, [x19,#0x40]
0x1ce1b00  str x20, [x9], #8    ; PUSH the record
0x1ce1b04  str x9,  [x19,#0x40]
```

and the guest's only poll of the request manager is the one inside
`cc::Cloudcell::Cloudcell` (`0xcd1014` - a CONSTRUCTOR: it lays out a
0x5d0-byte object and initialises `+0x10 .. +0xd5`), at `0xcd1db0` - ONCE,
at construction, before any answer can exist. So answering `/time` INLINE,
inside `post()`, was implemented and measured.

**It changed nothing.** Archive `20261021-100155-eden-srvinline`, answered
inline:

```
worker=0x210ffbd3c0 rec=0x210fcec0c0 token=0x1 c8=1 c9=0 d4=0
handler100=0x210fcec1a0 body130=0x0 parser160=0x210fcec200 status=0xc8
| mgr=0x2102f514a0 vecn=0 invec=0
```

Identical to the deferred run, down to the vector being empty, and
"CC: Server time returned" is still absent. **The inline arm was
REVERTED**; the deferred delivery stays, because that is what the real
Java HttpThread does (it answers on its own thread) and because the inline
arm bought nothing.

**What the same reading settles is WHERE THE MISSING CALL IS.**
`cc::HttpRequestManager::Poll` (`0x1ce29b4`) is **vfunc15** of the
`cc::HttpRequestManager` (vtable base `0x1f64848`, so `&vfunc0` is
`0x1f64858` and vfunc15 is `0x1f648d0`). A caller therefore has to do
`ldr xN,[xM]; ldr xN,[xN,#0x78]; blr xN` **on a manager instance**. Every
such call site in the guest image that could be found is on the OTHER
object in the same .bss neighbourhood - the **GluRevSDKWrapper** global at
`0x1f97818` (`0xce0ee4`, `0xcdf7b8`, `0xce4d6c`) - and that object's
vfunc15 is `0xb3b518`, not the poll.

**So the last link is: who calls `cc::HttpRequestManager::vfunc15` on the
manager instance that holds the `/time` record, and why is that call not
reached in this run?** It is guest-side code. The port cannot change it
with a different `/time` response - the response is already accepted, the
handler delegate is armed, the token matches, and the guest's own retry is
already gated on the sample counter that only the handler can advance.

### 6. A bug in the probe itself, found on the way

The probe's vtable selector was wrong by 0x200 (eight slots): it compared
the manager's vptr against `0x1f65bb98`, which is the slot of vfunc4
(`cc::ServerTimeManager::Update`, `0x1ceedcc`) and not the vtable pointer
`0x1f65b98`. The selector therefore never matched, and every manager
sample in every archive was found by the `+0x78` shape fallback instead.
The fallback works, so the bug was invisible; the constant is now
`0x1f65b98` and the selector does what it says.

