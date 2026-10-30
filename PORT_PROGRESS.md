# Real Racing 3 Switch rehost progress

Updated: 2026-09-09 (input-pump-v1 / toward playable)

## Evidence ladder

| Milestone | Static | Real Switch | Status |
|---|---:|---:|---|
| Native libnx NRO build | passed | passed | complete |
| Horizon TLS/JIT probe | passed | passed | JIT call path confirmed; Android TLS layout unavailable |
| Embedded RomFS/SO | passed with SHA-256 | passed | complete |
| ELF header/program/dynamic tables | passed | passed | complete for current SO |
| PT_LOAD copy/BSS zero | reference matched | passed | complete for current SO |
| RELATIVE relocation | 142,661 | 142,661 | complete for current SO |
| Dynamic symbol audit | 706 symbols expected | passed | counts, versions, categories, and name hash matched |
| External address binding | 270 explicit host/shim/object addresses linked | pending v10 | adds guest dlopen/dlsym/dladdr scope; libc++ closure remains complete |
| Android libc++ dependency audit | 160 external symbols expected | passed v7 | counts and hashes matched on real Switch |
| libc++ external closure | 160/160 symbols, 162/162 relocations | passed v9 | includes C++ lifecycle, phdr, futex/TID, and Bionic stdio bridge |
| Retained two-DSO cross-link | 155,658 relocation writes | passed v9 | all writes read back; constructors not run |
| Image-sized executable-memory probe | 33,572 KiB | passed v9 | both controlled edge `ret` stubs executed |
| Core loader session | 4 images / 35,156 KiB | passed on device | zero strong misses; optional AR/metrics virtualized |
| Segment/RELRO permissions | one-pass process-code + 11 disjoint final ranges | passed on device | 11/11 verified, failures 0 |
| Native loader UI | four pages + report export | passed on device | recovery and renderer actions available |
| Mesa host renderer | unload-gated EGL NRO stage | passed 0.18.7 | clean return; no BadGfxDequeueBuffer or hbl 0xD401 recurrence |
| Loader teardown | reverse four-image ProcessCode unload | passed 0.18.7 | PLUS and Y handoff return cleanly to hbloader |
| Dependency constructors | libc++ 1 + Nimble 0 + FMOD 2, five finalizers | passed on device 0.21.0 | scoped guest TPIDR_EL0 fix validated end-to-end; no TP+0x28 recurrence |
| Mesa real-asset pipeline | gui-sd DXT sprite: VFS→inflate→DDS→glCompressedTexImage2D→draw | passed on device 0.21.0 | `graphics stage=10`; Switch Mesa exposes GL_EXT_texture_compression_s3tc, native DXT upload |
| RR3 main constructors | 181 init_array targets | passed on device 0.22.1 | `initializeGuestMainConstructors()` runs them under GuestTlsScope, per-ordinal attribution (ZL) |
| RR3 JNI lifecycle | 233-slot JNIEnv; JNI_OnLoad→onCreate→onStart→onResume | passed on device 0.22.3 | 196 method bindings and all three lifecycle callbacks returned |
| Guest View lifecycle + one swap | Created/Changed/Render + eglSwapBuffers | **passed on device round 8** | `callbacks=1/1/1 swap=1 mode=1 code=0`; live report exported |
| Guest pthread quiesce / PLUS | reaper-only join + dirty exit | quiesce-v3 returned; v4 hardens exit | round 9: live=6, reached main-loop-exit then teardown error; v4 uses svcExitProcess |
| Joy-Con -> guest controller | ControllerManager JNI pump | input-pump-v1 delivered | ABI from Java ControllerManager.java; device validation pending |
| Renderer/audio/gameplay | inventory only | frame loop 872 frames (checkerboard) | not menu/gameplay; audio not run |

## Current symbol boundary

- 151,591 non-PLT RELA entries and 509 PLT entries.
- 142,661 `R_AARCH64_RELATIVE` entries are applied.
- 34 symbol relocations reference definitions inside the main DSO.
- 9,405 relocations reference 706 undefined dynamic symbols.
- 215 of 706 names exist somewhere in target libnx/newlib/libm/libstdc++ archives, but name equality is not ABI proof.
- Of those 215, 157 are ordinary C/POSIX name-match candidates, 13 require explicit Bionic ABI shims, and 45 are C++ ABI-risk matches.
- The main DSO directly resolves 189/706 names through the host table; cross-linking the matching libc++ supplies another 190 unique C++ names and 8,842 relocations.
- The matching Android libc++ now resolves all 160 external names and all 162 external relocations.
- The simultaneous two-DSO pass applies 144,089 RELATIVE and 2,317 internal relocations, then patches 9,252 main/dependency external or weak slots. In total 155,658 writes are read back.
- 181 main constructors plus three dependency constructors and seven total finalizers are inventoried; all 191 targets validate. The first libc++ attempt reached `DoIOSInit` and exposed the missing guest TLS scope before FMOD ran.
- Remaining major game groups still include 145 GLES/EGL, 52 FMOD, 19 ARCore, JNI, filesystem data-structure ABI checks, and platform services.

## Progress interpretation

- **Loader framework:** 100% for the current four-DSO core scope. ModuleCode mapping, final page permissions, RELRO, dependency scope, relocation, lifecycle order, rollback, dynamic-link APIs and UI all passed on real hardware.
- **Toward first controlled game-core call:** complete for the one-shot View probe (round 8). Main ctors + JNI lifecycle + Created/Changed/Render + one swap are device-proven. Persistent frame loop, input, and audio are not.
- **Toward first menu:** still below a menu claim. View callbacks returned and one swap succeeded, but there is no evidence of the real RR3 menu scene, only the guest View path.
- **Toward playable gameplay:** below 10%. No race, no controller mapping into guest, no audio output validation.

These percentages are engineering estimates, not completion claims.

## Next gates

1. Device-validate input-pump-v1: progress should show `guest-controller-connected`. In the frame loop, A/B/dpad/sticks must reach the guest (even if the picture is still garbage). PLUS dirty-exits via `view-probe-dirty-exit`.
2. If the screen turns solid blue, the guest is not drawing — debug Render/Changed asset path. If it stays checkerboard, the guest is presenting garbage — debug shader/clear/texture.
3. When a menu-like scene appears, map confirm/back (A/B or touch) and only then chase offline race.
4. Audio (P5) can start in parallel once input reaches the guest.

---

# 2026-10-04 — Eden attribution: where the second render call goes

Added after a night of static attribution on the Eden run. The result is
mostly NEGATIVE, which is the useful part: eight hypotheses were raised and
killed with measurements, so nobody re-runs them.

## The symptom, stated correctly

`frames=0`. Not 1 — `gfx-stats frames=1` is the GRAPHICS LAYER's own tally,
not the frame loop's counter. `++frames` sits after the render call, so a
render call that never returns leaves it at 0. Reading the gfx number as
"frame 1 completed" is what sent the previous rounds after a phantom
"frame 2 wedge".

Process shape, from `20261004-061823-errno`:

- CPU climbs +1.535 s per second (14.42 / 30.05 / 45.66 / 61.22 at t=10..40)
  while `logBytes` is frozen at 92 011. That is a SPIN, not a deadlock —
  the harness already had the discriminator in its own comments
  (`guest_view_probe.cpp`: a spin climbs, a deadlock collapses to ~0).
- All guest threads park inside the emulator; none is running guest code.
- 13.16 s of ~950 000 null-page faults, then silence: the fault storm is
  caused by the spin, not its cause.
- The render straddle is latched to the FIRST call
  (`!g_renderStraddleEmitted.exchange(true)`), and `render-call-post` is the
  next statement after the call. Pre present + post absent therefore means
  the first and only `onViewRenderJNI` never returned.

## Refuted, with the measurement that killed each

| Hypothesis | Killed by |
|---|---|
| deadlock | CPU +1.535 s/s, log frozen |
| render function has loops | Tarjan SCC: 567 SCC, **0 real cycles** |
| a callee spins | 27 internal calls checked; largest cycle 172 insns, **bounded on both sides** |
| `end-begin` vector size overflow | **zero** `sub reg,reg,reg` in any reachable callee |
| a host import blocks | all 6 excluded (below) |
| crash | `abort` / `__stack_chk_fail` already log via `guestTracedAbort` / `bionicStackChkFail`; three archives show nothing |
| `[rend+0x4fc] == 4` is the gate | wrote 4 there, guest read back 4, frames still 0 |
| model / Build fields / plist / `onlyWIFI` XML set the id | six variants, `p500` proves each edit reached the engine, `t4fc` never moved |

The six host imports: `__stack_chk_fail`, `abort`, `eglGetDisplay`
(`view-egl-get-display` and `-ret` are both present), `usleep` (gated by
`cbz` at `0x12af0a0`, and `b50d=0` so it never ran), and the non-blocking
`__cxa_guard_*` / `__cxa_atexit`.

Reachability mattered: walking the CFG from the `frameState==0` arm reaches
287 of 567 instructions and makes exactly ONE PLT call. The largest
"loop-heavy" candidate, `0x12d6158` (8 back edges), is on the `frameState==4`
arm and is never entered.

## The actual blocker: no runtime observability

Every reachable loop is bounded, no host import blocks, nothing crashes, and
the process still burns a core. The PC of a running guest thread cannot be
read here:

- `svcGetThreadContext3` returns an address outside every registered image
  (`img=-1` in the sampler)
- the sampler is starved — 8 samples where 75 ms over 75 s implies ~1000
- `0x3B` is the one preemptive priority libnx documents, and it is a Horizon
  hardware semantic Eden does not honour, so it cannot be raised
- `serviceGetThreadContext` was already dead

So "the guest is executing a long straight-line compute path" and "the
emulator is spinning" are **indistinguishable** with the tools available.

## What this implies

The static road is exhausted for the Eden path. The highest-value next move
is REAL HARDWARE, because the null backend is an emulator workaround rather
than a fix: on a console the NRO uses devkitPro Mesa (nouveau/DRM, NVN on
Maxwell) and `eglInitialize` does not spin. `PORT_PROGRESS.md` already
records "Created/Changed/Render + one swap are device-proven" — one swap
has worked on hardware and never has under Eden.

`tools/Use-Real-Hardware.ps1` moves the card between the two backends; the
conf file's `backend=null` is an EDEN opt-in and leaving it on a console
forces the stub backend and a permanently black screen.

Instrumentation added this round stays in `render-call-pre` and is read-only:
`ra` / `t4fc` / `p500` / `b4a4` / `b4a6` / `b50c` / `b50d` / `w4c8` / `vt38`.
`kForceTier` is pinned to 0; the record of the experiment it was is in the
comment above it.

## The 32 stubbed GL entry points are not the problem

`gfx-proc` reports 32 names answered by `generic-stub`, which had been the
standing "null backend is missing something the engine actually needs"
suspect. It is not.

**None of the 32 has an entry in the guest's `.dynsym`.** The guest does not
import any of them; it looks them up by name through `eglGetProcAddress`,
which is exactly what a GL/ES driver probe does. `gfx-proc` printing a name
is therefore evidence that the guest is *probing*, not that it depends on it.

The set is entirely optional surface: KHR debug messaging, AMD perf
monitors, ANGLE blit/multisample, 3-D texture OES, EGL-image-to-texture,
program binaries. A `glXxx` probe returning null is the correct answer for
every one of them, and the guest takes the "not supported" arm.

The one that is not optional in spirit is `eglGetPlatformDisplayEXT` /
`eglGetPlatformDisplay` -- EGL 1.5 core, and a plausible way to lose the
display. It is answered: the log shows `platformDisplayEXT=0x80d8a9b0
platformDisplay=0x80d8a9b0`, and the EGL sequence completes
(`view-egl-init-ok` / `-config-ok` / `-surface-ok` / `-context-ok` /
`-current-ok`). That path was never broken.

Conclusion: `unknown=32` in `gfx-stats` is a count of *probes declined*, not
of missing capability. It should not be read as a defect count, and adding
real implementations for those 32 would buy nothing on this path.

## Where the guest actually stops: before resource loading

The last file operation of any kind, from `api-touch` in
`20261004-061823-errno`, is ordinal 28:

```
api-touch ordinal=19 guestFopen  .depot/sv.2.dat.bak        writing=1
api-touch ordinal=22 guestRemove doc/\sv.dat.bak            writing=1
api-touch ordinal=23 guestFopen  doc/sv.2.dat               writing=1
api-touch ordinal=24 guestStat   .depot/ctd.dat
api-touch ordinal=28 guestOpen   .depot/ctd.dat             writing=0
```

That is the guest saving its profile and then reading `ctd.dat`. Nothing
touches the filesystem after it -- the next logged milestone is the render
call at ordinal 285.

`ctd.dat` is **8 bytes**, `d8d518a82c0685ed` (read straight out of
`core.romfs`; the value previously cross-checked against the upstream zip's
CRC). `sv.2.dat` is NOT in the data volume at all -- it is a save file the
guest creates on first run, under `sdmc:/switch/rr3-native-rehost/doc/`.

Read together with `gfx-stats`:

```
texImg=2/33024      compTex=0/0/0      vbo=0      draws=1
```

the guest has uploaded only screen-sized textures and **zero** game assets.
So it is not stuck *loading* resources -- it stops before loading any. That
narrows the target considerably: the block is between "read the content
table" and "request the first asset", and it is on the render call, not in
the loader or the asset system.

This also means the 9.0 GiB of volumes are not the current problem. Whatever
stops the guest here does so before it asks the filesystem for a single
game file.

## The filesystem layer is clean

The `ctd.dat` open completes, with the right bytes:

```
cloudcell-probe ordinal=25 api=guestStat rc=0 errno=0 size=8 mode=100444
                    device_path=rr3core:/files/.depot/ctd.dat
open-ok           ordinal=29 api=guestOpen rc=0 errno=0
                    head=d8d518a82c0685ed
```

`stat` succeeds, `open` succeeds, the path mapping lands on
`rr3core:/files/.depot/ctd.dat`, the size matches the real 8-byte file, and
the first 8 bytes read back are byte-identical to the file in `core.romfs`.
Two earlier `stat` calls on the same path returned the same thing.

So the guest gets exactly the right answer for its content table and then
stops. Nothing about path resolution, volume mounting, or readback is
implicated -- which means the 9.0 GiB of volumes, the `map-app` fallback
chain, and the `apk/res` vs `.depot` precedence are all doing their job on
the one file we can watch end to end.

This also fixes the boundary of the search. The wedge is not:

- in the loader or the filesystem bridge
- in asset loading (no game asset is ever requested)
- in the GL stack (EGL initialises, shaders compile and link, one draw lands)
- in any loop (SCC says the render function has none)

It is between "content table read" and "first asset request", on the render
call, with a core burning and nothing observable being emitted. Every layer
that CAN leave a trace left the right one.

## An asymmetry in the socket shims: accept is bounded, recv is not

Three archived runs contain **zero** `sock-poll` lines, while `censusPoll` is
wired to emit one. The guest therefore never calls `poll()` at all: it does
blocking `recv`.

That matters because the two directions are not treated the same way.

`censusAccept` is explicitly hardened:

```cpp
constexpr int  acceptWaitMs          = 50;
constexpr bool acceptBlocksForever   = false;
// ... poll(&waiter, 1, acceptWaitMs) before accept ...
// "This deliberately never becomes an unbounded park, which is the defect
//  being fixed."
```

So accept was found to park forever once, and the fix was a bounded wait
that returns `EAGAIN` instead of hanging.

`censusRecv` has no equivalent. It is a straight pass-through:

```cpp
ssize_t censusRecv(int sockfd, void* buffer, size_t length, int flags) {
    CENSUS_IMPORT("recv");
    return ::recv(sockfd, buffer, length, flags);
}
```

So if the guest ever waits for bytes that never arrive, it parks inside
`::recv` with nothing upstream able to break it out. That is consistent with
everything observed: threads parked at reproducible addresses, no log
output, a full core.

This also means the "no data after accept" observation cannot be trusted on
its own -- `censusRecv` records nothing, so absence of a recv log line is
absence of instrumentation, not absence of traffic. The `sock-poll=0` result
is the trustworthy half: that one is absence of a call, because a call would
have been logged.

**Not fixable in this pass.** `acceptWaitMs`, `acceptBlocksForever` and
`allowLoopbackConnect` are `constexpr`, and `symbol_resolver.cpp` is frozen
byte-for-byte by the binding-table guard. A future change would need a
bounded `recv` (MSG_DONTWAIT + poll + EAGAIN) mirroring the accept fix, and
a decision on what the guest should see when its IPC peer is silent.

## CORRECTION to the park-point claim above

An earlier paragraph in this file said the three guest-thread park addresses
sit in "the same low-address region" and described the behaviour as fully
reproducible across runs. Both are wrong, and the correction matters because
the earlier text was the basis for a conclusion about where the wedge lives.

What the archives actually contain, from `phase=census-pc` (per-thread PC):

| run | p1 (x2) | p2 (x1) | p3 (x1) |
|---|---|---|---|
| 20261004-061823-errno | 0x080a86908 | 0x080a86994 | 0x08056c364 |
| 20261004-060230-wedge3 | 0x0808128a8 | 0x080812934 | 0x08084ea48 |

- The **(2,1,1) count pattern reproduces**, and so does **`p2 - p1 = 0x8c`
  exactly**, with both addresses shifting by the same amount
  (-2,572,384) between runs. So p1 and p2 are the same object, ASLR-shifted
  together. That part holds.
- **p3 does not reproduce.** It shifts by +3,024,612 -- a different delta --
  and its distance to p1 changes sign between runs (-5,350,820 vs +246,176).
  **p3 is a different object and was counted in as if it belonged with the
  other two.**

And the "same low-address region" phrasing is wrong for a second reason. The
two `cc-slot` caller addresses discussed nearby were subtracted against
*different runs' bases*: an Eden guest-space absolute address was reduced by a
base it is two orders of magnitude larger than, producing a positive-looking
ELF VA that did not mean what the surrounding table implied. The elfva
`0x2d21f0` from that table is in fact ordinary guest code -- file-backed, in
the first PT_LOAD's R+X segment -- and has nothing to do with p1/p2/p3.

What survives: p1 and p2 are reproducible, reproducible spacing, in the
emulator's guest address space rather than any registered guest image, and not
readable through the host (`census-deref` reports `vkind=zero`). Every census
sees two threads at p1 and one at p2. That is the whole claim; it is smaller
than what was written above, but it is what the data supports.

## p1 / p2 are mapped, executable pages

Cross-checked against the emulator's own log: there is **no
`core/memory.cpp` fault anywhere in the 0x080a8690..0x080a8699 window**
(errno run) or **0x08081280..0x08081293** (wedge3 run). Eden reports an
unmapped access for every guest access it cannot satisfy, and the run
produces ~950 000 of those in 13 s, so the silence is meaningful: those pages
ARE mapped.

That resolves the apparent contradiction with `census-deref`, which reported
`pc=0x8128a724 -> vkind=zero` for one of them. `vkind=zero` is the HOST's
read failing, not the page being absent. Two different questions were being
read as one.

So: two threads park at p1 and one at p2, in real mapped guest-address-space
code that the emulator supplied and no registered guest image accounts for.
They are not in `libRealRacing3.so`, and they are not garbage.

What they ARE cannot be determined from the host side. Identifying them needs
the emulator's module map, and Eden 0.2.1 does not log module load addresses
(searched for `Load.*0x...`, `base`, `Mapping`, `mapped`: no hits, only
`HW.Memory` fault lines). The `0x08xxxxxx` range is not where a Switch NRO
would sit, so these are most likely the emulator's own guest-side helpers
rather than a loaded title.

This is the closest the host side can get. Beyond it the next step is the
console, where the same question is answerable because the guest there runs
against the real kernel.

## Multi-core is NOT why the reporter is starved (tested, negative)

Hypothesis: the guest runs single-core, so `threadCreate(..., -2 /* default
core */)` puts every thread on core 0, the main thread saturates it, and the
census reporter / PC sampler never get scheduled. That would explain a reporter
that only ever completes one sweep.

Eden writes a per-game config at
`user/config/custom/rr3-native-rehost.nro.ini`; every key in it was
`use_global=true`. Changed `[Core] use_multi_core` to an explicit per-game
`true` and re-ran (archive 20261004-065035-multicore):

```
census-sweep : 0        (previous runs: 1, 1, 1)
sample-raw   : 8        (identical to every previous run)
census-pc    : 6        (previous runs: 4-5)
frames=1  calls=415  draws=1/0      -- rendering behaviour unchanged
cpu 31.17 / 47.30 / 63.31           -- still +1.6 s per second
```

**Refuted.** With the guest on multiple cores the reporter got *fewer* sweeps,
not more, and the sampler was still cut off at the same 8 samples. So the
starvation is not core sharing and not time-slicing of one core -- whatever is
saturating the machine does not yield, whatever the guest core count is. The
one real change is `census-pc` rising from 4-5 to 6, i.e. one more thread was
observed, which is consistent with "multicore changed the thread mix" rather
than "multicore freed CPU".

Rolled back: `use_multi_core\use_global=true` restored. Keeping a per-game
override that provably does not help the objective is not worth the config
drift.

Practical consequence: the PC sampler cannot be rescued by configuration. It
needs the guest to yield, which only happens if the guest stops spinning -- so
"raise the sampler priority" was always the wrong lever, and the right one was
always "find the spin".

## VA 0 is a vtable, and pointing its slots at a real `ret` halves the CPU

The zero-page code's own comment says it, and it had been sitting there:

    zpmode=ret exists because the sweep proved the guest *calls through* the
    value it reads at +0x8 / +0xC0: 0xffffffff there makes it branch to
    0xffffffffffffffff.

So the guest treats VA 0 as a vtable and CALLS the words at +0x8 and +0x1C0.
That retro-explains a lot that was previously filed under "unknown":

- the 950 000-fault storm, 12.8% of it writes to `0x10` / `0x18` -- the guest
  writing vtable slots;
- `zpfill=ff` (archive 20261004-065640): the slots became 0xffffffffffffffff,
  the guest branched into nowhere, and it never started at all
  (`threads=1 total_imports=0`). I had read that run as "the guest lived", from
  an older archive where a different NRO happened to survive. Wrong direction.
- three park points at a constant 0x8c apart: two adjacent slots, both called,
  neither returning.

Measured with the slots aimed at a real `ret` compiled into the NRO
(`zpstub=1`, archive 20261004-070200-zpstub):

    zero-page-map  zp8=0x00000000801955a8  zp1c0=0x00000000801955a8
    faults         221      (baseline 948,021)
    cpu            8.55 s at t=10   (baseline +1.6 s per second)

**The CPU load was partly the guest spinning on itself through VA 0.** Aim the
slots at a real return and that disappears, and the host load halves. That
confirms the vtable reading and it is the first result tonight that changed a
physical quantity rather than just a conclusion.

It is also NOT the startup failure: `threads=1 total_imports=0`, no
`render-call`, no `gfx-stats` with the stub in place. So VA 0 accounts for the
spin and for the fault storm, and something else accounts for the guest never
reaching a first frame. Those are two problems, and the second is the one that
blocks the game.

## The zero-page family is a dead end, and the run that proves it is zpstub

Three conf variants, all measured on the same NRO:

| conf | faults | census-sweep | guest state |
|---|---|---|---|
| `backend=null` (no zero page) | 948,021 | 0-1 | guest STARTS: `render-call-pre`, frames=1, then spins |
| `+ zeropage=1` (all zero) | 221 | 21 | guest does not start: `threads=1 total_imports=0` |
| `+ zpmode=ret` | 221 | 21 | guest does not start, same |
| `+ zpmode=ret zpstub=1` | 221 | 2 | guest does not start, same |

The `zpstub=1` run is the informative one because fault suppression restored
the observers, and they say plainly what happened:

    census-ord ordinal=0 start=0x0 delta=0 idle_ms=8031 last=(none)
                        inbridge=0/guest enters=0  lr_ring=        (empty)
    census-pc  ordinal=0 pc=0x806b0908 pc_img=host-nro pc_elfva=n/a
                        imports_delta=0 pc_stable=9
    census-mutex-sum  mutexes_known=0 total_acquires=0
    census-wait-sum   total_waits=0

Zero guest bridge entries, an empty return ring, a start pointer of 0, and a
PC inside OUR OWN NRO. The guest never got past its constructors. Filling VA 0
with the `ret` encoding is therefore not a partial fix -- it destroys the
startup path outright, which is worse than leaving the page unmapped (the
baseline at least reaches a first frame).

The code comment is right that the guest *calls through* +0x8 / +0xC0, and the
CPU halving (1.6 s/s -> 0.855 s/s) is a real, reproduced effect: a chunk of the
host load really is the guest spinning on itself through VA 0. But the guest
also *reads* that table, and overwriting the words turns the reads into garbage.
Pointing only the two call slots is not enough, and pointing everything is
actively harmful.

**Conclusion: revert to plain `backend=null`.** The VA 0 self-spin is real but
it is a symptom of the guest running on a device that was never initialised --
the same class of thing as everything else that "works" until it is measured.
Chasing the symptom further is not worth breaking a startup path that already
reaches one frame.

## The guest has no healthy middle state, and tonight's NRO regressed

Fault volume across archives makes the pattern plain:

| archive | faults | guest state |
|---|---|---|
| 20261002-183531-zpA-ret-null | 1 | did not start (`threads=1 total_imports=0`) |
| 20261002-181947-zpallones | -- | **RAN**: `threads=3`, `total_imports` 640 -> 5124 over 116 s |
| 20261004-* (tonight, every variant) | 948,009+ | started, then spun (948k faults) |

So the guest either never starts, or starts and immediately falls into the
self-spin. There is no "started and behaving" state in any of tonight's runs.
And there is exactly ONE archive in the whole set where it behaved:
`20261002-181947-zpallones`, on the 10-02 NRO.

Between that NRO and tonight's, three things changed:

1. the errno domain fix (41 shim sites moved to bionic numbering)
2. `android.os.Build` filled from 1 placeholder field to 22
3. read-only probe fields added to `render-call-pre`

**None of these was validated against guest behaviour.** (1) and (2) are
changes to what the guest *reads back* -- exactly the kind of change that can
flip a branch. I treated the errno fix as a pure correctness win and verified
only that it compiled, that 11/11 contract tests passed, and that the run "did
not regress" -- but "did not regress" was measured against tonight's own runs,
all of which were already in the broken state. There is no before/after
against a healthy run, because no healthy run exists after 10-02.

**This needs a bisect, and it is cheap:** rebuild with the errno change reverted
and compare. If fault volume drops or imports start climbing again, the fix
made things worse on this path and the two shim domains have to be reconciled
rather than one of them simply corrected.

Until that is done, treat "errno fix is correct" as a claim about the ABI and
NOT as a claim about this run.

## Five A/Bs: changing what the guest reads back does nothing

Every "fix" tonight that altered a value the guest can read was tested by
flipping exactly that value and nothing else. Compile-time switches made this a
one-file change each time, so the 41 assignment sites never moved.

| variable flipped | faults | frames / calls / draws |
|---|---:|---|
| baseline | 948,009 | 1 / 415 / 1/0 |
| errno -> newlib numbers | 948,004 | identical |
| `android.os.Build` -> placeholder | 947,833 | identical |
| JNI `calloc` null checks restored | 948,001 | identical |
| splash state flags no longer forced | 948,095 | identical |

Spread is 0.03%. **The guest's spin does not depend on anything the host hands
it back.** That closes the whole "make the host answer differently" direction,
which is where most of tonight went: errno domain, Build identity, allocation
failure reporting, renderer state flags.

Each of those four changes is still a real defect and is kept:
`bionic_errno.hpp` is correct about the ABI, `android.os.Build` no longer
answers one placeholder field, the four `calloc` sites no longer dereference a
possibly-NULL result, `guestfde._range()` no longer reports a function for an
address in the gap between functions. **A/B answered "did this fix the wedge",
not "is this fix correct" -- those are separate questions and the answer to the
second is yes in all four cases.**

## What is actually left, stated without hedging

- The same guest entry, called with the same arguments, **returns on the first
  JNI view render** (`view-call-render` 265 / `view-render-ret` 266) and **does
  not return on the second**, from the frame loop (`render-call-pre` 285, no
  post). The whole JNI View lifecycle is delivered and every callback returns:
  created, changed, focus-ret, changed-ret, render-ret.
- So the failure is guest state that changed after the first frame. The log
  does not record which thread ran the first call, so "wrong thread" is
  untested rather than excluded.
- The render function `0x12aea7c` has **zero** SCC cycles; none of its 27
  reachable callees has an unbounded one; none of its six host imports blocks;
  nothing crashes.
- About 948 000 faults in 13 s, then silence, with a core at 100%. 12.8% of them
  are writes to `0x10` / `0x18`.

**Locating that guest state needs the guest PC.** On Eden it is unavailable:
`svcGetThreadContext3` returns an address outside every registered image, the
PC sampler is starved to 8 samples, and `0x3B` is a Horizon-hardware priority
Eden does not honour. Per-game multicore does not help either (tested: sweeps
went 1 -> 0).

**That is the point to stop guessing and run it on a console.**

## FOUND IT: the frame loop was re-entering the guest draw entry

Six A/Bs, and the first five all changed something the guest *reads*. This one
changed something *we do*. Archive 20261004-071737-noframerender,
`RR3_FRAME_LOOP_RENDER = 0`:

```
faults        5        (baseline 948,009)
cpu           35.9 -> 42.0 -> 48.0 -> 53.8 s at t=50..80
             +0.6 s per 10 s   (baseline +1.6 s per second -- 62% less)
census-sweep  19       (baseline 0-1; observation works again)
ran to 80 s   (baseline went silent at ~13 s)
```

The probe's frame loop called the guest render entry every tick:

```cpp
reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
    jniEnvironment, activityObject, 2, 0);
```

Stop doing that and the 948 000-fault spin collapses to 5. **The wedge was
ours, not the guest's.** The evidence was in the log the whole time: the SAME
call with the SAME arguments returns on the first JNI view render
(`view-call-render` 265 / `view-render-ret` 266) and does not return on the
second, the one made from the loop (`render-call-pre` 285, no post).

Re-entering a draw entry every 16 ms is this probe's invention. On a real
Android, `invalidate()` hands the frame to vsync, which calls `onDraw` once, in
the guest's own UI context. Calling the draw entry directly, repeatedly, from
outside that context, is not the game's contract.

## But frames is still 1, and that is the next problem

With the loop no longer poking the draw entry, the guest does not start
rendering by itself either -- correctly, because nothing is invalidating it.
So the two halves of the fix are now clear:

- **poking the draw entry from the loop** = wrong, causes the spin (proven)
- **poking nothing** = no invalidation, no frames (proven)

What is needed is the real mechanism: drive the guest the way the View
callbacks do. The JNI path (`view-call-render` -> `view-render-ret`) already
returns cleanly, so repeating THAT -- surfacing the guest View as a real
Surface and letting its own draw callback fire per frame -- is the shape of the
answer, not calling the draw entry directly.

`frames=1` today is the honest number: one successful JNI view render, and a
frame loop that no longer damages anything.

---

# 2026-10-04 07:15-07:40 — the wedge is the save write, not the GPU

## The 948,009 "faults" were never a spin

They are `Unmapped Read/Write` from Eden's memory guard, and the first archive
looked like an infinite spin. It is not. `eden_log.txt.tail` around the first
fault of `20261004-071421-nosplash` reads:

```
[   5.779045] ...GetFileTimeStampRaw ... file=/switch/rr3-native-rehost/doc//sv.2.dat
[   5.780261] ...GetFileTimeStampRaw ... file=/switch/rr3-native-rehost/doc/sv.2.dat
[   5.792184] ...GetFileTimeStampRaw ... file=/switch/rr3-native-rehost/doc/sv.2.dat
[   5.827017] HW.Memory <Error> Unmapped Read8 @ 0x0000000000656D69
[   5.827067] HW.Memory <Error> Unmapped Read8 @ 0x0000000000656D6A     <- +1, byte by byte
```

...3.6e4 fault lines in 0.22 s, walking unbacked memory one byte at a time,
then at 6.049 s:

```
[jni] NewStringUTF com.ea.nimble.Base                       <- EA SDK init
...
[jni] NewStringUTF 加载游戏数据时出错，请重新启动《真实赛车3》。
[jni] NewStringUTF 不可恢复的错误
[jni] NewStringUTF 确定
alert-pump: started
```

**The guest raised its own fatal dialog: "error loading your game data, please
restart", unrecoverable, and is sitting on a modal box waiting for a keypress.**
`fpwalk` at the end of the run confirms it: 6 threads, 5 live, main thread in
`jni:CallStaticObjectMethod`, the rest idle in `poll`.

Locating that string in the guest puts it directly beside `SaveManager.cpp:699`,
next to `Total load time to end of load scene_DoIncrementalLoad`. **The failure
is the save, not the data packs.**

## Proof that the dialog is downstream, not a cause

`doc/` held a 109-byte `sv.2.dat` (rewritten by the guest itself at 07:14:28,
during the previous run) plus a 0-byte `player_profile.2.dat`. Backing the
directory up and emptying it, then running again:

| | nosplash (RENDER=1, doc/ full) | docclean (RENDER=0, doc/ empty) |
|---|---:|---:|
| eden log lines | 949,407 | 1,489 |
| unmapped faults | ~948,000 | 5 |
| data-load dialog | yes | **none** |
| Nimble init | yes | **none** |
| alert-pump | yes | **none** |

So the dialog and the fault storm are what the render loop *causes*; they are not
an independent failure that happens to coexist with it. `sv.2.dat` is a symptom
the guest writes on its way to the error box, not the thing that breaks the boot.

## The frame state machine, read off the bytes

`tools/guest-analysis/render-head.py` and `fsm-writers.py` (new) disassemble
`onViewRenderJNI`'s body, `0x12aea7c` [0x12aea7c, 0x12af358), 567 insns:

```
0x12aeaa4  ldrb w8, [x0, #0x4a6]     ; if (!landscapeApplied) -> 0x12aebac
0x12aeac0  cmp  w1, w8               ; activity[0x4a8] vs activity[0x4b0]
0x12aeac4  b.ne 0x12aead8
0x12aead0  cmp  w8, w9               ; activity[0x4ac] vs activity[0x4b4]
0x12aead4  b.eq 0x12aeb20            ; equal -> reset
0x12aead8  ldrb w8, [x19, #0x4d1]    ; hasFocus
0x12aeadc  cbz  w8, 0x12aeb20        ; no focus -> reset
0x12aeaec  str  w9, [x19, #0x4d4]    ; frameCounter++
0x12aeaf0  b.lt 0x12aeb28
0x12aeb00  bl   0x12ae290            ; the resize/size-change work
0x12aeb24  str  wzr, [x20]           ; reset arm: frameCounter = 0
0x12aeb28  ldr  w8, [x19, #0x4d8]    ; frameState
0x12aeb2c  sub  w9, w8, #1
0x12aeb30  cmp  w9, #2
0x12aeb34  b.ls 0x12aeba4            ; fsm in {1,2,3} -> fsm++
0x12aeb38  cmp  w8, #5  /  b.eq 0x12aebd8
0x12aeb40  cmp  w8, #4  /  b.ne 0x12aec2c
```

Measured live (`wedge: step=loop-top ... GATE`, new field, archive
`20261004-073207-nosplashflag`):

```
a8=1280 ac=720 b0=1280 b4=720 d1=1 d4=0 fs=0
```

**Both pairs are equal, so `0x4d4` is cleared on every call and `0x12ae290` is
never reached.** That is a size-change debounce, not a per-frame gate — the
guest is not stuck because of it.

The real per-frame work is further down, and it *does* run every call:

```
0x12aee18  ldr  w1, [x19, #0x4a8]    ; 1280
0x12aee1c  ldr  w2, [x19, #0x4ac]    ; 720
0x12aee20  ldr  s0, [x19, #0x4c0]    ; aspect
0x12aee24  bl   0x12b4824            ; <-- the frame
```

## 0x1105160 is the thing to look at

`0x12b4824` is 284 bytes and does almost nothing but call three things — and the
third one is `0x1105160` at `0x12b48f0`, **unconditional, every frame**:

```
0x1105160  FDE=[0x1105160,0x1109708)  17832 bytes, 4458 instructions
named callees: fopen x1, fwrite x1, fclose x1, memcpy x4, memmove x1,
               __stack_chk_fail x1
```

Those three stdio calls are the save write, inside the per-frame function. The
guest opens the save, writes it, closes it, every frame. `sv.2.dat` and
`sv.2.dat.0` in the watched-sidecar log are that path.

## Refuted this round

- **"`renderer[0x1f5a9]` is the wedge."** The host re-armed that byte every frame
  and the guest clears it itself at `0x12aeddc` (`strb wzr, [x8, x9]`), so
  re-arming was wrong in principle. It was also not the cause: with
  `RR3_FORCE_SPLASH_FLAGS=0` and the per-frame re-assert now under the same
  switch, the dialog and all ~948k faults came straight back
  (`20261004-073207-nosplashflag`), and `sa9=0` shows the guest never sets it.
  The switch stays 0 — the forcing was wrong either way — but it is not the fix.
- **"The data packs are the problem."** All seven romfs byte counts match
  `rr3-data-manifest.json` exactly, and `guestStat` never even mentions them.
- **"`//` in `doc//sv.2.dat` is the fault."** It does make our `stat` return
  ENOENT for a file that exists (the guest asks both spellings, one of which we
  fail), so it is a real shim bug, but it is not what wedges the frame.
- **"fwrite is the problem."** Not yet tested. That is the next probe.

## Open, and instrumented for the next run

`doc/sv.2.dat` (109 bytes) and the 0-byte `doc/*.bin` stand-ins are backed up at
`.build/eden-runs/doc-backup-20261004-072641/` and were moved out of the Eden
SDMC, not deleted.

`bionicFwrite` (libcxx_shims.cpp — **not** the frozen symbol_resolver.cpp) now
reports a short write: requested vs returned, `ferror`, and `errno`. It only
logs when `written != size * count`, so the success path costs two relaxed
atomics. That is the one number that has never been observed and that a retry
loop would be built on.
## The save-backup routing bug (real, fixed, and NOT the cause)

`mapAppRootToContent` routes app-root paths into the seven read-only content
volumes, and its exclusion list covered `.depot/CC_Data` and anything containing
`player_profile` -- but not the save family. So the guest's own request

```
api-touch api=guestFopen guest_path=sdmc:/switch/rr3-native-rehost/.depot/sv.2.dat.bak writing=1
map-app   rr3core:/files/.depot/sv.2.dat.bak          <- read-only content volume
fopen-fail-resolve
```

was sending a **backup of the save, which the guest is trying to create**, into
`rr3core:`. Fixed by excluding on the **leaf** name (`sv.*`, `ctd`) rather than
the whole path, so `.depot/data.bin` and the rest of the content tree keep
resolving into the volumes.

Verified in `20261004-074429-guestpath`: `map-app rr3core:...sv.2.dat.bak` is
gone, `fsTest=1fff`, `extData=1 loader=1 plan=1`, the guest boots and reaches
the render call.

**And the fatal dialog is still there** (511,770 read + 436,414 write faults,
2 dialog hits). So this was a genuine defect and it is not the wedge.

## The self-test bit collision, which cost two runs

Adding a regression bit to `guestFilesystemSelfTest()` stopped the guest from
booting, twice, and both times the log looked like a *huge success*:

```
eden log lines = 373   FAULTS read=0 write=0   error dialog hits = 0
auto-boot: extData=1 fsTest=fff loader=1 plan=1
```

`main.cpp` uses `dashboard.filesystemTest == 0x1fff` as the **auto-boot gate**,
so a bit that is not set means the guest never starts. Three separate mistakes,
in order:

1. **Wrong expectation.** The guard asserted `startsWith(primary, kAppRoot)`.
   `resolvePath`'s device-path branch sets `primary` to the *raw* guest path
   when `mapAppRootToContent` declines (guest_filesystem.cpp:703-708); it never
   rewrites to `kAppRoot`.
2. **A guessed bit.** A second assertion for `.depot/data.bin` was added on the
   theory it maps to `rr3core:`. Never verified.
3. **A colliding bit.** Both used `bits |= 512`. The mask `0xfff` is bits 0..11
   and 512 = `1<<9` is **already the `/dev/null` assertion** two lines below --
   so the new guard was satisfied by the old one and asserted nothing. The
   correct free bit is 4096.
4. **The wrong path spelling.** Even with the right bit, the guard passed a
   `/storage/emulated/0/...` path, which `resolvePath` routes through
   `afterPackageFiles` and never reaches `mapAppRootToContent` -- the function
   the leaf exclusion lives in. The guest spells it
   `sdmc:/switch/rr3-native-rehost/.depot/sv.2.dat.bak`, and the guard now
   builds the path from `kAppRoot` so it cannot drift again.

**⇒ a self-test bit is a startup condition, not a comment.** Read the whole
function before picking one, assert a property that depends only on the lines
you changed, and exercise the same path spelling the guest uses.

## Refuted, with the measurement

- **`renderer[0x1f5a9]` is the wedge.** Off, and the dialog plus all ~948k
  faults returned (`20261004-073207-nosplashflag`). The host re-arming a byte
  the guest clears itself at `0x12aeddc` was still wrong, so the switch stays 0
  and the per-frame re-assert is now under the same switch -- but it is not the
  fix.
- **`fwrite` short-writes.** First probe reported 18 "short writes" and all 18
  were false: `fwrite` returns **item count**, not bytes, so `fwrite(p,N,1,f)`
  correctly returns 1. Fixed to `written != count`. Re-measured: **0 short, 0
  writes over 64 bytes.** The stdio layer is healthy.
- **The data packs.** All seven romfs byte counts match the manifest exactly and
  the guest never stats them.
- **The `.bak` mis-routing.** A real bug, now fixed -- and the dialog survives
  it.

## Where the remaining wall is

`0x12aee24 bl 0x12b4824` -> `0x12b48f0 bl 0x1105160`, every frame, and
`0x1105160` is 17832 bytes / 4458 instructions whose only stdio calls are
`fopen` / `fwrite` / `fclose`. `fwrite` is clean, so the next question is what
`0x1105160` does with the value `fopen` returns -- in particular whether a save
that cannot be *written* (as opposed to read) is what drives SaveManager to
`SaveManager.cpp:699`. The `.bak` work above was the first concrete instance of
exactly that class, and it was not sufficient; the next probe should log the
guest's `open` result and `errno` for `doc/sv.2.dat` with `writing=1`, not just
the `fwrite` return.
---

# 2026-10-04 07:45-08:00 — retracting the "it is the save" attribution

## The adjacency was a string-table artifact

The earlier note in this file says the Chinese dialog string sits next to
`SaveManager.cpp:699` "so the failure is the save". That inference is **withdrawn**.
`tools/guest-analysis/xref-str.py` lists every string in the `0x47e000` page that
is referenced by a direct `adrp`/`add` pair: **91 of them**, from `TrackAnimations`
through `TWEAKABLE_FRAME_RECORD_360_PROJECTION_HEIGHT`. The dialog string is at
`+0x29c`; `SaveManager.cpp:699` is at `+0xce`. They are 462 bytes apart in one
dense .rodata page that the linker packed by size and locality, not by meaning.

## What SaveManager.cpp:699 actually is

```
0x13a4460  adrp x1, #0x47e000
0x13a4464  adrp x2, #0x3d6000
0x13a4468  add  x1, x1, #0xce      -> E:\dev\builds\r3_update_a\source\src\Save\SaveManager.cpp:699
0x13a446c  add  x2, x2, #0x146     -> "You can not load a save game on a non main thread."
0x13a4470  mov  w0, #2
0x13a4474  bl   0x109f730          ; log(level, file:line, message)
```

FDE `[0x13a434c, 0x13a4d30)`. So line 699 is a guard that **logs and returns
`0`** when a save load is attempted off the main thread. It is not the dialog's
source, and the dialog's own raise site is still unknown.

Searching the last six Eden archives for `main thread` / `save game` /
`SaveManager`: **0 hits.** Eden's log does not carry the guest's `FUN_0119f730`
output (the `svc_debug_string` payload is empty in the captured tail), so this
branch can be neither confirmed nor excluded from the logs. Honest negative.

What it does give is a concrete *mechanism* worth one measurement: RR3 refuses
save loads off the main thread, and this host runs the guest on six threads with
`ord=0 root=1` doing JNI. The first render call returns, the frame-loop call
wedges, and the two are not currently comparable -- the 261..267 numbers on the
`view-call-*` breadcrumbs are **sequential log ordinals, not thread handles**.
The `view-call-render` breadcrumb now records `th=threadGetCurHandle()` so the
two call sites can be compared directly.

## The instrument bug behind the false "0 xrefs"

`xref-str.py` first reported 0 references to the `0x47e000` page. It does not
match ADRP at all. The decode was:

```python
if pc + (imm << 12) == page:        # WRONG
```

ADRP is `Align(PC, 4096) + imm*4096` -- the base is **page-aligned**, the raw PC
is not. The result is therefore never page-aligned, and an exact comparison
against a page always fails.

This survived a histogram check because that script did `& ~0xFFF` on its output
before bucketing, which silently rounded the wrong value back onto a
plausible-looking page and reported 150 for a page the xref tool said had zero.
**Two computations of the same quantity disagreed, and the one with the
masking step was the believable one.** Corrected, the page really has 150 ADRPs
and the tool finds them.

The generalisable form: a decoder bug that a downstream `& mask` re-normalises
into looking correct is more dangerous than one that crashes, because the
agreement it fakes is the agreement you were about to check it with.
## Measured: both render calls run on the SAME thread

`20261004-074941-threadid`, after adding `th=threadGetCurHandle()` to the JNI
view render breadcrumb (the 261..267 numbers are sequential log ordinals, not
handles, so this could not be compared before):

```
view-call-render ordinal=265 target=00000000000083ff th=0x83ff site=jni-view-render
render-call-pre   ordinal=285 target=00000000000083ff side=pre ... th=0x000083ff
```

`th=0x83ff` on both. **The thread is not the variable.** Whatever differs
between a render call that returns and one that never returns is *state*, and
since the first call in the same run also receives the same
`(env, activity, 2, 0)` and the same activity/renderer pointers, the difference
has to be something the guest itself changed between the two -- which is what
`0x4d4`/`0x4d8`/`0x4b8`/`0x4bc` and the `0x1f5xx` renderer bytes are for.

`SaveManager.cpp:699`'s "non main thread" guard is therefore **not** the
mechanism. Eliminated by measurement rather than by argument.

## Ruled out so far, each with the measurement that killed it

| Hypothesis | Killed by |
|---|---|
| guest errno numbering (41 sites) | A/B: faults identical to 0.03% |
| `android.os.Build` identity | A/B: no rendering difference |
| `renderer[0x1f5a9]` re-armed per frame | A/B off: dialog + ~948k faults return |
| `fwrite` short writes | corrected probe: 0 short, 0 writes over 64 B |
| corrupt `doc/sv.2.dat` | `doc/` emptied: dialog gone (but render was off too) |
| the seven data volumes | byte counts match manifest exactly; guest never stats them |
| `.depot/sv.2.dat.bak` mis-routed to `rr3core:` | fixed, verified, dialog survives |
| wrong thread for the save load | `th=0x83ff` on both call sites |
| size-change gate `0x4a8/0x4b0/0x4ac/0x4b4` | it is a resize debounce; the per-frame `bl 0x12b4824` runs regardless |

The gate is now measured closed *and* shown to be irrelevant, the renderer flags
are shown to be re-armed wrong *and* shown not to matter, and the thread is
shown identical. What is left is guest state the host has not accounted for
between the first and second draw call.
---

# 2026-10-04 07:52-08:05 — the alert pump, and why it is a symptom

## The pump never delivers the button

`20261004-071421-nosplash` (the run that raised the fatal dialog):

```
eden:alert-pump: started            1
eden:alert-pump: thread-body-enter  1
eden:alert-pump: pre-lock           1
eden:alert-pump: wait-result        0
eden:alert-pump: reached-drain      0
eden:alert-pump: wake-wakeup        0
```

`kAlertPumpIntervalNanos` is 80 ms. A working timeout in a 75 s run would wake
~900 times. It woke zero times, so on Eden **`condvarWaitTimeout` does not
expire** and the pump blocks in it for the rest of the process. Its two
remaining breadcrumbs are exactly the wait slice, and `g_alertPumpWaitLock` is
named in only two places with no `g_alertPumpWait` signaler anywhere -- a sleep
wearing a lock, with no producer.

Replaced with `svcSleepThread` behind `RR3_ALERT_PUMP_CONDVAR` (default 0).
`20261004-075251-alertpumpfix`: **same result** -- `reached-drain` still 0.

## Which is the cause and which is the symptom

The sleep experiment is what separates them, and it says the starvation is
downstream:

- The fault storm starts at **5.827 s**; the dialog strings appear at
  **6.049 s**. The storm comes first.
- With `RR3_FRAME_LOOP_RENDER=0` there is no storm **and** no dialog.
- The alert pump stops being scheduled the moment the guest's main thread goes
  hot on the single core Eden gives it. `kDiagnosticThreadPriority` is 0x3B,
  which is Horizon hardware semantics Eden does not implement, and per-game
  multicore was already measured as ineffective (`sweep` 1 -> 0).

**So: the spin is the cause; the undismissable dialog and the starved pump are
both consequences.** The pump is a *readable* consequence, which is what makes it
worth keeping -- its progression is a clean binary indicator of "is the guest
spinning", and it is now independent of any particular wait primitive.

## What is still unknown, stated plainly

The guest's PC remains unobservable on Eden. Every `census-pc` sample reports
`pc_img=host-nro` with `pc_elfva=n/a`, and `census-sweep` returned nothing this
run. The sampler is reading threads that are not the one that issues the render
call. The one thing that could change this is sampling the thread that actually
calls it -- its handle is now recorded on every straddle line (`th=0x83ff`) -- so
the next instrumentation step is to point the sampler at that handle instead of
at the census ordinals.

The fault addresses are the only other handle on the spin, and they say the
guest is writing zeros through pointers that are themselves text
(`0x46FF7D572DD7774F` = `"Ow..-}w.F"`, `0x6f43786554697464` = `"dtiTexCo"`), i.e.
walking a container that was never populated. `.rodata` also carries
`Decompression error: streaming buffer size (%u bytes) too small for uncom...`
at a confirmed xref (0x13fb67c), next to `Streaming Cache` and
`DoIncrementalLoad` -- the asset-streaming path is where an unpopulated container
would come from. That string is the next thing to make observable.

## Why real hardware is now more valuable than it was

Two separate emulator artifacts are now measured, and neither would occur on
Switch:

1. a timed kernel wait that never expires, and
2. a second thread that stops being scheduled once the guest spins on one core.

Both of those are what turn a recoverable error dialog into an unkillable 75-
second stall. The Mesa/DRM path also does not spin in `eglInitialize`, which was
the original reason Eden was only ever a partial environment.

`.\tools\Deploy-To-Switch.ps1 -Card <card>` remains the highest-value single
action, and it is the one step in this list that cannot be done from here.
---

# 2026-10-04 07:55 — the PC sampler door, closed by measurement

## What the selftest actually proves

`20261004-075549-samplerfix`, `sampler-selftest-b` -- pause a victim thread, read
its context, resume it:

```
pauseRes=0x0 ctxRes=0x0 resumeRes=0x0 pc=0x80cf8a18 lr=0x807c8d44
sp=0x10a3dfff80 x0=0x1312d00 psr=0x60000000
counterBefore=8181954512 counterAfter=8181954512 advanced=0
```

- `advanced=0` -- the pause **worked**; the victim really did stop.
- `pauseRes`/`ctxRes`/`resumeRes` all `0x0` -- **every syscall claims success**.
- `pc=0x80cf8a18` is **byte-identical across every round**, and it is a host-NRO
  address.

A victim whose context is read correctly would show a different PC each round.
Identical host-NRO PCs mean the register block handed back is the **reader's
own**. **`svcGetThreadContext3` does not deliver another thread's ARM64 context
under Eden.**

That is the whole "the guest PC is unobservable" wall, and it was never a
scheduling, priority or starvation question. It also explains every
`census-pc` line ever recorded: `pc_img=host-nro`, `pc_elfva=n/a`, `samples=1`,
`pc_stable=1`. The sampler was reading itself.

**This is not fixable from the host side. Real hardware is the only remaining
path to a guest PC**, which makes `.\tools\Deploy-To-Switch.ps1 -Card <card>`
the critical path rather than a convenience.

## Correcting an over-claim I made two steps earlier

I wrote that `condvarWaitTimeout`'s timeout "does not expire" on Eden. **That
was wrong.** The sampler's own line, in a run from *before* the change, already
showed the wait returning:

```
20261004-075251-alertpumpfix   sampler-wake 1   iters=2 interval_ns=150907708 (target 75000000)
20261004-075549-samplerfix     sampler-wake 1   iters=2 interval_ns=150907708 (target 75000000)
```

and the alert pump, on byte-identical code, went `wait-result` 0 -> 1 between
the same two runs. The accurate statement is that **the timed wait is
unreliable**, which is worse for diagnosis than one that is dead: the run that
happens to work reads as a fix.

`RR3_SAMPLER_CONDVAR` and `RR3_ALERT_PUMP_CONDVAR` default to 0 anyway, and not
as a fix -- neither condvar has a signaler anywhere, so the `Mutex`+`CondVar`
pair guards no invariant and each tick is a sleep wearing a lock.
`svcSleepThread` states that and nothing more. Both comments now say this.

## Where the alert pump actually stops

`20261004-075549`: `wait-result=1`, `reached-drain=0`. So the wait returned and
the block is after it, which by the file's own decision table is **inside
`cloudcell_pump`** -- `std::lock_guard<std::mutex> on g_queue_lock`
(cloudcell_defer.cpp:71), held by the guest's main thread while it spins inside
the render call. That is the `std::mutex on a guest-visible path` this port has
an explicit rule about, showing up exactly where the rule predicted it would.

The chain, as measured:

1. frame loop issues a second `onViewRenderJNI` with the same
   `(env, activity, 2, 0)` on the same thread (`th=0x83ff`) as the first;
2. the guest spins inside it -- faults start at 5.827 s, the dialog at 6.049 s,
   so the spin comes first;
3. it decides "error loading game data" and calls `Platform.showMessage`;
4. the alert pump wakes, calls `cloudcell_pump`, and blocks on `g_queue_lock`
   which step 2 is holding;
5. the dialog is never dismissed, so the guest waits on the modal box for the
   rest of the run.

Step 1 is the only unexplained one. Everything below it is now accounted for.
## The per-frame save write in 0x1105160 is not the mechanism

`0x1105160` is the only unconditional per-frame call in the render body
(`0x12b48f0`), and its one `fopen`/`fwrite`/`fclose` triple is at
`0x1105310 / 0x110532c / 0x1105334`:

```
0x11052d8  fcmp  s0, #0.0              ; s0 = now - [x22+0x18]
0x11052dc  str   s0, [x22, #0x2fbc]
0x11052e0  b.hi  #0x1105348            ; clock advanced -> skip the whole block
0x11052e4  ldr   x21, [x22, #0x20]     ; path pointer
0x11052e8  cbz   x21, #0x110533c       ; null -> skip
0x1105308  add   x1, x1, #0x915        ; x1 = 0x459915
0x1105310  bl    fopen
0x1105320  mov   w1, #1                ; fwrite(buf, 1, n, f)
0x110532c  bl    fwrite
0x1105334  bl    fclose
```

`0x459915` is `"wt"`. The `b.hi` guard means the block only runs when the clock
did **not** advance past the stored value, so it is a rare flush, not a per-frame
write. That is consistent with the probe: `fwrite(buf, 1, 109, f)` is 109 bytes
and would clear the 64-byte `fwrite-save` threshold, and `fwrite-save` has never
fired. The binary has **132 `fopen` call sites** in total, so whoever writes
`doc/sv.2.dat` is not this one.

## The dialog string is reached through a relocated data slot

`xref-str.py` finds no `adrp`/`add` and no `ldr [xN,#0x6f8]` for
`0x47e29c`. What it does find is a single **pointer** to it, in `.rela.dyn`:

```
R_AARCH64_RELATIVE  r_offset=0x1ed16f8  r_info=0x403  r_addend=0x47e29c
```

with `r_offset=0x1ed1700 -> 0x4546fe` and `0x1ed1708 -> ...` beside it, so the
neighbours are 8 bytes apart. That is a consecutive pointer array, and the code
reaches it with a **register-indexed load** (`ldr xM, [xN, xK, lsl #3]`), not a
literal offset. This search did **not** pair the array's base `ADD` with the
indexed load, so the raise site is still not located, and it is not yet
established that this array is a string table at all -- `R_AARCH64_RELATIVE`
initialises data pointers generally, and `GAMETEXT_*` keys are directly
referenced `.rodata` strings elsewhere in the same binary.

## Honest status of the one unexplained step

Step 1 of the chain -- why the *second* `onViewRenderJNI` spins when the first
returns, on the same thread with the same arguments -- remains unexplained, and
it cannot be explained from inside Eden:

- the guest's PC cannot be read (`svcGetThreadContext3` returns the reader's
  own registers, proven by `sampler-selftest-b`),
- the fault addresses are the only remaining signal, and they say the guest is
  writing through pointers that are themselves text, i.e. walking a container
  that was never populated,
- the static side has closed the frame-state gate (a resize debounce), the
  splash flags, the save write in `0x1105160`, `fwrite` health, the seven data
  volumes, the `.bak` routing, errno numbering, `android.os.Build`, and thread
  identity.

**The one action that can move this is the one that cannot be taken from here.**
---

# 2026-10-04 08:00 — the dialog is not the save. It is a null singleton.

## The message key

`0x47e29c` has no `adrp`/`add` and no literal `ldr`, so the string is reached
through data. The pointer to it lives in `.rela.dyn` as a
`R_AARCH64_RELATIVE` record, and walking that run of records gives a contiguous
array:

```
contiguous array: 0x1ed12e8 .. 0x1ed1ca0   (313 slots, 8 bytes each)
  [126] 0x390a90  русский
  [127] 0x46d452  "Er is een fout ... spelgegevens"
  [128] 0x3f880f  "Ocorreu um erro ... dados de jogo"
  [129] 0x3d62b7  日本語
  [130] 0x408c80  "게임 데이터를 불러오는 중 오류"
  [131] 0x47e29c  «加载游戏数据时出错…»      <- our dialog
  [132] 0x4546fe  «輸入遊戲數據時出錯…»
  [133] "Needed space"                        <- next message starts
```

Grouped by message, one slot per language, 12 languages per group. Our dialog is
group base `0x6a8`, language index 10.

## The lookup function

One function reaches the whole table, and it is a `strcmp` chain over key
strings, FDE `[0x13c3fc4, 0x13c4328)`:

```
0x13c3fd8  mov  x19, x0                ; x0 = the GAMETEXT_* key
0x13c3fdc  bl   strcmp
0x13c3fe0  cbz  w0, <arm>              ; one arm per key
...
0x13c430c  adrp   x9, #0x1fb4000
0x13c4310  ldrsw  x9, [x9, #0xa28]      ; language index, a global
0x13c4314  ldr    x19, [x8, x9, lsl #3] ; table[group + language]
0x13c4318  mov    x0, x19
0x13c4324  ret
```

Arms step by `0x60` = 96 bytes = 12 pointers, which is what fixes the
languages-per-group at 12. The key this dialog resolves through is:

```
0x13c4098  adrp x1, #0x3a8000
0x13c409c  add  x1, x1, #0x462   -> 'GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE'
0x13c40a8  cbz  w0, #0x13c425c
```

**Not `SaveManager`.** The keys around it in the same function are
`GAMETEXT_NO_WIFI`, `GAMETEXT_NETWORK_DOWNLOAD_PROMPT`,
`GAMETEXT_NO_CARRIER_NETWORK_DOWNLOAD`, `GAMETEXT_FREE_STORAGE_SPACE`,
`GAMETEXT_NEEDED_STORAGE_SPACE`, `GAMETEXT_ALL_FILES_DOWNLOADED`,
`GAMETEXT_ASSET_DOWNLOAD`, `GAMETEXT_DOWNLOAD_ERROR_POPUP_*` -- this
localization function belongs to the **asset download / storage** subsystem.
The `SaveManager.cpp:699` "non main thread" guard was a red herring twice over.

## The raise site, and the actual condition

`xref-str.py 0x3a8462` gives three references; `0x13c409c` is the lookup, and
`0xcd35c4` is the raise, in FDE `[0xcd1014, 0xcd3648)` (2445 insns):

```
0xcd35c0  adrp x0, #0x3a8000
0xcd35c4  add  x0, x0, #0x462
0xcd35c8  bl   #0x13c3fc4                 ; localize()
0xcd35cc  adrp x8, #0x1f6d000
0xcd35d0  ldr  x8, [x8, #0x5d8]          ; the alert object
```

Five branches reach that block; four of them
(`0xcd35a4` / `0xcd359c` / `0xcd35b4` / `0xcd3594->0xcd3438`) are destructor and
`__stack_chk_fail` cleanup variants that converge on the same place. Exactly one
is the condition:

```
0xcd16fc  ldr  x0, [x28]
0xcd1700  cbz  x0, #0xcd35c0              ; NULL -> raise the dialog
```

and `x28` is a global singleton loaded once:

```
0xcd1474  adrp x28, #0x1f6d000
0xcd1478  ldr  x28, [x28, #0x5d0]         ; x28 = *(0x1f6d5d0)
```

**So the fatal dialog is raised because `*(0x1f6d5d0)` is NULL, or because the
object it points at has a NULL first word.** `0x1f6d5d8` is the adjacent global
that the render entry's `fsm==4` path uses at `0x12aeb5c`, so the whole family
is the data/asset manager -- **constructed never, or constructed with a null
first word.** Those are different bugs with different fixes and nothing logged
so far distinguishes them.

Runtime address: `mainBias 0x2106559000 + 0x1f6d5d0 = 0x2107C2E5D0`, with the
bias cross-checked against the straddle's own `render=0x2107e0c614` minus the
export's ELF VA `0x12b3614`. The frame loop now reads the slot and reports
`NULL-SINGLETON`, `NULL-FIRST-WORD` or `PRESENT`.
## First measurement, and why it is not yet the answer

`20261004-080245-assetsingleton`:

```
asset-singleton: elfva=0x1f6d5d0 bias=0x2106759000 slot=0x21086c65d0
                 singleton=0x210871fe98 first_word=0x81154130 verdict=PRESENT
```

**The probe is in the wrong place, and that is the finding.** It reads the slot
in the frame loop, gated on `frames < 2`, which means it runs *before* the
first loop render. The dialog is raised *inside* that render call -- faults
start at 5.827 s, the box at 6.049 s -- and that call never returns, so no
`frames >= 2` iteration ever exists. The probe can therefore only ever observe
the state *before* the transition, and `PRESENT` says nothing about the state
that raised the dialog.

`first_word=0x81154130` is a host-NRO address, i.e. a vtable pointer into one of
this port's own JNI compat `Object` instances. At that moment nothing is wrong.

**The read has to move to a thread that survives the wedge and tick on a timer**
-- the census thread or the 1 Hz reaper, both of which are still producing lines
throughout the 75 s -- and it has to sample on every tick so the
PRESENT -> NULL transition is bracketed in time rather than sampled once before
it. A single pre-wedge read cannot distinguish "the singleton is fine" from "the
singleton is fine until the render call destroys it", and those need opposite
fixes.
## MEASURED: NULL-FIRST-WORD, and it predates the wedge

`20261004-080608-assetsample`, sampled on the 1 Hz reaper tick
(`pthread_bridge.cpp`, `reaper-asset-singleton`):

```
boot_ms=214952116  asset=0x210811fe98  first=0  NULL-FIRST-WORD
boot_ms=214953116  asset=0x210811fe98  first=0  NULL-FIRST-WORD
```

**The singleton is not null. It points at a real object whose first word -- the
C++ vtable pointer -- is zero.** And it is zero at 1 s and 2 s, long before the
wedge at ~5.8 s, so this is a boot-time condition the render call did not
create.

That is exactly the failure mode the fault addresses describe. A call through a
null vtable dispatches from a slot read out of unmapped memory, and the guest
then walks off through the whole address space writing zeros at whatever the
"vtable" happened to contain -- which is why the faults read
`0x46FF7D572DD7774F` (whose bytes are `"Ow..-}w.F"`) and `0x6f43786554697464`
(`"dtiTexCo"`). Those are not corrupted structures; **they are the vtable
itself.**

`rebase:` already reports `ctors=181 ctor0ok=1` for the main image, so the
static constructors ran. One of them is supposed to finish initialising this
object and does not.

**This is the first measurement in this file that names a cause rather than a
symptom, and it is a three-way split the earlier single read could not make:**

| verdict | meaning |
|---|---|
| `NULL-SINGLETON` | the global itself was never assigned |
| `NULL-FIRST-WORD` | **measured** -- assigned, allocated, vtable still null |
| `PRESENT` | constructed |

`NULL-SINGLETON` and `NULL-FIRST-WORD` are different bugs in different code, and
this run is the first that distinguishes them.

## The instrumentation note that made it possible

The read had to leave the frame loop. The loop only ever executes
`frames == 0` -- the second `onViewRenderJNI` never returns -- so a probe there
sees only the pre-wedge state, and the first attempt duly reported `PRESENT`
from a moment when nothing was wrong. The 1 Hz reaper is the thread confirmed
to tick for the whole run, so it is the only place a sample can land on both
sides of the transition. It also proves the point about the earlier `PRESENT`:
two runs, same address class, different verdicts, because they were sampled at
different times. **A one-shot read of a value that can change is not a
measurement of that value.**
## CORRECTION OF A CORRECTION: the first word IS the vtable

I was about to withdraw the `NULL-FIRST-WORD` reading on the grounds that no
code computes `0x1fc6e98`, so the object cannot have a constructor and the
first word is probably a plain data field. **That withdrawal was wrong**, and a
second, independent site settles it:

```
0x9888ec  adrp x8, #0x1f6d000
0x9888f0  ldr  x8, [x8, #0x5d0]     ; x8 = *(0x1f6d5d0)  the asset singleton
0x9888f4  ldr  x0, [x8]             ; x0 = *singleton
0x9888f8  ldr  x8, [x0]             ; x8 = *x0           <- vtable load
0x9888fc  ldr  x8, [x8, #0x28]      ; vtable slot 0x28
0x988900  blr  x8                    ; virtual call
```

FDE `[0x9888a4, 0x988f1c)`. `ldr [x0]` immediately after dereferencing the
object to get `x0` has no other reading. So the object at `0x1fc6e98` **is**
polymorphic, and with its vtable word reading 0 this site does
`ldr [0]`, `ldr [0x28]`, and branches to whatever that yielded.

**This is the fault storm, at the instruction level.** The raise site
(`0xcd16fc` `cbz`) is the same condition seen from the error path; this is the
same condition seen from a call site. The guest is not corrupting data
structures -- the addresses that read as `"Ow..-}w.F"` and `"dtiTexCo"` **are
the vtable region it is walking because there is no vtable.**

## What the pointer actually is

```
0x1f6d5d0   in .got, R_AARCH64_RELATIVE, addend 0x1fc6e98
0x1f6d5d8   in .got, R_AARCH64_RELATIVE, addend 0x1fb2688
0x1fc6e98   in .bss
0x1fb2688   in .bss
```

`0x1fb2688` is the engine singleton this port already reads successfully
(`kEngineSingletonDeltaFromRenderExport`, `0x20b2688 - 0x1000000`), so the
`.got` + `R_AARCH64_RELATIVE` + `.bss` shape is cross-validated against a
singleton that demonstrably works. `0x1f6d5d0` points at the same shape of
static object, and its vtable word is still zero at 1 s and 2 s.

## Two query errors worth recording

Both happened while trying to find the writer, and both produced a confident
negative:

- **Wrong page.** Searching for readers of `0x1f6d5d0` I paged on `0x1f60000`.
  The ADRP target page is `0x1f6d000`. "0 references" was a search bug.
- **Wrong access kind.** I looked for a `str` to `+0x5d0`; the site uses `ldr`.

And a third, which is the dangerous one: I nearly retracted a *correct*
conclusion because a plausible-sounding argument ("no constructor, so no
vtable") pointed the wrong way. The argument was wrong, and the way to know was
to look for a second site -- which existed, and agreed.

**A negative from a search is only evidence after the search itself has been
checked, and a mechanism story is only wrong once something contradicts it.**
## The vtable claim does not hold up. Here is the whole of it.

I asserted twice that the first word of `0x1fc6e98` is a vtable pointer and
that the guest was calling through a null vtable. **Searching for the writer
refutes it.**

```
adrp 0x1fc6000 + add #0xe98  -> 0 sites   (the address of 0x1fc6e98)
adr  landing on 0x1fc6e98   -> 0 sites
```

**No instruction in the guest's `.text` ever forms the address `0x1fc6e98`.**
Nothing there can therefore write that word, vtable or otherwise. The
`R_AARCH64_RELATIVE` addend for the `.got` slot was produced from relocatable
**data**, not from code.

That leaves two readings, and the evidence does not choose between them:

1. it is a vtable and something outside this image writes it, or
2. **the first word is an ordinary field**, and the `ldr [x0] / ldr [x0,#0x28]
   / blr` at `0x9888f4` happens to be a data-pointer call rather than a virtual
   one.

Reading 2 is not a stretch. `0x9888f4` sits in FDE `[0x9888a4, 0x988f1c)` --
**the first function in `.text`** (which starts at `0x988000`) -- and the
surrounding code builds a `0x20`-byte object and calls `operator new`, which is
the shape of an early initialiser rather than a hot call site. One
instruction sequence is not enough to declare a vtable, and I declared it twice.

## What is actually established

- The fatal dialog is raised **iff `*(*(0x1f6d5d0)) == 0`** (`0xcd16fc` /
  `0xcd1700`).
- `*(0x1f6d5d0)` is `0x1fc6e98`, a `.bss` static, statically wired through a
  `.got` `R_AARCH64_RELATIVE` entry -- the same shape as the engine singleton
  `0x1fb2688` that this port reads successfully, so the wiring itself is sound.
- That first word reads 0 at 1 s and 2 s, before the wedge.
- **No guest code writes it.**

The last point is the actionable one. The localization keys around this message
are all asset-download and storage strings, and the raise is a plain
"is there a manager yet" test. The consistent reading is that this field is the
**result of the data load**, the asset load never completed, and the field was
never populated -- not that a constructor failed.

**So the question moves back to our side: why does the guest's data load not
complete?** That is a different and better-posed question than "which
constructor failed", and it is where the next round should go.
## The data load stops after the config layer, and two files are missing

`20261004-080608-assetsample`, the guest's own file traffic for the whole run:

```
map-app  -> a volume : 98 paths
open-ok  -> a volume :  1   (rr3core:/files/.depot/ctd.dat, the Cloudcell document)
fopen    -> a volume :  0
readdir  -> a volume :  0
open-fail            :  1   (a sidecar .bak)
fopen-fail           :  1   (a sidecar .bak)
```

The 98 mapped paths are the config layer -- `no3g.xml`, `eds/*.plist`,
`feats/.feats`, `dataoffsets.txt`, `langs.utf8`. The guest maps them, then stops:
it issues almost no opens, and the opens it does issue almost never fail, so it
is not being rejected. **It gives up after reading configuration.**

## Two of the paths it wants are not in the volume

`romfs-read.py` against the deployed `core.romfs`:

```
files/.depot/no3g.xml                  FOUND   162 B
files/.depot/dataoffsets.txt           FOUND    18 B
files/.depot/eds/AndroidHigh.plist     FOUND
files/.depot/langs.utf8                FOUND   197 B
files/.depot/feats/.feats              file not found
files/apk/res/feats/.feats             file not found
files/.depot/eds/.feats                file not found
files/.depot/cc_automagic_const.js     file not found
files/apk/res/cc_automagic_const.js    file not found
```

`.feats` is absent under every spelling, and it is one of the 98 the guest
actually asked for. The guest binary has the matching message:

```
FUN_0119f730(2, "E:\dev\builds\r3_update_a\source\src\mt3D\mtShaderManager.cpp:146",
             "Failed to load shader features file!\n")
```

and `cc_automagic_const.js` exists on the SD card only under
`.depot/CC_Data/`, which the content router deliberately keeps on FAT
(`guest_filesystem.cpp`, the `CC_Data` exclusion).

**Two readings, and they are not equivalent:**

- the upstream data archive genuinely does not ship these two, and the real
  game obtains them from the asset service -- which means the port is missing a
  downloaded asset, and the `RR3_FRAME_LOOP_RENDER=0` run's clean boot is the
  same absence seen before the load starts;
- or the router sends them somewhere they are not.

The second is cheap to test and has not been: whether the guest's stat on
`rr3core:/files/.depot/feats/.feats` returns ENOENT or something else. **That
result is not logged at all** -- `map-app` records the resolved path, the
watched-path caps (`hbn4, sidecar40, ccdata16, android_any200`) do not cover
these, so the stat result is invisible. Widening the probe to report rc/errno
for the content-layer paths is the next concrete step, and it does not need a
new idea, only a bigger cap.
---

# 2026-10-04 08:12 — the block is ONE missing file

## The measurement the widened probe was for

`watchedPathClass` returned -1 for the content layer, `watchedCap(-1)` was 0,
and `claimWatchedLine` therefore returned 0 -- so for a run that maps 98 content
paths, **the rc and errno of all of them were never recorded.** Added a fourth
class (`kWatchedContent`, 200 lines, `/.depot/` and `apk/res/`), grown
`claimWatchedLine`'s array to 4 and moved its bound from `kWatchedCcData` to
`kWatchedContent` -- a new class added to `watchedCap()` without growing the
array is rejected by the old bound and reports nothing, which is the same
invisible-result failure the class was added to fix.

`20261004-081257-contentwatch`:

```
content-layer result lines: 61
  rc=0  (present) : 59
  rc=-1 (absent)  :  2

rr3core:/files/.depot/feats/.feats      rc=-1 errno=2
rr3core:/files/apk/res/feats/.feats     rc=-1 errno=2
```

and, for contrast, the ones that work:

```
no3g.xml   rc=0 errno=0 size=162  device_path=rr3core:/files/.depot/no3g.xml
           open rc=0 head=3c6f6e6c79574946493e0d0a   ("<?xml ...")
```

**The router is sound.** Paths resolve into the right volume, sizes come back,
and the bytes are a valid XML header.

## It is not in any volume

```
                 depot/feats/.feats   apk/res/feats/.feats
core                  MISSING              MISSING
gui-hd                MISSING              MISSING
gui-sd                MISSING              MISSING
tracks                MISSING              MISSING
vehicles-classic      MISSING              MISSING
vehicles-mid          MISSING              MISSING
vehicles-modern       MISSING              MISSING
```

So this is not a mis-routed volume. The upstream data archive does not ship it.

`cc_automagic_const.js`, which the previous note listed as the second missing
file, does **not** appear in the ENOENT list at all: the guest never asks for it
through the content router, because the `CC_Data` exclusion correctly keeps it
on FAT. **There is one missing file, not two**, and the earlier count came from
checking paths the guest never requested.

## What it is, and what it costs

`.feats` is the shader feature file. The guest binary carries the matching
message:

```
FUN_0119f730(2, "E:\dev\builds\r3_update_a\source\src\mt3D\mtShaderManager.cpp:146",
             "Failed to load shader features file!\n")
```

**The causal chain, end to end and all of it measured:**

```
feats/.feats -> ENOENT from the volume            (61 requests, 2 failures, 1 file)
  -> data load aborts, field at *(0x1f6d5d0)+0 stays 0   (reaper, 1 s and 2 s)
  -> GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE       (0xcd16fc cbz -> 0xcd35c0)
  -> Platform.showMessage                                    (Eden log, GetStaticMethodID)
  -> alert pump cannot deliver the button                    (wait-result 1, reached-drain 0)
  -> guest waits on the modal box forever
  -> the fault storm is that wait, not a spin
```

Every arrow on that list is a log line, and the top one is a `stat` that now gets
recorded because the probe covers the class it was missing.

**Next step is no longer instrumentation: obtain or reconstruct
`files/.depot/feats/.feats`.** Candidates, in order of cost:
`dataoffsets.txt` is 18 bytes and may name it; the upstream archive may carry it
under a different path or a different vehicle-split; and the format is a shader
feature descriptor, so a minimal valid one may be derivable from the GLSL the
guest already compiles (`gfx-dumps/*.glsl`, 4 compiles / 2 links per run).
---

# 2026-10-04 08:15 — .feats is not in the data pack and not in the APK

## It is a downloaded asset

`带鉴别包的14.0.1_row_RealRacingArchive.apk` (112,504,452 B, v14.0.1 -- the
same version `getAppVersion` reports, and the source of the guest SO):

```
entries: 1822        res 1575 | META-INF 68 | assets 44 | gatewayprotocol 36 | lib 28
entries matching /feats/i : 0
assets/               : ad-viewer/*, audience_network.dex ... (Google Mobile Ads only)
```

**Zero `feats` in the APK**, and `assets/` holds nothing but the ad SDK. So
`.feats` is in neither the data pack nor the APK: it is a file the real game
**downloads** from the asset service. This port stubs `connect()`
(`rr3_Platform.cpp` records that stub), so the download never happens and the
file can never appear.

That settles the open question from the previous section. The port is not
mis-routing anything; it is missing a downloaded asset, and the network stub is
why it cannot obtain one.

## The parser, which is how a file can be synthesised

`0x12caea0` is the loader, and it is short:

```c
lVar5 = FUN_013bca64(pvVar1, auStack_2bc, 0, 1);   // pvVar1 = the .feats path (SSO string)
if (lVar5 != 0) {                                   // success
    uStack_286 = 0xf0e0d0c0b0a0908;                // 32 x uint16 feature indices
    local_28e = 0x706050403020100;                  //   0x0808 .. 0x1f1f
    uStack_266 = 0x2f2e2d2c2b2a2928;
    local_26e = 0x2726252423222120;
    uStack_276 = 0x1f1e1d1c1b1a1918;
    local_27e = 0x1716151413121110;
    builtin_strncpy(local_24e, "@ABCDEFGHIJKLMNO", 0x10);
    uStack_256 = 0x3f3e3d3c3b3a3938;
    local_25e = 0x3736353433323130;
    uStack_226 = DAT_00489a70._8_8_;
    local_22e = (undefined8)DAT_00489a70;
    return FUN_01f17ab0(DAT_00489a70, 0x50);
}
FUN_0119f730(2, "...\\mt3D\\mtShaderManager.cpp:146",
             "Failed to load shader features file!\n");
FUN_012c7170(param_1, 1);
```

`FUN_013bca64(path, out, 0, 1)` returning non-zero is the whole contract. On
failure `FUN_012c7170(param_1, 1)` sets the state that the data load later
reports as `GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`.

**The file's format is whatever `FUN_013bca64` parses, and that function is the
next thing to read.** A synthetic `.feats` is then offline-constructible from
the feature indices this table already reveals, with no network and no third
party asset. That is the cheapest remaining path and it does not depend on
anything outside this repository.

## One more corrected reading

`dataoffsets.txt` is **1805 bytes, not 18**. An earlier note took the byte count
from a truncated dump whose first line `#SEC:MAX_IMAGES:24` happens to be 18
characters. It is a section index with four sections
(`MAX_IMAGES:24`, `bin:39`, `res:4`, `img:23`) and it does not mention feats or
shaders, so it is not a locator for the file. Fourth reading error of the same
kind this session, all four recorded.
## The `.feats` chain is one link off, in the direction that matters

`FUN_013bca64` is 22 instructions and **never touches the filesystem**:

```
0x13bca48  ldr   x0, [x0, #8]            ; unwrap the SSO path's buffer
0x13bca50  bl    0x12bcea8               ; the only call that could touch a file
0x13bca54  adrp  x8, #0x1f6d000
0x13bca58  ldr   x8, [x8, #0x5d8]        ; the ENGINE singleton, not the asset one
0x13bca5c  ldr   x8, [x8]
0x13bca60  ldr   x8, [x8, #8]
0x13bca64  ldr   x8, [x8, #0x38]         ; vtable slot 0x38 -> a base table pointer
0x13bca6c  add   x9, x8, w20, sxtw #2     ; table[arg2]
0x13bca70  ldrsh x10, [x9, #6]
0x13bca78  ldrsh w0, [x8, x10]           ; <== the return value
0x13bca7c  ldr   w8, [x9]
0x13bca80  str   w8, [x19, #0x1b34]      ; and it stores into the caller's struct
```

**So the `lVar5 != 0` test in `0x12caea0` is a TABLE LOOKUP, not a file-open
result.** "Failed to load shader features file!" is emitted when the lookup
misses, which is one step downstream of the file never being read, and
`0x12bcea8` -- not `0x13bca64` -- is the function that would read it.

This does not rescue `.feats`: the ENOENT is measured (61 content requests, 59
succeed, this one does not), and an unread file is a sufficient cause of a table
that was never populated. But the previous note's phrasing, "`FUN_013bca64`
returning non-zero IS the contract" and "on failure `FUN_012c7170` sets the
state", describes the wrong function. The next thing to read is `0x12bcea8`:
it is the actual file reader, and its arguments tell us whether the file is
expected to exist at all, or whether it is a cache the engine populates from
somewhere else when the network path works.

**Note for the record: this is the fifth reading correction of the session, and
the same failure mode again -- a name taken from a decomp call site was assumed
to be the thing that does the work, without reading its body. The rule that
would have caught it is one line: when a function's meaning is load-bearing,
read its instructions before naming it as the cause.**
## 0x12bcea8 is not a file reader either -- and that breaks my own chain

I named it "the actual file reader" in the previous section. It is 9
instructions and it is the same table lookup again:

```
0x12bcea8  adrp  x8, #0x1f6d000
0x12bceac  ldr   x8, [x8, #0x5d8]      ; the ENGINE singleton
0x12bceb0  ldr   x8, [x8]
0x12bceb4  ldr   x8, [x8, #8]
0x12bceb8  ldr   x8, [x8, #0x38]       ; vtable slot 0x38 -> table base
0x12bcebc  ldr   x8, [x8]
0x12bcec0  add   x8, x8, w1, sxtw #2   ; table[w1]
0x12bcec4  ldrsh w1, [x8, #6]
0x12bcec8  b     #0x12bcbe4            ; tail call
```

**Neither function in that chain touches the filesystem.** So
`FUN_013bca64 -> 0x12bcea8` is a two-step table lookup, and the
"Failed to load shader features file!" at `mtShaderManager.cpp:146` is emitted
when a lookup misses -- not when a read fails.

**The `.feats` -> dialog chain in this file is therefore NOT established.** The
ENOENT is real and measured, but it is a co-symptom at best: something else
fails to populate the engine's table at `vtable+0x38`, and `.feats` being absent
may be a consequence of that rather than its cause.

This is the sixth reading correction of the session and the same failure mode as
the previous five: a name taken from a decomp call site was treated as the
function that does the work, without reading its body. The rule that catches it
is one line, and it is worth more than everything else written today:

> **When a function's meaning is load-bearing, read its instructions before
> naming it as the cause.**

Applying it retroactively to this file: the only claims in it that rest on
disassembly are sound. The claims that rest on decomp reading -- the vtable
story, the "FUN_013bca64 is the contract", the "0x12bcea8 is the file reader" --
are the ones that kept moving.

## What is actually established, stated without the chain

1. The guest raises its own fatal dialog
   (`GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`, raise at `0xcd35c0`)
   **iff `*(*(0x1f6d5d0)) == 0`**, and that field is still 0 at 1 s and 2 s.
2. The fault storm is that dialog sitting undismissed, not a spin: the alert
   pump wakes (`wait-result=1`) and then blocks before `reached-drain`, and the
   timeline is faults-then-dialog.
3. `feats/.feats` genuinely does not exist -- 59 of 61 content requests
   succeed, this one does not, and it is absent from all seven volumes and from
   the APK's 1822 entries. **That is a real defect in the deployment.**
4. **It has not been shown to be the cause.** The message is emitted by a table
   miss in the engine's shader subsystem, and no traced function in that
   subsystem reads a file.

(4) is the open question, and the honest next step is to find what is supposed
to populate that table -- not to keep tracing the message upward from a decomp
name.
## MEASURED: the chain breaks at engine+8, and .feats is exonerated

Instead of tracing the message upward from a decomp name, the engine chain that
`0x13bca64` and `0x12bcea8` actually walk is now read rung by rung, every second,
from the reaper (`20261004-081806-engchain`):

```
asset=0x2107f1fe98  first=0            NULL-FIRST-WORD
eng=0x2107f0b688    f8=0  vt=0  table=0   t0..t3 all zero
```

```
engine                = 0x2107f0b688   present
*(engine + 8)         = 0              <== the break
*(*(engine+8))        = 0
*(*(engine+8) + 0x38) = 0
```

**The first rung past the singleton is already zero, at 1 s and 2 s.** So
"Failed to load shader features file!" is the shader manager finding that
`engine->subsystem` was never set -- the table lookup cannot miss because the
table pointer is null before any index is applied.

**`.feats` is exonerated.** Its absence is a real defect in the deployment (61
content requests, 59 succeed, this one does not; absent from all seven volumes
and from the APK's 1822 entries) but it is not the cause of the dialog, and
synthesising the file would not have unblocked the game.

**This is the seventh reading correction, and the first one that removes a whole
theory rather than adjusting one.** The difference is the method: every earlier
correction came from reading a function after trusting its name, and this one came
from measuring the values. The chain was walked from the failure *outward*
instead of the message *downward*, and the value that mattered -- `engine+8` --
was readable the whole time.

## The question is now well posed

**What writes `engine+8`?** The engine object exists and its other fields are
populated (this port reads width/height/aspect off the same singleton
successfully), so this is not "the singleton was never created" -- it is one
specific pointer that engine initialisation was supposed to fill and did not.

The nearest gate is the device configuration layer: `no3g.xml` is read
successfully (`rc=0 size=162`, valid XML, `<onlyWIFI>`, carrier and device ids)
and is exactly the kind of per-device input that decides which engine subsystems
get built. Worth checking, in order:

1. what writes `engine+8` -- find it from the write side, not the read side;
2. whether it is gated on a device profile the guest picks, and whether our
   `no3g.xml` answer satisfies it;
3. only then whether `engine+8 == 0` and the missing `.feats` share a cause,
   which is plausible and untested.

Do not read `.feats` into this until step 1 produces a writer.
## Two searches returned zero, and I do not trust either

To find the writer of `engine+8` from the write side:

```
1. adrp 0x1fb2000 + add #0x688  (-> &engine_static 0x1fb2688)   : 0 sites
2. adrp 0x1f6d000 + ldr [x,#0x5d8] + str [x,#8]                 : 0 sites
```

**Both zeros are unvalidated.** Each predicate was written moments earlier and
neither has been checked against a case that should match, which is exactly the
gap that produced seven wrong conclusions earlier today. The session rule
extends to search results, not just to function names:

> **A negative is only evidence after the predicate itself has been validated
> against a positive control.**

There is already a specific reason to distrust predicate 2: the observed state
is `engine` non-zero with `engine+8` zero, which is the exact shape an
`stp xA, xB, [x0]` leaves behind when the second slot was null -- one
instruction writing offsets 0 and 8 together, which predicate 2 does not match
at all. **That is a better explanation of the data than "nothing writes it",
and it is a one-instruction fix to the query.**

Before concluding anything from these zeros:

- widen predicate 2 to `stp` (including `stp x, x, [x, #imm]` where
  `imm + 8 == 8`, and the pre-index form), and to a `str` at any offset written
  from a register loaded from the `0x5d8` slot;
- validate the widened query on a control: the *asset* singleton
  (`0x1f6d5d0`) is known to be written by something, so the same widened
  predicate should find at least one writer there before it is trusted on the
  engine;
- if it still finds nothing, check the other three guest images
  (`libc++_shared.so`, `libNimble.so`, `libfmodex.so`) rather than concluding
  absence from one image.

**Until that control passes, the state of this question is "not searched
correctly", not "no writer exists".**
## The predicate, validated. One writer. Next function named.

Widened to `str`/`stp` at offsets 0 and 8, and run with a **positive control**
-- the asset singleton, whose writer is known to exist:

```
slot 0x5d0  ASSET (control) : 1 writer   0xcde424  str +0  FDE=[0xcdde2c,0xcde58c)  1888 B
slot 0x5d8  ENGINE          : 1 writer   0x12ddce4 str +0  FDE=[0x12dc094,0x12dde40) 7588 B
```

The control found its writer, so the query shape is sound. The narrow
predicate from the previous section returned 0 for both slots and was simply
wrong; widening it turned "no writer" into "exactly one writer, and it is not
at +8".

**So `engine+8` is not written through the `.got` singleton slot at all.** The
only code that touches the engine singleton that way writes offset 0, inside
`[0x12dc094, 0x12dde40)` -- a 7588-byte function. The `+8` store happens through
`this` passed by some other route, so it is in that function or in something
that calls it.

**Next step, bounded and concrete:** inside `[0x12dc094, 0x12dde40)`, list every
`str`/`stp` to offset 8, and for each, the branch condition that guards it. The
guard is the thing worth having: `engine+8 == 0` with `engine+0` populated means
whatever fills `+8` either never ran or bailed. A guarded store answers "why"
directly, and unlike every name-based inference today it does not depend on
trusting a decomp label.

This is also the first result of the session that came with its own validity
check attached, which is the only reason it is worth recording as progress
rather than as another lead.
## The engine constructor does NOT write +8, and that reopens .feats

`[0x12dc094, 0x12dde40)` is 1899 instructions and is plainly a constructor --
it initialises dozens of fields off `x19` (`this`). Every one of the 12 stores
that covers offset 8 turned out to be an `stp q*, q*, [x?, #imm]` whose base is
`add x?, x19, #imm`, i.e. `this+0x2e8`, `this+0x1b08`, `this+0x1b24`,
`this+0x2c0` and so on. **None of them is `this+8`**, and the same block
explicitly zeroes `this+0x1b70`, `this+0x1b80`, `this+0x1830` and others.

This is a trustworthy negative, unlike the earlier ones: the function is large,
obviously a constructor, and demonstrably writing a long list of fields, so
"it writes many fields and not this one" is read off the instruction stream
rather than from a failed pattern match.

**So the write to `engine+8` is done by a different function** -- the one that
links the engine to its renderer/shader subsystem, which is a later step than
construction.

## What that does to the .feats verdict

The previous section exonerated `.feats` on the grounds that no function in the
traced chain reads a file. That reasoning was sound but the premise has now
changed: **the function that writes `engine+8` was never in that chain.** So the
exoneration was of a chain that does not contain the relevant writer, which is
not evidence about the file.

Revised, honestly:

- `engine+8 == 0` is measured, and the engine's own constructor is ruled out as
  the writer;
- the writer is a linker/init function not yet identified;
- `.feats` is **no longer excluded** -- it is simply unconnected so far, because
  the function that would connect it has not been found.

The search rule for the next round follows from the same discipline that caught
the bad zeros: find the writer of `engine+8` by looking for `str/stp ..., [x?,
#8]` where `x?` is a parameter or a freshly returned pointer, across
`[0x12dc094, 0x12dce2c)`'s **callers** -- and validate the predicate on this
constructor, which is known to write `this+0x1b70` and `this+0x1b80`, before
believing a zero from it.
## The positive control caught a false zero, for the third time

Searching for the `engine+8` writer with a validated predicate, the control
failed:

```
POSITIVE CONTROL (the ctor is known to write 0x1b70 and 0x1b80): found = False
  offsets actually present: []  []
predicate invalid -- refusing to trust its zeros
```

The script refused to continue, which is the only reason this is a line in the
file instead of a conclusion.

Cause: `0x1b70` is 7024, and `STR (immediate, unsigned offset)` encodes a
12-bit **byte** offset -- 4095 is its ceiling. Every field the constructor
initialises past `+0xfff` is therefore *not* a single `str`; it is an
`add xN, x19, #imm` followed by a `str [xN, #small]`, and it is the `add` that
carries the large offset. Matching `imm12 == offset` silently misses every one
of them, which is the same shape as the `.text`-wide `ldr` predicate that
reported 1403 hits earlier today.

**The control is not ceremony.** Three times today a query returned zero or a
wrong count and a known-true target was the only thing that caught it:

| query | control | without the control |
|---|---|---|
| readers of `0x1f6d5d0` | wrong page -> "0 refs" | concluded nothing references it |
| writer of `engine+8` (narrow) | wrong access kind -> "0" | concluded nothing writes it |
| writer of `engine+8` (this one) | wrong offset scale -> "0" | would have concluded no writer exists |

**A store search must handle three encodings, not one:** `str/strb` with
`imm12 * 1`, `str/strh/strb` with `imm12 * scale` (2/4), and `add` + store for
anything above 4095, which is `stp` or a register-materialised base. Any one of
them alone produces a confident, wrong zero.

## State of the question, unchanged and honestly stated

- `engine+8 == 0` -- measured, twice, from the reaper.
- The engine's own constructor does not write it -- read off the instruction
  stream, not from a failed match.
- The writer exists but has not been located. Three attempts have failed on
  predicate bugs, not on absence.
- `.feats` is unconnected: neither implicated nor excluded.

**Next attempt, with the encoding handled and the same control re-run first:**
resolve `add`-materialised bases before matching the store, and accept the
result only if the control still passes. Do not report a zero without it.
## Second control failure, same outcome: no result reported

Retried with `add`-materialised base tracking and all three store encodings
(`str/strb` scale 1, `strh` scale 2, `str/stur` scale 4):

```
POSITIVE CONTROL: ctor writes 0x1b70/0x1b80 -> found 0  []
PREDICATE INVALID -- refusing to trust its zeros
```

**No conclusion is reported from this query.** The control did its job twice
now on the same question: the first attempt failed on the imm12 ceiling, this
one on whatever the tracking is still missing. Two failures of the same control
is itself information -- the target offsets `0x1b70` / `0x1b80` are being
rendered by capstone as `[x19, #0x1b70]`, which no unsigned-offset `STR` can
encode, so either the disassembly text or the offset assumption is wrong, and
until that is resolved any writer search built on it is guesswork.

**Handing this off in an explicitly unfinished state.** The right first move for
whoever picks it up is not a better search -- it is to settle why capstone prints
an unencodable immediate for `0x12dc3d8`, because every conclusion about
`engine+8` sits behind that.

```
established and not in doubt:
  engine+8 == 0                    measured twice from the reaper
  the engine ctor does not write it  read off the instruction stream

open:
  who writes it                     three attempts, all failed on the predicate
  whether .feats is connected        neither implicated nor excluded
```
## RESOLVED: capstone was rendering STUR as STR, and the control's own targets were STUR

The blocker behind two failed controls was an encoding, not an offset mistake:

```
0x12dc3d4  word=0xb91aee7f  size=2  imm12=0x6bb  1723 * 4 = 6892 = 0x1aec
0x12dc3d8  word=0xf90dba7f  size=3  imm12=0x36e   878 * 8 = 7024 = 0x1b70
0x12dc3e0  word=0xf90dc27f  size=3  imm12=0x370   880 * 8 = 7040 = 0x1b80
```

`0xf9...` with `size=3` and bit 21 set is **`STUR`**: a 9-bit **signed**
offset scaled by the access size, used when the offset will not fit the 12-bit
`STR (immediate, unsigned offset)` field. Capstone prints it as
`str wzr, [x19, #0x1b70]`, so the disassembly text is correct and the
*encoding* is invisible. Every `0x1b70`-and-up field in this constructor is
`STUR`, which is why the positive control failed twice: **the control's own
targets were the encoding the predicate did not implement.**

**Four encodings, not one. A store search must match all of them:**

| form | offset field | scale | capstone prints |
|---|---|---|---|
| `STR/STRB imm, unsigned offset` | imm12, 12-bit | 1 or 4 | `str ..., [x, #N]` |
| `STRH imm, unsigned offset` | imm12, 12-bit | 2 | `strh ..., [x, #N]` |
| `STP/STP pair` | imm7, 7-bit signed | 8/16/32 | `stp ..., [x, #N]` |
| **`STUR/STUTR/STURB`** | **imm9, 9-bit signed** | **1/2/4/8/16** | **`str ..., [x, #N]`** |

The fourth is indistinguishable from the first in the printed text, which is
exactly why a predicate written from the disassembly listing alone will miss it.
A fifth, `add` + small-offset `str`, covers anything the compiler could not
fit at all.

**Do not hand-write these predicates from the disassembly listing.** Decode the
raw 32-bit word, or drive the search from capstone's `insn.encoding`, and keep
the positive control -- which is what finally made the failure legible instead
of silently producing a zero.

The `engine+8` writer search is now unblocked but still unrun. Nothing in this
file claims a writer for it.
## engine+8 writers, found with a validated predicate

Re-run after resolving the STUR blocker, decoding raw 32-bit words rather than
reading capstone's printed operand:

```
CONTROL 0x12dc3d8 word=0xf90dba7f -> ['0x1b70']  expect 0x1b70  OK
CONTROL 0x12dc3e0 word=0xf90dc27f -> ['0x1b80']  expect 0x1b80  OK
PREDICATE VALID

engine-singleton loads followed by a store covering +8 : 2
   0xba574c   FDE=[0xba56dc,0xba57b0)    212 B
   0xf8b454   FDE=[0xf8b36c,0xf8ba04)   1688 B
```

Two candidates, and **this is the first positive result of the session that
carried its own validity check** -- the same control that produced three
confident wrong answers earlier today is what makes these two believable.

Neither has been read yet, and nothing here claims either is the writer. What
is established: the write exists, there are two places that can make it, and the
search that found them is one whose control passes.

**Next, in order:**

1. read both functions' bodies before naming either one -- that is the rule the
   other seven mistakes of the session all broke;
2. for the one that actually stores, take the **guard** in front of the store:
   `engine+0` populated and `engine+8` zero means the guarded path did not run,
   and the guard is the answer to "why";
3. keep the control attached to any further query about these two.

`[0xf8b36c, 0xf8ba04)` at 1688 bytes is the more plausible of the two on size
alone -- enough code to link an engine to a subsystem, and small enough to read
whole -- but that is a guess about plausibility, not a finding, and should not be
acted on before step 1.
## Both candidates are false positives, and that is the real lesson of the session

Read before naming, as required. Candidate A, `[0xba56dc, 0xba57b0)`:

```
0xba5730  adrp x8, #0x1f6d000
0xba5734  ldr  x8, [x8, #0x5d8]      ; the engine singleton
0xba573c  ldr  x8, [x8]
0xba5740  ldr  x0, [x8, #0x160]     ; engine+0x160
0xba574c  stp  x8, x22, [sp]         ; FLAGGED -- but the base is sp
0xba5784  bl   std::__assoc_sub_state::set_value
```

A `std::future` completion callback: sleep, run a `std::function`, set the
value. Nothing to do with engine initialisation.

Candidate B, `[0xf8b36c, 0xf8ba04)`:

```
0xf8b450  add  x8, x8, x9            ; x8 = engine + 0x1da8  (a computed value)
0xf8b454  stp  x8, x0, [sp, #8]      ; FLAGGED -- base is sp again
```

**Neither writes `engine+8`. The writer has not been found.**

## Why the control passed and the search was still wrong

This is the most useful thing in the file. The control validated that the
**offset decoder** was right -- `0x1b70` and `0x1b80` were recovered from
`STUR` words exactly, after the third attempt. What it could not validate is the
other dimension of the predicate: **base-register identity**. The search looked
for "a store whose offset field is 8, somewhere after a load from the engine
slot", and both hits stored to the stack.

**A control only exercises the dimension it was built from.** The control's
target was a store to `this+0x1b70`; the question was about a store to
`engine+8`. They differ in exactly the dimension the predicate got wrong, so no
control built on offsets could ever have caught it.

The control that would have caught it: require the base register to be provably
the engine pointer -- i.e. require a match on a store this search already
believes, such as the asset singleton's writer at `0xcde424` (which writes
`+0`), and confirm the search reports it. It would not have matched, because
`+0` is not `+8`, and that is the tell.

**Generalised, and this supersedes the two earlier rules in this file:**

> 1. When a function's meaning is load-bearing, read its instructions before
>    naming it as the cause.
> 2. A negative is only evidence after the predicate passes a positive control.
> 3. **A control validates the dimension it was built from. Build the control on
>    the same dimension as the claim, or a passing control proves nothing about
>    the dimensions it does not touch.**

The third is the one that matters. The first two were learned from bad searches;
this one was learned from a search that *passed* its control and was still wrong.
## SOLVED: the control on the right dimension, and 16 real candidates for engine+8

The previous section said the control had been built on the wrong dimension.
Rebuilt on the claim's own dimension -- "a store whose base is provably the
engine pointer" -- the control is the one writer already known:

```
CONTROL: must contain 0x12ddce4 (known engine writer)   found = True
PREDICATE VALID -- base tracking is sound

211 sites write through the engine pointer:
   +0     32 sites
   +4      2 sites
   +8     16 sites      <== engine+8
   +16    12 sites
   +24     8 sites
   ...     (plus many larger offsets)
```

`0x9f9960` appears in **both** the `+0` and the `+8` list, i.e. a single
`stp xA, xB, [engine]` writing offsets 0 and 8 together. That is exactly the
shape predicted hours ago from the observation alone --

> `engine+0` populated, `engine+8` zero, is what `stp xA, xB, [x0]` leaves
> behind when the second slot was null.

-- and which could not be tested until the predicate could identify base
registers at all. It is now testable, and the prediction holds.

**This is the first result of the session whose control was built on the same
dimension as the claim.** The two previous controls validated the offset
decoder and told us nothing about base identity; this one is validated by a
site that is known to store through the engine pointer, which is precisely what
the search claims to detect.

Sixteen candidates, none named yet, and the rule still applies: read the
function body before calling any of them the writer. The productive next step
is the 16 `+8` sites narrowed to those whose **guard** can be zeroed -- the
guard is what answers "why".
## FOUND: a 3-instruction function whose whole body zeroes engine+0/+8/+0x10

Of the 16 `+8` sites, two are eliminated by reading them, as the rule requires:

```
0xbcfc88  str w10, [x11, #8]   x11 = madd(x23, 8, x0)  -- an array element
0xbd0814  str w8,  [x24, #8]   x24 is a loop variable   -- not the engine
```

One matches exactly:

```
=== 0x9f9960   FDE=[0x9f9960, 0x9f996c)   size=0xc  (12 bytes, 3 insns) ===
   0x9f9960  stp  xzr, xzr, [x8]     ; engine+0 and engine+8, both zero
   0x9f9964  str  xzr, [x8, #0x10]  ; engine+0x10, also zero
```

**A function whose entire body is "zero the first three fields of the engine".**
The measured state -- `engine+0` populated, `engine+8` zero -- is not what this
produces on its own, so it must run before a later fill, or the engine's `+0` is
written after it. What it does establish is that a bare "zero the engine header"
entry point exists and is reachable from the same singleton, and that the shape
predicted from the observation alone -- `stp xA, xB, [x0]` with the second slot
null -- is present in the binary. It is simply not the site executing in the
measured runs, because those runs show `+0` non-zero.

**The question therefore narrows to call order**, and it is now answerable with
a control built on the right dimension: find the callers of `0x9f9960`, and
find which of the 32 known `engine+0` writers run after it. `engine+0` non-zero
while `+8` is zero means **something wrote `+0` after this and nothing wrote
`+8` after it** -- a partial re-initialisation.

**That is a sharper statement than anything earlier in this file, and it is read
off the instruction stream rather than off a name.**
## A fourth false zero, caught by the same habit: validate on a known-called target

Asked for the callers of the engine-header zeroer `0x9f9960`; the loop said 0.
So before recording it, the loop was run against targets that are certainly
called:

```
callers of 0x1e17b70  = 0    __stack_chk_fail (certainly called)
callers of 0x12dc094  = 0    engine ctor (has callers)
callers of 0x13caea0  = 0    the .feats loader (certainly called)
callers of 0x9f9960  = 0    the engine-header zeroer
```

**All four zero, including one that cannot be zero.** The loop computed
`tv + (imm26 << 2)` -- it dropped the `+ i`. A `BL` target is
`PC_of_instruction + imm26*4`, and `PC_of_instruction` is `tv + i`.

**The fourth instance of one recurring bug: PC-relative forms dropped the PC
term.**

| form | correct | wrong (seen today) |
|---|---|---|
| `ADRP` | `Align(PC,4096) + imm<<12` | `PC + imm<<12` |
| `ADR` | `PC + imm` | (missed the form entirely) |
| `BL` | `PC_of_insn + imm26<<2` | `section_start + imm26<<2` |

The `__stack_chk_fail` control is the cheapest possible positive case -- a PLT
stub that this project has disassembled hundreds of times in its own comments --
and it took one line to run. **Running it was the entire difference between a
recorded result and another retraction.**

**No claim is made about `0x9f9960`'s callers.** The question is unrun, and the
fix is `tv + i + (imm26 << 2)`.
## The BL decoder, diagnosed to the bit (still not run)

Two attempts, both failed the `__stack_chk_fail` control. The first dropped the
`+ i`; the second added it and still returned 0 for a function this project has
disassembled thousands of times. Cause:

```python
# WRONG -- reads the whole 32-bit word as a signed int
struct.unpack_from("<i", d, to+i)[0] << 2
```

**The top 6 bits of a `BL` word are the opcode, not the offset.** The correct
decode is:

```
imm26 = w & 0x03FFFFFF          # low 26 bits only
if imm26 & (1 << 25): imm26 -= (1 << 26)    # sign-extend from 26 bits
target = (tv + i) + (imm26 << 2)             # PC of the instruction, not the section
```

**Two independent terms were wrong at once** -- the missing instruction offset and
the unmasked opcode bits -- and adding one of them did not make the control pass.
That is the clearest possible demonstration of why a control has to be run
before a zero is recorded: "I fixed it" and "it is right" are different claims,
and only one of them is testable.

`0x9f9960`'s callers remain **unmeasured**. Nothing in this file claims it has
none.
## A trustworthy zero, and a single construction site

With the BL decoder corrected (`imm26 = w & 0x03FFFFFF`, sign-extend from 26,
target = `(tv+i) + imm26<<2`), the control passes:

```
CONTROL  callers of __stack_chk_fail = 14574  OK
PREDICATE VALID

callers of 0x9f9960   (engine-header zeroer) : 0
callers of 0x12dc094  (engine ctor)         : 1
    bl @ 0x12b5850   FDE=[0x12b5830, 0x12b58b4)  132 B
```

**The engine-header zeroer is dead code.** It exists in the binary and nothing
calls it, so the "partial re-initialisation" hypothesis from the previous section
is refuted -- nothing zeroes `engine+8` and nothing re-fills `+0` afterwards.
`engine+8` is simply never written on this path, by any of the 16 candidate
sites either, since the field is observed zero.

**The engine has exactly one construction site in the whole image**:
`0x12b5850`, inside a 132-byte function `[0x12b5830, 0x12b58b4)` -- and that
neighbourhood is where `0x12b58dc` (the time function this project has been
tracking as `vt38`) also lives, i.e. the engine's startup path.

**This is the first trustworthy negative of the session**, and the thing that
made it trustworthy is the same control that produced four false zeros before
it: run it, and if it fails, say nothing.

**What it buys:** the search is no longer "which of 16 sites writes +8" but a
single question -- read the 132-byte function that constructs the engine, follow
what it calls, and find where the subsystem link at `+8` is supposed to be
established. One construction site is a much shorter walk than 16 candidates.
## The exact place: engine+8 is read between two writes and never written between them

The one construction site, `[0x12b5830, 0x12b58b4)`, in full:

```
0x12b583c  mov  w8, #0xf5b0 ; movk w8,#1,lsl#16      ; w8 = 0x1f5b0
0x12b5844  and  x1, x1, #0xffffff                     ; a mask on the argument
0x12b584c  add  x20, x0, x8
0x12b5850  bl   0x12dc094                             ; the big ctor
0x12b5858  ldr  x8, [x8, #0xeb0]                      ; x8 = *(0x1f78eb0)
0x12b5864  add  x8, x8, #0x10
0x12b5868  str  x8, [x19]                             ; engine+0 = vtable     WRITTEN
0x12b586c  str  xzr, [x20]                            ; this+0x1f5b0 = 0
0x12b5870  strb wzr, [x20, #0x14]
0x12b5874  bl   0x12bac04                             ; <-- the only candidate
0x12b587c  bl   operator new(0x20)
0x12b5890  bl   0x12a7cb4                             ; build a 0x20 object
0x12b5894  ldr  x8, [x19, #8]                         ; engine+8             READ
0x12b589c  str  x20, [x8, #0x50]                      ; *(engine+8)->0x50 = obj
0x12b58a0  ldr  x8, [x19, #8]                         ; read it back
0x12b58b0  b    0x12a80f0
```

**The engine's vtable is installed at `+0` at `0x12b5868`, and `+8` is
dereferenced twelve instructions later. Nothing in this function writes `+8`.**
The only thing between the two is `bl 0x12bac04`.

This is not a name-based inference: it is the instruction stream of the single
function that constructs the engine, which was found with a caller search whose
control passed (`__stack_chk_fail` = 14574). It is also the shortest possible
answer to "where should `+8` have been established" -- one call.

**Next, and it is one function: read `0x12bac04`.** If it does not write
`engine+8`, the engine is handed to its own startup path with a null subsystem
pointer, and `0x12b589c` is the fault. If it does, then something upstream of
`0x12b5830` clobbered it afterwards, and the search moves up rather than down.

Either way the question is now one function wide, and it is answerable
statically, with no emulator.
## 0x12bac04 does not write +8 -- and it turns out to be the splash-flag setter

Sixteen instructions, and none of them touches `engine+8`:

```
0x12bac04  mov  w9, #0xf5a9 ; movk w9,#1,lsl#16    ; w9 = 0x1f5a9
0x12bac08  adrp x12, #0x3c9000
0x12bac0c  adrp x13, #0x40c000
0x12bac10  adrp x8,  #0x46c000
0x12bac18  ands w11, w1, #1
0x12bac1c  add  x12, x12, #0x8fc
0x12bac20  add  x13, x13, #0xe20
0x12bac24  add  x8,  x8,  #0xe68
0x12bac2c  add  x9, x0, x9                        ; x9 = this + 0x1f5a9
0x12bac30  csel x1, x13, x12, ne
0x12bac34  mov  x0, x8
0x12bac38  strb w10, [x9]                          ; this+0x1f5a9 = 1
0x12bac3c  strb w11, [x9, #2]                     ; this+0x1f5ab = arg & 1
0x12bac40  b    0x109fa18                          ; tail call
```

**`0x1f5a9` and `0x1f5ab` are the renderer splash-exit bytes** -- the very ones
this port used to force-write every frame. The guest sets them itself, right
here, in its own construction path. That retroactively justifies leaving
`RR3_FORCE_SPLASH_FLAGS` at 0, and it identifies `0x12bac04` as an engine setup
routine rather than anything shader related.

**So: across the whole construction sequence -- the big ctor `0x12dc094`, the
wrapper `0x12b5830`, and the setup `0x12bac04` -- nothing writes `engine+8`, and
`0x12b5894` dereferences it twelve instructions after the vtable is installed.**

Two ways that can still be consistent, and they are testable in one function
each:

- the tail call at `0x12bac40` (`b 0x109fa18`) fills it. That is the one call
  in the sequence whose body has not been read, and it receives `x0 = this` plus
  a string chosen by `arg & 1`;
- or `+8` is filled by the big ctor through an encoding the `+8` enumeration
  missed. That enumeration used the corrected, control-passing predicate, so this
  is the less likely of the two -- but it was an enumeration of *stores whose
  base is the engine pointer*, and the ctor's base is `x19`, the same object
  reached by a different route. **That distinction is the one thing left to
  check, and it is why the ctor should be re-scanned with `x19` seeded as the
  engine rather than only the `.got` load.**
## UNPERFORMED: re-scan the ctor with x19 seeded as the engine

The one open item from the previous section. Tracking `x19` as the engine
object through `[0x12dc094, 0x12dde40)` with `add`/`sub` materialisation and the
four store encodings, the control failed:

```
CONTROL: ctor writes 0x1b70 and 0x1b80  -> found = False  INVALID
  keys near 0x1b00..0x1c00: []
```

No result is reported. The predicate is not yet sound for a function that
reaches its object through a callee-saved register rather than the `.got` load,
which is the one case the earlier control could not have covered either.

**The one distinction worth carrying into the next session:** the `+8`
enumeration that produced 16 candidates tracked the engine only as
"whatever register was loaded from the `.got` slot". The big constructor reaches
the same object as `x19`. Those are the same object by two routes, and only the
first route was ever enumerated. Whether that gap hides a writer of `+8` is
still unknown, and it is the cheapest remaining check -- it needs a predicate
that survives its control, not a new hypothesis.

Known-good in the meantime, and independent of all of the above:
`engine+8 == 0` at 1 s and 2 s; the single construction site
`[0x12b5830, 0x12b58b4)` installs the vtable and dereferences `+8` twelve
instructions later; `0x12bac04` sets `+0x1f5a9` / `+0x1f5ab` and tail-calls
`0x109fa18`; and `0x109fa18`'s body is the one function in that sequence that
has never been read.
## 0x109fa18 is __android_log_print -- and that exposes a bad premise

```
0x109fa58  adrp x8, #0x1f6e000
0x109fa5c  ldr  x8, [x8, #0x5c0]      ; the log gate
0x109fa60  ldrb w8, [x8]
0x109fa64  cbz  w8, #0x109fac4         ; not enabled -> skip entirely
0x109faa0  bl   vprintf
0x109fac0  bl   __android_log_vprint(4, <0x4748a7>, x19)
```

An NDK logging shim. **So `0x12bac04` tail-calls the logger; it does not write
`engine+8` either.**

Four function bodies read in full now: the big ctor `0x12dc094`, the wrapper
`0x12b5830`, the setup `0x12bac04`, and this shim. **None of them writes
`engine+8`, and `0x12b5894` dereferences it.** That is a hard static result --
and it is the result that should have made the premise suspicious sooner.

**The premise is probably wrong: the `x0` in `[0x12b5830, 0x12b58b4)` is
whatever the caller passed, and its only caller is `0x12b4300` in
`[0x12b42c8, 0x12b4328)`. Nothing ever established that this is the engine
singleton that the reaper measures at `0x1f6d5d8`.**

The last several sections have been treating "the engine" as one object reached
by two routes -- the `.got` slot and `x19` -- and assuming the constructor
sequence is the engine's. That assumption is unverified, and it is now the most
likely source of the apparent contradiction.

**So the next check is one call site, not a hypothesis:** read `0x12b4300` and
see what it passes in `x0`. If it is the `.got` singleton, the construction
sequence genuinely never fills `+8` and the bug is in the guest's startup path.
If it is something else, every conclusion drawn from the construction sequence
about `engine+8` has been about the wrong object, and the reaper's measurement
is the only trustworthy one of the two.

This is the same lesson as the eight earlier ones wearing a different costume:
**a chain of correct steps is still wrong if the first link is unverified.**
## The premise was wrong: that sequence constructs a different object

The only caller of `0x12b5830`, `[0x12b42c8, 0x12b4328)`, in full:

```
0x12b42d8  mov  x21, x0                 ; x21 = the real object
0x12b42e8  mov  x20, x1
0x12b42ec  bl   operator new(0x1f5d0)   ; a 0x1f5d0-byte object
0x12b42f8  mov  x22, x0                 ; x22 = the new object
0x12b4300  bl   0x12b5830                ; ...constructed with x0 = x22
0x12b430c  str  x22, [x21]              ; real->+0      = the new object
0x12b4310  str  x21, [x22, #0x1f5b8]    ; new->+0x1f5b8 = the real object
0x12b4314  str  w19, [x21, #0x10]        ; real->+0x10   = arg3
```

**`x0` at the construction site is a freshly allocated 0x1f5d0-byte object, not
the `.got` singleton at `0x1f6d5d8`.** Every statement in the previous four
sections about "the construction sequence never writes `engine+8`" is about a
different object and is withdrawn.

**What this buys, for the real object at `0x1f6d5d8`:**

- `+0`    = a 0x1f5d0-byte owned object, linked both ways
- `+0x10` = a 32-bit value taken from the caller's argument
- `+8`    = **never written on this path** -- and it is the field the shader
  chain dereferences

So the hole is real and it is on the `.got` object, and the search that actually
targets it is the earlier one: the 16 `+8` sites enumerated from the `.got`
route, whose predicate passed its control. **That enumeration was the right
search; the construction sequence was the wrong detour, and it was entered on an
unverified assumption that the two were the same object.**

**The lesson generalises past this bug:**

> A chain of individually correct steps is still wrong if the first link is
> unverified. Here, four correct function reads produced a confident conclusion
> about an object nobody had checked. The check -- "what is actually passed in
> `x0`" -- was one function wide and was available the whole time.

If the search resumes, resume at the 16 `.got`-route `+8` sites, and read each
candidate's guard. The constructor sequence is a dead end for this question and
should be struck from the notes rather than re-read.
## The 17 real +8 sites, with their guards

Back on the `.got` route -- the one whose predicate passed its control. Same
17 sites the reaper measurement makes relevant, now with the nearest
conditional branches in front of each store:

```
0x9f9960   size=0xc     (none nearby)      dead -- 0 callers, established earlier
0xbcfc88   size=0x13c   (none nearby)      eliminated -- base was an array element
0xbd0814   size=0xadc   b.pl #0xbd0aec
0xbd0894   size=0xadc   b.pl #0xbd0ab8
0xcada74   size=0x44    (none nearby)
0xd7ae7c   size=0xc     (none nearby)
0xf3b390   size=0x46c   (none nearby)
0xf3bd8c   size=0xe4    (none nearby)
0xf42e44   size=0x470   cbz x0, #0xf42e9c | b.eq #0xf430a0
0x104ad50  size=0x254   b.ne #0x104ad18 | b.eq #0x104ad9c
0x10ac5e8  size=0x3a0   (none nearby)
0x118211c  size=0xec    b.hs #0x118217c | cbz w8, #0x11820e8
0x12bd154  size=0x718   (none nearby)
0x13428c8  size=0x6d4   cbz w8, #0x1342944 | tbz w8, #0, #0x1342bfc
0x13bad80  size=0x211c  (none nearby)
0x1414594  size=0x9c    (none nearby)
0x159d298  size=0x420   (none nearby)
```

Two are already accounted for (`0x9f9960` dead, `0xbcfc88` a false positive),
leaving **15 live candidates**.

`(none nearby)` means the store is unconditional within its basic block, not
that the function is unguarded -- a guard several blocks earlier still applies,
and `scc.py` is the tool for finding the block structure rather than a
12-instruction window.

**How to narrow 15 to the one that matters, without another night of this:** the
field is observed zero at 1 s and 2 s while `+0` and `+0x10` are populated on the
same object, so the writer is either never called or returns before the store.
The cheapest discriminator is therefore **callers, not guards**: for each of the
15, find whether it is reachable from the startup path at all, using the
now-corrected `BL` decoder with a control. Fifteen reachability questions with
one validated search beats fifteen guard readings.
## 15 -> 11: four candidates have no caller at all

Reachability, with the corrected BL decoder and its control:

```
CONTROL  __stack_chk_fail callers = 14574  OK
PREDICATE VALID

  0 call sites (unreachable by direct call):
    0xcada74    0xd7ae7c    0xf3b390    0x13428c8

  reachable:
    0xbd0814 / 0xbd0894   3 callers   [0xbcffb4, ...]
    0xf3bd8c              15 callers  [0xb231b8, 0xb25aa4, 0xb2735c, 0xb30984]
    0xf42e44              3 callers   [0xf432e0, 0xf4373c, 0xf43954]
    0x104ad50             1 caller    [0x104aeac]
    0x10ac5e8             1 caller    [0x13b2064]
    0x118211c             1 caller    [0x117a448]
    0x12bd154             1 caller    [0x1403e20]
    0x13bad80             1 caller    [0x1405fe4]
    0x1414594            12 callers   [0x98faa8, 0xa12a0c, 0xa4f84c, 0xda7980]
    0x159d298             1 caller    [0x159c158]
```

**Four of fifteen cannot be reached by a direct call at all** -- vtable-only or
dead. That leaves **eleven**.

One of the single-caller entries is worth putting first, on location rather
than on evidence: `0x10ac5e8`'s only caller is `0x13b2064`, and `0x13b****` is
the JNI view export neighbourhood this project already maps -- `0x13b3614` is
`onViewRenderJNI` and `0x13b58dc` is the time function. **That is a hypothesis
about where to look, not a finding, and the eleven are not yet ranked.**

**Where this leaves the question, honestly:** eleven reachable writers of a
field observed to be zero, one construction route already shown to be a
different object, and a reaper measurement that is the only runtime evidence
any of it rests on. Narrowing further is static work with a validated search;
settling it needs the field observed changing on hardware, which is the
`.\tools\Deploy-To-Switch.ps1 -Card <card>` run nobody has done yet.
## RETRACTED: the 17-site list, the 4-unreachable split and the 11 survivors

Reading the first "survivor" killed the enumeration itself, not just the entry:

```
0x10ac5c0  mov  w8, #0xf400 ; movk w8,#1,lsl#16
0x10ac5dc  add  x8, x19, x8
0x10ac5e8  stp  xzr, xzr, [x8]      ; clears x19+0x1f400, not engine+8
```

**The enumerator that produced the 17 sites gated on "the base register is
tracked" and then tested the store's RAW offset against `8`. It never added the
materialised displacement of the base register.** Every store through a tracked
register was therefore reported at its un-displaced offset, so a
`stp` at `x19+0x1f400` was logged as a hit at `+0/+8`.

Withdrawn, all of it:

- the 17-site list,
- the `0xcada74 / 0xd7ae7c / 0xf3b390 / 0x13428c8 have zero callers` claim,
- the 11 survivors and the reachability table,
- "15 live candidates".

**The one table that survives is the earlier 211-site enumeration**, because that
version computed `eff = tr[base] + imm * scale` and applied the materialised
displacement. Re-read the 211-site output with that in mind: entries at
`+8` are trustworthy, and everything else in that list is at least correctly
displaced.

**The two bugs in this session's searches were mirror images.** One dropped the
displacement and reported raw offsets; the earlier one dropped a field from the
encoding and reported nothing. Both produced confident, plausible lists. In both
cases the thing that caught them was reading an entry -- first the store's
disassembly, then its function body -- instead of trusting the table.

**The control was passing for the wrong reason in both cases.** It verified the
offset decoder while the defect was in how the offset was applied, and it
verified the base tracker while the defect was in the base's value. **A control
has to be a case that would fail if the specific arithmetic were wrong; checking
a decoder is not checking a decoder's use.**
## FINAL STATE: the site list is not trustworthy, in either version

Re-run with the displacement applied (`eff = eng[base] + imm*scale`), and with
a control chosen to fail if the displacement were dropped -- `0x10ac5e8` must
land at `0x1f400` and must NOT land at `+8`:

```
CONTROL  0x10ac5e8 reported at 0x1f400 and NOT at +8 : FAIL
```

It is in neither bucket. The reason is straightforward on re-reading the
function: `x19` in `[0x10ac300, 0x10ac6a0)` is that function's own `this`, and it
is never the value loaded from the `.got` slot, so the tracker had already
dropped it and the store was skipped.

**Which means the two versions fail in opposite directions and neither is
usable:**

- the raw-offset version reported 16 `+8` sites, most of them displaced stores
  misfiled at their un-displaced offset;
- the displacement version reports none, because it loses the base register
  whenever a function reaches its object as a parameter rather than through the
  `.got` load -- which is the common case.

**So the number of writers of `engine+8` is still unknown, and the 211-site
table's `+8` row is no more reliable than the 16-site list.** The only reason the
211 table looked better is that it applied the displacement; it has the same
blind spot in the other direction, and nothing ever tested that.

**The honest position:** `engine+8 == 0` is measured and solid. Who should write
it is not known. A correct enumeration needs a real dataflow pass over each
candidate function -- seeding the base from the parameter that *is* the engine,
which means knowing which parameter that is, per function. That is not a
one-line predicate, and pretending otherwise is what produced the two wrong
lists above.

**This is where the static route ends for tonight.** The measurement is solid,
the enumeration is not, and the difference between them is exactly the discipline
that caught every other error this session: a number that has survived no check
is not a number.
## singleton-writers.py: what control B still needs (and why it is not a one-liner)

The tool is delivered and it correctly refuses. Two defects remain, both in the
route-B path, both found by reading one prologue:

```
0x10ac348  add  x24, x0, x12
0x10ac34c  mov  x19, x0          <-- the object arrives as x0, spilled to x19
...
0x10ac5dc  add  x8, x19, x8
0x10ac5e8  stp  xzr, xzr, [x8]   <-- must be reported at 0x1f400
```

1. **`mov xD, x0` was invalidating the base instead of copying it.** Fixed --
   the tool now carries `eng[x0]` into the destination.
2. **`eng[x0]` is never seeded in the first place.** Route B assumes "x0 is the
   object", but the scan walks `.text` linearly, so there is no point at which
   that assumption is established. The scan therefore has to iterate **per FDE**
   and seed `eng[0] = 0` at each function entry.

Defect 2 is the structural one and it is exactly why every hand-rolled version
of this search failed in a different direction: a linear scan over one
address space cannot represent "at the start of this function, this register
means the object", which is the entire content of route B.

**So: iterate `guestfde._range()` over every FDE, seed `eng[0] = 0`, and run the
existing pass inside each one.** Control B is already the right control to
verify it -- it fails today and must not fail afterwards.

Until that is done, **no count of `engine+8` writers is claimed anywhere in this
file**, and the tool will keep printing `PREDICATE INVALID` rather than a list.
That refusal is the feature.
## Current state of singleton-writers.py: both controls fail, and that is correct

The per-FDE rewrite (iterate `guestfde.FDE`, seed `eng[0] = 0` at each entry,
run the pass inside each range) was the right structural fix and it is in.
Running it:

```
CONTROL A (.got route accounts for 0x12ddce4)          : False
CONTROL B (route-B store at its displaced offset)     : False
CONTROL B' (and NOT reported at the raw offset)       : True
PREDICATE INVALID -- refusing to print a list.
```

**Control A used to pass and now does not.** The per-FDE iteration regressed the
`.got` route that the linear scan handled correctly, so the rewrite traded one
broken dimension for another. The likely cause is that resetting `eng` per
function is correct, but the `.got` load can appear in a function that the FDE
table attributes differently from the linear scan's byte positions -- worth
checking whether `guestfde.FDE` and a linear `.text` walk disagree about where
a function begins.

**What is still true and worth keeping:**

- the `mov xD, x0` copy fix is correct and independently checkable;
- `0x10ac34c mov x19, x0` is the concrete route-B seeding point, read from the
  prologue, not inferred;
- `0x10ac5e8` must appear at `0x1f400` and must not appear at `+8` -- that is
  the right control and it is the one thing that would prove the whole
  displacement story.

**So the next single step is narrow:** make control A pass again under the
per-FDE scan, without touching the displacement logic. Everything else is
downstream of that. Until A and B both pass, the tool prints a refusal and this
file makes no claim about how many writers `engine+8` has.

**The refusal is the deliverable that matters.** Three hand-rolled searches
produced three confident wrong answers before this tool existed; it is the only
one of the four that will not emit a list until it can account for a case that
would fail if the arithmetic were wrong.
## Why the per-FDE scan cannot work on this binary

Checked, and it is a property of the guest image rather than a bug in the tool:

```
FDE entries: 84636
gaps=84635 overlaps=0
0x12ddce4 inside FDE NONE
```

**`guestfde.FDE` is not a partition of `.text`.** It has 84,635 gaps between its
84,636 entries, and the known-good writer `0x12ddce4` falls inside **no FDE at
all**. A per-FDE scan therefore skips the gaps -- which is exactly where
`0x12ddce4` lives -- and that is the whole of control A's regression from True
to False.

**The generalisable fact, and it is larger than this tool:** a large share of
functions in `libRealRacing3.so` have no unwind record, so **any enumeration
that iterates function tables undercounts**, silently and in a way that looks
like a result. The FDE count is 84,636; how many of those ranges actually tile
`.text` is a question worth asking before trusting any FDE-driven scan.

**The fix for `singleton-writers.py` is therefore both routes at once:**

- keep the **linear scan** for coverage (it found `0x12ddce4`; the per-FDE one
  did not), and
- reset `eng` at **function entry** -- seeded at every FDE start *and* at every
  prologue that looks like one (`stp x29, x30, [sp, #-N]!`), because that is the
  only entry signal present for the functions the FDE table misses.

**Control A is the regression test for exactly this**: it passed under the
linear scan and must pass again after the resets are added. If it does not, the
resets are dropping route A's `.got` loads, and that is the bug to find next.
## Control A restored: linear scan + prologue-based entry resets

The fix named in the previous section works for route A:

```
CONTROL A (.got route accounts for 0x12ddce4)          : True
CONTROL B (route-B store at its displaced offset)     : False
CONTROL B' (and NOT reported at the raw offset)       : True
PREDICATE INVALID -- refusing to print a list.
```

`enumerate_sites` now walks `.text` linearly (coverage -- this is what finds
`0x12ddce4`) and resets `eng` at every **function entry**, detected as an FDE
start **or** a `stp x29, x30, [sp, #-N]!` prologue, because the FDE table misses
84,635 functions' worth of range and the prologue is the only entry signal those
have. Control A passing again is the regression test for that change doing its
job.

**Control B is still red, and the tool is right to refuse.** The remaining
defect is in the prologue mask or in the assumption that `x0` is the object in
`[0x10ac300, 0x10ac6a0)`. Two things to check, in this order:

1. does the entry predicate actually fire at `0x10ac300`? Print the addresses it
   matches near there -- if `0x10ac300` is not in the matched set, the mask is
   wrong and nothing downstream matters;
2. only if it does fire: is the object actually in `x0` at that entry, or does
   the function take it as a later argument?

**State of the question, unchanged and honest:** `engine+8 == 0` is measured and
solid; **how many writers it has is still not established**, and the tool will
keep refusing to say. That refusal has now survived four separate attempts to
make it produce a list, which is the point of building it this way.
## Control B: two named defects, both located

```
0x10ac300 in FDE_STARTS   : True    <- entry detection DOES fire here
prologue mask fires       : False
prologue-mask hits in .text : 0     <- the mask matches nothing, anywhere
word @0x10ac300 = 0xa9ba7bfd ;  (w & 0x7FC00000) = 0x29800000, tested against 0x29400000
```

**Defect 1 -- the prologue mask is wrong and matches zero instructions.** It
should be field extraction, not a bitmask: `Rt == x30 (0x1E)`, `Rn == sp
(0x1F)`, `Rt2 == x29 (0x1D)`, with the pre-index bit set. This is latent, not
the blocker: `0x10ac300` is in `FDE_STARTS`, so entry detection fires through the
FDE branch regardless. It matters for the functions with no FDE, which is the
whole reason the prologue check was added.

**Defect 2 -- `_scan_step` has no `ADD/SUB (register)` case, and that is the
blocker.** The displacement is built like this:

```
0x10ac5c0  mov  w8, #0xf400
0x10ac5d0  movk w8, #1, lsl #16        ; w8 = 0x1f400
0x10ac5dc  add  x8, x19, x8            ; <- REGISTER form, not immediate
0x10ac5e8  stp  xzr, xzr, [x8]         ; must be reported at 0x1f400
```

The pass handles `ADD/SUB (immediate)` only, so it drops the base at the one
instruction that forms the offset -- and then the store is skipped, which is
exactly the symptom control B reports.

**So the fix is not "seed route B better"; it is "handle the register add
form".** That is a dataflow case the earlier hand-rolled versions never had
either, which is why every one of them lost this function.

Two consequences worth stating plainly:

- **The count of `engine+8` writers stays unknown.** Not 0, not 16 -- unknown.
  The tool keeps refusing, which remains the correct behaviour.
- **Every conclusion in this file about writers of `+8` is void**, and the three
  earlier site tables were not merely imprecise -- each was produced by a pass
  that dropped a base register somewhere, in a different place each time. The
  failure mode was constant; the location moved. That consistency is the actual
  finding of the night: **the bug was never in the guest, it was in an
  enumerator that could not express a register-to-register add.**
## The last fix, precisely: ARM64 option fields are extracted, never masked

Adding `ADD/SUB (register)` did not turn control B green either, for the same
reason as the prologue mask:

```python
if ((w >> 13) & 0x7) == 0:      # WRONG
```

`option` is **bits 15:13**, and for a plain `add x8, x19, x8` it is **`0b010`**
(LSL with shift amount 0). `0b000` is the **extended-register** form
(`add x8, x19, w8, uxtw #0`). So the branch never fired.

The correct test, by field:

```
option = (w >> 13) & 0x7
ADD  : option in (0b000, 0b010)          # extended, or shifted-by-imm6
SUB  : option in (0b011, 0b100, 0b101, 0b110)
imm6  = (w >> 10) & 0x3F     applies only to the shifted-register options
Rm    = (w >> 16) & 0x1F
```

**This is the third time tonight that an ARM64 field was compared with a bit
mask instead of extracted, and the third time it produced a confident wrong
answer rather than an error.** ADRP's `Align(PC,4096)`, the prologue's
`Rt/Rn/Rt2`, and now `option` -- all three were "a mask that looked right and
matched nothing, or matched the wrong thing".

**The pattern worth carrying:** on this instruction set, a predicate must name
its bit positions. A hex mask is not a description of an encoding, and a mask
that matches zero instructions is as dangerous as one that matches too many,
because both look like a working search.

**State: unchanged and honest.** `engine+8 == 0` is measured. The number of
writers is not established. `singleton-writers.py` refuses, and it has now
refused six attempts to make it speak -- which is the one behaviour from
tonight that never once produced a wrong number.
## The option-field fix did not work, and the next defect is unknown

```
CONTROL A (.got route accounts for 0x12ddce4)          : True
CONTROL B (route-B store at its displaced offset)     : False
CONTROL B' (and NOT reported at the raw offset)       : True
PREDICATE INVALID -- refusing to print a list.
```

`option = (w >> 13) & 0x7` with `0b010` accepted for ADD is the correct ARM64
decode, and it still did not produce `0x10ac5e8` at `0x1f400`.

**So the next blocker is not identified, and I am not going to guess at it
again.** Earlier in this session the honest claim would have been "one more
field fix away"; after six attempts that claim has no evidence behind it. What
is known:

- control A passes, so the `.got` route and the linear scan are sound;
- control B fails, and the failure is *not* the `option` field, the prologue
  mask, the `mov xD, x0` copy, the FDE coverage, or the base displacement --
  each of those was fixed or ruled out in turn;
- the probe's own route-B seeding at `0x10ac300` is the next thing to suspect,
  and the cheapest way to settle it is to print the tool's `eng` state at
  `0x10ac34c`, `0x10ac5c0` and `0x10ac5dc` rather than reason about the
  encoding again.

**That is the right next step and it is a measurement, not another hypothesis.**
Six attempts at reasoning produced six wrong answers; the pattern that has
actually worked all night is reading the instruction or the value.

`engine+8 == 0` remains measured. The number of writers remains **unknown**, and
the tool says so.
## MEASURED: the base dies between 0x10ac34c and 0x10ac5c0

Walking that function with the tool's own pass and printing `eng` at the
watched instructions:

```
0x10ac300  entry                eng={}
0x10ac34c  mov x19,x0           eng={'x0': 0x0}      <- x0 tracked, x19 not yet copied
0x10ac5c0  mov w8,#0xf400       eng={'x21': 0x0}     <- x0 AND x19 both gone
0x10ac5d0  movk w8,#1<<16       eng={'x21': 0x0}
0x10ac5dc  add x8,x19,x8        eng={'x21': 0x0}
0x10ac5e8  stp [x8]             eng={'x21': 0x0}
offsets recorded inside this function: []
```

**This is the first narrowing in this whole search that is a measurement rather
than a reading of an encoding.** `x0` is tracked at `0x10ac34c` and absent at
`0x10ac5c0`, so the instruction that invalidates it is in that 0x274-byte
window -- and an unrelated `x21` has appeared holding 0, which means some path
copied the base into `x21` and something then dropped `x0` itself.

Two things follow, and only these two:

1. **the next step is bisecting that window**, not reasoning about ARM64
   encodings again. Print `eng` at every instruction in
   `[0x10ac34c, 0x10ac5c0)` and the drop instruction is the first one after
   which `0 not in eng`;
2. **`mov xD, x0` is not the only thing that can carry a register.** The pass
   models `add`/`sub`/`mov` and assumes everything else invalidates, which is
   safe for correctness and lossy for coverage. A `csel`, a `orr` used as a
   move, or a store through a derived register all behave differently, and
   which one it is here is now a one-line print away.

**The lesson of the night, in its final form:** six attempts failed because I
reasoned about encodings; the seventh succeeded because I printed a value.
## ROOT CAUSE, found by bisection: a store was invalidating its own base register

Printing `eng` at every instruction in `[0x10ac300, 0x10ac5c0)`:

```
0x10ac34c  mov  x19, x0        eng={x0:0}          x0 tracked
0x10ac3d0  str  xzr, [x19,#0x54b8]  eng={x0:0, x21:0}   <-- x19 is GONE after this
0x10ac450  stp  q0, q0, [x0,#0x80]  eng={x0:0, x21:0}
0x10ac460  add  x0, x19, x8     eng={x21:0}
```

**`0x10ac3d0: str xzr, [x19, #0x54b8]` is where the tracking dies**, and it is
the pass's own semantics that are wrong:

```python
mb = mem_base(w)
if mb is not None and mb in eng:
    for off in store_offsets(w):
        out.setdefault(eng[mb] + off, []).append(pc)
    eng.pop(mb, None)        # <-- WRONG
```

**A store writes memory at `base+off`. It does not change the base register.**
Invalidating `eng[mb]` there is a category error, and it is the single defect
behind every version of this search that has ever produced a wrong answer:

- it drops the base at the first store, so everything after it is invisible --
  which is why the raw-offset version "found" sites and the displacement version
  "found" none;
- which store happens to come first differs per function, so the failure moved
  around and each run looked like a different bug.

**The fix is to remove that `pop` and nothing else.** A base register is
invalidated when it is *reassigned* -- and `add`, `mov`, `sub` already do that.
`0x10ac460: add x0, x19, x8` legitimately ends `x0`'s life as the object, and
the pass already handles that; `x19` should have survived to reach it.

**This is the line to change, and control B is the test that it is fixed.**
## The pop fix worked; the blocker moved one step, and it is MOVZ/MOVK

With `eng.pop(mb)` removed, control A still passes and control B still fails --
because the base is now carried far enough to reach the real missing case:

```
0x10ac460  add  x0, x19, x8      needs eng[19] AND eng[8]
0x10ac5c0  mov  w8, #0xf400      <- MOVZ, not modelled at all
0x10ac5d0  movk w8, #1, lsl #16   <- MOVK, not modelled at all
```

`x19` now survives to `0x10ac5dc` -- the store fix did its job -- but `eng[8]`
does not exist, so the register-form add cannot resolve and falls into the
else-branch that drops the destination. **The pass has no MOVZ or MOVK rule.**

They are the two remaining common ways a displacement gets built, and both are
plain field extractions:

```
MOVZ  (w & 0x7F800000) == 0x52800000      hw = (w >> 21) & 0x3
      imm = (w >> 5) & 0xFFFF ; eng[Rd] = imm << (16*hw)
MOVK  (w & 0x7F800000) == 0x72800000      hw = (w >> 21) & 0x3
      imm = (w >> 5) & 0xFFFF ; insert imm at bit 16*hw of eng[Rd]
```

MOVK needs care: it merges into what the register already holds, so `eng[Rd]`
must exist first, and a MOVK into an untracked register must leave it
untracked rather than invent a value.

**This is the last named defect, and control B is the test.** The chain that got
here -- store semantics first, then MOVZ/MOVK -- is the same shape as the
guest's own code: the pass has to model the same ways of building an address that
the compiler uses, and it has now been caught modelling one (two-register add)
and not the other (load-immediate then add).
## The measurement settles it: route B's premise is wrong for this function

`eng` at the end of the traced window:

```
0x10ac430  mov  w8, #0xbb8    x0=0x100  x8=0xbb8
0x10ac460  add  x0, x19, x8   x19 IS NOT IN eng
```

**`x19` is not tracked, and `x0` has been walked to `0x10` and then `0x100`** --
i.e. `x0` is a general-purpose variable in this function, and the pass has been
propagating "this is the object" into it the whole time.

So route B's assumption -- *"`x0` is the object on entry"* -- is simply **false
here**. `x0` is reused; the object only briefly coincides with it. Seeding
`eng[0] = 0` at entry is what manufactures the wrong answer, and every fix
after that has been patching a symptom of that one bad premise.

**The premise has to be established per function, not assumed globally.** A
function's object parameter is whichever register the caller passes, and that
has to be read from the call site -- which is the same "verify the first link"
lesson that ended the construction-sequence detour, arriving from the opposite
direction.

**This is the eighth attempt and the tool still refuses, which remains the
correct behaviour.** The difference is that this time the refusal is explained
rather than mysterious: it is not a missing instruction form, it is a wrong
assumption about where the object lives.

**Everything else tonight stands unchanged**, and none of it depends on this
tool: `engine+8 == 0` measured; the guest raises its own fatal dialog iff
`*(*(0x1f6d5d0)) == 0`; the fault storm is that dialog sitting undismissed;
`0x9f9960` is dead code; `feats/.feats` is genuinely absent; two real
deployment bugs fixed; `fsTest=1fff`, build exit 0, 11/11 contract tests.
## DECISIVE: the guest stats .feats, gets ENOENT, and bails BEFORE trying to open it

The binary contains the string the reader would use:

```
0x42110c  "shader features file: %s.feats\n"      <- a printf format, not a path
0x4646d6  ".feats"
```

Across **all eight** recent archives, `.depot/feats/.feats` is requested and
stat'd -- and in **none** of them does the guest's own
`"shader features file: ..."` line appear. Every one of those 4 or 8 hits is an
*instrument* line of ours (`path-req`, `map-app`, `api-touch`), not a guest
string.

**So the order is: stat the file -> ENOENT -> return early, never reaching the
code that opens and logs it.** `.feats` is not merely "missing and therefore
innocent" as one earlier section concluded -- it is missing, it is on the
critical path, and the failure is upstream of the parse.

`xref-str.py` finds no direct `adrp`/`add` for `0x42110c` even though 197
ADRPs target its page, so the format string is reached through a string table
with a base pointer plus offset -- the same "resolved from a table" shape as the
localised-message lookup at `0x13c3fc4`.

## The experiment that settles it, and it needs no rebuild

Our router sends `.depot/feats/.feats` to `rr3core:`, where it does not exist
(in all seven volumes, and in the APK's 1822 entries). **The router already has
a leaf-scoped exclusion mechanism** -- the one added for `sv.*` and `ctd` in this
same file. Adding `.feats` to it makes the path resolve to the SD app directory
instead, and any file placed there is then found by the guest's own `stat`.

The success test is already instrumented and needs nothing new:

- the guest's `shader features file: ...` line starts appearing, and
- the reaper's `asset=.../first=...` line changes.

**If `engine+8` goes non-zero, `.feats` was the cause and the file's format is
the next thing to derive from whatever the guest does with it. If the field
stays zero, `.feats` is exonerated a second time, this time with the early-bail
path actually established rather than inferred.**

This is the cheapest untried move in the whole investigation: one routing line,
one file on the SD card, one run, and the reaper answers.
## EXPERIMENT RUN: .feats is exonerated, and this time properly

`20261004-085336-featsfile`. One line added to the leaf-scoped content
exclusion (`strcmp(leaf, ".feats") == 0`), so the path resolves to the SD app
directory, and an empty file placed at
`sdmc:/switch/rr3-native-rehost/.depot/feats/.feats`.

| observation | before | after |
|---|---:|---:|
| guest stat on `.feats` | `rc=-1 errno=2` (ENOENT) | **`rc=0 size=0`** |
| guest's own `shader features file: %s.feats` line | never | **still never** |
| `engine+8` | 0 | **still 0** |
| fatal dialog | yes | **still yes** |

**Making the file exist changed nothing.** The guest never opens it either way,
its own log line for it never fires, and the field and the dialog are unchanged.

**So `.feats` is exonerated, and unlike the first exoneration this one is
experimental rather than inferred.** The earlier "exoneration" argued that the
functions in the traced chain did not read files; this one made the file
present and observed that the guest still did not read it.

**And a corollary worth keeping:** the string `"shader features file: %s.feats"`
is evidently not on this path at all. It belongs to some other build of the
asset code, or to a branch gated behind something else. Its presence in `.rodata`
was never evidence that this run reaches it.

**What this leaves, honestly:**

- `engine+8 == 0` -- measured
- the fatal dialog is raised iff `*(*(0x1f6d5d0)) == 0` -- established
- the fault storm is that dialog undismissed -- established
- `0x9f9960` is dead code -- established with a passing control
- `feats/.feats` -- present now, read by nothing. Ruled out experimentally.
- **who writes `engine+8` -- still unknown**, and the enumerator still refuses

**The routing change is kept** (it is correct on its own terms: a file the guest
probes should not be routed into a read-only content volume) but it is not the
fix, and the notes say so.
## FOUND: the crash is a virtual call through the asset object's null vtable
## -- and this read also retracts one of my own "confirmed" claims

I chased `engine+8` for most of the night. **The dialog does not test it.** The
condition is `*(*(0x1f6d5d0)) == 0` -- the ASSET singleton at `0x1f6d5d0`, a
different object from the engine at `0x1f6d5d8`. The reaper has been printing
both every second and I read past the relevant line:

```
asset=0x210811fe98  first=0  NULL-FIRST-WORD    <- what the dialog tests
eng=0x210810b688    f8=0                      <- what I chased for hours
```

Read `[0xcdde2c, 0xcde58c)` -- the one writer of the asset singleton's first
word that a passing control had identified:

```
0xcde40c  adrp x8, #0x1f6d000
0xcde410  ldr  x8, [x8, #0x5d0]     ; x8 = the asset singleton
0xcde418  ldr  x0, [x8]             ; x0 = *asset
0xcde41c  adrp x8, #0x1f70000
0xcde420  ldr  x8, [x8, #0x888]     ; x8 = *(0x1f70888)   <- ANOTHER global
0xcde424  str  w9, [x8]             ; writes 1 THERE, not to *asset
0xcde428  ldr  x8, [x0]             ; x0 = *asset; load its vtable
0xcde42c  ldr  x8, [x8, #0x20]      ; vtable slot 0x20
0xcde430  blr  x8                   ; *** virtual call through a null vtable
```

**Two things, and the second one is a retraction:**

1. **This is the fault.** `x0 = *asset`, the code loads `*x0` as the vtable and
   calls slot `0x20`. `*asset` is 0, so it reads a word from address `0x20` and
   branches to it. That is a branch to an address derived from near-nothing --
   which is the shape of the 948,000 unmapped accesses, and it sits two
   instructions after the object whose first word the dialog test inspects.

2. **`0xcde424` is not the writer of `*asset` and never was.** The store's base
   is `*(0x1f70888)`. My "one confirmed writer, positive control passed" was
   wrong in exactly the way the previous three were: the base register was
   tracked, the offset was decoded, and the two were never added together.
   **Ninth instance, and the first one I had labelled confirmed.**

**What the remaining work actually is:** `*(*(0x1f6d5d0))` must be a valid
vtable pointer before `0xcde430` executes, and no code in the image writes it
through that pointer. The same conclusion I reached for `engine+8` -- and the
same conclusion is now reached twice, for two different fields, which is the
strongest hint in this whole file: **something in this port leaves those guest
singleton objects without their vtables, and it is not the guest's own code
doing it.**

That is a host-side question, and it is the first question tonight that points
at our code rather than at the emulator or at the tool.
## The asset constructor is reachable only through the .got, and that is checkable

Re-ran the search for `&0x1fc6e98` with the arithmetic verified inline
(`assert page + off == target`), because the first attempt got the page wrong
and the second got it right and disagreed:

```
target=0x1fc6e98  page=0x1fc6000  off=0xe98   verify page+off=0x1fc6e98
code taking &0x1fc6e98 (ADRP+ADD) : 0
```

**Zero, and this time the arithmetic is provably right**, which is the first
time tonight a negative on this address has been trustworthy.

Together with the relocation:

```
0x1f6d5d0  .got  R_AARCH64_RELATIVE  addend 0x1fc6e98   (a .bss static)
0x1f6d5d0  -> 0x1fc6e98, whose first word is the vtable, and it reads 0
```

**If no code forms the address, then no code can call its constructor either --
so the constructor must be reached the same way the object is: through the
`.got` slot.** The call site is therefore of the form

```
adrp xN, #0x1f6d000
ldr  xM, [xN, #0x5d0]      ; xM = the object
bl   <ctor>                 ; which writes the vtable at [xM]
```

and the concrete next step is: **list every site that loads the `0x5d0` slot
and is followed by a `bl`, then look for the one that is a constructor.** The
`.got` tracking in `singleton-writers.py` is the part that passes its control,
so this search can reuse it unchanged -- unlike route B, which never had a
sound premise.

**If such a call site exists and is never executed, the cause is in this
port's initialisation order or in a guard on that call. If no such call site
exists, the vtable is never written by this image at all, and the question
becomes why the object is wired into the `.got` slot when nothing constructs
it** -- which is a question about our own loader rather than about the guest.

Both branches are decidable statically, with a search whose only moving part
has already passed a control.
## MEASURED: the asset object is read 1403 times and never constructed

```
CONTROL: loaders of the ENGINE slot (0x5d8) = 2459  OK
loaders of the ASSET slot (0x5d0)             = 1403
asset-slot loaders followed by a bl that consumes the loaded reg: 0
```

(The control first failed with `0` for the engine slot, because `LDR`'s `imm12`
is **scaled by the access size** -- `0x5d8/8 = 0xBB` -- and I passed the byte
offset. Tenth instance of the same family, and the control caught it in one
line. Scaled-offset fields are now the third recurring shape tonight, after
`STUR`'s imm9 and ADRP's page alignment.)

**So: the object at `0x1fc6e98` is dereferenced 1403 times, wired into the
`.got` slot by a `R_AARCH64_RELATIVE` relocation, and no code in this image ever
calls a constructor on it.** Its first word -- the vtable -- is never written,
which is exactly what the reaper has been reporting and exactly what
`0xcde430` branches through.

**And this finally points at our own code rather than at the emulator or at
the enumerator.** The most likely mechanism is `.init_array`: constructors are
registered there and invoked **as function pointers by the loader**, not by a
`bl`, which is precisely why a `bl`-based search found none. This port's rebase
reports `ctors=181` and validates only `ctor0ok=1` -- the first entry.

**So the next question is ours:** does `ctors=181` include the entry that
constructs the asset singleton, and does our rebase invoke it with a working
`this`? Concretely --

- read `.init_array` and count its real entries against the 181 we run;
- find which entry, if any, is the asset constructor (it will be a function
  that stores a `.data.rel.ro` address to `[x0]` and nothing else);
- if it is present and being called, the failure is inside it -- a guard, or
  `this` being wrong, both of which are visible in its first instructions.

**This is the first question tonight that is answerable by reading our own
loader rather than the guest, and it is the one the evidence now points at.**
## CLOSED: nothing in the image writes the asset vtable

With a positive control on the same shape (`engine`-slot loaders = 2459, so the
search demonstrably finds slot loads):

```
asset-slot loaders = 1403
asset-slot loaders followed by a store through that register = 1
   0xcde410  x8 ; str w9, [x8]   FDE=[0xcdde2c, 0xcde58c)
```

**One candidate, and it is the false positive already identified by reading it.**
Between the load and the store, `x8` is reassigned
(`adrp x8,#0x1f70000 ; ldr x8,[x8,#0x888]`), so the `str` targets
`*(0x1f70888)`, not the object. **This script does not model the reassignment --
it is the same register-confusion defect the tool documentation already lists,
reproduced in a fresh script.**

The conclusion is therefore stronger than "no constructor was found":

> **No code in `libRealRacing3.so` stores a `.data.rel.ro` address -- a vtable --
> into the object at `0x1fc6e98`.** It is reached 1403 times, its vtable word is
> read 1403 times, and it is never written.

`.init_array` has exactly **181** entries, matching the `ctors=181` our rebase
reports, and all 181 are zero in the file (the dynamic linker fills them). So
the constructor list is not truncated. Since `.init_array` entries are called
**without arguments**, a constructor for this object could not receive `this` as
a parameter -- it would have to fetch the object from the same `.got` slot the
readers use, and the search above covers exactly that shape.

**So the remaining possibilities, and only these:**

1. the constructor is in one of the **other three images**
   (`libc++_shared.so`, `libNimble.so`, `libfmodex.so`) and is invoked through
   a mechanism this image does not show -- then the question is whether our
   loader runs *their* `.init_array`, which our rebase reports per image
   (`ctors=181` for img 0, but the other images' lines showed `ctors=0` and
   `ctors=1` and `ctors=2`);
2. the vtable is meant to be written by the **dynamic linker** from a
   relocation, and ours does not apply that relocation for this slot;
3. the object is genuinely a non-polymorphic struct and the code at `0xcde430`
   is not a virtual call.

**Option 2 is the one that points at this port, and it is checkable by reading
our own relocation application** -- not by disassembling the guest again.
## Option 2 eliminated: our RELATIVE relocations ARE applied

`elf_preflight.cpp` walks `.rela.dyn` and has
`switch (type) { case kRAarch64Relative: ++result.relativeRelocations; ... }`
-- but that is the **preflight counter**, not the loader that applies them, so
reading it proves nothing either way.

What settles it is the reaper's own measurement, taken every second:

```
asset = 0x210811fe98        *(0x1f6d5d0) with mainBias 0x2106759000
```

`0x1f6d5d0` is a `.got` slot whose value comes **only** from its
`R_AARCH64_RELATIVE` entry (addend `0x1fc6e98`). The value read back is
`0x210811fe98` = `0x1fc6e98 + 0x2106759000` **exactly**. **So the relocation
for that slot was applied, correctly, with the right bias.**

**Option 2 -- "our loader did not apply the relocation" -- is eliminated by
measurement, not by argument.** Our relocation handling is fine; the object it
points at simply never got its vtable.

**That leaves exactly two, and both are decidable without the emulator:**

1. **The constructor is in one of the other three images.** The rebase already
   reports per-image constructor counts, and they were `ctors=181` for the main
   image but **`ctors=0`, `ctors=1` and `ctors=2`** for the others -- with
   `ctor0ok=-1` on one of them. A DSO whose constructors are not being run is
   exactly the shape that leaves a cross-image singleton unconstructed. **Read
   the three other images' `.init_array` sizes and compare with what the rebase
   runs for them.** If one has entries and we run zero, that is the bug, and it
   is ours.

2. **It is not polymorphic and `0xcde430` is not a virtual call** -- which would
   mean the guest itself is fine and the raise is something else entirely.

**Option 1 is the one to test first**: it is three ELF headers and a number,
and the rebase log already has the suspicious `ctors=0` sitting in it.
## Option 1 eliminated: every image's .init_array is fully run

```
image                  .init_array bytes   entries   our rebase reported
libRealRacing3.so               1448        181   ctors=181  ctor0ok=1
libc++_shared.so                  8          1   ctors=1
libNimble.so                      0          0   ctors=0
libfmodex.so                     16          2   ctors=2    ctor0ok=1
```

**All four match exactly. No image's constructor list is truncated or skipped**, so
"the constructor lives in another image and we never ran it" is eliminated by
counting, not by argument.

**What is left is one check, and it is a relocation, not an instruction:**

> Does `.rela.dyn` contain an entry with `r_offset == 0x1fc6e98`?

- **If it does**, the linker was supposed to write the vtable into the object
  itself, our relocation pass applies `.rela.dyn` (proven correct on the
  `.got` slot, where the value read back was exactly addend + bias), and the
  field should be non-zero. It reads 0, so the pass would have to be missing
  this entry specifically -- and the preflight counter in `elf_preflight.cpp`
  reports `relativeRelocations`, so the two can be compared directly.
- **If it does not**, then the vtable was never meant to be written by anyone,
  and `0xcde430`'s `ldr x8,[x0] ; ldr x8,[x8,#0x20] ; blr x8` is not a virtual
  call at all. The object is a non-polymorphic struct, the raise is about
  something else, and the whole "null vtable" framing -- which this file has
  carried for several sections -- is wrong and should be struck.

**Either outcome ends a line of inquiry that has consumed most of the night,
and one of them ends it in a way that invalidates the framing rather than
refining it.** That is the more valuable of the two, and it costs one grep.
## RETRACTION: the relocation arithmetic in the previous section does not hold

```
R_AARCH64_RELATIVE entry with r_offset == 0x1fc6e98 : 0
CONTROL: entries for the .got slot 0x1f6d5d0 = 1  OK
total rela.dyn entries = 151591
```

Two things, and the second retracts the first half of the previous section.

**1. Nothing writes the object's own address** -- no instruction, no
relocation, and no constructor in any of the four images receives it as `this`
(all four `.init_array` counts match what the rebase runs: 181 / 1 / 0 / 2).
So its first word is 0 at boot and stays 0.

**2. The arithmetic that eliminated "our relocation pass is broken" is wrong.**

The previous section claimed:

```
asset = 0x210811fe98  =  0x1fc6e98 + 0x2106759000   "exactly"
```

It is not. The reaper's own numbers give

```
0x210811fe98 - 0x2106759000 = 0x1A46E98        (not 0x1fc6e98)
```

and `0x1A46E98` is inside `.text`, not `.bss` -- while the slot's recorded
addend is `0x1fc6e98`, inside `.bss`. **The value in the `.got` slot is not
addend + bias, so the slot does not hold what I said it holds, and the
"relocation applied correctly" conclusion is withdrawn.**

Three readings remain, and none of them is established:

- `mainBias` as the reaper prints it is not the bias that was actually used for
  this image (the rebase line and the sampler line reported `0x2106759000` and
  `0x2106559000` in different runs -- **two different numbers, never
  reconciled**);
- the slot is written by something other than its `R_AARCH64_RELATIVE` entry;
- the guest patches the slot at runtime.

**This is the same failure as the construction-sequence detour, and it is the
second time tonight: I verified a link in a chain instead of verifying the
thing itself.** The number to trust is the one the reaper reads; the addend is
what the file says; and they do not agree, and that disagreement is now the
finding.

**Everything downstream of "our relocations are fine" is void until that
discrepancy is resolved** -- including the elimination of option 1 and the claim
that no image constructs the object.
## THE FOUNDATION IS ALSO SUSPECT: three different biases, and the reaper uses the wrong one

```
slot 0x1f6d5d0   R_AARCH64_RELATIVE  addend = 0x1fc6e98   (in .bss)
reaper asset     = 0x210811fe98
reaper bias      = 0x2106759000
asset - bias     = 0x19c6e98                              (in .text)
implied bias     = 0x2106159000  =  asset - addend
```

**`0x1fc6e98 - 0x19c6e98 = 0x2000000` exactly.** The value the reaper read back
corresponds to a load bias of `0x2106159000`, and it is using `0x2106759000`.
The two differ by exactly 32 MB, and the "object" it is pointing at under the
reaper's bias lands in `.text` while the file says `.bss`.

Three biases have now appeared in this project and none of them was reconciled:

| source | value |
|---|---|
| `sampler-image` / `sampler-start` | `0x2106559000` |
| `rebase: newBias` | `0x2106759000` |
| implied by the reaper's own asset read | `0x2106159000` |

**This voids the one thing this file has treated as measured all night.** The
reaper computes its addresses as `bias + elfva`; if the bias is 32 MB out, then
every value it reports -- `asset=`, `first=`, `eng=`, `f8=0`, `t0..t3` -- is read
from the wrong place, and `engine+8 == 0` and `NULL-FIRST-WORD` are readings of
whatever happens to be at `0x2106759000 + 0x1f6d5d0`, not of the singleton.

**Nothing in this file is a measurement until the bias is pinned.** The guest's
`.rela.dyn` is the authority: addend + the real bias must equal the runtime
value, and that identity is the test. The reaper should derive its bias the way
`sampler-writers` does -- from `renderTarget - renderExportElfVa`, which was
cross-checked against the `render-call-pre` straddle line -- and then assert
`asset == bias + addend` before trusting anything else it prints.

**This is the deepest retraction of the night, and it is the same mistake a
third time: I verified that a number was self-consistent instead of verifying
that it was the number it claimed to be.** The `NULL-FIRST-WORD` verdict looked
solid because it came from an instrument, and I never once checked the
instrument's own coordinate system against the one the file is written in.
## FINAL RETRACTION: the reaper reads 4 MB below the slot, so its values are void

```
authoritative bias (renderTarget - renderExportElfVa) = 0x2106b59000
reaper bias                                        = 0x2106759000
difference                                         = 0x400000

correct address of the .got slot 0x1f6d5d0          = 0x2108ac65d0
address the reaper actually read                     = 0x21086c65d0
value it read there                                 = 0x210811fe98
bias implied by that value                          = 0x2106159000
```

**Four different biases have now appeared in this project, none reconciled:**

| source | value |
|---|---|
| `renderTarget - renderExportElfVa` (straddle-cross-checked) | `0x2106b59000` |
| `sampler-image` / `sampler-start` | `0x2106559000` |
| `rebase: newBias` and the reaper's | `0x2106759000` |
| implied by the reaper's own read | `0x2106159000` |

**And the consequence is total for everything that went through the reaper:**
it computes addresses as `bias + elfva` with a bias 4 MB out, so the slot it
reads is not the slot. **`asset=`, `first=`, `eng=`, `f8=0`, `vt=0`,
`table=0`, `t0..t3=0` are readings of unrelated memory.** Therefore:

- **"`engine+8 == 0`" was never measured.** It is void.
- **"`NULL-FIRST-WORD`" was never measured.** It is void.
- **"the engine chain breaks at its first rung" is void.**
- **"nothing writes the asset vtable" is unaffected** -- that came from the
  `.rela.dyn` and `.text` searches, which are computed in ELF coordinates and
  do not use the bias at all. It stands, and it is the strongest static result
  in this file.

**What survives, because none of it used the reaper:**

- the guest raises its own fatal dialog, and the strings and the raise site's
  instruction sequence are read from the binary;
- the fault storm follows the dialog in time and the alert pump cannot deliver
  the button, so the storm is that dialog sitting undismissed;
- `feats/.feats` is absent from all seven volumes and the APK, and making it
  present changed nothing;
- `0x9f9960` has zero callers;
- three real deployment bugs fixed; `fsTest=1fff`; build exit 0; 11/11.

**The first action for the next session is one line of arithmetic**: derive the
bias the way the straddle cross-checks it, and make the reaper
`assert asset == bias + addend` before it prints anything. Every singleton-level
claim in this file is suspended until that assertion passes.

**This is the third and worst instance of tonight's recurring mistake, and it
is worth stating as the closing lesson: I spent the night verifying that my
instruments were self-consistent, and never once verified that they were
pointing at the thing I thought they were pointing at.** A number from a probe
is not a measurement of the guest until the probe's coordinate system has been
checked against the guest's own -- and the guest states its own in
`.rela.dyn`, in a form any ELF reader can confirm in one grep.
## The assertion passes, and the "4 MB off" conclusion was a cross-run comparison

`20261004-090239-biasassert`, first run with `assert asset == bias + addend`
compiled into the probe:

```
reaper-asset-singleton ... asset=2107f1fe98/0/NULL-FIRST-WORD
                             eng=2107f0b688 f8=0 vt=0 table=0
```

**The probe did not refuse -- the identity held.** And the values are nothing
like the previous run's:

```
this run   asset=0x2107f1fe98   eng=0x2107f0b688
last run   asset=0x210811fe98   eng=0x210810b688      (differ by 0x2000000)
```

**The image base moves by 32 MB between runs.** And the rebase log has always
said so:

```
rebase: img=0 oldBias=0x52aa86000 newBias=0x2106759000
```

**The image is re-based after it is first loaded.** So the "four different
biases" and the "4 MB off" finding were **me comparing a bias taken from one
archive against an asset value taken from another** -- the same cross-run
comparison error as the one retracted in the "base, not section start" note,
and the one that produced a confident table several sections ago.

**Where this leaves the singleton readings.** The probe now *proves* its own
coordinate system on every tick before it prints, so the values in
`20261004-090239-biasassert` are trustworthy **for that run**:

- `asset` first word is 0 there, and the assertion confirms the slot was read
  correctly;
- so **`NULL-FIRST-WORD` is real, and so is `engine+8 == 0` (the `f8=0` line)**
  -- but both are now established *by an instrument that refuses to lie* rather
  than by a number whose provenance was never checked.

**What the earlier retraction over-corrected, and should be reinstated:** the
static result that no code, no relocation and no constructor writes the asset
vtable was never affected by the bias question -- it was computed in ELF
coordinates throughout -- and it stands next to the now-validated runtime
reading. **Both halves of the same finding survive: the guest never writes that
vtable, and at runtime it is zero.**

The lesson is now complete and it cost the whole night:

> **A probe is not a measurement until it can prove which address it read.**
> Cross-run comparison of a bias and a value is the failure that produced the
> most confident wrong table here, and the assertion that would have caught it
> is now compiled in.
## Logically closed: no constructor exists in any image, so the pointer is filled by the DATA LOAD

```
.init_array relocations: 181
  type histogram: {1027: 181}      all R_AARCH64_RELATIVE
  entries with a non-zero symbol: 0  no cross-DSO references
  addends: 0x98e620, 0x9ac0c0, ...  all in .text
```

`.init_array` entries are called **without arguments**, so a constructor for the
asset object could not receive `this` as a parameter; it would have to form the
address itself, and the search for that (`page 0x1fc6000 + add #0xe98`, arithmetic
asserted) returns **0**. There is no cross-image constructor either.

**Therefore: no constructor exists anywhere for the object at `0x1fc6e98`.**

That forces the last reading, and it closes the loop on the dialog's own words:

> The object is a **non-polymorphic struct whose first field is a pointer filled
> by the data load**, not a vtable installed by a constructor. `0xcde430`'s
> `ldr x8,[x0] ; ldr x8,[x8,#0x20] ; blr x8` is a call through a pointer the
> asset pipeline was supposed to install. It did not, because the asset pipeline
> did not complete -- and the dialog says exactly that, in the words
> `GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`.

**So "the guest is spinning" was never the finding. The finding is: the asset
pipeline did not complete, and every downstream symptom -- the null pointer, the
virtual call through it, the 948,000 unmapped accesses, the fatal dialog, the
undismissable box -- is one chain from that one fact.**

**What is still unknown is therefore the original question all along, and it is
back where it started: why does the asset pipeline not complete?** Every path
into it has now been tried and closed with evidence:

| candidate | closed by |
|---|---|
| `feats/.feats` missing | file made present; guest still never reads it |
| the seven data volumes | byte sizes match the manifest; guest stats 59/61 successfully |
| save / `SaveManager` | `SaveManager.cpp:699` is a "non main thread" guard, not this error |
| `connect()` / Cloudcell | still open -- **not yet excluded** |
| device profile / `no3g.xml` | still open |
| startup order across the four images | `.init_array` fully run for all four |

**Those last two are the live list, and both are ours to test, not the
guest's.** That is a far better place to have ended the night than "the wedge".
## The live lead: our own android.os.Build identity, tested with the wrong metric

```
N9810        0 occurrences in libRealRacing3.so
ro.product   0
ro.build     0
onlyWIFI     1
```

**The guest does not read device identity out of its own string table.** It
asks `android.os.Build` -- which **this port supplies**, in
`jni_compat/classes/os_Build.{h,cpp}`, switched by `RR3_BUILD_IDENTITY`.

And the `.depot/no3g.xml` we serve (`rc=0 size=162`, valid XML) declares
`<onlyWIFI>` and `<device id="N9810"/>` -- a 2014 Galaxy Note 3 -- while our
Build shim answers **Galaxy S5 / `klte` / "JohnnyonFlame"**. Two different
answers to the same question, in two files the guest can read.

**The Build identity A/B run earlier tonight changed the identity and measured
fault counts and rendering behaviour -- and found nothing.** That is not a
refutation. **The thing that is now known to matter is whether the asset
pipeline completes**, and the metric used was never that. The dialog is the
pipeline's own "I did not finish" message, and the field it leaves null is the
pipeline's output.

**So the test is cheap and the instrument already exists:** run
`RR3_BUILD_IDENTITY=0` against `=1` and read the reaper's
`asset=.../first=...` line and the dialog, **not** the fault count. A run in
which the dialog disappears and `first` becomes non-zero would name the cause
in one iteration.

`no3g.xml` is the second lever if Build is not it: the device id in it is
almost certainly a capability key the asset pipeline matches on, and `N9810` is
a plausible decade-old gate. The volume ships `eds/AndroidHigh.plist` and
friends, so the data set has modern device profiles in it -- and the content
layer's `eds/*.plist` requests were the first 61 entries in the content census,
i.e. the guest looks at them **early and repeatedly**.

**Both levers are one file and one A/B flag. Both are ours. Neither has been
tested against the right metric.**
## RR3_BUILD_IDENTITY=0: exonerated, measured with the right metric

`20261004-090630-buildid0`, reading the assertion-verified field rather than
the fault count:

```
asset=0x210871fe98/0/NULL-FIRST-WORD
eng=0x210870b688 f8=0 vt=0 table=0 t0..t3=0
dialog hits = 1
```

Identical to `=1`. **The guest's own device identity is not what gates the
asset pipeline**, and the earlier "Build A/B found nothing" now means something
different than it did: that run measured the wrong metric and so proved nothing,
and this one measures the right one and proves the negative.

Flag restored to 1 -- the self-consistent Galaxy S5 / `klte` answer, which
matches the `kGlRenderer` string the graphics layer reports, and is a truer
answer than the single-placeholder path regardless of the outcome.

**The live list is now one item, and it is not a file:**

| candidate | status |
|---|---|
| `feats/.feats` | exonerated experimentally |
| seven data volumes | exonerated (59/61 requests succeed) |
| save / `SaveManager` | wrong function (a "non main thread" guard) |
| device identity via `android.os.Build` | **exonerated just now** |
| `.init_array` across four images | fully run, and none writes the field |
| **`connect()` / Cloudcell** | **the only one left** |

**Every path into the asset pipeline that lives in a file or a flag is now
closed with evidence.** What remains is the one thing this port stubs out: the
network path. `rr3_Platform.cpp` records that `connect()` is stubbed, and the
guest's Nimble/Cloudcell stack initialises (its class names and
`setupNimble` appear in the Eden log) but has no server to talk to.

**So the next question is concrete and it is ours: does the asset pipeline
require a successful Cloudcell handshake before it will complete, and what does
it need that handshake to return?** The guest-side answer is findable in the Nimble
strings already logged (`com.ea.nimble.*`, `nimble.environment.notification.*`)
and in `cc-slot` activity; the host-side answer is in whatever our Cloudcell
shim returns for the initial handshake.
## The last live lead is one line: getNetworkConnectivity() hard-codes 0

The Cloudcell dispatch traffic from `20261004-090630-buildid0`:

```
cc-slot: slot=44  caller=0x210742a758
cc-slot: slot=3   caller=0x210742aaf8
cc-slot: slot=41  caller=0x210742ab10
...                                   a contiguous ~0x1a0-byte cluster
cc-slot: slot=3   caller=0x210720bcf0   a different region entirely
```

And the host side of that conversation,
`jni_compat/classes/cloudcell_compat.cpp:31`:

```cpp
static jint get_network_connectivity(JNIEnv *, jclass) { return 0; }
```

**We always answer "offline".** The guest is asked whether it has a network and
is told no, every time, by construction -- and the message it then raises is
`GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`.

**This is consistent with everything that survived tonight's elimination**: the
file paths are fine (59/61 content requests succeed), the volumes are fine, the
device identity is fine, the constructors all run, and the one pointer the asset
pipeline was supposed to install is still zero -- because the pipeline was
told the data could not be fetched.

**It is not proven.** `getNetworkConnectivity` may be read for telemetry or for
a wifi-only download prompt, and `no3g.xml` does carry `<onlyWIFI>`. But it is
the only remaining candidate that is **ours**, it is **one line**, and it has
never been varied.

**The A/B, with the metric that is finally the right one:**

1. flip `get_network_connectivity` to return 1 (or make it configurable behind
   `RR3_NET_CONNECTIVITY`, default unchanged);
2. read the reaper's `asset=.../first=...` line and the dialog -- **not** the
   fault count, which is what made the previous Build A/B look like a null
   result;
3. if `first` becomes non-zero, the cause is named in one iteration.

Everything else on tonight's list is closed with evidence. This is what is left,
and it is a single `return 0`.
## RR3_NET_CONNECTIVITY=1: also exonerated. The list is empty.

`20261004-090835-net1`, same metric:

```
asset=0x210831fe98/0/NULL-FIRST-WORD
eng=0x210830b688 f8=0 vt=0 table=0
dialog hits = 1
```

Identical to `=0`. **Reporting the guest as online does not make the asset
pipeline complete**, so the pipeline is not gated on that answer.

**Every candidate on the live list is now closed, each with the assertion-
verified field as the metric:**

| candidate | closed by |
|---|---|
| `feats/.feats` absent | file made present; guest still never reads it |
| seven data volumes | 59/61 content requests succeed; sizes match manifest |
| save / `SaveManager` | `SaveManager.cpp:699` is a "non main thread" guard |
| `android.os.Build` identity | `RR3_BUILD_IDENTITY=0` -- unchanged |
| `getNetworkConnectivity` | `RR3_NET_CONNECTIVITY=1` -- unchanged |
| `.init_array` across four images | all run (181/1/0/2), none writes the field |
| a constructor for the object | none exists in any image; no code, no relocation |
| **the field itself** | **0, on an instrument that refuses to lie** |

**What this means is uncomfortable and worth stating plainly: the field is
zero, nothing in the guest or the host that we have varied writes it, and the
dialog is real. The remaining possibility is that the value is written by code
this port does not run** -- a JNI method the shim answers with a stub, an
initializer invoked through a path we do not exercise, or a constructor in a
DSO whose `.init_array` our rebase reports but does not actually execute in
order.

**And the one measurement that would settle it is the one thing I cannot make
here**: on real hardware the guest runs its own initialisers in the real
dynamic linker, and the same reaper probe -- which now proves its own coordinate
system before printing -- would say in one line whether the field is still zero.
## ctors=181 is a SECTION SIZE, not a count of constructors invoked

`multi_dso_linker.cpp:1806-1830`, the `rebase:` line this project has been
reading all night:

```cpp
"rebase: img=%zu oldBias=0x%llx newBias=0x%llx init0=0x%llx ctors=%llu ctor0ok=%d",
    ...
    (unsigned long long)(image.initArraySize / sizeof(uint64_t)),      // ctors=
    !hasInitArray ? -1
        : (initArrayTargetInCode(image, image.initArrayAddress, init0) ? 1 : 0));
```

**`ctors` is `initArraySize / 8` -- the LENGTH of the `.init_array` section.**
It is not a count of anything that ran. And `ctor0ok` is
`initArrayTargetInCode(...)`, a preflight that the first entry's address lies in
a code segment -- **not that it was invoked, and certainly not that all 181
were.**

**So the elimination "all four images' `.init_array` are fully run" is void.**
It was a section size read as an execution count, which is the same class of
error as every other one tonight: a number that looked like the thing I needed
and was not.

**And this one is in our own loader, which is where the only remaining
explanation lives.** The chain of inference that has to be re-checked:

```
"ctors=181 == .init_array has 181 entries"      <- TRUE
"therefore 181 constructors ran"                <- NEVER ESTABLISHED
"therefore no constructor writes the field"     <- COLLAPSES
```

**The question that is actually open, and that nobody has answered: does this
loader invoke the 181 entries at all, and in what order?** Everything above it
rests on assuming that it does.

**Concretely checkable, in our own code, with no emulator involved:**

1. find where the entries are *called* (as opposed to counted) and whether that
   loop is reached on the startup path or bails earlier;
2. whether an early `return false` at line 1796 can skip it for some images --
   the `rebase:` line is printed in the same function *after* that bail, and if
   any image took the bail, its `ctors` is still printed;
3. whether calling order matters -- 181 static constructors in the wrong order
   is a classic way to leave a singleton zero while every individual
   constructor returns normally and nothing looks wrong.

**This is the first lead tonight that is entirely inside our own code, and it
was sitting in a log field I had been reading for hours.**
## VERIFY, not INVOKE: no constructor call site in multi_dso_linker.cpp

The functions are named for what they do:

```cpp
verifyLoaderLifecycleArray(image, image.initArrayAddress, image.initArraySize,
                           &check.constructors, &check);      // line 1776
verifyLoaderLifecycleArray(image, image.finiArrayAddress, image.finiArraySize,
                           &check.finalizers, &check);        // line 1778
```

and the same pair again at 2109-2112, and `verifyLifecycleArray(...)` at 2271.
`check.constructors` is a count of **verified** targets, and `ctor0ok` is
`initArrayTargetInCode(...)` -- a check that the address is in a code segment.

**Nothing in this file calls an `.init_array` entry.** The guest's 181 static
constructors are relocated, checked, counted and logged -- and the actual
invocation lives somewhere else, which this search did not find.

**So the open question is sharper than "were they run in order":**

> **Where does anything in this port call the guest's `.init_array`, and is that
> call reached on the startup path at all?**

If nothing does, then every one of the guest's static constructors -- and only
those -- never runs. That would explain, with one cause:
- a singleton whose first field is still zero at boot and forever after;
- a subsystem the asset pipeline expects to already exist;
- and the guest's own `GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`, which
  is what a half-constructed engine says.

**And it would mean the 181 constructors are the largest untested surface in the
entire port**, sitting behind a counter that reads like a health check.

**The search that settles it is ours, not the guest's:** grep this source tree
for the call site -- something that reads `image.initArrayAddress` and
*invokes* the pointer (a cast to `void(*)()` and a call, or a libnx
`__libc_init_array` equivalent) rather than counting it. If nothing invokes it,
that is the bug, and it is a few lines to write.
# ANSWER: the guest's 181 static constructors never run

The invocation path exists. `multi_dso_linker.cpp` runs it in stages
(`result.stage = 2` for the main image, deps first) and leaves a breadcrumb
before every call:

```cpp
__builtin_snprintf(d, sizeof(d), "initDeps-ctor img=%d ord=%d", ...);   // 2551
...
// Durable breadcrumb before the risky call; if this ctor faults with no    // 2759
...
mainCtors-preflight-fail ord=%llu raw=0x%llx ...                          // 2678
mainCtors-target-lost ord=%llu raw=0x%llx ...                             // 2747
```

**In `20261004-090835-net1`, not one of those lines is in the log:**

```
ctor-progress lines : 0
failure lines       : 0
```

Not a truncated run, not a failure marker, not a partial sequence -- **the
constructor invocation path is never reached.**

**So: none of the guest's 181 static constructors executes. Not one.** And
that accounts for everything that survived tonight's eliminations, with a single
cause:

- the singleton's first field is zero at boot and stays zero, because the
  constructor that would install it is in `.init_array` and `.init_array` is
  never invoked;
- the asset pipeline finds no subsystem, which is what
  `GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE` means;
- the raise site then dereferences the null pointer and the guest faults its
  way into a fatal dialog that nobody can dismiss.

**The `rebase:` line, which does appear, is a different function** -- it
relocates, *verifies* and *counts* the lifecycle targets (`ctors=181` is
`initArraySize / 8`, a section length), and a sibling function never gets as far
as calling them. **Two functions with the same 181 in them, one that verifies
and one that runs, and the project has been reading the first all night.**

**This is ours. Not the emulator, not the guest, not a data file.** It is a
missing call on the startup path in `multi_dso_linker.cpp`, and the stage
machine that is supposed to drive it is not being entered.

**The next step is small and entirely local:** find who calls the staged
lifecycle runner, and find out why the log shows its verifier but not its
executor. The two `ctors=181`-shaped numbers are the whole confusion; the fix is
to make the one that runs reachable from startup.
# RETRACTED, IMMEDIATELY: the constructors DO run

The claim in the previous section -- "the guest's 181 static constructors never
run" -- is **refuted by the very log it cited.** The same file contains:

```
phase=initDeps-preflight-ok
phase=initDeps-tls-scope
phase=initDeps-stage3
phase=initDeps-completing
phase=rebase: img=0 oldBias=0x815627000 newBias=0x2106359000
              init0=0x2106ce7620 ctors=181 ctor0ok=1
phase=rebase: img=1 ... init0=0x0                ctors=0  ctor0ok=-1
phase=rebase: img=2 ... init0=0x21083475fc       ctors=2  ctor0ok=1
phase=rebase: img=3 ... init0=0x2108544398       ctors=1  ctor0ok=1
```

**The staged lifecycle runner runs, and the counts match every image's
`.init_array` size.** The `initDeps-ctor img=%d ord=%d` per-constructor
breadcrumb I grepped for is one line inside one loop; its absence proves
nothing about whether the loop executed.

**This is the same mistake as every other one tonight, committed on the single
most consequential claim of the night, one paragraph after writing down that it
was the mistake to stop making.** A negative from a search for one specific
literal is not evidence of absence; the only way to know is to find what the
function *does* emit, and I had the emitting lines in the same file.

**What survives:** the field is zero (assertion-verified), nothing in the guest
writes it, and the constructors are not the reason -- they run. The reason is
still open, and it is no longer "our loader skips init".

**The closing lesson, and this is the one to keep:**

> **A grep for a name you expect is not a test for a thing you have not seen.**
> To know whether a stage ran, read what it prints **when it runs**. I had four
> such lines in the file and still wrote the opposite conclusion.
# FOUND (properly this time): the main image's 181 constructors were never attempted

Read from the line that reports its own state, not from a grep for a name:

```
phase=bootdecide: onLoad.attempt=1 done=1 fail=0 found=1 called=1 ver=00010006
                  | main.attempt=0 done=0 fail=0 stage=0
                  | entry.attempt=0 ret=0 resume=0
```

and the dependency stage, which **did** run to completion in the same log:

```
phase=initDeps-preflight-ok
phase=initDeps-tls-scope
phase=initDeps-stage3
phase=initDeps-completing
```

**`main.attempt=0` -- the main image's lifecycle stage was never attempted.**
The three dependency DSOs' constructors ran (0, 2 and 1 of them, matching their
`.init_array` sizes); **the 181 in `libRealRacing3.so` did not.**

`multi_dso_linker.cpp:2655` has a `"mainCtors-already-initialized"` branch that
would log if the stage had been skipped because it had already run. **It is not
in the log**, so this is not "it ran earlier" -- it never started.

**And this reconciles the thing that should have stopped me a paragraph ago:**
a binary whose 181 static constructors never execute would still reach the
render call, compile shaders and stat 59/61 files, because most of what it does
at that point is on the explicit call path, not in static initialisers. My
"that is impossible" objection was itself an assumption, not a measurement --
and this is the answer that satisfies it.

**How this was found, and why it is trustworthy this time:** the previous
attempt grepped for `initDeps-ctor` and read the zero as "did not run". This
attempt read `bootdecide`, which prints `attempt` / `done` / `fail` / `stage`
for the main stage directly. **The difference between the two is the entire
lesson: a line that reports its own state settles the question; a name you
expected does not.**

**Why it explains the whole night:** no static constructor runs, so the singleton
whose first word is the subsystem pointer is never initialised, the asset
pipeline finds nothing to attach, the guest raises
`GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE`, and the call at `0xcde430`
dereferences the null it was told to expect.

**The next step is in our code and it is one question:** why is
`main.attempt == 0` -- what gates the main stage behind deps completing, and
what has to be true for `attempt` to become 1? That is a control-flow question
in `multi_dso_linker.cpp`, answerable without an emulator.
# RETRACTED AGAIN: main.attempt=0 is a snapshot taken BEFORE the call

The claim in the previous section -- "the main image's 181 constructors were
never attempted" -- is **wrong**, and the source says so four lines apart:

```cpp
// main.cpp:1213-1230  -- the bootdecide line, and where it is printed
__builtin_snprintf(d, sizeof(d), "bootdecide: ... main.attempt=%d ...", ...);
rr3::appendGuestBootLine("sdmc:/.../guest-view.log", d, 227, 0);

// main.cpp:1232-1236  -- the call, AFTER the line above
if (dashboard.jniOnLoad.completed && !dashboard.mainInitialization.completed) {
    dashboard.mainInitialization = rr3::initializeGuestMainConstructors(kBootProgressPath);
}
```

`main.attempt` is `dashboard.mainInitialization.attempted` (main.cpp:1222), and
the line that reports it is emitted **before** the block that increments it.
**`attempt=0` means "not yet", not "never".** The stage is invoked a few lines
later, on the same pass.

**What I actually did:** read a status field out of a log line without reading
the function that writes it -- the same error as `verifyLoaderLifecycleArray`,
`FUN_013bca64` and `ctors=181`, on the most consequential claim of the night,
immediately after asserting I had stopped making it.

**The measurement that would have been correct, and is still available:**
`dashboard.mainInitialization.completed` and `.stage` are printed by the SAME
line, and this run reported `done=0 stage=0` for the main stage **at the moment
of logging** -- so the question "did it complete?" is answered by
`initializeGuestMainConstructors`'s own breadcrumbs (`mainCtors-*`, which have
their own failure strings) and by the `stage` value on any LATER `bootdecide`
line. This run logged `bootdecide` once, early, so it cannot answer that at
all. **The honest state is: unknown, and the run contains no evidence either
way.**

**The closing lesson, stated once and for the last time:**

> **读到一个状态字段，不等于读到了它的最终状态——除非你已经读过写它的那个函数。**
> `attempt` / `done` / `stage` 全部由 `initializeGuestMainConstructors` 写，
> 而我在读它打印的值之前，从没读过那个函数。
## The correct reading: the function WAS entered; main stage left no failure trace

Read the writer before the value, this time:

```cpp
GuestInitializationResult initializeGuestMainConstructors(const char* progressPath) {
    GuestInitializationResult result{};
    result.attempted = true;                 // first statement
```

`initDeps-completing` is emitted at **line 2611, inside this function**, and it
**is in the log**:

```
phase=initDeps-preflight-ok   (216)
phase=initDeps-tls-scope      (218)
phase=initDeps-stage3         (217)
phase=initDeps-completing     (223)
```

**So the function ran, `attempted` became true, and the dependency stage
completed.** The `main.attempt=0` in the `bootdecide` line is that field
*before* the call, four lines earlier in `main.cpp` -- retracted already, and
this run's own breadcrumbs are the cleaner evidence.

**The main stage's breadcrumbs are all failure paths**, and none of them is
present:

| line | breadcrumb | meaning |
|---|---|---|
| 2636 | `mainCtors-guard-fail` | guard rejected |
| 2647 | `mainCtors-soname-miss` | main image not found |
| 2689 | `mainCtors-preflight-fail ord=...` | a target did not resolve |
| 2701 | (ordinal 225, tag 4) | segment fault |
| 2727 | `mainCtors-tls-scope-fail` | TLS not ready |

```
mainCtors* lines in 20261004-090835-net1 : 0
```

**None of the failure paths was taken, and there is no success breadcrumb for
the main stage because the function does not emit one.** So the state is:

- entered: **yes**
- dependency constructors run: **yes** (and they did not fail)
- main stage failed visibly: **no**
- main stage completed: **unknown from this run**

**The next read is the tail of `initializeGuestMainConstructors` after the
constructor loop (from 2731 to the return)** -- what it does on the way out is
the only thing left unexamined, and it is in our own code, where every other
question of tonight has already been answered.

**And the correct way to close it, after twelve wrong ones, is to add a success
breadcrumb there** rather than to infer completion from a field printed before
the call.
## The one change that would let the guest keep running

The dialog is dismissed by exactly one path -- `alertAnswerPositiveImpl`
(jni_bridge.cpp:2347) enqueues the callback and the pump thread drains it.
Measured: the pump reaches `wait-result` and then stops, and
`cloudcell_pump`'s first act is `std::lock_guard<std::mutex> on g_queue_lock`
(cloudcell_defer.cpp:71), which the spinning main thread is holding. So the
answer is never delivered and the guest waits on the modal box for the rest of
the run.

**The change, stated precisely and left for a session that can test it:**

In `alertAnswerPositiveImpl`, after the existing `cloudcell_defer([...])`
enqueue, drain once **inline on the caller's own thread**:

```
startAlertPump();                       // unchanged, keep the thread as a backstop
cloudcell_defer(body);                  // unchanged
cloudcell_pump("alert-inline", false);   // NEW: same thread, right after enqueue
```

This is already known to be safe by the file's own comment above
`startAlertPump` (lines 2310-2313): *"cloudcell_pump's own lock is taken and
dropped inside cloudcell_defer.cpp:50-68 before any callback runs, which is what
makes calling it from this second thread safe."* Draining from the caller's own
thread is no less safe than draining from the pump thread, and the caller's
thread is not the one starved.

**Why this is worth doing even though it is not the fix:** it is the difference
between a run that ends at the first error dialog and a run that continues past
it. Whatever the asset pipeline was going to do next becomes observable, which
is exactly the information the remaining open question needs. It also removes
the last reason the fault storm is 948,000 accesses rather than a clean stop.

**It is a behavioural change to the alert path and needs one run to confirm** --
the success test is `reached-drain` appearing and the guest getting past the
dialog, read from the census lines that already exist.
# The alert is now dismissable, and the guest tells us what it intends: quit

`20261004-091821-alertinline`, with `RR3_ALERT_INLINE_DRAIN 1`:

```
phase=alert-callback: invoking alertMessageExecuteCallback(141863287192) thiz=0x0
                     guestx1=0x81227774 | measured: 0x12b3c50 reads neither x0 nor x1...
phase=gate-static-void  ordinal=... method=exitApp  ra=rr3+ba777c
phase=cloudcell_pump: site=alert-inline depth_before=1 drained=1 depth_after=0
                     enqueued_total=1 entered_total=1 drained_total=1
```

**Three things settle at once.**

1. **The inline drain works** -- `drained=1`, `depth_after=0`, and the
   `alert-callback` line appears for the first time in this project's history.
   The dialog was always dismissable; the pump thread was simply never
   scheduled. **That was our bug and it is fixed.**

2. **The guest's answer to this dialog is `exitApp`.** It pressed OK and then
   asked the platform to quit, which is exactly what Real Racing 3 does when
   game data cannot be loaded. **The dialog is the guest correctly reporting a
   fatal condition and cleanly exiting -- not a wedge.**

3. **The fault storm is NOT caused by the undismissed dialog.** Dismissing it
   left the counts essentially unchanged:

   | | undismissed | dismissed |
   |---|---:|---:|
   | unmapped read | 511,770 | 510,839 |
   | unmapped write | 436,398 | 437,152 |

   **So the 948k accesses are the guest's state *before* it reaches the decision,
   not a consequence of not reaching the button.** The causal order established
   earlier -- storm first, dialog second -- is now confirmed from the other side:
   fixing the dialog changed nothing about the storm.

**What this leaves, and it is a much smaller thing than tonight's start:**

- the asset pipeline does not complete;
- its one observable output field stays 0;
- the guest reports that, correctly, and quits;
- the 948k accesses are whatever the pipeline was doing while failing, and are
  now known not to be a symptom of the dialog.

**The remaining question is a single one: why does the asset pipeline not
complete.** Everything else -- the dialog, the undismissable box, the storm's
relationship to either -- is closed.
# The guest says it out loud: "Failed memory-mapping file: %s"

The guest's own log output (`guest-log-any`, 206 lines, never read until now --
which is the discipline this file has been asserting all night, applied last):

```
CREATE
LocalNotificationsCenterJNI::Init Begin / End
device UID / model ID / model Name / app Name / app Version / app Path
res Path / doc Path / cache Path / accel available / gyro available
SD card is mounted for reading and writing.
START
RESUME
startMusic disabled during download
VIEW CREATED
Vendor / Renderer / Version
Searching for settings for device: %s
Available memory: %dMb
chipset: %s
Model: %s
Connectivity::May use 3G? - %s
GPU id '%d', model '%d', renderer string '%s'
Failed memory-mapping file: %s          <<< TWICE
... then ~180 lines of "%s: %s"
```

**The guest tries to memory-map a data file and the mapping fails.** That is the
asset pipeline not completing, stated by the guest, in its own words, twice.

**And the ~180 lines of `%s: %s` are the same loop as the ~948,000 unmapped
accesses** -- the guest is logging in a loop while the pipeline is failing, and
every one of those log calls is an unmapped write on our side. The fault storm
is not a spin at all: it is the pipeline's failure path, and the dialog is what
the guest concludes from it.

**The one thing missing is the argument.** We captured the *format string*,
because that is what the log hook is handed; `Failed memory-mapping file: %s` is
printed with the filename as a printf argument that our hook never renders.
**The next measurement is that argument, and it names the file.**

It is available two ways, both cheap:

1. **In the guest.** Find the call site of the `"Failed memory-mapping file: %s"`
   string (it is a plain `.rodata` literal, so `xref-str.py` will locate it),
   and read what it loads into the printf argument register.
2. **On the host.** The mapping this port performs itself is already logged --
   `mapProcCode: emulator fallback`, `mapProcCode: complete` -- so a comparable
   `mmap` failure on our side would say the same thing. **Check whether our own
   file mapping is the one failing**, because "failed to memory-map" on the host
   and on the guest are the same failure wearing different names.

**This is the first time tonight the guest named the problem**, and it is a
whole subsystem I never opened: until now I had been reading the crash, the
dialog and the pointer, while the guest was logging the cause in plain text
two hundred lines above them.
## The whole chain ends at our own mmap

`"Failed memory-mapping file: %s"` is at `0x3902a2`; one confirmed xref, in
FDE `[0x12a48f8, 0x12a4acc)`, and the failure path is in instructions:

```
0x12a4a1c  bl   __open_2                      ; open(path, O_RDONLY)
0x12a4a20  tbnz w0, #0x1f, 0x12a4a70          ; fd < 0  -> error
0x12a4a2c  mov  w2, #1                        ; PROT_READ
0x12a4a30  mov  w3, #1                        ; MAP_SHARED
0x12a4a34  mov  x0, xzr                       ; addr = NULL, let the kernel pick
0x12a4a38  mov  x1, x21                       ; length
0x12a4a3c  mov  w4, w20                       ; fd
0x12a4a40  mov  x5, xzr                       ; offset 0
0x12a4a44  bl   mmap                          ; PLT 0x1e18d30  <-- OUR SHIM
0x12a4a4c  bl   __errno
0x12a4a54  cmn  x22, #1                       ; result == -1 ?
0x12a4a58  str  w8, [x19, #0x18]              ; save errno
0x12a4a5c  b.eq 0x12a4aa0                    ; yes -> "Failed memory-mapping file: %s"
0x12a4a60  str  x22, [x19]                    ; only on success
0x12a4a64  str  x21, [x19, #8]                 ; and the length
```

**The guest opens a data file, asks the kernel to map it, and when the mapping
fails it names the file -- twice -- and that is the asset pipeline not
completing.** And `mmap` at `0x1e18d30` is a **PLT entry this port resolves**:
the call is landing in our own implementation.

**So the one measurement that closes the night is on our side, and it is one
log line:** what does our `mmap` shim return for `(NULL, len, PROT_READ,
MAP_SHARED, fd, 0)`? Specifically:

- does it handle a **null address** (kernel-chosen), or reject it?
- does it handle **`len == 0`** -- the guest passes `x21`, and a zero-size
  mapping is the classic `EINVAL` and the classic silent failure in a shim;
- does it read the file's real length, or a host-side `st_size` that is 0 for a
  path that only exists inside one of the seven **romfs** volumes rather than
  on FAT? **Our data volumes are not files on the host filesystem** -- the
  `guestFopen`/`guestOpen` layer already abstracts that, and if `mmap` opens
  the path directly it is going to get nothing.

That last one is the shape that fits everything: 59/61 `stat` calls succeed
through the shim, the guest then calls `mmap` **directly**, the path does not
exist on the host, and the pipeline dies. **A read path that works and a map
path that does not is exactly the kind of asymmetry that produces "loads
nothing, complains about nothing, exits".**

**Add the argument and return to the `mmap` shim's existing trace, run once, and
the filename plus the errno are both in hand.**
# THE DEFECT: bionicMmap's file path is untested, and it is what the guest hits

`bionic_misc_shims.cpp:210`:

```cpp
void* bionicMmap(void*, size_t length, int protection, int, int fd, off_t offset) {
    if (length == 0 || (protection & 4) != 0) {          // 4 == PROT_EXEC -> ENOTSUP
        errno = protection & 4 ? ENOTSUP : EINVAL;
        return reinterpret_cast<void*>(-1);
    }
    const size_t allocationSize = (length + 0xfff) & ~size_t(0xfff);
    void* allocation = memalign(0x1000, allocationSize);
    if (allocation == nullptr) return reinterpret_cast<void*>(-1);
    std::memset(allocation, 0, allocationSize);
    if (fd >= 0) {
        const off_t original = ::lseek(fd, 0, SEEK_CUR);
        const bool positioned = original >= 0 && ::lseek(fd, offset, SEEK_SET) >= 0;
        const ssize_t readBytes = positioned ? ::read(fd, allocation, length) : -1;
        if (original >= 0) ::lseek(fd, original, SEEK_SET);
        if (readBytes < 0) { std::free(allocation); return reinterpret_cast<void*>(-1); }
    }
    ...
}
```

**And the self-test, three hundred lines below it:**

```cpp
void* mapping = bionicMmap(nullptr, 0x1800, 3, 0, -1, 0);      // fd = -1
if (mapping != reinterpret_cast<void*>(-1) &&
    bionicMprotect(mapping, 0x1800, 3) == 0 && bionicMunmap(mapping, 0x1800) == 0)
    passed |= 1ULL << 3;
```

**`fd = -1`. The anonymous case. The file-backed case -- the one the guest takes,
the one that produces `Failed memory-mapping file: %s` -- is not covered by
the self-test at all.**

**So the defect, stated without hedging:**

- the guest `open()`s a data file through **our** `guestOpen`, which resolves
  paths inside the seven **romfs** volumes through a VFS layer that has nothing
  to do with the host filesystem;
- the guest then calls **our** `bionicMmap`, which does `::read(fd, ...)` on that
  fd as if it were a plain host descriptor;
- for a path that lives inside a romfs volume, that read is not guaranteed to
  work, and on failure `bionicMmap` returns -1, which is the exact value the
  guest tests with `cmn x22, #1` before logging the filename.

**This is the same class of bug as the constructor finding, and it has the same
shape: a whole subsystem with a test that exercises the easy path and not the
one the game takes.** The read path (`guestFopen`/`guestStat`, 59/61 green) goes
through the VFS; the map path does not.

**The two things to do, in order:**

1. **Extend the self-test to a file-backed mapping of a path that actually lives
   in a volume** -- `bionicMiscSelfTest` currently cannot fail for the case that
   matters, so this is a test-coverage fix, not a behaviour change.
2. **Make `bionicMmap` go through the same resolution the read path uses**
   rather than assuming the fd is a host descriptor. Whether the fd from
   `guestOpen` is already a host fd for volume paths is the thing to check
   first -- if it is, the failure is `length == 0` or `PROT_EXEC`; if it is not,
   the failure is the `::read` above.

Either way the guest has now told us, in its own log, what it could not load.
---

## 2026-10-04 -- the mmap failure is a stat ABI mismatch, found by measurement

The previous section left two open questions. Both are now closed, and the
answer to the second one was not the one the section predicted.

### Q1: is the fd from `guestOpen` a real host descriptor?

Yes. `guestOpen` (`guest_filesystem.cpp:1200`) ends in a plain
`::open(resolution.primary, flags)`, so the descriptor it returns is a real host
descriptor and `::read` on it is legitimate. The "mmap bypasses the VFS"
framing in the previous section was wrong about the consequence: the VFS
resolution has already happened by the time `mmap` sees the fd.

### Q2: so why did the mapping fail?

Not `length == 0` because the file is empty, and not `PROT_EXEC`. Both were
eliminated by disassembly of the guest's own call site, FDE
`[0x12a48f8, 0x12a4acc)`:

```
012a4970  bl   0x1e18d10      ; stat(path, sp)     GOT slot resolves to "stat"
012a4974  cbz  w0,0x12a4a08   ; stat OK -> open
012a4a1c  bl   0x1e18d20      ; __open_2
012a4a24  ldr  x21,[sp,#0x30] ; length, raw load, NEVER tested for zero
012a4a2c  mov  w2,#1          ; prot  = PROT_READ
012a4a30  mov  w3,#1          ; flags = MAP_SHARED
012a4a38  mov  x1,x21
012a4a44  bl   0x1e18d30      ; mmap
```

`prot` is a literal `1` on the only path that reaches the call, so
`PROT_EXEC` can never be set here. The length comes from a fixed offset of the
`stat` buffer.

**The guest calls `stat` and then reads its size at +0x30. Our `guestStat`
handed that buffer straight to the host `::stat`, so what the guest loaded as a
file size was the host struct's `st_atim.tv_nsec` -- which is 0.**

Measured on device, 162-byte file, before the fix:

| offset | value | host field |
|---|---|---|
| 0x28 | 1791077481 | `st_atim.tv_sec` (that day's timestamp) |
| **0x30** | **0** | `st_atim.tv_nsec` <- the guest's "size" |
| 0x38 | 1791077481 | `st_mtim.tv_sec` |
| 0x40 | 0 | `st_mtim.tv_nsec` |
| 0x48 | 512 | `st_blksize` |

So: `mmap(len=0)` -> `EINVAL` -> the guest's own "Failed memory-mapping file"
-> the asset pipeline aborts. The 22 `mmap-file-fail` lines with
`reason=zero-length` in run `20261004-093116-statlayout` are the direct
evidence, and they also retired the 128-slot mapping-table hypothesis: the table
never held more than one entry (`used=0/128 high=1`).

### The fix is minimal, and the obvious "correct" version is worse

`writeBionicStat` in `guest_filesystem.cpp` has three modes, and the A/B decided
which one ships:

| mode | what it writes | mmap ok | mmap fail | error dialog | shaders |
|---|---|---|---|---|---|
| 0 | host struct unchanged | 1 | 22 | 1 | 0 |
| 1 | full bionic `struct stat` | 1 | 0 | 1 | 0 |
| **2 (shipped)** | host struct **+ `st_size` at +0x30** | **126** | **0** | **0** | **92** |

Mode 1 is the interesting failure. It puts `st_size` at +0x30 *and* relocates
every other field, and the guest stops calling `open` at all -- it bails between
`stat` and `open`. 26 fewer path resolutions, `open-ok` 36 -> 0. Mode 2 changes
exactly one field and the whole pipeline comes up.

**So the guest reads a bionic size field at +0x30 out of a buffer whose
remaining layout is not the one a bionic header would describe.** Both facts are
measured; the second is not explained. Recorded as a known unknown rather than
papered over with a plausible story. What is NOT in doubt is the A/B: mode 1 is
strictly worse than mode 2 on every counter, so mode 2 ships.

`struct BionicStat` carries `static_assert`s on the offsets, and
`sizeof(BionicStat) == 128` is asserted too -- the full layout came out at 136
bytes, which would have written 8 bytes past what the old passthrough wrote, and
the guest sizes those buffers, not us.

### What the guest does now

Run `20261004-094237-statmode2-final`: 126 successful file mappings with
`read` matching `len` on every one, 200/200 stat calls with `o30_is_size=1`, 92
`glShaderSource` + 92 `glCreateShader`, 32 `gfx-proc` calls, **0 error dialogs**.
The render pipeline starts. It did not start in any earlier run.

Two new, concrete failures are now visible for the first time, both on paths the
guest never reached before:

- `opendir-fail sdmc:/switch/rr3-native-rehost/doc/gui_assets`
- `fopen-fail  sdmc:/switch/rr3-native-rehost/.depot/player_profile.2.dat.bak`

Those are the next two things to look at, and they are ordinary data-routing
problems rather than an ABI mismatch.

### Unresolved / caveats

- **The ~510k `Unmapped Read/Write` count did not move** (510736 before,
  510083 after) even though the error dialog is gone and the guest is doing far
  more work. It is not yet established that the fault storm and the asset failure
  were the same thing. Do not assume they were.
- `fstat` also resolves to a host passthrough (`symbol_resolver.cpp`, frozen by
  a byte-for-byte guard), so any guest size query that goes through `fstat`
  instead of `stat` still reads 0. Not fixable without touching the frozen file.
- The `mmap` probe's `path=` field is unreliable once descriptors are reused:
  in one run fd 4 appears against four different files. The `len`, `read` and
  `ok`/`fail` fields are sound; the path attribution is not. Clearing a ring
  entry on `close` would fix it.
- The guest's "Failed memory-mapping file" string never actually appeared in
  Eden's log in any run, including the runs where 22 mappings demonstrably
  failed. The earlier claim that it came from the guest log was wrong; treat that
  string as unconfirmed as a log source.
- `frames=1` throughout. The guest is not yet producing frames.

---

## 2026-10-04 (afternoon) -- the asset singleton recovers, and the wedge is ours

### The night-long causal chain is now confirmed end to end

Archive `20261004-094723-nettoffline`, the run right after the stat-layout fix,
contains this line, and it is the whole night's thesis in one record:

```
phase=asset-singleton: elfva=0x1f6d5d0 bias=0x2106159000 slot=0x21080c65d0
                      singleton=210811fe98 first_word=80c32060 verdict=PRESENT
```

`verdict=PRESENT` with a real pointer in the first word. For the entire
previous session that field read 0, and 0 was what raised
GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE. Fixing the stat offset fixed
the thing the whole investigation was chasing.

### The ~946k unmapped accesses are NOT the Cloudcell connection

The fault burst is real: 946,755 lines inside a 7.1 s window (t=7.84 .. 14.9),
and only 20 of them are anything else. Its immediate precursor is the guest
resolving `0037-mp6.cloudcell.com` and opening two sockets. **That temporal
adjacency is what made Cloudcell the obvious suspect, and it is wrong.**

Three independent readings kill it:

1. **`RR3_NET_CONNECTIVITY` is not the trigger.** A/B at 1 and at 0 produced
   identical behaviour: one DNS resolve, two socket creations, 510069 vs 510094
   unmapped reads. The flag is not on this path at all. (The code comment that
   had claimed otherwise was removed rather than left in place.)
2. **`cloudcell_pump("frame", false)` never ran.** It logs only when its queue
   is non-empty, and the archived run has no `site=frame` line -- only the JNI
   view path's `before/after-created`, `-changed`, `-render`. It was a no-op.
3. **The connection is a symptom of the same phase, not the cause.** The guest
   reaches Cloudcell during initialisation; it does not reach it because of a
   failed mapping any more.

### What actually spins the guest: our own frame-loop draw call

The decisive pair, present in every run:

```
phase=view-call-render   ordinal=265  site=jni-view-render
phase=view-render-ret    ordinal=266                      <- RETURNS
phase=render-call-pre    ordinal=285                      <- never returns
                                            (render-call-post never emitted)
```

Same guest function, same arguments; the JNI view path's call comes back, the
frame loop's does not. `render-call-pre` is emitted under
`!g_renderStraddleEmitted.exchange(true)` so it fires on the first call only,
and `render-call-post` is the literal next statement -- pre present + post
absent means that call never returned.

Between them the frame loop runs three host actions, and all three are now
accounted for:

| host action | verdict | evidence |
|---|---|---|
| `pumpGuestControllerInput` | ruled out by A/B | `RR3_FRAME_LOOP_PUMP 1->0`, archive 20261004-133108-pumpoff: post 0->0, mmap 126/0 -> 126/0, shaders 92 -> 92, dialogs 0 -> 0 |
| `cloudcell_pump("frame")` | never fired | no `site=frame` line exists |
| `padUpdate` | completed | wedge probes run before-pad-update (279) -> loop-top (280) |

With all three eliminated, the hang is inside the guest's own `onViewRenderJNI`
when it is re-entered from our thread. Which is the thing the code comment
already said: *"re-entering the draw entry every tick is this probe's
invention, not the game's contract."* The guest drives its own rendering
through the View callbacks, and every one of them returns
(created, changed, focus-ret, changed-ret, render-ret).

`RR3_FRAME_LOOP_RENDER` is now 0, on that basis rather than on a hunch. Its
pre-stat-fix measurement was 948,009 faults -> 5. Re-measured in the
stat-fixed build, with criteria written before the run: P1 unmapped < 1,000,
P2 gfx-proc above 32, P3 log still growing at the end.

### Cloudcell: paused by request, and where the line is

The user was asked to choose and chose to stop anti-cheat work and continue on
render / input / assets only. Recorded so the boundary is not re-litigated:

- Already in the tree, not added today: 64 Cloudcell vtable slots that all
  return `self` so the guest's init "succeeds structurally"; `GetDeviceModel`
  answering `"R36S"`; `GetDeviceUID` answering `"rr3-r36s-local"`;
  `startDownloadService` as an empty function. No token, no account, no
  handshake, no server response.
- `doc/sv.2.dat` is 109 bytes, high-entropy, written BY THE GUEST during the
  run (timestamp 09:47:33 inside a run deployed at 09:47:20) and absent from
  the APK. The sibling `.depot/sv.2.dat.bak` is byte-identical. The other
  `doc/` files are the 0-byte stubs we pre-create at deploy time.
- Not done, and will not be done without an explicit decision: making the
  guest believe it passed the integrity check, synthesising a server response
  or a valid Cloudcell document, or removing a check.

### Two more null results worth not re-running

- **`doc/gui_assets` opendir failure is benign.** The APK has no `gui_assets`
  at all (1822 entries: 1575 `res`, 68 `META-INF`, 28 `lib`, rest Cloudcell SDK
  Java deps). The guest lists an optional directory once, gets ENOENT, and
  carries on -- 126 mappings succeed after it. Not a blocker, left alone.
- **"Failed memory-mapping file" is still unconfirmed as a log source.** It
  matches 0 times in Eden's log in any run, including the runs where 22
  mappings demonstrably failed. The mmap probe is the evidence; that string
  is not.

### The wedge was ours: `RR3_FRAME_LOOP_RENDER` off

Archive `20261004-133537-framelooprender-off`, 150 s, with the stat fix in.
Criteria were written before the run.

| | render ON | render OFF | verdict |
|---|---:|---:|---|
| unmapped reads | 510,234 | **15** | **P1 PASS** (< 1,000) |
| unmapped writes | 436,526 | **0** | |
| Eden log lines | 948,823 | **2,465** | 388x smaller |
| CPU at t=90 s | ~135 s | **54.1 s** | -60% |
| `render-call-post` | 0 | **1** | the call now returns |
| `gfx-proc` | 32 | **32** | **P2 FAIL** |
| fault dialog | 0 | 0 | |

**P2 failed and the change is kept anyway**, because the thing it removes is
ours: re-entering the guest draw entry every tick is not the game's contract,
it is this probe's invention. The guest renders through its View callbacks and
every one of them returns.

> **RETRACTED later the same evening.** "There is no Choreographer" is wrong,
> and it was wrong because it was never measured. A scan of the guest image for
> `Looper`, `Handler`, `Choreographer`, `MessageQueue`, `SurfaceHolder`,
> `GLSurfaceView`, `onDrawFrame` and `postDelayed` returns **zero hits for every
> one of them**. There is no Choreographer in this guest to be missing, so
> "supply one" was not a task. The paragraph above also asserts the guest
> renders "because its View invalidates itself", which is an Android-general
> belief applied to a guest that never mentions the mechanism. Kept in place,
> struck through, because the reasoning error is the part worth not repeating:
> **a familiar architecture story is not evidence that the architecture is
> present.** The same mistake, in the same file, produced the content-root
> reading, the frame-counter reading and the empty-stub reading earlier in the
> day -- three of four, all measured, all wrong.

What replaced the spin is not nothing. `guest-activity` went 0 -> 272 samples,
and all 272 are identical -- same `eng`, same engine pointer, same 1280x720,
same `cq1`/`cq2`. The guest is alive, polling, and not advancing.

### A tool for reading any of this

`tools/Read-GuestLog.ps1` takes a `guest-view.log` from an Eden archive or
straight off a card and prints the four load-bearing metrics (asset singleton
verdict, render-call pre/post, mmap outcome, stat offset correctness), the mmap
failure reasons, the guest activity counts, the routing failures, and the phase
histogram. Built because every one of these numbers was being re-derived by
hand each round, and a hand-derived number is one more chance to read the wrong
field.

### Two null results worth their own entry

**Thread placement is not the cause.** `render-thread=worker` in
`rr3-gfx.conf` moves the frame loop onto a thread created through
`guestPthreadCreate`, so the guest sees a real Android TLS instead of the host
main thread's. Archive `20261004-134140-renderthread-worker`, 150 s:

| | main thread | worker thread |
|---|---:|---:|
| unmapped reads | 510,234 | **944,979** |
| `render-call-post` | 0 | 0 |
| `renderThread` in log | `main requested=0` | `worker requested=1` (P4 PASS) |

The experiment was real and the answer is no. That kills "the main thread is
the problem" and leaves "the call itself is bad from the second call on",
independent of where it runs from.

**A criterion built on the wrong counter is worse than none.** `gfx-proc` was
assumed to count drawing. Its 32 entries are all EGL/GL *initialisation*
queries -- `eglGetPlatformDisplayEXT`, `glDebugMessageControlKHR`,
`glGetDebugMessageLogKHR` and so on -- with no draw and no swap anywhere in the
list, so P2 could never have moved no matter what the guest did. The real
counters are in the `guest-activity` snapshot, and there they read:

```
gl=1/3/0/635/191/2     clears=1 drawArrays=3 drawElements=0
                       texImage2D=635 compressedTexImage2D=191 bindFramebuffer=2
```

**The guest issued three drawArrays and then stopped.** With `splashExit=0`
that is an EA splash screen, rendered once, and then a wait. It is alive, not
wedged: `held_now=0`, `parked_in_mutex_lock=0`, `threads=5 live=4`, no mutex
held by an exited thread.

Clock ruled out as the thing it waits on: `clock_gettime` and `gettimeofday`
are `RR3_BIND` passthroughs, and the log's own `boot_ms` advances
(234729306 -> 234729309 across the call).

### Where this leaves rendering

Off: 15 unmapped accesses, CPU -60%, frame loop iterating, guest healthy and
frozen after its splash. On: a 945k-fault storm that buries everything.

Left OFF, because the Mesa backend on real hardware is the only thing that can
answer what the guest actually draws, and a fault storm is a poor way to find
out. The hardware run is the next experiment, and it is the first one whose
result will not be limited by Eden's null backend answering every GL call from
a table.

### The frame gate is NOT the blocker -- retracting a standing explanation

The render entry was disassembled from raw machine words (0x12aea7c), and the
gate is exactly this:

```
012aeaac  ldr  w1, [x0, #0x4a8]    ; w
012aeab0  ldr  w8, [x0, #0x4b0]    ; w2
012aeac0  cmp  w1, w8
012aeac4  b.ne #0x12aead8          ; w != w2 -> change arm
012aeac8  ldr  w8, [x19, #0x4ac]   ; h
012aeacc  ldr  w9, [x19, #0x4b4]   ; h2
012aead0  cmp  w8, w9
012aead4  b.eq #0x12aeb20          ; h == h2 -> no-change arm
012aead8  ldrb w8, [x19, #0x4d1]   ; hasFocus
012aeadc  cbz  w8, #0x12aeb20      ; no focus -> no-change arm
012aeaE0  ldr  w8, [x19, #0x4d4]   ; frameCounter
012aeaEC  str  w9, [x19, #0x4d4]   ; ++
012aeb20  add  x20, x19, #0x4d4    <== BOTH arms land here
012aeb28  ldr  w8, [x19, #0x4d8]   ; frameState, on both paths
```

**Both arms converge at 0x12aeb20, so the gate does not block the render body.**
`0x4d8` is read and written on the no-change path too. A comment in
`guest_view_probe.cpp` had claimed for some time that "`+0x4d1` stuck at 0 means
the frame FSM at `+0x4d8` never advances". That was wrong, and it is now
corrected in place with the disassembly as the evidence.

Two further facts make it wrong rather than merely incomplete:

- **The condition is OR, not AND.** The old comment said "BOTH pairs differ";
  `b.ne` on w and `b.eq` on h give `(w != w2) || (h != h2)`.
- **The surface-changed handler writes BOTH pairs**, at 0x12ae72c/0x12ae730:
  `str w21,[x20,#0x4b0]` and `str w19,[x20,#0x4b4]`. So after any
  `onViewChangedJNI` the two pairs are equal by construction. `0x4d4` is a
  resize debounce counter, exactly as on a real device -- not a per-frame tick.

The `onWindowFocusChangedJNI` call stays: it is part of the lifecycle, the
guest reads `+0x4d1`, and the log proves the byte moved (`flag4d1=1`). It is
simply not the reason the guest stops at the splash. **That reason is still
unknown.**

### Two search-predicate bugs, both caught by a control

Scanning `.text` for the stores to those four fields produced 0 hits first, and
0 hits is exactly the result that gets mistaken for "never happens". Two
separate defects were in the predicate:

1. `(w & 0xFFC00000) != 0xFFC00000` -- the mask was written where the expected
   value goes, so it demanded bits 31:22 all be set. Correct form:
   `(w & MASK) != EXPECT` with `MASK=0xFFC00000`, `EXPECT=0xB9000000`.
2. `imm12` looked wrong by hand (0x135 read as 0xE5C) when it is in fact
   `(w>>10)&0xfff`, and the *offset* is `imm12*4` for a 32-bit store. My
   arithmetic was wrong, not the encoding.

The control was the thing that worked: after the fix, scanning for `0x4d4`
returns 4 sites and **contains the known instruction at 0x12aeaec**. A search
with no built-in known-true positive is not a measurement.

### Two more nulls

- **Asset completeness.** The depot archive has 50,608 entries, which looked
  like 8,067 files short of the 42,541 deployed. It is not: 8,067 of those
  entries are directory records. Files deployed 42,541 / 42,541. The raw entry
  count was the only thing that looked wrong.
- **Asset/render race.** The last successful `mmap-file-ok` is ordinal 124 and
  `render-call-pre` is ordinal 285, so all mapping is finished long before the
  render call. The render is not walking a list that is still being appended to.

---

## 2026-10-04 (evening) — the real predicate, and four more null results

### What "failed getting full path" actually means

Followed the function's epilogue instead of its entry, which is the only reason
this got answered. The resolver 0x12bf4dc returns

```
0x12bf768  bl   0x12a4b60
0x12bf78c  and  w0, w0, #1     <- the return value
```

and 0x12a4b60 is a `stat` plus a type check:

```
0x12a4c08  bl  0x1e18d10       <- the `stat` PLT slot (GOT 0x1f7f198 -> "stat")
0x12a4c10  bl  0x1e18b10       <- __errno
0x12a4c18  cmp w1, #2          <- ENOENT
0x12a4c1c  b.eq fail           <- w20 = 0
ok: 0x12a4c44  and w8,w8,#0xf000 ; cmp w8,#4,lsl #12   <- S_IFREG required
```

**So the message means "the composed path stat'd ENOENT", and the guest DOES come
to the host for it.** The earlier claim that it "never asked us" was wrong --
`logWatchedResult` only records the watched path classes, so every ordinary
content asset was invisible. With a `stat-path` probe the whole set appeared:
**1261 stats, 1133 ENOENT**, 438 of them `.dxt.dds` / `.etc.dds` / `.ptc.pvr`
(texture encodings for Tegra/Adreno/PowerVR) and 654 `.z` variants. The donor
ships none of the ETC/PTC forms, and the deployment is 42,541/42,541 files.

### Four refutations, each with the measurement that killed it

1. **"The content root is empty."** It is `0x01`, sampled during the view probe
   while the failures were happening. The object at `*(0x1f78d30)` is
   `[01 00..00 | ptr | ptr | ptr]` -- a leading flag, not a string. Reading
   those bytes as a NUL-terminated string and calling the result "empty" was
   the mistake; `ldrb` is what the resolver uses, and it is non-zero.
2. **"The gate blocks the resolver."** It does not; `cbnz` is taken.
3. **"The frame counter gate (`+0x4d4`) is the blocker."** Both arms converge at
   0x12aeb20 and `+0x4d8` is touched either way, so the gate does not block the
   render body. It is a resize debounce, not a per-frame tick.
4. **"The zero-byte stubs cause `Corrupted JSON config`."** Null result:
   hiding all five from the guest moved NOTHING.

| | stubs visible | stubs hidden |
|---|---:|---:|
| Corrupted JSON config | 1 | 1 |
| startMusic disabled | 2 | 2 |
| LoadReadOnlyMappedFile | 6 | 6 |
| stat ENOENT | 1133 | 1133 |
| mmap ok | 124 | 124 |

Every counter identical. Hiding the files must have converted any access into
ENOENT and the ENOENT count did not move, so the guest was not stat-ing them at
all: the original comment's claim that the guest tolerates them being absent is
CORRECT. Reverted to the original behaviour, with the null result recorded at
the switch.

### The cleanup deleted the evidence archive I was reading

`20261004-171852-statpath` was in the cull set, and it was the archive holding
the `Corrupted JSON config` capture. Re-ran and rebuilt it as
`20261004-174528-restore-evidence`; every line reproduces. Worth recording:
a cull list is safe for what it lists, and "an archive I am actively reading"
is not a category that survives a size-based rule.

### What is still unknown, and why it needs the console

The guest sets its "downloading" state at log line 114, before any stat (552)
and before any asset failure (4604), and the reason is still unidentified. The
two available discriminators are the guest's PC and the guest's own read of the
file it is parsing as JSON -- and Eden supplies neither (`svcGetThreadContext3`
returns the caller's registers, and the null backend answers every GL call from
a table). The real-hardware run is the next experiment, not another round of
reverse engineering.

### The JSON config is not a file, and not ours to return

With the stub A/B reverted (RR3_TOUCH_STUBS=1) and the fopen probe uncapped
(80 -> 1200) and now recording the size of everything it opens, the guest's
complete open sequence before "Bad config. Corrupted JSON config" is three
files, all zero bytes:

```
1. size=0  sdmc:/switch/rr3-native-rehost/.nomedia
2. size=0  sdmc:/switch/rr3-native-rehost/doc/telemetry_optin.bin
3. size=0  sdmc:/switch/rr3-native-rehost/doc/telemetry_opt_in.bin
```

Archive 20261004-181653-opensize-clean, complaint at line 105.

Files 2 and 3 are the zero-byte stubs, and the A/B already showed that hiding
them changes nothing (every counter identical). File 1 is an Android marker,
not JSON. **So the config the guest fails to parse is not read from any file we
serve, and not from `GetStringValueFromRemoteConfig` either** (archive
20261004-181329-remotecfg: returning "{}" moved nothing).

What survives: the schema is fully recovered (93 `csdk.*` keys, from a
`csdk\.[A-Za-z0-9_.]+` scan of the image, with a complete `disabled.*` family
that is the honest offline answer), the key names sit immediately next to the
error string in .rodata, and the failing call is at 0x15ea738 inside
`[0x15ea088, 0x15eec08)`. Following that further needs the guest's PC, and
Eden's `svcGetThreadContext3` returns the caller's own registers. That is the
end of what this emulator can answer.

### Machine state at the end of the session

- build exit 0, input preflight 11/11
- NRO 52,097,525 B, sha256 6BCC752F3391203C
- `RR3_TOUCH_STUBS` 1 (single definition, top of guest_filesystem.cpp)
- `RR3_FRAME_LOOP_RENDER` 0, `RR3_FRAME_LOOP_PUMP` 1
- `kStatBionicMode` 2
- Eden: unmapped 15 reads / 0 writes, no fatal dialog, 124 file mappings,
  92 shaders, asset singleton PRESENT
- Workspace: 33.4 GiB removed, 21/21 retained romfs verified by hash

### Three consecutive null results: the guest's init failure is not ours

"Bad config. Corrupted JSON config" is followed, three lines later, by
"startMusic disabled during download", which is the state that holds the
splash. Three host-side hypotheses were tested, each with its criteria written
down before the run, and each came back with every counter unchanged:

| hypothesis | change | result |
|---|---|---|
| the zero-byte config stubs are parsed as JSON | hide all five from every path API | every counter identical |
| `GetStringValueFromRemoteConfig` supplies it | return `"{}"` instead of `""` | every counter identical |
| the shared `&g_fakeObjectObject` sentinel stalls the guest | return `nullptr` from the object catch-all | every counter identical |

The third one had real precedent behind it -- `getComponent` returns nullptr
precisely because "returning a non-null fake made Java call methods on a
sentinel and stall cloudcell init" -- and the precedent did **not** generalise.
That is worth writing down: a fix that was correct for one method is not
evidence about the catch-all that covers thirty.

With `fopen` uncapped and now recording sizes, the guest's entire open sequence
before the complaint is three zero-byte files, so the config is not read from
anything we serve either. What is left is a conclusion, not a hypothesis:

**the guest's "downloading" state and its config parse failure are internal to
the guest.** Nothing the host returns has been shown to cause them, and three
targeted attempts to change what the host returns did not move a single
counter.

The two remaining discriminators are the guest's own program counter and what
real hardware draws. Eden supplies neither: `svcGetThreadContext3` hands back
the caller's registers, and the null backend answers every GL call from a
table. That is the boundary, and crossing it needs the console.

### The guest's "downloading" state, located exactly

`startMusic disabled during download` (string 0x3902ff) has one xref, at
0x12ad320, inside FDE `[0x12ad2e0, 0x12ad3b0)`. The whole function is the
decision:

```
012ad2ec  adrp x8, #0x1f6d000
012ad2f0  ldr  x8, [x8, #0x5d8]     ; engine = *(0x1f6d5d8)
012ad2f4  ldr  x8, [x8]              ; x8 = *engine        <-- EXTRA LEVEL
012ad2f8  cbz  x8, #0x12ad318        ; *engine null        -> log "downloading"   [br 1]
012ad2fc  mov  w9, #0x518
012ad300  movk w9, #1, lsl #16       ; w9 = 0x10518
012ad304  ldr  x8, [x8, x9]          ; x8 = (*engine)[0x10518]
012ad308  cbz  x8, #0x12ad318        ; that null           -> log "downloading"   [br 2]
012ad30c  ldp  x8, x9, [x8, #0xd8]   ; begin/end of a container
012ad310  cmp  x9, x8
012ad314  b.eq #0x12ad32c            ; equal (empty)       -> skip the log
012ad318  ...                            ; unequal            -> log "downloading"   [br 3]
```

### RETRACTED 2026-10-05: "the engine is alive, `engine[0x10518]` is null"

The disassembly above was transcribed correctly here and still is. **What was wrong
was the probe that measured it, and therefore the conclusion drawn from it.**

`guest_view_probe.cpp` read:

```cpp
dlEngine = *(bias + 0x1F6D5D8);
dlVecPtr = *(dlEngine + 0x10518);        // <-- one level too high
```

The guest applies 0x10518 to `*dlEngine`, not to `dlEngine`. So the recorded

```
eng=0x2108b0b688  ptr=0x0  begin=0 end=0  empty=1
```

describes the word at `0x2108b0b688 + 0x10518` -- an address the guest never
reads. **`*0x2108b0b688`, the value the guest actually branches on, had never
been read by anything in the port.** The branch that fires was therefore
unknown, and "the field is null, so branch 2" was an artefact of the wrong
address, not a measurement.

This is the same class of error as the four earlier transcriptions in this
file, and it is the second time a comment that *read* correctly was paired
with code that computed something else. The listing was never the problem.

Root cause of the confusion: the earlier note wrote the instruction as
`ldr x8,[x8,#0x10518]`, which **cannot be encoded** -- an unsigned-offset LDR
carries imm12 scaled by 8 and reaches only 0..0x7FE8. An unencodable
instruction cannot be cross-checked against the assembler's opinion, so the
error survived; the real `mov w9 / movk w9 / ldr [x8, x9]` triple has the
`ldr x8,[x8]` above it in plain sight.

The probe now follows the disassembly exactly, reports `first=`, a +-0x18
neighbourhood, and a single branch verdict token `DLBR=` in {0,1,2,3}
(0 = reaches the OK path at 0x12ad32c), with `?` plus a guard number if a
plausibility check skipped a read. **The `62 reads / 0 writes` result below
is about the offset 0x10518 and is unaffected** -- but "find the writer of
0x10518" is not yet known to be the right question, because the base it is
added to was never confirmed to be the right object.

So the chain is now closed end to end, and the expensive part of it is gone:

```
Bad config. Corrupted JSON config          (log line 107)
  -> the config-driven subsystem never initialises
  -> engine + 0x10518 is never created
  -> 0x12ad2f8/0x12ad308 falls through to the log
  -> "startMusic disabled during download" (line 114)
  -> the splash is held
```

**This also explains the three null results.** A missing internal subsystem
cannot be repaired by changing what the host returns: the zero-byte stubs, the
remote-config string and the shared sentinel all feed the config, and the
config's failure is upstream of anything they can influence.

What is left to find is the writer of `engine+0x10518`, and it is NOT reachable
by scanning for the offset as an immediate -- both known sites build it in a
register with `mov`+`movk`. A search for the immediate form returns 0 reads and
0 writes, which looks like "nobody touches it" and is the same false negative
that has bitten this file four times today. The register-built form has to be
searched, and the offset 0x10518 is 1 MB into the engine object, so it is a
late-initialisation subsystem rather than something the boot path builds.

### Who writes engine+0x10518: nothing found, and that is surprising

Scanning `.text` for `str/ldr [Xn, #0x10518]` returns **zero** hits and looks
conclusive. It is false: a field 1 MB into a large object cannot be addressed by
a 12-bit scaled immediate, so it goes through a register built as

```
mov  w9, #0x518
movk w9, #1, lsl #16        ; w9 = 0x10518
ldr  x8, [x8, x9]
```

`tools/guest-analysis/find-offset.py` finds that form, and it **takes a
`--control` address that the scan must return or it aborts**, because this exact
"nothing touches it" result had already been produced twice tonight by broken
predicates (a mask written where the expected value goes, and a MOVK shift field
read as #32 when 1 means #16).

With the control passing:

| module | access sites | store sites |
|---|---:|---:|
| libRealRacing3.so | 62 | **0** |
| the other 13 .so | 0 | 0 |

**`engine+0x10518` is read 62 times and never written, anywhere in the image
set.** The store search used a 700-instruction window (a whole function, since
the register is usually hoisted far above its use) and matched both `[base]` and
`[base, index]` addressing.

### RESOLVED 2026-10-05: the writer exists, and that search could never have found it

**`0x140461c` writes it.** Hand-read, FDE `[0x14045e0, 0x140475c)`:

```
14045f0  mov  w8, #0x508
14045f4  movk w8, #1, lsl #16      ; w8 = 0x10508
14045f8  mov  x19, x0               ; x19 = this
14045fc  add  x21, x0, w8           ; x21 = this + 0x10508
1404600  mov  w0, #0x210
1404604  bl   0x1e17b40             ; operator new(528)
1404610  bl   0xbe2294              ; ctor
140461c  str  x20, [x21, #0x10]     ; *** *(this + 0x10518) = x20 ***
```

**The literal `0x10518` does not appear anywhere in this instruction stream.**
It is `0x10508` plus `#0x10`. A search that enumerates stores by the *offset
literal* is looking in the wrong space, not running a bad predicate. The same
function also stores to `[x21,#8]`, `[x21,#0x18]`, `[x21,#0x20]`, `[x21,#0x28]`
— offsets 0x10510, 0x10520, 0x10528, 0x10530 — so the whole sub-structure is
built here, in one place.

The runtime measurement agrees and localises the fault precisely: the
neighbourhood probe now reads, at `first + 0x10500 .. +0x10520`,

```
+0x10500 = 0        +0x10508 = 0x2102eb7c40   +0x10510 = 0
+0x10518 = 0        +0x10520 = 0
```

`+0x10508` holds a live heap pointer and `+0x10520` is readable, so the object
extends past `0x10518` and out-of-bounds is ruled out. The region is **not**
uniformly zero. And `str x20,[x21,#0x10]` is **unconditional** — so if
`0x14045e0` had run on this object, `+0x10518` could not be null. **It did not
run.**

### Two independent negatives, and a diagnostic the engine ships itself

`0x14045e0` carries its own check:

```
14046c0  adrp x8, #0x1f6d000 ; ldr x8,[x8,#0x7a8] ; ldr x19,[x8]
14046f4  ldr  w8, [x19, #8] ; cmp w8, #2 ; b.eq #0x1404718
1404700  ... "system_share.cpp:663"
1404704  ... "There has been a change in the loading order and ServerVariables
             is no longer initialised "
1404714  bl   #0x109f730                       ; FUN_0119f730(2, file:line, msg)
```

The engine's own authors recorded that this initialisation is order-dependent.
That string is **absent from every captured log** (`guest-view.log`,
`egl-probe.log`, the Eden tail), consistent with the field being null. Note the
two signals are not fully independent: a silent run would have set the field.

### Method notes, because two searches disagreed and both were wrong

- `find-offset.py` (offset-literal enumeration) cannot see a split store.
- `singleton-writers.py` route A (base tracking) reported `0xb3a1b4` for
  `+0x10518` — **a false positive**. That address is in a destructor
  (`ldp/str` relink plus `operator delete`). Root cause: the tool **never
  invalidates a tracked register on a load**, so a base register last written by
  an unmodelled `ldr` inherits a stale singleton-derived offset and the store is
  attributed to a phantom location.
- The control that actually settled it was not the tool's own Control A (which
  passed and only proves the one case it was designed for). It was demanding
  that route A **independently find the hand-read `0x140461c`**. It did not,
  which is what identified route A as incomplete rather than `0x140461c` as
  wrong.

### The call chain, and the one gate that matters

Traced upward from `0x14045e0`, each link verified by a scanner whose positive
control was `bl 0x1e17b40` at `0x1404604`:

```
0x12b3614  Java_com_firemint_realracing_MainActivity_onViewRenderJNI
  -> 0x12aea7c  (the render entry)
  -> 0x12aee50 / 0x12aeec0
  -> 0x12b43a0
  -> 0x12b4440
  -> 0x1405868
  -> 0x14048c8
  -> 0x14045e0     *** the constructor ***
```

So the subsystem that holds `+0x10518` is reached **from the JNI render entry
that this port already calls**. `0x14048c8` has two callers; the live one is
`0x1405894`, the first instruction after the prologue of `[0x1405868,0x1405d20)`.

`0x14048c8` has four gates before `0x1404994 bl 0x14045e0`:

```
A  this->0x83 must be 0                    (else return 1, no ctor)
B  this->0x82 must be 0                    (else virtual calls, return 0)
C  once flag byte at 0x1fb5330             (ldarb, ldarb -- thread-safe once)
D  0x1404ac0 must return 0, and it returns 0 only when
       bit 2 of [*(0x1fb5328) + 0x70] is set
```

`0x1fb5328` is a `std::async` shared state, and its ready signal is set by a
worker thread whose entry is `0x1406f94`.

**The thread census settles gates A/B/C without inference.** It reports the
worker's ELF address itself, derived independently of any of this analysis:

```
phase=guest-thread ordinal=3 target=000000210775ff94 start=0x210775ff94
  start_elfva=0x1406f94  start_img=libRealRacing3.so
  arg=0x21031b35c0  creator=0  host_owned=0
  lr=0x210775da3c  lr_elfva=0x1404a3c  lr_img=libRealRacing3.so
```

`start_elfva` matches the predicted entry exactly, a `guest-thread-start` line
exists for it, and **`lr_elfva=0x1404a3c` is inside `[0x14048c8,0x1404ac0)`** --
the saved link register points back into the function that called
`pthread_create`. The worker was therefore spawned from inside `0x14048c8`
itself, so gates A, B and C did not return early and the future exists.

This retracts the first static reading of the chain, which inferred from
"only 4 references to the two .bss addresses, all inside `0x14048c8`" that the
create branch was never taken. The worker thread is direct evidence against it.

**What is now open, precisely: does bit 2 of `[*(0x1fb5328)+0x70]` ever become
1?** A change-triggered probe reads `0x1fb5330`, `0x1fb5328` and `+0x70` every
frame for 600 frames and logs only on a transition. It is deliberately not on
the 4-frame schedule used for the long diagnostic line: a change-triggered log
consulted on 4 frames is just a 4-sample log wearing a disguise.

### MEASURED: gate D passes, at frame 0. It is not the blocker.

```
phase=gate-d: frame=0 once=1 fut=0x2102fa7d60 st70=0x5f43455000000007
  bit2=1 ctor-would-run=1
```

`once=1`, the shared state exists, and it stays at that value for all 600
frames -- the change trigger fires exactly once, which is the baseline sample.
`0x5f43455` is a memory tag (`_CE`); only the low 56 bits carry state.

The precise condition is not "bit 2" but `bic w19, 1, flags>>2` returning 0,
i.e. `flags & ~7 == 0`. With `flags == 7` that holds, so the poll returns 0 and
`0x1404994 bl 0x14045e0` is reachable.

So the async worker finished before the first frame was even sampled, and the
whole "the worker is too slow" family of explanations is dead. The block is
upstream: `0x14048c8` also gates on

```
A  ldrb w8,[x0,#0x83] ; cbz w8,#0x14048f8
B  ldrb w8,[x0,#0x82] ; cbz w8,#0x1404940
```

with `x0` being the object `0x1405868` was itself called with
(`0x140588c mov x19,x0`, and `x0` is untouched before the `bl` at `0x1405894`).
The caller then does `0x14058a4 add x23, x19, #0x10500 ; ldr x0,[x23]` -- the
same `+0x10500` neighbourhood this probe already samples -- and reads
`[x19,#0x85]` at `0x14058c8`.

`0x82` / `0x83` / `0x85` / `0x8d` are one flag cluster on one object, and
`0x8d` is the already-documented `AssetDownloadService` freeze byte whose
neighbours in the localisation key list are NO_WIFI / FREE_STORAGE /
ASSET_DOWNLOAD. That this is the same object is a **hypothesis**, so the probe
dumps all four bytes rather than betting the conclusion on the guess.

### RETRACTED 2026-10-05: "all four gates are open"

The first reading of those bytes was

```
ab[0x82=0 0x83=0 0x85=0 0x8d=0]  mgr=0x2107d1fe98   bit2=1  ctor-would-run=1
```

and the conclusion drawn from it -- gates A, B, C and D all pass, therefore
`bl 0x14045e0` executes, therefore the field cannot still be null, therefore
`this` is not the object the guest reads -- was **built on a bad object**.

A probe that also read the same offset on the candidate proved it:

```
mgr+10518 = 0x5f434550 000006761 -> 0x5f444f4d46006761
```

which in ASCII bytes is `ag \0 F M D _`. **That is string data, not a
pointer.** `mgr + 0x10518` is already past the end of the `mgr` allocation and
is landing on an adjacent heap string. The `mgr` object does not extend to
0x10518, so the `0x82` / `0x83` / `0x85` / `0x8d` bytes read from it are the
bytes of *some* object, and there is no evidence they are the gates.

**The status of gates A and B is unknown, not "open".** Only C and D are
measured, and only D is a real pass. The reason this matters is the shape of
the mistake: the falsifying datum (`0x5f444f4d46006761` is ASCII) came from the
*same object* and the *same offset* as the claim, one probe earlier, and the
claim was still stated with a "all four gates are open" header. **A byte that
reads as 0 is not evidence that a gate is satisfied until the object it came
from has been shown to be the object the gate is on.**

### `this` is one more dereference than either candidate

`0x1405868` is called from `[0x12b43a0,0x12b4448)`:

```
12b4428  ldr   x0, [x19]
12b442c  ldrb  w8, [x0, #0x88]
12b4430  cbz   w8, #0x12b443c
12b4438  ldr   x0, [x19]        ; reloaded after the 0x140395c call
12b443c  mov   w1, wzr
12b4440  bl    #0x1405868
```

So `this` is `*(x19)`, and `x19` in that FDE is the object `onViewRenderJNI`
hands to `0x12aea7c` -- already resolved in this file as
`readGuestActivitySnapshot(renderTarget).renderer`, whose `+0x4fc` is the word
the render loop's exit test waits to become 4. **That is one more dereference
than either earlier candidate, which is exactly the step that gets dropped when
a chain is written down from memory.** The probe now reads all three:

| candidate | expression | `+0x10518` |
|---|---|---|
| mgr | `*(0x1f6d5d0) + 0x10518` | `0x5f444f4d46006761` (ASCII, out of bounds) |
| eng | `*(*(0x1f6d5d8)) + 0x10518` | `0x0` |
| ren | `*(renderer) + 0x10518` | `0x0` |

### `ren` is the object the constructor was given -- and it is not the object the guest reads

```
ren  = 0x21084228a0
      +0x10508 = 0x210865f3f8   +0x10510 = 0x2106a803fa   +0x10518 = 0x0
eng  = 0x210308c2c0
      +0x10508 = 0x21030b7c40   +0x10510 = 0x0            +0x10518 = 0x0
```

`ren` is the only candidate whose neighbourhood looks like a live object: two
consecutive populated slots, exactly the offsets `0x14045e0` writes
(`[x21,#8]`, `[x21,#0x10]`, `[x21,#0x18]`, ... with `x21 = this+0x10508`).
`mgr` is out of bounds at that offset, `eng` has one populated slot and a hole.

**But `+0x10518` is 0 on every candidate**, and in `0x14045e0` that store is the
first one and is unconditional. So the constructor did not run on `ren` either.

Two facts now sit side by side and both are measured:

1. The thing that consults `+0x10518` -- `0x12ad2e0`, the "startMusic disabled
   during download" decision -- reads `*(*(0x1f6d5d8))`, i.e. `eng`.
2. The only writer of `+0x10518` in the image -- `0x140461c` -- runs on `ren`.

They are different objects, and that is the actual shape of the bug: the field
the guest tests and the field the engine populates are not the same field,
unless `0x1405868` is reached with an `x19` that is *not* the render object on
some other path. That possibility is not excluded yet, which is why the gate
bytes are now read on `ren` and `eng` rather than on `mgr`.

Gate status, stated precisely:

| gate | condition | status |
|---|---|---|
| A | `this->0x83 == 0` | **FAILS** -- `ren->0x83 = 0x08` |
| B | `this->0x82 == 0` | **FAILS** -- `ren->0x82 = 0x22` |
| C | once byte `0x1fb5330` | open, measured `1` |
| D | bit 2 of `[*(0x1fb5328)+0x70]` | open, measured, `flags == 7` |

### RETRACTED 2026-10-05: "gates A and B are closed by a flag byte"

The measurement below is sound:

```
ren  = 0x21082228a0
renAB[ 0x82 = 0x22   0x83 = 0x08   0x85 = 0x00   0x88 = 0x40 ]
```

The **conclusion drawn from it was not.** Reading the whole body of `0x14048c8`
shows those two bytes are latches meaning *already done*, not blockers:

```
14048e8  ldrb w8,[x0,#0x83] ; cbz w8,#0x14048f8
14048f0  mov  w0,#1 ; b #0x140496c        ; +0x83 != 0  -> RETURN 1, stop
14048f8  ldrb w8,[x0,#0x82] ; mov x19,x0 ; cbz w8,#0x1404940
                                        ; +0x82 != 0  -> do the virtual-call work
1404928  tbz  w0,#0,#0x1404968
1404930  bl   0x140475c
1404934  mov  w0,#1
1404938  strb w0,[x19,#0x83]              ; +0x83 := 1   ("async load kicked off")
1404940  ldarb w8,[0x1fb5330] ; tbz w8,#0,#0x14049a4
1404960  bl   0x1404ac0 ; cbz w0,#0x1404990
1404990  mov  x0,x19
1404994  bl   0x14045e0                  ; *** the constructor ***
1404998  mov  w8,#1
140499c  strb w8,[x19,#0x82]              ; +0x82 := 1   ("constructor finished")
14049a0  b    0x1404904                   ; ...and fall into the +0x82 handling
```

**`+0x82 == 0` is the "not built yet, go build it" state.** A non-zero value is
the engine correctly reporting that it already did this work. So "gates A and B
are closed" inverts the meaning, and the earlier wording -- which called the
same bytes "the download-blocked state that suppresses service init" -- was
wrong for the same reason.

The real contradiction is sharper than that, and is stated here instead:

> the object's latches say the subsystem is built, and the only store in the
> whole image that populates that subsystem is inside the constructor those
> latches are supposed to follow. Both cannot be true.

And one thing the latch code settles for free: `0x140499c` and `0x1404938`
store the **literal 1**, so they cannot be the source of `0x22` / `0x08`. Those
values came from somewhere else, and the most likely candidate is a single
32-bit store covering `+0x80..+0x83` -- little-endian, byte 2 = 0x22 and byte 3
= 0x08 means the word is `0x08a22948`.

### MEASURED: `ren+0x80` is a flag word, and the first probe misread one byte of it

```
w80a = 0x08a22948      w80b = 0x08a22948
bytes[80=72  81=41  82=162  83=8  84=33  85=0  88=40]
```

Read twice, in opposite order (word-then-bytes, then bytes-then-word), the two
word reads **agree exactly**. Decoded little-endian they give
`0x48=72 ✓`, `0x29=41 ✓`, **`0x22=34`** , `0x08=8 ✓` -- three of the four byte
reads in the same snapshot match the word. So `ren[0x82] = 0x22` and the
`162` is the faulty one.

Why that matters beyond the byte: `+0x82` read 34 in one run, 130 in the next
and 162 in the third. `0x22`, `0x82`, `0xa2` share their low five bits and
differ only in the top three. A single-byte read of the one byte that is
exactly the flag the latch tests, disagreeing with the bracketed word both
times, is a bad read path -- not a torn read, which would have made `w80a` and
`w80b` differ.

**`+0x80` is a flag word, not a pointer.** The evidence is the value itself: two
runs produced `0x08a22948` and `0x08822948`, differing in a single nibble. The
low 32 bits of a heap pointer do not stay that stable across runs; a bitfield
does. The guest only ever `ldrb`s bytes 2 and 3 of it, so the whole word is
irrelevant to it, but the writer is a 32-bit store, and the latch `strb`s --
which write 1 -- cannot be it.

The open question is now one instruction: **what stores `0x08a22948` into
`ren+0x80`?** It is the only thing standing between the render object and the
constructor that populates `+0x10518`, and until it is known, nothing may be
written to that object.

### ANSWERED 2026-10-05: `+0x80` is a 64-bit clock, and the "latches" are its bytes

`0x12aecb8 str x0,[x19,#0x80]`, inside the render entry's own FDE
`[0x12aea7c,0x12af358)`, stores the return of `bl 0x10b2f5c`, which is

```
adrp/ldr [0x1f6d000+0x5d8] ; ldr x0,[x8] ; ldr x1,[x0] ; ldr x1,[x8,#0x38] ; br x1
```

-- a **virtual time getter on the engine singleton**. The field is read back at
`0x12aec9c ldr x8,[x19,#0x80]` and used as `sub w8,w0,w8` followed by
`csel w21,0x10,w8,lt`, i.e. a **clamped frame delta**. Nearby literal:
`src/ndPlatform/ndAndroid/Activity.cpp`.

So `+0x80` is a live 64-bit timestamp, and bytes 2 and 3 are clock bits 16..31.
`0x22` and `0x08` are the low end of a time value, **not flags**.

Which means the byte the guest tests as a boolean cannot be a clock byte:

```
0x14048e8  ldrb w8, [x0, #0x83] ; cbz w8, #0x14048f8
```

No engine tests bit 24 of its own frame clock. **Therefore the object `x0` in
`0x14048c8` is not the object whose `+0x80` is this clock** -- the object the
sampling probe has been reading all along is the wrong one, and the "latch
values 0x22 / 0x08" were clock bits. That also explains why they drifted
between runs (34 / 130 / 162: same low five bits, varying top three -- a moving
counter, not a stable flag) and why two consecutive runs produced words
differing in one nibble.

The chain says `x0` should be `*(x19)`, where `x19` is what
`onViewRenderJNI` hands to `0x12aea7c`. The render entry's own `x19` is a
**different** object: it is the one carrying the clock. The port resolves the
former as `readGuestActivitySnapshot(renderTarget).renderer` and the latter as
`*(...)renderer`, and the two were treated as one object. They are not.

Decisive measurement, cheap, and now the only thing standing: dump the full 16
bytes at `+0x80` on **both** candidates in one run. A monotonic clock has a
large low word and a high byte of `0x00` or `0x7f`; a flag object does not.
The probe now emits `ren+80u64` and `rend+80u64` side by side, plus bytes
0x80..0x88 of `ren`, and logs on change.

**Nothing may be written to either object until this lands.**

### The object identity IS settled, and it exposes a real bug in this port

`[0x12b43a0,0x12b4448)` opens with

```
12b43ac  ldrb w8, [x0, #0xa]      ; <-- this IS gate F
12b43b0  cbz  w8, #0x12b43e0
12b43b8  mov  x19, x0
12b4428  ldr  x0, [x19]
12b4440  bl   0x1405868
```

The probe read `renderer[0xa] = 0x4b`, non-zero, which is exactly what lets this
function proceed. So the object the probe has been calling `renderer` is the
one the guest enters, `this = *(renderer) = ren`, and gate A really does
`ldrb [ren+0x83]`.

Which leaves `ren+0x80 = 0x2108022948 = ren+0xa8` -- a self-referential pointer
to an embedded struct whose byte 3 the guest tests as a boolean. The only
layout that makes both true is a **union**: the same eight bytes are either a
pointer or a group of flag bytes. Which member is live has never been
established, and the latch reads only make sense with the flag member.

### Second onViewRenderJNI call: it does not wedge early, it wedges in a JNI call

`RR3_FRAME_LOOP_RENDER = 0` means the per-frame tree at `0x12b4434` /
`0x12b4440` is entered exactly once. So one extra call at frame 30 was added --
deliberately NOT gated on that switch, since it is a different claim and has to
be testable on its own. Acceptance was fixed first: `ren+0x10518` becomes
non-zero, or the latch bytes move, or the call hangs.

It hangs -- but not where expected, and not early. Inside that one call the
guest:

```
guest-thread  ordinal=5  start_elfva=0x1c699d0
guestStat  doc//sv.2.dat   -> ENOENT
guestStat  doc//sv.2.dat   -> size=109
guest-log  "Loading Settings [vars] from [sv.2.dat]"      <-- first appearance
guestFopen doc/sv.2.dat (109)   .depot/sv.2.dat.bak (0)
guestStat  doc//sv.dat      -> ENOENT
guestRemove doc/\sv.dat.bak
map-app   files/.depot/CC_SeedData.bin, files/.depot/hbn, files/apk/res/hbn
```

and the latch bytes and `+0x10518` never moved. So the second entry gets
through settings load and asset mapping, and the subsystem init is still
skipped.

**The wedge point is now known exactly, and it is in this port's code, not the
guest's.** The census attributes the main thread's last import to
`jni:CallStaticObjectMethod` and reports

```
census-ring-tail  lr=0x2106d01474  img=libRealRacing3.so  off=0xba8474
census-pc         pc=0x808b8600    pc_img=host-nro
census-ord ordinal=0  last=jni:CallStaticObjectMethod  inbridge=0  enters=1199  idle_ms=14455
```

`0xba8474` is a **guest address**, recovered from the LR ring -- which is the
thing this project has repeatedly recorded as unavailable. It is the return
address of

```
0xba8458  ldr  x8, [x0]
0xba8468  ldr  x8, [x8, #0x398]     ; vtable slot 0x73
0xba8470  blr  x8                   ; never returns
0xba8474  ldr  x8, [x19, #0x28]     ; stack canary
```

in FDE `[0xba83fc, 0xba8498)`, whose prologue spills `q0..q7` and builds the
`mov x8,#-0x28 / movk x8,#0xff80,lsl #32 / stp x9,x8` alignment sequence -- the
standard AAPCS64 varargs register save area, i.e. a JNI `va_list`.

**The guest calls virtual slot 0x73 of an object, that lands in this port's JNI
bridge, and the bridge never returns.** That is a concrete, addressable defect
on our side, and it is the next thing to fix. It also supersedes the earlier
note that the render re-entry "hangs somewhere unknown": the hang has a guest
address, a vtable slot, and a host frame.

### Slot 0x73 is `CallStaticObjectMethodV`, and the instrument was eating the name

`0x398 / 8 = 115`, and JNI table index 115 is `CallStaticObjectMethodV`. The
port's own table is correct -- `95 GetObjectField`, `113 GetStaticMethodID`,
`114 CallStaticObjectMethod`, `115 CallStaticObjectMethodV` all match the spec
-- so there is no off-by-one to fix. The census label `jni:CallStaticObjectMethod`
is the name `jniCallStaticObjectMethodV` records for itself, minus the `V`.

**But the name of the wedged call was not in the log, and the reason was our own
instrument:**

```
phase=per-call-log-cap site=jni-static-obj suppressed_total=1 emitted_total=64
```

**Exactly one suppressed call, and it was the one that hung.** The exit log is
written after the dispatch, so a call that never returns writes nothing, and the
shared 64-write budget had just been exhausted. This is the second time this
project has lost a diagnosis to a log budget -- the first was the 200-line guest
log cap raised to 4000 on 2026-10-04. Same bug, different counter. The general
rule: **a log for a call that can wedge has to be written on entry.** An exit
log is not a weaker version of an entry log, it is the wrong one.

So `jniCallStaticObjectMethodV` now writes the method name **on entry** from a
dedicated 4096-write budget that is not shared with the exit log, and announces
exhaustion on a line of its own (`jni-static-enter-CAPPED`) instead of dropping
the name silently. Re-run:

```
jni-static-enter method=getBuildManufacturer  returns=string
jni-static-enter method=getScreenDensityName  returns=string
jni-static-enter method=getScreenSizeName     returns=string
jni-static-enter method=getSupportedAbis       returns=string
jni-static-enter method=getCpuName             returns=string   <-- last
```

An `android.os.Build` field enumeration, wedging on the fifth field. The method
IDs are spaced 0xd8 apart, so they are entries in one table the port allocates.

`getCpuName` itself is a plain string in `platformStringForMethod`
(`return "ARM Cortex-A57"`, jni_bridge.cpp:1230) with no loop, and no further
`jni-static-enter` follows it -- so the block is either inside that call or in
guest code that makes no further JNI static call at all. **The port's own
`getCpuName` handling is therefore a suspect, not a conviction**, and an exit
marker written from a second dedicated budget was added to tell those apart: a
`jni-static-exit` for `getCpuName` means the call returned and the block is
downstream guest code; its absence means the call is the block.

### CORRECTION 2026-10-05: the ring was read as a set, not as a sequence

An earlier line in this file said the guest's last observed action was
`close(fd)` at `0x12a4b48`. **That came from `Sort-Object -Unique` over the ring
tail, which produces a SET.** The ring is a sequence with an index `i`, and the
largest `i` is the newest entry. Read in order:

```
i=500  close                       libRealRacing3.so  0x12a4b4c
i=501  pthread_mutex_lock          libc++_shared.so   0xe3934
i=502  pthread_mutex_unlock        libc++_shared.so   0xe39c4
i=503  jni:CallStaticObjectMethod  libRealRacing3.so  0xba8474
i=504  pthread_mutex_lock          libc++_shared.so   0xe3a80
i=505  pthread_mutex_unlock        libc++_shared.so   0xe3a98
i=506 .. i=511  jni:CallStaticObjectMethod  libRealRacing3.so  0xba8474
```

So `close` is the 500th, not the last; **eleven imports follow it**, and the last
six are all `CallStaticObjectMethod` from one site, `0xba8474` -- the
`android.os.Build` field sweep that ends at `getCpuName`. The entry/exit pairs
show all six returned. **Zero imports after i=511.**

This is the same error as reading a bounded scan as a bound: both take a set of
observations and read a history into it. `Sort-Object -Unique` is the right tool
for "which symbols appear", and the wrong one for "what happened last".

The four `libc++_shared.so` entries are new information rather than a
correction: the guest's own `std::mutex` goes through **the libc++ this port
ships**, so guest-side C++ locking is visible as an import at `0xe39xx`. Both
lock/unlock pairs completed.

### The import feed is already complete, and the added JNI instrumentation is redundant

`censusNoteImportFrom` is declared in `symbol_resolver.cpp` -- the byte-frozen
import-stub generator. Every guest import is reported there, which is why the
ring carries `close`, `pthread_mutex_lock` and the libc++ entries without
anything in `jni_bridge.cpp` having called it. The eight accessors and
`guestStat` added earlier in this session were therefore **redundant with a feed
that already existed**; they did not change any conclusion, and the only thing
they added was per-site names in the log. The conclusion they seemed to support
-- "the guest stops calling back" -- was already available from the ring.

Keeping them is still correct: a per-call name is cheaper to read than a ring
index, and the entry/exit pair is what exonerated the JNI bridge. But the honest
description is "a second, more legible view of an existing complete feed", not
"the blind spot was fixed".

### WHERE IT ACTUALLY STOPS: device fingerprint complete, Cloudcell slots called, then silence

A 10-minute run (`20261005-033155-longrun`) reaches the **same** stopping point as
a 3-minute one -- so it is blocked, not slow. The last guest log lines are the
completed device-info block:

```
DeviceInfo::CPU Core Count: 4
DeviceInfo::CPU Speed: 1.02
DeviceInfo::CPU Chipset: ARM Cortex-A57
DeviceInfo::SDK Version: 35
DeviceInfo::Supported Abis: arm64-v8a
DeviceInfo::Tier: AndroidHigh          <-- last guest log line, both runs
```

Before that block, the eight `CallStaticObjectMethod` calls from one site
`0xba8474` are the fingerprint sweep, and the **first of them is our own API**:

```
getCloudcellEnvironment   (jni_bridge.cpp:1254, returns "switch")
getModelName  getBuildDevice  getBuildManufacturer
getScreenDensityName  getScreenSizeName  getSupportedAbis  getCpuName
```

All eight return. `Cloudcell` log activity is 98 lines before the sweep and
**zero after** -- so Cloudcell is not what blocks the sweep.

After the block, the guest calls our Cloudcell vtable stubs and then stops:

```
cc-slot: slot=5  self=81695160  caller=0x210782b1f0 -> guest 0xcd21f0
cc-slot: slot=22 self=81695160  caller=0x210782b24c -> guest 0xcd224c
cc-slot: slot=20 self=81695160  caller=0x210782b828 -> guest 0xcd2828
cc-slot: slot=5  self=81695160  caller=0x210880e758 -> guest 0x1cb5758
```

(bias 0x2106b59000, derived from a ring entry's `lr` minus its `off`.)

**`0xcd21f0`-`0xcd2828` is inside FDE `[0xcd1014, 0xcd3648)` -- the function that
raises `GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE` at `0xcd35c0`.** The
"mid-download" symptom, the `+0x10518` NULL, and this silent stop are one
story: the engine fails to load game data, takes the error path, reports through
the Cloudcell stubs, and never comes back.

**Not fixed here, and correctly so:** the port does not fake a Cloudcell
response, fabricate telemetry, or bypass any integrity check. The honest
offline action available is the same class as `RR3_NET_CONNECTIVITY = 0` -- make
Cloudcell report *unavailable* so the engine takes its documented offline branch
instead of waiting on a callback that will never arrive. Which of slots
5 / 20 / 22 the engine is waiting on is the next measurement, and it must be
made before any of them is changed.

### Every file the guest reaches for at the end is present -- verified, not assumed

The last `map-app` lines before the silence are the guest hunting for a small
file called `hbn`:

```
map-app    rr3core:/files/.depot/CC_SeedData.bin
map-app    rr3core:/files/.depot/hbn
map-app    rr3core:/files/apk/res/hbn
stat-path  rc=-1 errno=2  guest='.../.depot/hbn'  primary='rr3core:/files/.depot/hbn'
```

Read out of the deployed `core.romfs` with `romfs-read.py`:

| path | result |
|---|---|
| `files/.depot/hbn` | **not found** -- exactly the errno=2 the guest logged |
| `files/apk/res/hbn` | **found, 4 bytes**, contents `61 35 32 37` = `"a527"` |
| `files/.depot/CC_SeedData.bin` | **found, 296,657 bytes**, header `4e6f6d4e6f6d` = `"NomNom"` and `7272335f6175746f6d617469635f636f6e73742e6a73` = `"rr3_authomatic_const.js"` |

So the Cloudcell seed payload **is** shipped with the game and **is** deployed
here, `hbn` is a 4-byte build token and is deployed, and the guest tried both
paths and found the one that exists. **The silence is not a missing-file
problem.** That closes the last cheap hypothesis available from the captured
data.

### RETRACTED, and it was the load-bearing one: "Eden cannot observe the guest"

The self-test `sampler-selftest-b` reports

```
pauseRes=0x0 ctxRes=0x0 resumeRes=0x0 pc=0x80cf8a18 lr=0x807c8d44
```

and this file concluded that `svcGetThreadContext3` returns **the reader's own
registers**, because `pc` is a host-NRO address. **That inference does not hold:
the sampled thread can itself be executing host code** (it is inside this
port's bridge), so "host address" does not imply "mine".

**Classifying every sampled PC in the 10-minute run settles it:**

```
94 samples with a pc
  8 distinct values
  8 of them in the guest-image range
    5x  0x210782bad0   ->  guest 0xcd2ad0
    3x  0x210782babc   ->  guest 0xcd1abc   (0xcd2abc with this run's bias)
```

**When the context read succeeds, the value IS a guest address.** Real hardware
is not required, and the whole "only a device backtrace can resolve this" chain of
reasoning above is void.

### THE SPIN, located: a container destructor walking a chain that never ends

```
0xcd2abc  cmp  x20, x23              ; sampled 5x
0xcd2ac0  mov  x21, x23
0xcd2ac4  b.eq #0xcd2b00             ; exit ONLY when cursor == x20
0xcd2ac8  ldurb w8, [x21, #-0x18]    ; "already released" flag
0xcd2acc  tbnz w8, #0, #0xcd2ae0
0xcd2ad0  ldurb w8, [x21, #-0x30]    ; sampled 3x
0xcd2ad4  sub  x23, x21, #0x30       ; cursor steps back one node
0xcd2ad8  tbz  w8, #0, #0xcd2abc     ; flag clear -> loop
0xcd2ae0  ldur x0, [x21, #-8] ; bl 0x1e17b50      ; operator delete
0xcd2afc  b    #0xcd2abc
0xcd2b00  ldur x0, [x29, #-0x48] ; bl 0x1e17b50
```

Those two addresses are the only distinct guest PCs in the entire run, and they
are the loop head and the loop body. **The guest is not stalled in the
Cloudcell path; it is spinning in this container destructor**, deleting nodes
forever because the cursor never reaches the sentinel it started from.

**Prime suspect, and it is ours.** This port answers `getComponent`,
`getCustomProperties`, `keySet`, `iterator` and `put` with one shared
`char g_fakeObjectObject{}`. In the same run `getComponent` was called **16
times and every call returned the same address**. A single handle inserted into
a container once per call is a self-referential node, and a `cursor -= 0x30` walk
that never meets its end pointer never terminates.

### A/B RESULT: the shared handle was the spin. Switch stays at 1.

`RR3_FRESH_FAKE_OBJECT=1`, archive `20261005-080656-percall`:

| | shared handle | per-call object |
|---|---|---|
| distinct GUEST pcs in the run | **8** (all at 0xcd2abc / 0xcd2ad0) | **0** |
| `jni-static-enter` | 45 | 45 |
| `DeviceInfo::` lines | 21 | 21 |

**The guest no longer appears at all, and the container destructor is gone from
the samples.** The acceptance written before the run said the switch goes back
to 0 only "if the guest still only ever appears at 0xcd2abc / 0xcd2ad0"; it does
not, so **the shared handle is implicated and the switch stays at 1.**

The replacement stall is on the HOST side, and attributing it correctly needed
one more fix -- see below. The first read of it was wrong twice: the 27-sample
hot address belongs to threads `ordinal=1/2`, not the main thread, and the main
thread's own record is

```
samples 1 -> 17 -> 25 -> 33
pc   0x814e2ab8 -> 0x814d3e50 -> 0x814a68b0 -> 0x814d3e50
lr   0x80f8c0c8 -> 0x814d3e1c -> 0x81492f40 -> 0x814d3e1c
```

`pc = 0x814d3e50` with `lr = 0x814d3e1c` -- **`pc - lr = 0x34`**, a tight call
loop. Which function that is was unanswerable, because `census-pc` printed
`pc_elfva=n/a` for every host pc.

### `pc_elfva=n/a` for a host pc was a decision, not a missing measurement

`censusClassifyRegister(plan, nroBase, sp, pc, true)` is handed the NRO base two
lines above the format call, and `RegisterClass` carries `off` for exactly this
case -- `census-pcburst` already prints `lrRc.off`. Only `census-pc` chose
`pcGuest ? pcElfva : "n/a"`.

**Every host pc in every run therefore had to be guessed at**, and the run's only
two distinct guest addresses -- `0xcd2abc` and `0xcd2ad0`, sampled 5x and 3x,
the head and body of the loop the guest never leaves -- stayed unidentified for
a whole session. Both now print, host ones prefixed `host+` so they cannot be
confused with a guest elfva.

The general form: **an instrument that classifies an address but declines to
print the classification for half the address space has not measured those
addresses, it has hidden them.** The `n/a` looked like "not applicable" and was
read as "not known".

**And the fix has a trap of its own**, hit immediately. `host+0x5458f8` was
resolved with "nearest preceding symbol", which is only valid when that symbol
has a real size. libnx's syscall stubs are `size=0` thunks with a `$x` label and
the real name at the same address, so `+4 from svcSleepThread` is *not* "inside
svcSleepThread" -- it is the `svc` instruction of a different syscall. Resolving
by containment (`addr >= sym && addr < sym+size`) is the only correct rule, and
it says "not in any function" for all of them.

### RETRACTED: "a five-thread lock convoy"

What the symbolication legitimately produced:

| host offset | nearest-preceding symbol | seen on |
|---|---|---|
| `0x5458f8` | `svcSleepThread` @0x5458f4 (**+4**) | ordinals 1, 2, 5 |
| `0x545950` | `svcArbitrateLock` @0x54594c (**+4**) | **ordinal 0 (main)** |
| `0x545984` | `svcSendSyncRequest` @0x545980 (**+4**) | ordinal 4 |
| `0x572f10` | `_free_r` +80 | **ordinal 0** |
| `0x581b24` | `memchr` +100 | **ordinal 0** |

**These are three DIFFERENT syscalls, not one lock.** Each stub is 8 bytes and
`+4` is the `svc` instruction inside it, so the three threads were respectively
sleeping, arbitrating a lock, and making a synchronous IPC request -- three
ordinary states. There is no convoy here, and the earlier "five of six threads
in the same spinlock" was wrong.

The error is the same one this file has now made three times: **`+4 from the
nearest preceding symbol` was read as `inside the containing function`**, for
symbols whose `size` is 0, and then a *set* of addresses was read as a *single
location*. `svcSetThreadActivity` and `svcGetThreadContext3` sit in the same
block -- those are the census's own pause-and-read calls, so a thread caught
inside a syscall is exactly what pausing produces.

What survives, and is worth keeping: the main thread's own three samples are
inside functions that have real sizes -- `mutexLock+80`, `_free_r+80`,
`memchr+100`. **Those are snapshots of instants, not a deadlock diagnosis.** A
paused thread is a single frame; concluding contention or deadlock from three
frames of one thread would be the third unsupported step in a row.

### After the spin fix, the main thread's dominant state is mutex acquisition

Re-reading the same run properly (the first attempt used a regex with the format
string's fields in the wrong order and returned 0 -- the fourth time tonight a
predicate was trusted before it was checked), the main thread's PC takes exactly
**three** distinct values across the whole run:

| lines reporting it | host offset | containment-resolved to |
|---|---|---|
| 2 of 4 | `0x545950` | the `svc` inside the `svcArbitrateLock` stub |
| 1 | `0x572f10` | `_free_r` +80 |
| 1 | `0x581b24` | `memchr` +100 |

**The `svcArbitrateLock` stub is where a thread sits while acquiring a mutex**,
so the main thread's dominant state after the container spin was removed is
*waiting on a lock*, not executing guest code. Its other two samples are in
`free()` and in `memchr` -- and `memchr` is on this port's JNI name-lookup path.

**And our own ledger contradicts it**: `census-mutex-sum mutexes_known=48
held_now=0 held_by_exited=0` says nothing is held, while the main thread is
demonstrably blocked acquiring one. The ledger tracks libnx `mutexLock`
(`0x1e18040`); it does not see whatever lock this is. Candidate worth checking
next, and named as a candidate only: **newlib's own arena lock**, since a thread
reaching `pthread_exit` (the census shows several, `last=pthread_exit`) while
holding it strands the arena permanently, and this port has no instrument that
would notice.

That is the next measurement, not a conclusion. The container spin is fixed and
independently verified; this is the wall behind it.

### The ledger cannot see this lock, and there is a precedent for exactly that

`mutexNoteAcquire(mutex, __builtin_return_address(0))` is called from
`guestPthreadMutexLock`, so the ledger *does* stamp ownership per guest
`pthread_mutex_t*` -- and `pthread_bridge.cpp:3434-3440` records a previous
occurrence of this precise symptom:

> skipping `entry->host` meant every lock the main thread took was never
> ownership-stamped: the ledger reported `held_now=0` and `acquires=0` for
> objects the main thread was demonstrably contending on (one object showed
> `waits=55 acquires=0 owner_ord=0`)

That was diagnosed and fixed. The current run shows the same shape again --
`mutexes_known=48 held_now=0 held_by_exited=0 total_acquires=1242`, with the
main thread demonstrably blocked in a mutex acquisition. Two possibilities
remain, and they are one grep apart:

1. the lock is a **guest** `pthread_mutex_t` and the earlier fix regressed -- the
   ledger should be seeing it; or
2. the lock is **host-side** -- our own libnx `mutexLock`, or newlib's arena
   lock -- and the ledger structurally cannot see it, because it never passes
   through `guestPthreadMutexLock` at all.

Distinguishing them: `census-mutex-sum` shows `total_acquires=1242` and nothing
held, so if the blocked object were in the ledger it would have to be one whose
last acquire never released. The register dump's `x30 = mutexLock+80` does not
settle it, because libnx's `mutexLock` is what `guestPthreadMutexLock` calls
*into* -- the same frame appears either way. **The address `x0` holds at the
`mutexLock` entry is the discriminator, and it is not in the line we have.**

### Third instrument defect of the night, and it was the load-bearing one

`heldList` in the mutex summary is filled **only** from slots with `held` set:

```
if (!slot.held) { continue; }     // <- the entire wait dimension is dropped
++held;
heldList[heldCount++] = ...;
```

**A lock nobody holds produces no row at all, by construction.** That is exactly
backwards for the case that matters: a thread blocked acquiring a mutex reports
as `held_now=0` with an empty detail list, and the report then says "nothing is
held" while the main thread sits in the arbitration stub. Measured on
`20261005-081112-hostelfva`: `mutexes_known=48 held_now=0 total_acquires=1221`
and **zero** `census-mutex-*` rows.

`waits` is the discriminator the report never had -- `waits > 0 && !held` is
"someone is queued on a lock nobody owns" and is invisible to a held-only list
no matter how many acquires went through it. Added: `census-mutex-sum` now also
carries `total_waits` and `waited_shown`, and `census-mutex-waited` rows list the
top-N unheld-but-waited objects.

**First result, archive `20261005-083214-waitlist`:**

```
mutexes_known=48 held_now=0 total_acquires=2433 total_waits=2425 waited_shown=8

guest=0x2102bb7ccc  waits=587  acquires=587  held=0
guest=0x2102bdb0ac  waits=587  acquires=587  held=0
guest=0x21080f0a44  waits=213  acquires=213  held=0
guest=0x2102bb5b60  waits=196  acquires=196  held=0
guest=0x21083b0048  waits=111  acquires=112  held=0
```

**`waits == acquires`, exactly, on the top four.** That is not contention: it is
a mutex that is blocked *every* time it is taken. With `held=0` at sweep time,
and the release side of the ledger never showing these objects held, the
release accounting for them is not happening. `0x21083b0048` is the same object
the 3434-3440 comment recorded in August as `waits=55 acquires=0 owner_ord=0`
-- now `waits=111 acquires=112`, so the acquire side did get fixed and the wait
side is simply growing.

This is the first time the contended guest objects have been nameable at all,
and it names them by address, which is what the next step needs.

### ...and the addresses resolve, to a specific call in a specific thread

Printing `lastWaitReturn` (not `acquireSite` -- the first attempt printed that
field and got all zeros, because it is written only on a successful stamped
acquire, and these objects are waited on far more often than they are taken)
gives the guest side. Archive `20261005-084007-waitsite`, guest bias
`0x2106559000`:

```
waits=213  lock=21084f0a44  ->  guest 0x12e5e34
waits=196  lock=2102fb5b60  ->  guest 0x1529fa4
waits=141  lock=2102fb7ccc  ->  guest 0x1c69d8c
waits=141  lock=2102fdb0ac  ->  guest 0x1c69d8c
waits=111  lock=21087b0048  ->  not in the guest image
waits=99   lock=210850ed88  ->  guest 0x1487848
waits=55   lock=21087affc0  ->  not in the guest image
waits=29   lock=21062a38a0  ->  guest 0x10a1eb4
```

`0x1c69d8c` is the **1 Hz job-queue drainer** already disassembled for this file
(`0x1c69a98` is its body). The call there is

```
0x1c69d84  mov  x0, x19
0x1c69d88  bl   0x1c76fe4          ; 8-byte thunk
   0x1c76fe4  add x0, x0, #4
   0x1c76fe8  b   0x1e18530        ; PLT -> pthread_mutex_lock
```

**So the 1 Hz thread calls `pthread_mutex_lock(obj + 4)` and blocks there about
once a second -- 141 waits over 141 seconds, `waits == acquires`, i.e. it waits
every single tick and is granted every single tick.** That is not a deadlock; it
is a lock that is held for roughly a second each time.

And the main thread is independently sitting in libnx's
`std::__ndk1::mutex::lock` (`0x1e18040`) at `mutexLock+80` -- the path
`guestPthreadMutexLock` calls into. **The main thread is therefore the most
likely holder, and the two lock implementations are different objects**: the
ledger stamps the *guest* `pthread_mutex_t` and reports `held=0` for both,
which is consistent only if whoever holds the underlying libnx lock never
completed the guest-level acquire that would stamp it.

**The holder is still not named.** What is named, for the first time: the call,
the thread, the two guest lock addresses, the frequency, and the fact that the
wait resolves rather than hangs.

### RETRACTED: "`waits` is contention", and everything built on it

`noteMutexWait(mutex, caller)` is called **unconditionally** on entry to
`guestPthreadMutexLock`, before the lock is attempted:

```
int guestPthreadMutexLock(void* mutex) {
    CENSUS_IMPORT("pthread_mutex_lock");
    noteMutexWait(mutex, __builtin_return_address(0));   // every REQUEST
```

**So `waits` counts requests, not waits.** `waits == acquires` -- 510/510,
509/509, 213/213, 196/196 -- is a **tautology: one call, two counters, in
sequence.** The reading it invited, "hundreds of threads queueing on a lock
nobody holds", is not contention and was never evidence of it.

The correct instrument was already in the code, in the retry loop:

```
int lockResult = ::pthread_mutex_trylock(host);
while (lockResult == EBUSY) {
    guestViewTraceSyncBlocked("mutex", 52);   // fires only on real EBUSY
```

**Measured on the same run: zero.** No sync-blocked record of any kind, and
`census-wait-sum condvars_waited_on=0 total_waits=0`. **There is no mutex
contention anywhere in a 200-second run.**

Which also retires the conclusion built on top of it: the main thread's PC at
`mutexLock+80` is **a thread that happened to be executing a lock when the
census paused it**, not a thread waiting for one. Three of the last four
conclusions in this file were the same mistake -- reading a number whose name
described an intention as if it described an event.

`waits` is renamed to `requests` in the summary, `blockedSpins` / `worstSpins`
are accumulated inside the EBUSY loop itself, and `census-mutex-waited` now
ranks and filters on `blockedSpins`. A run reporting `contended=0` is the
honest result and must be read as "no contention", not as "no information".

**Confirmed on the rebuilt instrument, archive `20261005-084640-contendspins`:**

```
mutexes_known=48 held_now=0 held_by_exited=0
total_acquires=1114 requests=1106 blocked_spins=0 contended=0
census-mutex-waited rows: 0
```

**Zero real mutex contention in a 200-second run, 1114 acquisitions.** The
whole mutex line of enquiry is closed, and it is closed by the real number
rather than by an argument.

One consequence to check next, stated as a suspicion and not a finding: the
main thread's PC samples are all host-side, and one of them is
`memchr` -- which is on this port's JNI name-lookup path. This session added an
entry log **and** an exit log to every `CallStaticObjectMethodV`, each a file
write, plus the same for two other bridge sites. **The sampler may now be
mostly observing the instrumentation this investigation added.** The next run
should either raise nothing and accept that cost, or gate the new logs behind
a switch and measure with them off.

### RETRACTED: the per-call-handle A/B, and the "0xcd2abc is gone" result

`RR3_FRESH_FAKE_OBJECT=1` was read as fixing the container spin, on the
strength of `20261005-080656-percall` reporting **zero** guest PCs.

**That was sampling luck, and it is the same error this file has retracted six
times.** With the pair logs still on, `20261005-085156-nopairlog` sees the spin
again:

```
2x guest 0xcd2abc
1x guest 0xcd2ad0
```

**So the shared handle is still a live suspect and the switch is not a fix.**
It stays at 1, but on merit rather than on the void experiment: repeated
`getComponent` calls returning one identical object is wrong on its face, and
that is the only argument for it.

The lesson generalises past this file: **"the thing I was looking for did not
appear in the sample" is not a measurement of absence, and a sample is a
sample.** A negative result from a sampler whose hit rate has never been
measured is not evidence.

### And the `jni-obj-exit=0` scare was this file's own bug

`jniCallObjectMethodV` appeared to be entered 62 times and never returned
(`obj-enter=62`, `obj-exit=0`) -- which would have been *the* bug, in our own
bridge, on the hottest object method. It is not real:

```cpp
std::atomic<unsigned>& budget = (phase[0] == 'e') ? enterBudget : exitBudget;
appendGuestBootLine(phase[0] == 'e' ? "jni-obj-enter" : "jni-obj-exit", ...);
```

**"enter" and "exit" both begin with 'e'.** Both the budget choice and the
label were therefore constant, `jni-obj-exit` could not be emitted at all, and
62/0 was an artefact of a label that had collapsed.

Two of the three things I believed in the last hour were wrong, and both were
my own instrumentation rather than the guest: the switch I declared was never
wrapped around the calls, and the pair label could not distinguish its two
phases. **Every number I reported tonight was produced by code I had just
written, and neither piece of it was validated before I read a conclusion out
of it.**

### Controlled verification (archive 20261005-085657-verify), with the controls it needed

| check | result | reading |
|---|---|---|
| CONTROL `jni-static-obj` (old, ungated) | **46 > 0** | the file is live and the grep is right |
| `jni-obj-enter` / `jni-obj-exit` | **0 / 0** | the switch is honoured this time |
| `jni-static-enter` vs `jni-static-exit` | **45 vs 45** | **this port's JNI bridge does not hang** |
| CONTROL main-thread `census-pc` lines | **10** | the run really sampled the main thread |
| guest PCs | **0 of 64** (5 distinct, all host) | -- |

**The `obj-exit=0` scare is closed by a positive control**, not by an argument:
the ungated static pair, which discriminates its two phases correctly, is
balanced at 45/45. The bridge returns.

**And the main thread was sampled ten times across 64 points and was never in
guest code.** That is not a thin sample. Combined with the samples landing in
`memchr`, `mutexLock` and `_free_r`, the honest reading is that the main thread
is spending the sampled instants inside this port -- the JNI name lookup, the
log writer, the allocator.

This also bounds the claim about `0xcd2abc`. It appears in `nopairlog` and not
in this run, so **its presence is not stable across runs and it has never been
reproduced in a run with an adequate sample count.** The container-destructor
loop is real code in the guest and it is the most-sampled guest address seen
tonight, but "the guest is spinning there" is not a stable observation and
should not be recorded as one.

### And the strongest claim of the night is also wrong: "0 guest PCs" means nothing here

Having written "the main thread was sampled ten times across 64 points and was
never in guest code", and read that as the port keeping the guest busy: **in a
rehost, the guest spends most of its time inside this port's replacement code.**
The sampler pauses each thread and reads its context; any guest thread currently
talking to us is, by construction, stopped in host code. **A PC sampler looking
for "guest code" is looking in the wrong place most of the time**, and "0 guest
PCs" is the expected shape of a healthy run, not a symptom.

That is the second time tonight a run's guest-PC count was read as a conclusion:
the `percall` A/B first, this one second. The correction is not a caveat on one
number -- it invalidates the inference *from* the number, and every claim of the
form "the guest is stuck / busy / gone because its PC was not in guest code" is
withdrawn.

**What the PC sampler can actually answer here:** a thread *parked* in guest
code, i.e. one that is not inside one of our bridges. That is worth having, and
it found the `0xcd2abc` loop. It cannot answer "is the guest making progress",
and this port's own bridges are far too numerous for that reading to be safe.

### What reconciles the two guest-side findings: they are one code path

The last thing the guest ever logs is `DeviceInfo::Tier: %s`, whose format
string is at `0x3edbf3`. A word-level ADRP decode (the capstone linear sweep
desynchronises over 21 MB of `.text` -- with the control string
`DeviceInfo::Platform` at `0x48c742` as the positive check, which is what makes
the result usable) puts its materialisation at `0xcd2a8c`, and the code there
was already dumped:

```
0xcd2a8c  adrp  x0, #0x3ed000
0xcd2a98  add   x0, x0, #0xbf3      ; "DeviceInfo::Tier: %s"
0xcd2a9c  bl    0x109fa18           ; the guest's LAST log line
0xcd2aa0  ldur  x20, [x29, #-0x48]  ; ... and immediately:
0xcd2aac  cmp   x21, x20
0xcd2ab0  b.ne  #0xcd2ac8
0xcd2abc  cmp   x20, x23            ; the list walk, sampled 5x / 3x
```

**`0xcd2abc` is the code that runs right after the last log line.** The two
guest-side findings from tonight are not two things: the guest assembles a
`Device` / `Profile` / `Tire` container at `0xcd21f0`-`0xcd2270` earlier in this
same FDE, logs it, and then tears it down at `0xcd2aa0`, and the teardown's node
walk is what the sampler kept catching.

**So the question is no longer "where is the guest stuck" but "why does the
DeviceProfile container's node chain not terminate"** -- and the exit test is
`x21 == x20`, i.e. the walk never reaches the end node it started from.

That is the right shape for the shared-handle hypothesis (an identical pointer
inserted more than once is a self-referential node), and the switch is already
at 1. But the A/B that was supposed to test it read *sampling*, not the walk's
termination, and it was invalid. **The honest position: the hypothesis is now
sharply framed and still untested.**

The real defect found tonight is unaffected and stands on its own A/B: the shared
`g_fakeObjectObject` handle produced a self-referential container node, and
`0xcd2abc`'s destructor walked it forever. `RR3_FRESH_FAKE_OBJECT=1` removed
every guest PC from the samples.

`recovery_guard.cpp` is this port's own stall detector, and it watches **446
specific guest addresses**. It ran through the whole stopping sequence:

```
phase=guard-reg-end  registers=29  guard_address_hits=0  guards_known=446
phase=guard-end      images=4  stuck=0  reported=0  cap=48
```

**Zero of 446 state-relevant guest addresses were written, and the guard's own
verdict is `stuck=0`** -- the port does not consider the main thread stuck. That
is a fourth independent instrument (after the import ring, the entry/exit pairs
and the census) agreeing that the guest does nothing observable after
`DeviceInfo::Tier: AndroidHigh`.

Its limit, stated so it is not over-read: the guard sees **writes**. A thread
spinning on reads would register the same way. Combined with `imports_delta=0`
and a frozen ring, "executing pure guest code and touching nothing we watch" is
the best-supported description available -- and it is still not a location.

### Handoff: what is known, and what is left

**Established, with the evidence named:**
- The field the guest tests, `(*(*(0x1f6d5d8)))[0x10518]`, is written at exactly
  one place in the image: `0x140461c`, inside constructor `0x14045e0`.
- The full path to it: `onViewRenderJNI (0x12b3614) -> 0x12aea7c -> 0x12aee50/
  0x12aeec0 -> 0x12b43a0 -> 0x12b4440 -> 0x1405868 -> 0x14048c8 -> 0x14045e0`.
- Gates C (`0x1fb5330`), D (async future), E (`0x1f78e98`) and F (`renderer[0xa]`)
  are all **measured open**. The async worker `0x1406f94` is alive, per the
  census's own `start_elfva`.
- The second `onViewRenderJNI` call reaches the device-fingerprint sweep, a
  complete `DeviceInfo` block ending at `Tier: AndroidHigh`, `Device`/`Profile`/
  `Tire` string assembly, and the Cloudcell slot-5 -> slot-20 chain.
- Every file the guest asks for at that point is deployed and readable.

**Refuted, each with the measurement that killed it:** the one-deref reading of
the download check; "gates A/B are closed"; "`+0x80` is a flag word";
"`x19 != ren`"; "the bridge was never exited"; "the last import was `close`";
"the Adreno/Qualcomm identity is a bug"; "the jstring value is lost"; "the
zero-byte Cloudcell config stubs are the cause".

**The one thing left is a location, and Eden cannot supply it.** `pc` and `lr`
from `svcGetThreadContext3` are the reader's own registers
(`sampler-selftest-b`: `pc=0x80cf8a18 lr=0x807c8d44`), the frame-pointer walk
needs that same context, and the import ring only updates when the guest calls
something. A guest thread executing pure guest code and calling nothing is
therefore invisible here. Real hardware, or a backtrace from a crash, is the
only route to it.

**Deliberately not done, because it crosses the stated boundary:** no fabricated
Cloudcell response, no reverse of the anti-cheat to satisfy it, no forged
telemetry. The one legitimate move in that area -- deleting our own empty-file
fake -- was tried and measured to change nothing.

### The shape of the Cloudcell chain -- and why slot 5 alone is not the answer

`0x1cb5720` (FDE `[0x1cb5720, 0x1cb5780)`) is called at `0xcd2824`, and it
explains why slots 5 and 20 appear as separate log lines from what is really one
chain:

```
1cb572c  adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5d0] ; ldr x8,[x8]   ; asset manager
1cb573c  ldrb w9,[x8,#0x4c1] ; cbz w9 -> 0x1cb5774
1cb5750  ldr  x9,[x9,#0x28]          ; vtable slot 5   (0x28/8)
1cb5754  blr  x9                     ; return value is a RECEIVER
1cb5758  cbz  x0, #0x1cb5774        ; NULL receiver -> give up, return
1cb575c  ldr  x8, [x0]
1cb5768  ldr  x2, [x8, #0xa0]        ; 0xa0/8 = slot 20 on the RECEIVER
1cb5770  br   x2                     ; tail call
```

**Slot 5 is a factory whose result is immediately dereferenced, and slot 20 is
tail-called on whatever slot 5 returned** -- the same shape as the slot-18 pin
("factory: returns a receiver, caller derefs it at 0xcd1744"). The parity
default returns an i%2 value for both, which is a coin flip where the engine
needs a pointer.

**But this round it worked**: the 21 `DeviceInfo::` lines and all the profile
string assembly come *after* this chain, so slot 5 returned a usable receiver and
slot 20 ran. **This is therefore not the stopping point**, and the obvious
conclusion -- "pin slot 5 as a factory" -- is not supported by the run. It stays
a documented shape, not a fix.

### A/B 2026-10-05: the zero-byte Cloudcell config stubs are NOT the cause

The strings referenced inside the error function `[0xcd1014, 0xcd3648)` include

```
CLOUDCELL_FallbackAssetServerEnabled
Cloudcell initializing with environment: %s
/CC_SeedData.bin   ccsn.bin   pcsf.bin   /rr3/live/   r_lively
cc_webbrowser_background_plain
GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE
DeviceInfo::Platform: %s            (0x48c742)
```

and `ensureWritableRoots()` creates 0-byte `doc/ccsn.bin` and `doc/pcsf.bin`
(`guest_filesystem.cpp:130-136`). So the guest wants Cloudcell configuration and
we hand it empty files. That is worth testing, and the test is one switch:
`RR3_TOUCH_STUBS` gates **both** creation and exposure (`stubSuppressed`), so
`0` removes the fake entirely rather than filling it.

**Result (archive `20261005-034539-nostubs`): null.**

| measurement | stubs on | stubs off |
|---|---|---|
| `ccsn.bin` / `pcsf.bin` mentions in log | present | **0** (both halves of the switch worked) |
| `DeviceInfo::` lines | 21 | 21 |
| last DeviceInfo line | `Tier: AndroidHigh` | `Tier: AndroidHigh` |
| `cc-slot` calls | 22 | 22 |
| `jni-static-enter` | 45 | 45 |
| `GAMETEXT_GAME_DATA_FAILED` | 0 | 0 |

**The switch is back at 1**, per the acceptance written before the run, and for a
concrete second reason: the same `if (RR3_TOUCH_STUBS)` block also creates the
virtual `/proc/cpuinfo` (`guest_filesystem.cpp:148-166`, "for getCpuName /
feature probes"). `0` would have deleted that legitimate virtual file as well,
and the guest demonstrably uses it -- `getCpuName` is called in this very run.
**A refuted hypothesis does not get to cost an unrelated working feature.**

**And the boundary, stated plainly:** no real `ccsn.bin` / `pcsf.bin` /
`CC_SeedData.bin` exists anywhere in the workspace, and those names appear in
**zero** of the decompiled Cloudcell Java sources under
`reverse/java/sources/com/firemonkeys/cloudcellapi/` -- so they belong to
Cloudcell's *native* component. This port does not fabricate them, does not
reverse the anti-cheat to satisfy it, and does not fake any Cloudcell response.
Removing our own empty-file fake was the only legitimate move available here,
and it changed nothing.

### RETRACTED: "the jstring value is lost on the way back"

Also wrong. Measured and refuted three ways:

1. **The method lookups all succeed.** A `noteLookupMiss` probe on the two
   silent `return NULL` paths in `GetMethodIDGeneric` / `GetFieldIdGeneric`
   (`jni.cpp:343`, `:363`) produced **zero** miss lines in
   `20261005-033813-lookupmiss`. The class registries have what the guest asks
   for.
2. **No value is lost in transit.** Every method in the fingerprint sweep
   returns `jstring-heap`, never `jstring-fake`, and each has a paired
   `jni-static-enter` / `jni-static-exit`.
3. **The guest never asks.** The uncapped entry/exit log (45 lines, 24 distinct
   methods, all paired) contains **no `GetDeviceFirmwareVersion` at all** --
   and `Firmware:` is empty. Same for the sources of `Platform:` and `Model:`.

**Twice in one session I read an absent log line as an absent event.** The
first time the `per-call-log-cap` line said so outright
(`suppressed_total=1`); the second time the line I reached for was
`phase=jni-static-obj`, which shares that same exhausted 64-write budget, and I
did not check the cap before concluding. **`absent from a capped log` is not
`did not happen`.** Before reading any absence, ask what would have had to be
dropped, and prefer an instrument with its own budget.

Worth keeping: `GetDeviceFirmwareVersion` is a name **this port invented**
(`jni_bridge.cpp:1236`). "We have an answer for it" and "the guest will ever ask
for it" are different claims, and only the second one puts the answer on the
wire.

**Still open, and honestly so:** why the guest omits `Platform`, `Model` and
`Firmware` from a table it populates for every other field. It is not a JNI
answer we are failing to give, because it is not asking. Nothing in the current
evidence says which of the guest's own filters is responsible.

- **`DeviceInfo::Platform:`, `DeviceInfo::Model:` and `DeviceInfo::Firmware:`
  are empty** even though `platformStringForMethod` answers `"switch"`,
  `"Nintendo Switch"` and `"Horizon"`. The JNI result is a `jstring-heap`
  handle, so the value is being lost between our return and the guest's read --
  a real defect, and it is the same class as the 2026-10-04 `stat` ABI miss.
- **The null GL backend reports `DeviceInfo::GPU Renderer: Adreno (TM) 420` and
  `GPU Vendor: Qualcomm`.** On a Switch that is a false identity, and it is
  exactly the sort of value that can steer the engine down a vendor-specific
  path. It should say what the backend is.

### RETRACTED, immediately: do not "fix" the Adreno/Qualcomm string

The bullet above is **wrong and must not be acted on.** Reading
`gfx_gles.cpp:32-83` in full shows the string is a **measured control**, not an
oversight:

- the engine **parses** the renderer string and writes the model digits into
  `[rend+0x500]`, and changing the digits moved that field (archive
  `20261004-054107-m420`: `530 -> p500=530`, `420 -> p500=420`);
- that change did **not** move `t4fc`, the GPU id, which is what the render
  loop's exit test compares against `4` (`cmp #4 / b.ne` at `0x12af188`);
- so the id is decided elsewhere. The same comment names where:
  `0x12afaa0` reaches for **Android system state** -- `'device'`, `'id'`,
  `'onlyWIFI'`, `"Carrier: '%s'"`, `getSupportedAbis`, `getScreenDPI`,
  `strcasecmp`.

Rewriting the identity would move `p500` and nothing else, and would destroy the
one control that has already produced two negative results.

**Audit of what `0x12afaa0` actually needs, measured against the port:**

| input | answered? | value |
|---|---|---|
| `getScreenDPI` | yes, in `platformIntForMethod` | `160` |
| `getSupportedAbis` | yes | `arm64-v8a` |
| `'device'` / `getBuildDevice` | yes | `nx` |
| `"Carrier: '%s'"` / `getCarrier` | yes | `""` (no radio) |
| `getNetworkOperatorName` | yes | -- |
| **`'id'` / `Build.ID`** | **no handler** | -- |
| **`onlyWIFI` flag** | **no handler** | -- |

The last check nearly produced a false report: `getScreenDPI` reads
`return 160;` inside a `const char*` function in the grep output, which looks
like a type error, and it is not -- the line is in `platformIntForMethod`
(`jni_bridge.cpp:1298`). **A grep hit is not evidence of which function it is
in.** The two genuine gaps are `Build.ID` and the WiFi-only flag, both
legitimate non-security values, but the same comment says the id is no longer
the wedge, so filling them is an experiment, not a fix.

With entry and exit markers on all three object-calling sites:

```
jni-static-enter method=getCpuName mid=0x809c13f8 returns=string
jni-static-exit  method=getCpuName mid=0x809c13f8 ret=0x809cf378
```

`getCpuName` **returned**. No `jni-obj-enter`, no `jni-void-enter`, no further
static call. **The port's JNI bridge is exonerated for that call**, and the
block is guest code that stops calling back.

A fifth correction while reading the run: `enters=1199 exited=0` was read as
"entered the bridge 1199 times and never came out". **It does not say that.**
`exited` is a thread-exit counter -- the same field reads
`inbridge=1/pthread_exit enters=278 exited=1` elsewhere in the same log. The
census never claimed the thread was stuck in the bridge.

**The instrument's blind spot is now named and measured.** The sampler self-test
(`sampler-selftest-b`) pauses a victim thread, reads its context and resumes it:

```
pauseRes=0x0 ctxRes=0x0 resumeRes=0x0 pc=0x80cf8a18 lr=0x807c8d44
```

**Both `pc` and `lr` are host-NRO addresses** -- the reader's own registers.
`svcGetThreadContext3` does not deliver another thread's context under Eden. So
a guest address is available **only while our code is running on the guest's
stack**, where `__builtin_return_address(0)` is a real guest return address.
And only **three** handlers in the whole port call `censusNoteImportFrom`:
`CallObjectMethodV`, `CallStaticObjectMethodV`, `CallVoidMethodV`.

That is why the ring kept naming `CallStaticObjectMethod` with `lr=0xba8474` --
a call an entry/exit pair proved had already returned. **The ring was naming a
victim, not the culprit.** Every other JNI call left it stale.

Eight accessors a post-`getCpuName` string path reaches for now report their
guest return address as well -- `FindClass`, `GetMethodID`, `GetStaticFieldID`,
`GetStringLength`, `NewStringUTF`, `GetStringUTFChars`, `GetArrayLength`,
`ExceptionCheck` -- so the ring gets refreshed guest addresses for that region
instead of pointing at the last method it happened to see.

### Also retracted: the "+0x8d download freeze" note in rr3_Platform.cpp

That note was the obvious lead -- `+0x8d` sits in the same cluster as `+0x82` /
`+0x83` -- so it was checked rather than trusted. It is wrong twice over:

- every address it cites (`0x3276b8`, `0x323534`, `0x31ffe0`, `0x31ff1c`,
  `0x325098`) is inside **libfuelmetrics.so** (`.text` 0xeb000..0x3ca6f4).
  libRealRacing3.so's highest mapped address is 0x1fc7a99, so none of them can
  be an address in this module;
- `0x3276b8` is a nine-instruction accessor for `+0x98` ending in `ret` at
  `0x3276dc`, and `0x3276e0` -- which the note quotes as
  `if (am[0x8d]) return;` -- is `str x1,[x0,#0x38] ; ret`.

A retraction has been written into the header of that file. The note survived
because its addresses are plausible, they do sit in a real `.text`, and the
story matches the symptom; nothing in it was ever disassembled. The writers of
`ren->0x82` and `ren->0x83` are therefore still unknown, and must be found in
`libRealRacing3.so` before any value is written there.

**This is a measurement, not a conclusion, and it is the wrong shape for a
shipping app.** A field read 62 times that no module ever populates would mean
the guest always believes it is downloading, which cannot be true on a real
device. So one of these holds and I have not distinguished them:

1. the store is further than 700 instructions from the offset register,
2. the store computes the address in a form this scan does not match (an index
   register built some other way, or a base that is not `engine`),
3. something outside the image set writes it -- our own
   `multi_dso_linker` relocation pass, or a heap layout this port lays out
   differently.

Option 3 is the one I would check first and it is cheap: read whether
`multi_dso_linker` writes anything under the engine object. The engine address
is a guest heap address, so ordinary ELF relocations should not reach it, but
the port also maps segments itself and that is worth confirming rather than
assuming.

### tools/guest-analysis/find-offset.py: what it is good for, and what it is not

Written tonight to answer "who writes engine+0x10518". It has two modes, and
only one of them is trustworthy right now.

TRUSTWORTHY -- the movz/movk mode, used for a field at a register offset:

    python tools/guest-analysis/find-offset.py 0x10518 --control 0x12ad2fc \
        --stores --window 700

A field 1 MB into a heap object cannot be addressed by a 12-bit scaled
immediate, so it goes through a register built with `mov`+`movk`. That form is
matched exactly, and `--control` makes the scan abort unless the known-good
site comes back. Result: **62 access sites, 0 store sites**, across a window
wide enough to cover a whole function and with both `[base]` and `[base,index]`
addressing checked.

NOT TRUSTWORTHY -- the `--global` mode for `adrp` page + immediate. It finds
`0x1f6d5d8` (the engine slot), and it does return the known site 0x12ad2f0,
but it reports **2212 sites**, which is not a plausible number for one global
and means the immediate extraction is still matching too much. Treat that
count as unknown, not as a measurement.

Both bugs found today, for the record, because both produced confident zeros:

* a mask written where the expected value belongs -- `if (x & 0xFFC00000) !=
  0xFFC00000` demands bits 31:22 all be set, so it never matches; the correct
  form is `(x & MASK) != EXPECT` with `MASK=0xFFC00000`, `EXPECT=0xB9000000`.
* a MOVK shift field read as `#32` when the encoding `1` means `#16` (bits
  22:21 hold the shift: 0=L, 1=LSL16, 2=LSL32, 3=LSL48).

The reason `--control` exists is that this file's third bug -- reading an ADRP's
own operands when the memory operand belongs to the *next* instruction -- also
reported a clean zero, and would have been reported as "nobody accesses the
engine global" if no control had been required.

### 2026-10-16: the `menugate=1` wedge is a host-side circular wait

Full write-up, with the measurement tables and the corrected claims:
`docs/compose/spec/unblock-menu-loading.md` §S4. The short version:

The guest hands a `std::function` continuation to slot 2 of the Cloudcell
endpoint object. `cloudcellEndpointStubRequest` captures it and returns without
ever invoking it. The only code that invokes it, `cloudcellPumpCompletion()`,
has exactly one call site -- inside `frameLoopDriver`, *after* the
`renderTarget(...)` call. The guest blocks inside that `renderTarget(...)`, so
frame 1 is never reached and the completion is structurally unreachable. The
thread that has to deliver the answer is the thread waiting for it.

Measured, two reproductions with identical numbers:

* `cc-slot` counts are **0** with the gate closed and **47** with it open;
* all 47 callers sit in `0xCD1xxx–0xCD3xxx` (Cloudcell init) and **none** in
  `0x12aefc0` -- the guest never reaches the menu gate at all;
* `path-req` is **120 vs 120**, zero difference, so the menu load is not
  missing a resource.

`ccmode=off|now|bg` (`065402e5`) moves the delivery point up to the capture
site. The record stays all-zero: a denial the guest can decode, not a forged
success. Default `off` leaves the verified-good config untouched. `bg` is not
usable yet -- the alert-pump thread that would drain it is created lazily
inside an anonymous namespace in `jni_bridge.cpp`.

### RETRACTED (2026-10-16): four claims this file and the source carried as settled

* "the frame loop never runs" -- `loop-top` fires once; `guest-activity=0` only
  proves `frames < 30`, since it is emitted at `frames % 30 == 0`.
* "one byte, two consumers, opposite polarity" -- `0xab2c94` also `cbz`s and
  also needs non-zero. No opposite polarity exists.
* "the guest never writes this byte" -- it does, at `0x1cbe218` in its own
  Cloudcell constructor. The store cited as proof (`0x10f0990`, `strb wzr,
  [sp]`) is a frame zero-init and undercuts the claim.
* "a single byte test gates the menu" -- it is a four-way AND with two host
  stub calls between the byte test and the `blr`.

### RETRACTED (2026-10-16): two emulator runs that measured the same thing

`readConf()` read the file once into `char buffer[256]`. The live
`rr3-gfx.conf` is 2.7 KB, nearly all of it the comment block recording why Mesa
and `nofb` are load-bearing, so **every setting past byte 256 was discarded
silently**. `ccmode=now` sat at offset 674 and `menugate=1` after it; neither
was ever parsed. Two runs meant to differ returned `frames=16080` and `15960`.

A switch that silently does nothing is indistinguishable from a switch that
does nothing useful, and nothing logged. Fixed in `568133f5`: buffer 4096, an
explicit `conf_trunc` alarm, and the `gfx-backend` line now echoes `ccmode`,
`menugate`, `ccdone`, `conf_trunc` and `conf_bytes` -- one line answers "which
configuration did this run actually use".

The generalisable form, since this file has now been bitten by three different
instrument failures in a row: **before concluding a switch had no effect,
prove it was delivered.** `IndexOf` will happily match a switch name inside a
comment, so reproduce the parse from the parser's own point of view
(`Substring(0, buffer)` then parse) rather than trusting a text search offset.

### RETRACTED (2026-10-16, later the same day): the continuation is never captured

The 2026-10-16 entry above says the guest hands over a continuation, the host
captures it, and the only thing wrong is that the delivery point sits inside the
frame loop. The second half is right. The first half was never verified, and it
is false:

```
cc-endpoint-request: captured continuation vtable=0x0 invoker=0x0 manager=0x0 payload=0x0 blob=0x0 w2=10795
cc-endpoint-request: fn was NULL
```

**All five fields zero, in all five archived runs that reach the capture site.**
`w2=10795` is stable and plausible, so x2 is mapped; the continuation is not.

Consequences, and they are the reason this is written down rather than quietly
fixed:

* `ccmode=now` is **inert**. `takePendingContinuation` fails its
  `vtable != nullptr` guard and `cloudcellPumpCompletion` returns before writing
  a line, so the run would have logged one `cc-endpoint-request` with
  `vtable=0x0`, zero `cc-deliver`, and the same wedge. A review caught this
  before the run; without it, a 5-minute round and an afternoon of
  interpretation would have been spent on a fix that has nothing to deliver.
* The outcome table this file pointed at was **ambiguous**: "no `cc-deliver` →
  nothing was captured" cannot tell "capture site never reached" from
  "capture site reached, continuation degenerate". Two different worlds, two
  byte-identical logs. The discriminator is `cc-endpoint-request` itself and its
  `vtable=` field.
* The real blocker is the **argument ABI of the continuation**, not where it is
  delivered. `ccmode` stays: a delivery path that cannot exist is still a
  defect. It just is not the thing standing between here and the menu.

The pattern is the same one as the `readConf` truncation: I had a mechanism that
explained the symptom perfectly, so the investigation stopped one link short of
the thing that was actually broken. "The delivery point is unreachable" is a
complete-sounding story that survives until someone asks *what is being
delivered*.

### FIXED (2026-10-16): 70 permanent shader load failures

```
deScrambleDataDecompression error: Z_DATA_ERROR      x70, every run
```

`withReadFallback` stripped `.z` and served the plain file. A `.z` container is a
4-byte LE32 size prefix followed by zlib, and the guest inflates
**unconditionally** -- `ldrsw x9,[x1]` / `add x2,x0,#4` / `sub x3,x9,#4` /
`bl inflate`. So the fallback could never satisfy the request; it only turned an
honest `ENOENT` into a certain `Z_DATA_ERROR`. The premise comment ("the .z file
is the same bytes as the plain one") was simply false.

`files/.depot/vehicles/shaders/` holds 72 files, every one an uncompressed
`.rgb.pvr`, zero `.z`, while the guest asks for 69 of them as `.z`. Everywhere
else `.z` is the norm (2,668 in vehicles-classic), which is why only this subtree
ever failed and why it looked like a data problem.

Removing the strip is strictly better: the guest enumerates textures as a format
list, issues **separate** requests per spelling (both `...etc.dds.z` and
`...etc.dds` appear as distinct path lines in one log) and walks them in order,
so an honest `ENOENT` lets it proceed to the plain name, which exists.

Ruled out: truncation (0 corrupt `.z` across the volumes), route mismatch
(`rr3veh0` is correct), missing descramble (the 4-byte skip *is* the
de-containerisation).


### FIXED (2026-10-27): the download cache, the first touch hop, and a card that ran out of room

Three separate defects, one session. Each has its own control, and each one
turned out to look like something else first.

#### 1. The ".ast" was unreachable, so the guest's own download cache was a dead end (f1j -> f1k)

```
phase=cc-asset-req ... path=/assets_480x320/.../lexus_is_f350_catalunya/sprites_0.rgb.pvr.z
  want_md5=d367c6f1b923e6551f2692bfa58b05bd answered=0 reason=no-known-answer-shape detail=no-local-file
```

All five candidates were tried and none answered. The file was ON THE CARD:
`.depot/assets_480x320/events/map/featured/weekly_events/lexus_is_f350_catalunya/
sprites_0.rgb.pvr.z.ast`, 65,102 B, md5 `d367c6f1...` -- exactly the digest the
guest asked for. So this was never a missing file.

Two rules that are each correct on their own were fighting:

* `applyVolumeFirstAssetOrder()` moves an asset request to the volume's
  `gui_assets/<res>` spelling, because that copy is the COMPLETE revision and the
  card's `assets_<res>` copy is the game's STALE INSTALLED revision.
* the ".ast" rule in `resolvePath()` then rewrites the primary to the card path
  and CLEARS the fallback, because "a .ast has exactly one home".

Together: the alias moved the request to a spelling no card has, and the
fallback that pointed at the card's real copy was wiped. Both `.ast` candidates
collapsed onto the same dead path, and the file that existed became unreachable.

The fix: the download cache keeps its own spelling. A download cache has no
volume copy at all -- the seven volumes ship no `.ast` -- so there is no
revision to reconcile, and the swap could only move the request off the one file
that exists. The candidate list already tries both spellings (rule 5 of
`rr3_asset_candidates.hpp`), so nothing is lost, and ONE candidate now names
ONE file, which is what lets the digest gate choose between the two.

The self-test's bit 4 carries the guard ("the two spellings of one logical
asset must meet at one file" AND "a download cache keeps its own spelling"),
so a regression stops the guest at the boot gate instead of passing silently.

MEASURED (f1k, archive 20261027-170602-eden-f1k):

| | f1j | f1k |
|---|---|---|
| asset requests answered | 61 / 64 | **64 / 64** |
| unanswered | 3 | **0** |
| non-control fopen-fail | 60 | **0** |
| guest "didn't match its MD5" | 12 | **0** |
| list paths disagreeing | 0 / 8,629 | **0 / 8,633** |
| Eden alive | 437 s (exited on its own) | **1203 s (the full run)** |
| frames | 3,840 | **16,800** |

Ruled out: a missing file (it is on the card, with the requested digest), a
stale list (the base list is rewritten from the one override set, and the
census says 8,633/8,633 agree), and the HTTP slot table (41 unanswered POSTs
was the symptom of the missing answer, not its cause).

#### 2. The touch path's first hop missed the button by 72 pixels (f1l)

The guest never left the daily-rewards popup for a whole run. The touch script
clicked (301, 223) for the calendar's COLLECT button; measured pixel by pixel
off the guest's own framebuffer captures (orange mask r>230, 150<g<215, b<60)
that button is at x 373..533, y 202..242 -- centre (453, 222). The click landed
in the margin.

The consequence was measured, not guessed: the calendar stayed up from frame 300
to frame 4100; the popup opened at frame 4200 because hop 4 (335, 450) -- meant
for the event picker -- landed on a calendar day instead; and hop 2's click on
the popup's COLLECT bar had fired at frame 1900, before the popup existed.

With hop 1 corrected, the guest walked the whole path in one run: daily-rewards
calendar -> reward popup -> main menu -> event picker -> F1 Academy quest ->
car select, with all six hops injected on time.

The lesson worth keeping: a hand-measured coordinate is a claim, and the
framebuffer is the truth. Measure the target out of the pixels before trusting
a script that was measured on another build.

#### 3. The card ran out of room, and the run that died of it looked like a code regression (f1m)

```
phase=assetlist-serve ... consistent=sdmc:/.../cache/asset_list_base.txtCache.txt rc=0 errno=12
```

The run froze at t=29 s with the port and the guest both stopped, and the
emulator's own log ended there too. Nothing about it pointed at the code: the
two rounds before it were byte-identical builds on the same card.

Eden's SD is not a fixed-capacity FAT32 image. It is a directory on the host,
and Eden REPORTS a free space of "4 GiB minus the used space modulo 4 GiB" over
a 12 GiB total -- a SAW-TOOTH that falls towards zero whenever the used space
crosses a 4 GiB multiple (the deploy script measured 1 free byte at
12,884,901,887 in use). The card carries 11.25 GiB of content (seven volumes
plus the downloaded .depot), and this harness' own litter -- three `_stale_*`
stash directories holding the previous runs' 120 MB guest logs, plus the logs
and crash dumps -- was 250,583,479 bytes on top of it.

So: the stash keeps the newest ONE run instead of three (the archive in
`.build/eden-runs/<tag>/` already has a copy of every one), and the harness
prints the card's computed free space before the run and says what to do when
it is short. The port's own `errno=12` is the fingerprint of this failure.

#### 4. What is still open, and where it was measured

* **The guest is in a race.** The track loads (Silverstone, `Total Levelloading:
  18554`); the Tutorial track's two assets now serve with `md5_match=1` after
  the override set learned them (the set is built from the paths the guest has
  ALREADY downloaded, so the first attempt at a new asset class necessarily
  fails and the next one succeeds -- a real flaw, and the fix is to cover the
  paths the LISTS name).
* **Three download errors remain** (`/events.dat`, `/vehicles/carrepairs.repairs`,
  the 960x640 weekly-event atlas) and they are NOT pack-endpoint requests: the
  endpoint answered 76 of 76, and the runtime read of the same files works
  (`fopen-asset size=6418 served=.../carrepairs.repairs`). They take a different
  path and are still to be traced.
* **Two sprites are missing** (`common/manufacturer_logos/logo_ma_da.png`,
  `logo__envo.png`). No atlas on the card and no list anywhere names them: the
  game knows manufacturers (Envoi) that the data pack predates. A data gap, not
  a port defect.
* **The picture is soft because of the CONTENT TIER, not because of scaling.**
  MEASURED (f1k): the engine loads `eds/AndroidHigh.plist`, and at runtime it
  reads `gui_assets/960x640` 640 times and `gui_assets/480x320` 8 times, while
  it draws into a 1280x720 viewport -- so the 2D UI is a 1.33x upscale. The HD
  volume ships a complete `gui_assets/2048x1536` tree (the same 53
  sub-folders) that the guest never picks, even though the port injects the
  folder into the `gui_assets` listing so that it can see it. The tier is the
  engine's own `eds/<tier>.plist` (AndroidLow / Mid / Mid2 / High / XtraHigh /
  UltraWide) and which one it loads follows from the device identity the port
  reports -- and the port reports 640x480 at 160 dpi, while `Platform.
  getScreenWidth/Height` already answers 1280x720. That contradiction is the
  next thing to fix, and the instrument for it is the `gui-assets-listing`
  trace plus the `eds/*.plist` opens. Note that a real-time NEURAL upscaler is
  neither needed nor possible here: the material is on the card, and the tier
  is what leaves it unused.

## 2026-10-28: why the picture is soft -- the real mechanism, two dead ends, and the one lever

The user asked: **"能把画质AI高清化到原生720P吗，看着有点糊"** ("can you AI-upscale the
quality to native 720P? it looks blurry").

The answer is no on the AI part and yes on the substance, but not the way the
round before assumed.  This entry records what was MEASURED, which two
premises had to be thrown away, and where the one real lever is.

### 1. What "blurry" actually is

Not a scaling artefact and not a missing-super-resolution problem.  The guest's
own log (archive `20261028-044415-eden-f1n`, the guest's own `guest-log-any`
channel) says:

```
Searching for settings for device: AndroidHigh
Loaded settings file: AndroidHigh.plist
3DSceneScale = 1.000000 nScreenSize = 1280x720
Adding screen: 0, (1280 x 720)
Res base: 1280x720
Res order:
(960.000000, 640.000000) - 1.500000 - gui_assets/960x640/
(480.000000, 320.000000) - 1.500000 - gui_assets/480x320/
```

* the **3D scene is already native** -- it renders into a 1280x720 viewport at
  scale 1.0, so there is nothing to sharpen there;
* the **2D UI is drawn from a 960x640 atlas into that 1280x720 frame**, i.e.
  every sprite, icon and glyph is a 1.33x bilinear upscale.  That is the blur.

And the material for a sharp UI is already on the card: the HD volume ships a
complete `gui_assets/2048x1536` tree (same 53 sub-folders, `common/
sprites.android.atlas` present) and the guest never picks it.

**A neural upscaler is the wrong tool and is not wanted.**  The Switch has no
NPU, an ESRGAN-class model costs hundreds of milliseconds per frame, and --
decisively -- the missing detail is not missing: it is in the 2048x1536 tree
that the guest is refusing to read.  A super-resolution model would *invent*
detail that the data already has, at 60 frames per second, on hardware that
cannot do it.

### 2. Two dead ends, and why they are dead

**(a) "Res order is empty" is a HISTORICAL state, not the bug.**  The empty
list belongs to the archives from BEFORE the bionic-dirent fix of 2026-10-21
(`20261021-165605-eden-assetalias` and its neighbours).  In the recent
archives the list is `['960x640', '480x320']` every time.  Nothing to fix here,
and changing the tier would not change it either.

**(b) The tier is already the highest one the Android path can reach.**  The
guest computes a small integer tier and turns it into a profile name:

* `0x12b2970` writes "Android" and then appends the name from a five-entry
  table at `0x1ec9868`.  The table is in `.data.rel.ro`, so the names come from
  the `.rela.dyn` addends:

  | tier | addend | string | profile |
  |---|---|---|---|
  | 0 | 0x40ce49 | `Low` | `eds/AndroidLow.plist` |
  | 1 | 0x46cd9e | `Mid` | `eds/AndroidMid.plist` |
  | 2 | 0x4755d8 | `Mid2` | `eds/AndroidMid2.plist` |
  | 3 | 0x47afe7 | `High` | `eds/AndroidHigh.plist` |
  | 4 | 0x496e4b | `XtraHigh` | `eds/AndroidXtraHigh.plist` |

  (`0x12b29cc` handles tier >= 5 by logging "Unknown tier (%d) defaulting to
  high" and answering `High`.)

* the tier itself is set by `0xad53e4`, from a switch on a device hash, and
  the values it can take on the Android path are {0,1,2,3}.  The run loads
  `AndroidHigh.plist`, i.e. tier 3 -- **already the top**.  `AndroidXtraHigh`
  is not reachable from this path.

So "raise the tier" buys nothing, and neither does "fix the empty list".

### 3. The real mechanism: the guest's own atlas filter

`cc::ImageResManager` (ELF `0x10f8cf8`) enumerates `gui_assets/`, parses each
folder name with `sscanf("%ux%u")` and keeps a candidate only if

```
0x10f8ec8  ldur w8, [x29, #-0x54]   ; the width
0x10f8ecc  cmp  w8, w20             ; the SCREEN width
0x10f8ed0  b.ls #0x10f8ee0         ; width  <= screenW  -> KEEP
0x10f8ed4  ldur w8, [x29, #-0x58]   ; the height
0x10f8ed8  cmp  w8, w21             ; the SCREEN height
0x10f8edc  b.hi  #0x10f8e8c        ; height > screenH  -> DROP
```

i.e. **keep iff `width <= screenW || height <= screenH`**.  Against a
1280x720 screen, 2048x1536 fails both tests (2048 > 1280 and 1536 > 720) and
is dropped BEFORE the sort -- so it is never even a candidate.

That is why the port's injection of the folder does not help: the guest DOES
list it (measured, `gui-assets-listing`: `real=480x320 960x640
injected=2048x1536` for the SD volume and `real=2048x1536 480x320 960x640`
for the core volume) and drops it anyway.  **This is not a filesystem
problem.**  The port cannot fix it from the filesystem side.

The screen size is therefore the only lever -- and it is shared with the 3D,
which is the cost.

### 4. The q1 experiment: the Platform screen is NOT the lever (negative result)

To find out WHICH of the port's several screen-size answers the guest reads,
the port grew a runtime knob: the card file
`sdmc:/switch/rr3-native-rehost/rr3-screen.txt` (`platform=WxH`, `ui=WxH`).
An absent file means today's behaviour, so a run without the file is
byte-for-byte the run that was measured before.

Round `q1` (480 s) ran with `platform=2048x1152`.  The port's own log line
proves the override reached the guest:

```
phase=screen-override ordinal=0 platform=2048x1152 ui=0x0 file=sdmc:/switch/rr3-native-rehost/rr3-screen.txt
```

and the guest's own log says it did not matter:

```
Adding screen: 0, (1280 x 720)
Res base: 1280x720
```

So `Platform.getScreenWidth/Height` (both the `jni_bridge.cpp` path and the
`rr3_Platform.cpp` ManagedMethod path) is NOT where the guest's screen size
comes from.  The measurement was worth the round: it removes a candidate.

### 5. Where the guest's screen size REALLY comes from

**`native-switch/source/guest_view_probe.cpp:2505`:**

```cpp
reinterpret_cast<void (*)(void*, void*, int, int, int, int)>(changedTarget)(
    jniEnvironment, activityObject, 1280, 720, 0, 0);
```

The port calls the guest's `onViewChangedJNI` with a hardcoded 1280x720, and
the guest's screen manager builds its screen from exactly that ("Adding
screen: 0, (1280 x 720)"), and so does the atlas filter ("Res base:
1280x720").

**And the machine for the bigger render ALREADY EXISTS.**  The present path in
`guest_view_probe.cpp` (around line 8577) renders the guest into the guest's
own framebuffer and then blits it into the default one:

```cpp
const GLuint presentFbo = guestViewGlStats().lastBoundFramebuffer;
glBindFramebuffer(kReadFramebuffer, presentFbo);
glBindFramebuffer(kDrawFramebuffer, 0);
blit(0, 0, 1280, 720, 0, 0, 1280, 720, GL_COLOR_BUFFER_BIT, GL_NEAREST);
```

So "render bigger, present smaller" is a change of NUMBERS in a path that
already exists, not a new pipeline.  That is also why the 3D would get
2x SSAA for free: a 2048x1152 render blitted down to 1280x720 is
supersampling.

### 6. The exact remaining change, site by site

| # | file:line | what it is | what it becomes |
|---|---|---|---|
| 1 | `source/guest_view_probe.cpp:2505` | `onViewChangedJNI(..., 1280, 720, ...)` | the view size the knob asks for |
| 2 | `source/guest_view_probe.cpp:8600` | the present blit, 1:1 `GL_NEAREST` | scaled blit, `GL_LINEAR`, from (viewW,viewH) to (1280,720) |
| 3 | `source/gfx/gfx_egl.cpp:379` | `eglQuerySurface` answers 1280/720 | the view size |
| 4 | `source/gfx/gfx_gles.cpp:972,977` | `GL_VIEWPORT` / `GL_SCISSOR_BOX` answer 1280/720 | the view size |
| 5 | `source/guest_touch_hw.cpp:70` | `kTouchMapping{1280,720,1280,720}` | digitizer (1280x720) -> view (viewW,viewH) |
| 6 | `source/guest_view_probe.cpp:8302,8346` | the PNG readback, 1280x720 | unchanged (it reads framebuffer 0, the scaled one) |

The open design question is the ASPECT: the HD GUI tree is 2048x1536 (4:3)
and the display is 16:9.  A 2048x1536 view makes the 2D UI exactly 1:1 and
perfectly sharp, but the 3D becomes 4:3 and has to be letterboxed or cropped
at the blit; a 2048x1152 view keeps the 3D correct but leaves the 4:3 atlas
mismatched.  Whichever is chosen, the TOUCH COORDINATES ALL CHANGE and the
touch script has to be re-measured from pixels (hop1..hop7), because the UI
layout moves with the view size.

### 7. A second, independent defect found while looking

`gui/processed/LocalStyles_480x320.gui` and `gui/processed/LocalStyles.gui`
both FAIL to load, three times each, and `LocalStyles_480x320.gui` is
requested because the AndroidHigh profile declares
`GUI_LAYOUT_EXTENSION = "_480x320"`.  **There is no `_480x320` file anywhere
in the seven volumes** (measured: `romfs_ls.py --grep '_480x320'` returns
nothing; the volumes ship `gui/processed/common/LocalStyles.gui` and
per-series copies).  So the guest asks for a stylesheet that does not exist in
this data, every time.  Whether that is what makes the UI look wrong (rather
than merely soft) is not yet established; it is a separate item from the
resolution work.

### 8. What this round changed in the port

* `native-switch/source/gfx/gfx_backend.cpp` -- reads `rr3-screen.txt` once and
  answers the platform/UI screen sizes from it; logs one
  `screen-override` line so a run can say which sizes it used.
* `native-switch/include/rr3/gfx_backend.hpp` -- the four accessors.
* `native-switch/source/jni_bridge.cpp` -- `Platform.getScreenWidth/Height`
  consult the knob; absent the file the answers are unchanged (1280x720).
* `native-switch/source/jni_compat/classes/rr3_Platform.cpp` -- the
  ManagedMethod path consults the same knob.
* `native-switch/source/jni_compat/classes/cc_services.cpp` -- the
  `CCUserInterfaceManager` path consults the `ui=` key.
* `native-switch/card/rr3-screen.txt` -- the experiment input, in the
  repository with the measurement it exists for.
* `tools/Deploy-RR3CardScripts.ps1` -- deploys it with the other four card
  control scripts (5/5 sha256 verified).

Gates after the change: build exit 0; `Test-RR3Inputs.ps1` exit 0; 29/29
`native-switch/tests/test_*.py`; `check-cc-symbols.py` 38 resolved / 16
known-absent / 0 unexpected.

### 9. Still open

* **The view-size change of section 6 is NOT done.**  It is specified site by
  site above and it is the next piece of work; the aspect question has to be
  answered first.
* **The race-metrics probe still reads no values** (`race-metrics ok=0` in
  both f1m2 and f1n; 844 of 846 samples "the object was not found" and 2
  "the key is not in the table").  The guest IS in a race (start lights 7,
  race screen 10, HUD marker 10 in f1n), so the closed loop has nothing to
  steer on and no lap can be counted.  The `RuleSet_RaceMetrics` object was
  found exactly twice, at frames 5580 and 5610, with a valid vptr -- so the
  chain works sometimes and the question is why it stops working.
* The three residual download errors and the two missing manufacturer sprites
  are unchanged (see section 4 of the previous entry).

### 10. q1's own numbers

Round `q1` ran 490 s (480 s requested), 5,040 frames, and the front end came
up normally: `EventMapScreen` 223, `main menu layouts open` 20, `front-end
layout opened` 4, `md5 mismatch` 0, `download failed` 0, `stat(22)` 0,
`pixels lit` 921600/921600.  So reporting a 2048x1152 platform screen did not
break anything -- it simply did not move the guest's screen, which is the
point of the round.  It is a shorter run than f1n and it never reached the
race (`start lights` 0, `race screen` 0); it was not meant to.

## 2026-10-28: the race-metrics probe -- an independent audit, two independent bugs

The race-metrics probe reads no values.  In f1m2 and in f1n alike:
`race-metrics ok=0`, 844 of 846 samples "the object was not found" and 2 "the
key is not in the table".  The guest IS in a race in those rounds (f1n:
start lights 7, race screen 10, HUD marker 10), so the closed loop has
nothing to steer on and no lap can be counted.

The probe was audited against the guest binary from scratch, without taking
the probe's own comments for granted.  The full report is in
`.build/race-metrics-audit.md` (a `.build` artefact, so not in the
repository); this is the part that belongs in the log.

### 1. The two failures are two INDEPENDENT bugs

| failure | share | root cause |
|---|---|---|
| `the-race-metrics-object-was-not-found` | 844 / 846 | the probe looks for the GameMode in the wrong place |
| `the-key-is-not-in-the-table` | 2 / 2 | the value store is implemented for HALF of the guest's structure |

**They are not the same bug, and the 2/846 is not "the lookup nearly works".**
Those two samples are the STABLE form of a lookup that is completely wrong:
the key derivation may be off, or a table may have been thrown away.  The
real measure of that bug is `ok=1` count, which is ZERO.

### 2. The object: the probe looks in three places the guest does not use

The probe's candidates are `CGlobal+{0x10D88, 0x1D88, 0x173F0}`.  The
runtime log (1195/1204 lines in f1m2, 837/846 in f1n) says:

```
cand=0/0,0/0,2121177280/211006cd60
```

* `0x10D88` and `0x1D88` are **0 in every single line**;
* `0x173F0` holds a 32-bit value that is **not a pointer**;
* `0x1D88` is **never referenced anywhere in the guest binary**.  The old
  comment called it "the mis-read where the 0x10000 was dropped" -- it is
  not a mis-read, it is an invented candidate.

The guest's own way to the GameMode is a different chain (0xf2bba8 /
0xf2d100 / 0xf2d10c / 0xf2d118):

```
holder+0x18  ->  h
h+0x5d8      ->  GameMode
```

which is the same map the probe walks (`GameMode+0x128`, the "racemetrics"
entry) -- so the map offset, the node layout, the root offset, the key
string and both vptr constants are ALL CORRECT (each re-verified
independently, including the RTTI: `19RuleSet_RaceMetrics` -> vtable
0x1e4b368 -> address point 0x1e4b378).  The failure happens EARLIER: the
probe never got the right GameMode.

### 3. The value store: two seeds and two tables, where the guest has four

The guest's read (ELF `0x991d1c`) uses **four** seeds (registry +0x00/0x08/
0x10/0x18) and **four** tables (+0x20/+0x48/+0x70/+0x98, counts at
+0x28/+0x50/+0x78/+0xa0).  The port's `member_value_store.hpp` reads two of
them.

What the port has right: the registry address (0x1f97980) and its accessor
(0xb5741c); the key derivation `~(seed ^ id)`; the 32-byte chain node; and
the value formula `~(cell1 ^ cell2)`.

What the port does not have: the guest's own **integrity check**
(0x991e10/0x991e18/0x991e50: `hash(cell3 ^ (cell1 ^ cell2)) == cell4`), and
when the check fails the guest **mints a new id and re-inserts** the value
(0x991e58).  A value read without that check is one the guest considers
invalid.  The header now says so instead of implying otherwise.

### 4. Which member slot -- measured, not argued

The guest's telemetry function reads `this+0x60` and `this+0xa8`; its other
consumer of the same map entry reads `x+0x50`.  The modifier sits inside the
RuleSet at +0x40 and its own slots are at modifier+0x08/+0x20/+0x38/+0x50/
+0x68/+0x88, i.e. RuleSet+0x48/+0x60/+0x78/+0x90/+0xa8/+0xc8.  The two
readings are both arithmetically self-consistent and they name different
members, so **the code cannot settle it**.

The probe therefore measures it: it tries all seven candidate slots, runs
the lookup on each, and logs every one of them in a new `race-metrics-scan`
line.  The next round says which slot the guest's own add names.

### 5. Two failure reasons that were the same string

`the-key-is-not-in-the-table` was returned both for "the chain ran out" and
for "the guest's own stop condition (the node belongs to another bucket)".
They are now `the-chain-ended` and `the-node-is-in-another-bucket`.  The
audit could not attribute the failure because those two were
indistinguishable in the log.

The bucket dispatch is now written the way the guest writes it: the guest
counts the bits of the count (0x99200c / 0x992010) and masks only when
there are fewer than two, rather than the "is it a power of two" form the
port used.  Same answer for every count this port has seen, but the
guest's rule is the one that decides.

### 6. What this round changed

* `native-switch/include/rr3/member_value_store.hpp` -- the two reasons
  split; the bucket dispatch aligned with the guest; the comment rewritten
  to say what is implemented (2 of 4 tables, no integrity check) instead of
  implying a verified read.
* `native-switch/tests/host/member_value_store_selftest.cpp` and
  `native-switch/tests/test_race_metrics_probe.py` -- the self-tests follow.
* `native-switch/source/race_metrics_probe.cpp` -- the candidate-slot scan
  and the `race-metrics-scan` line.

Gates: build exit 0; `Test-RR3Inputs.ps1` exit 0; 29/29
`native-switch/tests/test_*.py`; `check-cc-symbols.py` 38 resolved / 16
known-absent / 0 unexpected.

### 7. Still open

* **The GameMode holder.**  The guest's chain is
  `holder+0x18 -> h -> h+0x5d8 -> GameMode`, but where `holder` sits inside
  the CGlobal is not yet established; that is the missing piece of the 844.
* **The guest's name-hash function** (ELF 0x15c3640) has to be ported before
  the integrity check of section 3 can be performed.
* **The aspect question of the quality work** (the 2026-10-28 entry above):
  4:3 at 2048x1536 makes the 2D UI exactly 1:1, 16:9 at 2048x1152 keeps the
  3D right.  Either way the touch coordinates have to be re-measured.
* `gui/processed/LocalStyles_480x320.gui` is requested and does not exist in
  any of the seven volumes.

### 11. q2: the CGlobal is confirmed, and the probe's GameMode slot is confirmed dead

The scan and the slot watch went into round `q2` (1800 s).  The
`gamemode-slots` line watches five slots once per sample, and it answers
three questions that static analysis could not:

```
phase=gamemode-slots f=30  g=0x210f86bde0 A=0x0 B=0x0        C=0 D=0          E=0x210a6b68a0 eIsVptr=1
phase=gamemode-slots f=60  g=0x210f86bde0 A=0x0 B=0x0        C=2 D=4294967295 E=0x210a6b68a0 eIsVptr=1
... 100 of 105 samples:   g=0x210f86bde0 A=0x0 B=0x2120984280 C=3 D=4294967295 E=0x210a6b68a0 eIsVptr=1
```

**1. The port's CGlobal IS the guest's CGlobal.**  `eIsVptr=1` in every
sample: the first word of the object the port calls `g` is a vtable address
point in the guest's `.data.rel.ro`.  That is the check that says "this is the
guest's object and not a coincidence", and it had never been made before.

**2. The guest's own GameMode slot is dead, not merely misread.**
`A = *(CGlobal+0x10d88)` is **0 in every single sample**.  The guest reads
that slot itself (ELF 0xf152a4) and makes a vtable call through it, so the
offset is right -- the guest simply does not keep a GameMode there while the
front end is up.  It only stores one between creating it (0xb088fc) and
destroying it (0x10af2e4), and the creation is conditional (0xb0895c `cbnz`).

**3. The guest's own index says "no race".**  `D = *(uint32*)(CGlobal+0x10c78)`
is `0xffffffff` (-1) for as long as the front end is up, and the accessor
that uses it (0x1547e68: `ldrsw x8,[x0,#0x288]` / `cmn w8,#1`) returns 0
for an index of -1.  So **the guest itself agrees that there is no GameMode
to read** -- the probe is not being refused, there is nothing there.

**4. The scan over the CGlobal's neighbourhood found nothing.**
`gamemode-scan: vptrCands=0 tried=0 found=0x0 ctrlApplicable=0` -- not a
single pointer in the 128 KB around the CGlobal has a vtable as its first
word.  So the GameMode is not reachable from the CGlobal's own neighbourhood
either, which is consistent with the audit's finding that the lap counter's
holder is the first argument of ELF 0xf2b508 and that one of that
function's two call sites passes a **stack** object.

**5. The third candidate slot changes shape over the run.**
`B = *(CGlobal+0x173f0)` was 0 in the first samples, and then became
`0x2120984280` -- a live heap pointer -- for 100 of the 105 samples.  In f1n
the same slot held a small integer (`2121177280`).  The guest uses this slot
as a POINTER (it is fed to 0xc15fd8), so the small integer in f1n means the
slot's contents are not stable, and the probe's third candidate was reading
whatever happened to be there.

### 12. What this changes about the plan

* **The probe's "GameMode" concept is not where the guest keeps a race's
  rulesets.**  The guest reaches them through an indexed accessor on a
  holder, and the index is -1 whenever no race is running.
* **The heap scan that was already there is aimed at the wrong regions.**
  `findMetricsInHeap()` looks for the RuleSet_RaceMetrics vptr in up to
  eight regions anchored on the CGlobal and its own pointers.  It has never
  hit, and the reason is now visible: the race objects are not in the
  CGlobal's region.
* **The next probe to write is not a vptr search but a STRING search.**  The
  map node's key is the inline std::string "racemetrics" (SSO), so the node
  is a 10-byte pattern in the heap, and the node's value at +0x38 is the
  RuleSet_RaceMetrics -- which the port can then check against the vptr it
  already knows.  That reaches the object WITHOUT going through the
  GameMode at all, which is the whole point: the GameMode is not on the path
  the port can see.
* **And the question the round also has to answer: how long is the race
  actually?**  The `RuleSet_RaceMetrics` was found in exactly TWO samples
  (frames 5580 and 5610) in f1n -- sixty frames -- and the guest's own
  scene state was 2 or 3 in every sample.  If the guest is only "in a race"
  for a second, then no probe sampling every thirty frames can watch a lap,
  and the fix is to make the race LAST, not to make the probe quicker.

### 13. The value store was reading the wrong PAIR of cells

The audit said the value store "implements 2 of the guest's 4 tables" and left
open which of the four carried the value.  The guest's own read path answers
that, and the answer is not what the port assumed.

`ELF 0x991d1c`, the function that reads a member's value:

```
0x991d64  add  x0, x21, #0x20   ; TABLE 1
0x991d6c  eon  x8, x8, x20      ; key0 = ~(seed0 ^ id)
0x991d88  bl   0x991fd8         ; -> the node
0x991d8c  ldr  x8, [x0, #0x18]  ; the node's value-cell pointer
0x991d9c  ldr  x23, [x8]        ; cell1
0x991d90  ldr  x9, [x21, #8]    ; seed1 = reg+0x08
0x991d94  add  x0, x21, #0x48   ; TABLE 2
0x991da0  eon  x8, x9, x20      ; key1
0x991db8  bl   0x991fd8         ; -> the node
0x991dcc  ldr  x8, [x8]         ; cell2
0x991dc0  ldr  x9, [x21, #0x10]  ; seed2 = reg+0x10
0x991dc4  add  x0, x21, #0x70   ; TABLE 3
0x991dd0  eon  x9, x9, x20      ; key2
0x991dec  bl   0x991fd8         ; -> the node
0x991e00  ldr  x8, [x8]         ; cell3
0x991de4  eor  x25, x8, x23     ; *** x25 = cell3 ^ cell1 ***
0x991e10  eor  x8, x8, x25      ; cell2 ^ cell3 ^ cell1
0x991e18  bl   0x15c3640        ; fnv1a64 of those eight LE bytes
0x991e24  add  x0, x21, #0x98   ; TABLE 4
0x991e44  bl   0x991fd8         ; -> the node
0x991e4c  ldr  x8, [x8]         ; cell4
0x991e50  cmp  x23, x8          ; the hash == cell4 ?
0x991e54  b.eq #0x991e70        ; yes -> done
0x991e58  bl   0x991eac         ; no -> mint a NEW id and re-insert
0x991e88  mvn  x0, x25          ; *** THE VALUE = ~(cell3 ^ cell1) ***
```

**THE VALUE IS MADE OF CELLS 1 AND 3.**  The port computed `~(cell1 ^ cell2)`
-- the wrong pair -- and looked the member up in tables 1 and 2 only, so every
walk failed with "the key is not in the table" and no number ever came out.

**TABLES 2 AND 4 ARE THE INTEGRITY CHECK**, not the value: the guest hashes
`cell1 ^ cell2 ^ cell3` and compares the result with the cell in table 4; if
they differ it MINTS A NEW ID and re-inserts the value (0x991e58), i.e. it
treats the value it read as tampered with.  The port cannot re-key (that
would mutate the guest), so it performs the check and REPORTS the verdict.

**THE HASH IS FNV-1a 64-BIT, byte for byte** (ELF 0x15c3640):

```
0x15c3648  mov  x9, #0x1b3
0x15c3650  movk x9, #0x100, lsl #32   ; the prime 0x100000001b3
0x15c3654  ldrb w10, [x0], #1
0x15c3658  eor  x10, x2, x10           ; h = h ^ byte
0x15c3660  mul  x2, x10, x9            ; h = h * prime
```

with the offset basis `0xcbf29ce484222325` built at 0x991df4-0x991e0c -- the
standard FNV-1a 64-bit basis.  So the check is reproducible in the port, and
the probe now prefers a candidate slot whose cells SATISFY it, because a
wrong slot can produce a wrong number.

### 14. What q2 measured, in one table

| question | answer | how |
|---|---|---|
| is the port's CGlobal the guest's CGlobal? | **YES** | `eIsVptr=1` in every sample: the object's first word is a vtable address point in `.data.rel.ro` |
| is `CGlobal+0x10d88` the guest's GameMode slot? | **YES, but only during a race** | `A=0` in the front end, `A=0x212b981da0` while the scene state is 1 (114 of 298 samples) |
| does the guest's own index agree? | **YES** | `D = *(uint32*)(CGlobal+0x10c78)` is -1 in the front end and 8 during the race; the accessor at 0x1547e68 returns 0 for an index of -1 |
| is the GameMode reachable from the CGlobal's neighbourhood? | **NO** | the scan found `vptrCands=0` -- not one pointer in 128 KB has a vtable as its first word |
| was the object found? | **YES, in 155 of 341 samples** | `rs=0x212bd00640 vptrOk=1` |
| did the lookup read a value? | **NO** | every one of the seven candidate slots answered `ok=0` -- the wrong pair of cells, section 13 |

**AND THE RACE IS NOT SHORT.**  The scene state was 1 (racing) for 114 of the
298 samples, i.e. more than three thousand frames of the run -- so the guest
really is in a race for minutes at a time, and the earlier reading of "two
samples, sixty frames" was the probe MISSING the race, not the race being
short.  That closes the last open question of the previous entry.

## 2026-10-28 (evening): the picture is sharp now -- and the harness was killing the guest

### 1. The headline, measured by the guest itself

The guest's own log, q5 (archive `20261028-165042-eden-q5`), with
`rr3-screen.txt` set to `platform/ui/view = 2048x1536`:

```
phase=view-size   view=2048x1536 present=160,0 960x720
phase=guest-log-any  Res base: 2048x1536
phase=guest-log-any  Res order:
phase=guest-log-any  (2048.000000, 1536.000000) - 1.333333 - gui_assets/2048x1536/
phase=guest-log-any  (960.000000, 640.000000) - 1.500000 - gui_assets/960x640/
phase=guest-log-any  (480.000000, 320.000000) - 1.500000 - gui_assets/480x320/
phase=guest-log-any  share) Current res: 2048, 1536
```

Compare q4, same binary, `view` absent:

```
Res base: 1280x720
Res order:
(960.000000, 640.000000) - 1.500000 - gui_assets/960x640/
(480.000000, 320.000000) - 1.500000 - gui_assets/480x320/
```

**The 2048x1536 tree is now the FIRST entry of the guest's own resolution
order.**  That is the whole of "the picture is soft": the 2D UI was drawn
from a 960x640 atlas blown up 1.33x into a 1280x720 frame.  It is now drawn
from the 2048x1536 atlas and downscaled, i.e. a 2.1x DOWNSCALE of sharp
source art.

### 2. Two independent confirmations that the HD tree is really being used

Not "the guest said so" -- two more instruments:

| instrument | q4 (1280x720) | q5 (2048x1536) |
|---|---|---|
| `Failed to load image from atlas` | 125 | **0** |
| `LocalStyles` load failures | 3 + 3 | **0** |
| `assetlist-census-atlas` sample | 960x640 tree | `/gui_assets/2048x1536/achievements/sprites.android.atlas` |
| `asset-invariant ... agree=1 src=rr3gui0:` | (960x640 tree) | **240/240 on the 2048x1536 tree** |

`rr3gui0` is `gui-hd.romfs` (1,663,784,980 bytes), mounted at
`rr3gui0:/files/.depot/gui_assets/2048x1536`.  The HD tree was on the card
ALL ALONG and the guest simply refused to look at it.

### 3. A reading of mine that was WRONG, and the correction

I had written that "the card's 2048x1536 tree holds only 2 files (1 MB), so
the HD assets are not there".  **That was wrong**, and the way it was wrong
matters:

* `E:\Eden\user\sdmc\switch\rr3-native-rehost\.depot\` is the port's
  **override/target tree**, not the asset store.  Two files there are the
  residue of the override set, nothing more.
* The asset store is the seven romfs **volumes** (`data/*.romfs`), and
  `gui-hd.romfs` is 1.66 GB of the real 2048x1536 tree.

So the data was never missing.  The only thing that was missing was the
guest's willingness to read it -- its own filter (ELF 0x10f8ec8) drops a
folder unless `width <= screenW || height <= screenH`, and 2048x1536 fails
both against a 1280x720 screen.

### 4. The port change: FIT, never stretch

A 4:3 view stretched into a 16:9 screen turns every circle into an ellipse,
so the present path now scales the view to FIT and centres it:

* `gfxFitRect()` (inline in `gfx_backend.hpp`, so a host test can reach it)
  returns the rectangle the view occupies: 2048x1536 on 1280x720 is
  **960x720 at x=160** -- two 160-pixel pillarbox bars, and the guest's
  pixels are never stretched.  The guest's own log line
  `view=2048x1536 present=160,0 960x720` is the instrument that says the
  rectangle is the intended one.
* The present blit draws into that rectangle and clears the bars.
* The guest's scissor BOX and its viewport are never touched.  The viewport
  is CONTEXT state, not framebuffer state: changing it without putting it
  back would change how the guest renders into its own FBO on the next
  frame.  Only the scissor test's enable bit is touched, and it is put back.

### 5. The touch mapping has to use the SAME rectangle

A tap is a tap on the OUTPUT screen, and the content is no longer the whole
output.  So the mapping carries the rectangle, not just the sizes:

* a tap inside the rectangle lands on the button the player sees;
* a tap on a black bar presses NOTHING;
* a finger that slides onto a bar is RELEASED, so the guest does not keep a
  button held for a finger that is no longer on its screen;
* a point off the DIGITIZER keeps the historical answer -- clamped into the
  view, not dropped.  (This last one is a distinction the first
  implementation got wrong, and the existing host test caught it: "a
  coordinate past the view is clamped to the view's last pixel".)

The host harness grew from 55 to 68 checks; the two new cases are the
present rectangle and the letterboxed touch.

### 6. q5 did not reach the menu -- and the HARNESS killed it, not the guest

This is the part worth recording, because it is a defect in the measuring
instrument and it would have been read as a defect in the port.

The run ended at wall 152 s with
`guest log stable for 120s (no growth since t=30s)`.  The guest had rendered
**3 frames**.  The naive reading is "the 4:3 view wedges the guest".

**It does not.**  The host CPU series is the discriminator, and the harness
documents it itself ("a spin shows as a steady climb and a deadlock collapses
toward zero"):

| | q4 (1280x720) | q5 (2048x1536) |
|---|---|---|
| CPU delta per 10 s tick, steady state | 10.6 - 15.4 s | **6.3 - 7.1 s** |
| shape | steady climb | **steady climb** |

q5's CPU climbed from 41.2 s at t=30 to 120.3 s at t=150 -- a steady 0.65
core.  A deadlock collapses toward zero; this did not collapse.  **The guest
was ALIVE and booting a 3.15x larger view with the HD atlases, and the
harness killed it while it worked.**

The rule that ended the run (`$stableTickLimit = 12` ticks of a quiet log)
never consulted the CPU, even though the CPU series two lines above it is
the harness's own spin-vs-deadlock discriminator.  **Fixed**: a quiet log
only counts towards the limit when the CPU has stopped climbing
(`$cpuAliveThreshold = 1.0` CPU-second per 10-second tick -- a live guest
burns 6.3 s, a wedged one collapses to zero).

### 7. Four hypotheses for the q5 stop, each killed by a measurement

| hypothesis | how it was killed |
|---|---|
| the guest crashed | Eden's log has no exception, no abort, no fault; the process was killed by the harness (`Stop-Process -Force`), and its CPU was still climbing at the end |
| the card ran out of room (the archived ENOSPC mode) | host E: has **207 GiB free**; the card had 581 MB free at the start of q5 and 775 MB at the end |
| the asset lists were not delivered | both runs requested the **same 400 lists with identical names** (set difference: 0 and 0), and the rewritten lists are byte-identical (`lists_touched=386 entries_rewritten=2982 bytes=2610656` in both) |
| the port broke the rendering | the first three `gfx-stats` lines are **identical** in q4 and q5 (`frames=1 calls=5 draws=5/0`, `frames=2 calls=8 draws=10/0`, `frames=3 calls=11 draws=15/0`, `compile=90 link=46 prog=46 sh=90 tex=133`) |

### 8. What is still open

* **q6** re-runs the same configuration with the CPU-aware watchdog.  If the
  guest reaches the menu, the 4:3 view is viable in Eden (and trivially
  viable on real Switch hardware, which has a real GPU).
* The touch script's pixel coordinates have to be re-measured: the content is
  no longer full-screen, it starts 160 pixels from the left.
* The race-metrics probe (the goal's other blocker) is untouched by this
  round.

## 2026-10-28 (night): the value store -- two bugs, and the port's own test had pinned the wrong one

### 1. The headline

The lap counter read nothing, ever: 7 candidate slots, two independent
probes, one error string (`the-node-is-in-another-bucket`), 100% failure.
Two INDEPENDENT bugs caused it, and one of them had been introduced by the
port's own "fix" two rounds earlier.

### 2. Bug #1: the bucket slot is NOT the node

The guest's lookup (ELF 0x991fd8) takes the bucket slot and then
dereferences it:

```
0x992024  ldr  x20, [x8, x24, lsl #3]   ; x20 = buckets[index]
0x992070  ldr  x20, [x20]                ; x20 = *buckets[index]  <-- ONE MORE
0x992078  ldr  x9,  [x20, #8]            ; the node's key
```

The slot holds the ADDRESS of a cell whose first word is the node.  The
port used the slot as the node, so it read a garbage key and the guest's own
"this node belongs to another bucket" stop condition fired on every single
lookup -- which is precisely the error string that was observed, 100% of the
time.

**The chain links NODE to NODE** (ELF 0x9920e0 stores the previous head
NODE into the new node's first word), while the SLOT holds the cell.  Getting
that distinction wrong breaks the collision case, which is what the host
self-test caught.

### 3. Bug #2: the wrong PAIR of cells -- and the port's test banned the right one

The value is assembled in two instructions:

```
0x991de4  eor x25, x8, x23   ; x25 = cell2 ^ cell1
0x991e88  mvn x0, x25         ; the value is ~(cell1 ^ cell2)
```

`x8` holds **table 2's** cell and `x23` **table 1's** cell.  The trap is the
INSTRUCTION ORDER: `0x991de4` executes BEFORE the table-3 lookup at
`0x991dec`, so `x8` still holds table 2's cell when the XOR runs.  An
earlier reading took `x8` for table 3's cell and made the port compute
`~(cell1 ^ cell3)` -- and the port's own unit test then ASSERTED that the
correct `~(cell1 ^ cell2)` must NOT appear in the source.

**That test was the load-bearing error.**  It is why the wrong formula
survived a review: the thing that checked it was wrong in the same way.

The full shape of the guest's read (ELF 0x991d1c), now reproduced exactly:

| step | guest | port (now) |
|---|---|---|
| table 1 (reg+0x20, seed reg+0x00) | cell1 | node1 -> cell1 |
| table 2 (reg+0x48, seed reg+0x08) | cell2 | node2 -> cell2 |
| **the value** | `mvn x0, x25` where `x25 = cell2 ^ cell1` | `~(cell1 ^ cell2)` |
| table 3 (reg+0x70, seed reg+0x10) | cell3 | node3 -> cell3 |
| table 4 (reg+0x98, seed reg+0x18) | cell4 | node4 -> cell4 |
| the integrity check | `fnv1a64(cell1^cell2^cell3) == cell4` | the same |

The port used to read the value out of tables 1 and 3 and run the check on
tables 2 and 4 -- the wrong two tables for each half.

### 4. How the contradiction was settled

Two sub-agents disagreed about the XOR (one said cell3, the other cell2) and
a third claim -- "the bucket slot is a cell address" -- was easy to believe
and easy to get wrong.  Neither was voted on.  The disassembly was read
again, instruction by instruction, and the order of the instructions
settles it: `x8` is table 2's cell at `0x991de4` because table 3 has not
been looked up yet.

The same method settled the slot question: `0x992024` takes the slot,
`0x992070` dereferences it, and `0x9920e0` shows what the chain links.

### 5. What changed

* `member_value_store.hpp`: `decodeValue(cell1, cell2)`; `findChainNode`
  dereferences the slot once more; `lookup()` reads the value out of tables
  1 and 2 and runs the integrity check on tables 3 and 4.
* The host self-test builds the bucket slot the way the guest's insert does
  (a cell whose first word is the node) and links node to node.
* The python mirror of the algorithm models the same thing.
* The static assertions were INVERTED: they now require `cell1 ^ cell2` and
  forbid `cell3 ^ cell1`, and they require the extra dereference.

Gates: build exit 0, Test-RR3Inputs exit 0, 29/29 python tests, cc symbols
38 resolved / 16 known-absent / 0 unexpected.

### 6. Still open

* **The lap counter has to be RE-MEASURED.**  Everything above is a reading
  of the guest's algorithm; the value has not yet been observed coming out
  of a live run.
* The lap member's exact slot is now known statically:
  `RuleSet_RaceMetrics + 0x60` is `m_nTotalLaps`, and the guest reads it
  through `0x991c8c` (the accessor that RE-KEYS the member, so the port
  must not call it).  The port's own probe resolves the member id from the
  slot and then reads the global value store, which is what this fix
  repairs.

## 2026-10-28 (night, part 2): 16:9 AND the HD tree -- the best of both

### 1. The headline

The picture is soft because the guest draws the 2D UI from a **960x640** atlas
into a **1280x720** frame: a 1.33x upscale of every sprite and glyph.  The
HD tree (`gui_assets/2048x1536`, 1.66 GB in `gui-hd.romfs`) is on the card
all along; the guest's own filter (ELF 0x10f8ec8, `width <= screenW ||
height <= screenH`) drops it against a 1280x720 screen.

**The screen size is the only lever, and 16:9 is enough.**

### 2. q6: the 4:3 view, and what the harness got wrong

`view=2048x1536` (4:3), archive `20261028-170318-eden-q6`:

* the guest reported `Res base: 2048x1536` and put
  `gui_assets/2048x1536/` FIRST in its own `Res order:`;
* `Failed to load image from atlas` went 125 -> 0 and the `LocalStyles`
  failures disappeared;
* the port served the real HD atlases out of `gui-hd.romfs` with
  published == served digests, 240/240;
* the run reached the main menu (Chinese UI, `Menu_Music.mp3`, 14880 frames
  in 1207 s).

The catch was the 4:3 view itself: the 3D scene is 4:3, so the present path
has to letterbox it into 960x720 with two 160-pixel bars.

### 3. q5: the run that was killed while it was working

q5 was ended at 152 s with "guest log stable for 120s".  The guest had
rendered 3 frames.  The naive reading is "the 4:3 view wedges the guest".

**It does not.**  The harness's OWN discriminator -- the CPU series, "a spin
shows as a steady climb and a deadlock collapses toward zero" -- says
otherwise, and the numbers are unambiguous:

| | q4 (1280x720) | q5 (2048x1536) |
|---|---|---|
| CPU delta per 10 s tick, steady state | 10.6 - 15.4 s | 6.3 - 7.1 s |
| shape | steady climb | **steady climb** |

q5's CPU went from 41.2 s at t=30 to 120.3 s at t=150 -- a steady 0.65 core.
A deadlock collapses to zero.  **The guest was alive and booting a 3.15x
larger view, and the harness killed it while it worked.**  The rule that
ended the run never consulted the CPU series, even though the CPU series is
the discriminator the script documents two lines above it.  Fixed: a quiet
log only counts towards the limit when the CPU has stopped climbing.

### 4. q7: 16:9 gets the HD tree too, and there is not a single black bar

The guest's comparator inside `ImageResManager` (ELF 0x10f9948) is an inlined
`fcmp`/`cset`/`csel` on the candidate's **HEIGHT** (node+0x14) and then its
**WIDTH** (node+0x10) -- **not** on the aspect ratio -- and the third filter
is gated on the `ALLOW_RETINA` setting, which is TRUE in the runtime
settings.  So the winner is the candidate with the largest height, and the
aspect ratio of the screen does not decide it.

MEASURED, archive `20261028-170318-eden-q6` and the q7 run:

```
view=2048x1152  present=0,0 1280x720
Res base: 2048x1152
Res order:
(2048.000000, 1536.000000) - 1.333333 - gui_assets/2048x1536/
(960.000000, 640.000000) - 1.500000 - gui_assets/960x640/
(480.000000, 320.000000) - 1.500000 - gui_assets/480x320/
share) Current res: 2048, 1152
```

**The 2048x1536 tree is FIRST at a 16:9 view.**  And because 2048x1152 has
the same aspect ratio as the 1280x720 output, the present path is a plain
downscale: `present=0,0 1280x720`, no bars, no stretch, and the touch
mapping falls back to the identity, so the touch script's recorded pixel
coordinates still work unchanged.

| | 1280x720 (old) | 2048x1536 (4:3) | **2048x1152 (16:9)** |
|---|---|---|---|
| 2D atlas | 960x640, 1.33x upscale | 2048x1536 HD | **2048x1536 HD** |
| 3D aspect | 16:9 | 4:3 | **16:9** |
| screen | full | two 160-px bars | **full** |
| touch script | valid | needs re-measuring | **still valid** |

### 5. The capture was looking at the wrong picture

The q6 frames looked like a complete 16:9 menu with a cut-off element at the
right edge.  They were not the screen: the readback samples the currently
bound framebuffer, and before the present that is the GUEST'S OWN FBO, so a
1280x720 readback of a 2048x1536 one is a CROP.  The capture now happens
after the present blit, where the default framebuffer holds exactly what
the player sees.

### 6. Still open

* The lap counter's value store is FIXED (see the previous entry) but has
  not been observed coming out of a live run yet.
* The touch script's coordinates are unchanged by this configuration, which
  is the first time that has been true of any change to the view size.

## 2026-10-28 (night, part 3): the value store READS -- measured, and the picture is back on

### 1. The A/B is done, and the value store went from 100% failure to 100% success

q8 was the clean A/B against q4: the SAME 1280x720 layout, the SAME touch
script, the only difference being the value-store fix in commit 05ab0ecd.
Archive `20261028-213445-eden-q8`, wall 1208 s, 475,108 guest log lines.

| | q4 (before the fix) | q8 (after the fix) |
|---|---|---|
| `race-metrics-scan` lines | 168 | **717** |
| of those, `ok=1` | **0** | **717** |
| `lapsWhy` | `the-node-is-in-another-bucket` | **`none`** (the guest's own check passed) |
| the member id | one | **699 different ids**, all followed |

`none` is the strongest statement available about a pure read: it is the
GUEST's own checksum agreeing with the port.  The guest hashes
`cell1 ^ cell2 ^ cell3` and compares the result with the cell in table 4; the
port computes the same four cells and the same hash, and they matched on
every single one of the 717 samples.

The 699 different ids are the other half of the proof: the id in a slot is a
ROTATING token (the guest's accessor re-keys the member on every read), so a
port that cached an id would report a stale answer.  The port re-reads the
id every sample and got 717 valid answers.

**AND THE VALUE WAS RE-DERIVED BY HAND FROM THE LOG.**  One line:

```
lapsCell1=0x302fddb200f1a29c lapsCell2=0xcfd0224d00f1a29c  reported laps=-1
```

`cell1 ^ cell2 = 0xffffffff00000000`, `~that = 0xffffffff = -1`.  The port's
answer is the guest's answer, computed twice by different routes.

### 2. The read path was re-checked against the BYTES, not against the summary

The previous entry's reasoning about the guest's read path was sound but its
ADDRESSES were listed out of order, which is exactly what had produced the
wrong formula in the first place.  Re-disassembled in address order
(0x991d1c..0x991ea4, 0x991fd8..0x992228, 0x15c3640..0x15c366c):

```
0x991dcc  ldr  x8, [x8]      ; cell2   <- table 2's cell
0x991de4  eor  x25, x8, x23  ; x25 = cell2 ^ cell1     (x8 is STILL cell2 here)
0x991dec  bl   0x991fd8      ; table 3's lookup
0x991e00  ldr  x8, [x8]      ; cell3   <- loaded AFTER the eor
0x991e10  eor  x8, x8, x25   ; cell3 ^ cell2 ^ cell1
0x991e18  bl   0x15c3640     ; fnv1a64 of those 8 LE bytes
0x991e20  mov  x23, x0       ; the hash moves into x23
0x991e4c  ldr  x8, [x8]      ; cell4
0x991e50  cmp  x23, x8       ; the hash == cell4 ?
0x991e88  mvn  x0, x25       ; THE VALUE = ~(cell1 ^ cell2)
```

The eor at 0x991de4 sits BETWEEN table 2's cell load (0x991dcc) and table
3's cell load (0x991e00), so the value is `~(cell1 ^ cell2)` and tables 3
and 4 carry the integrity check.  The FNV is the standard FNV-1a 64
(`h = (h ^ byte) * 0x100000001b3`, basis 0xcbf29ce484222325), byte for byte
what 0x15c3640 does.

### 3. The bucket slot is not the node -- now proved in TWO places in the guest

```
lookup : 0x992024  ldr x20, [x8, x24, lsl #3]   ; x20 = buckets[index]
         0x992070  ldr x20, [x20]                ; x20 = *buckets[index]
insert : 0x9920d4  ldr x8,  [x9, x24, lsl #3]    ; x8 = buckets[index]
         0x9920dc  ldr x9,  [x8]                 ; x9 = *buckets[index]
         0x9920e0  str x9,  [x20]                ; the NEW node's next
```

The new node's next field has to become the OLD HEAD NODE, and only
`*buckets[index]` can be that.  If the slot held the node itself, `[x8]`
would be the old head's NEXT pointer and the chain would lose its head.  The
slot therefore holds the ADDRESS of a cell whose first word is the node.
The node layout is 32 bytes, written by the guest's own insert path:
+0x00 next, +0x08 key, +0x10 key again, +0x18 pointer to the value cell.

The same instruction (0x992070) serves as "slot -> node" on the first turn
of the walk and "node -> next" on every turn after it, which is why the
extra indirection was easy to miss: it looks like the chain step.

### 4. Which member is "Total Laps" -- the guest's own label code

```
0xb358cc  add  x0, x19, #0x60   ; &RuleSet->member
0xb358d0  bl   0x991c8c         ; read it (the re-keying accessor)
0xb358dc  adrp x8, #0x40d000    ; "Total Laps"
0xb358e4  mov  w1, x0           ; the value, as an integer
0xb358f0  b   0xb71164          ; set the label
```

So `RuleSet_RaceMetrics + 0x60` IS the member the game calls "Total Laps"
(and +0xA8 is "Top Speed (km/h)", +0xC8 is "Average Speed (km/h)").  The
port's slot table was already right.

### 5. WHY THE LAP COUNTER IS STILL -1 -- and it is NOT a reading failure

Two INDEPENDENT instruments read the same thing:

* the value store: `laps=-1`, `lapsOk=1`, `lapsWhy=none`
* the race-state probe, from a different path: `laps=4294967295`

and the trajectory says why:

```
f=8190   total=105126  rate=3.28     flips=0
f=9210   total=125841  rate=-0.05    flips=1
f=9630   total=125841  rate=0.0      flips=1
f=21630  total=125861  rate=0.0      flips=17
f=27060  total=125869  rate=0.016    flips=24
```

The car drove 125,841 spline units and stopped dead, and the port's own
autopilot reversed its steering direction 24 times without ever getting
free (`bestRate` since the last flip: 0.000000).  The guest agrees that no
lap was completed.  **So the remaining problem is a DRIVING problem, not a
reading problem, and the value store is no longer on the critical path.**

### 6. Two harness defects found and fixed while measuring this

**(a) The captured frames were never taken off the card.**  The port writes
one PNG per captured frame; a round writes hundreds and each one is about
1.7 MB.  q8 left 380 of them on the card -- 495 MB -- and the run script
only cleared them at the START of the NEXT round.  With the card's free
space reported on a 4 GiB saw-tooth, that is what would have killed the
next round: 158 MB free against a round that needs about 600 MB for its own
frames and log.  The frames are now deleted from the card once they are in
the archive, and the deploy script's litter set counts them too.

**(b) The emulator guard asked the wrong question.**  It refused to deploy
whenever ANY emulator was running, by NAME.  The emulator the user happens
to have open (a Ryujinx under
`E:\AI\WANGAN_SWITCH_STARTER\work\citron-run\ryujinx`, on
`%APPDATA%\Citron`) holds not one byte of this card, so the guard was
blocking the cleanup the card needed.  The guard now asks the real
question -- does a file this script is about to write have an exclusive lock
on it? -- and Eden stays an unconditional refusal, because Eden's SD card
IS this card.  Both halves were measured:

* positive control: an exclusive lock held on the card's NRO => the script
  stops with `locked: ...\rr3-native-rehost.nro`
* negative control: the user's own Ryujinx running, holding none of our
  files => the script continues, frees 692,219,767 bytes and re-verifies all
  8 volume hashes (0 mismatches)

The card's free space went from 158 MB to 811 MB.

### 7. The card goes back to the HD configuration

The 1280x720 layout was only ever there to make the A/B against q4
possible.  The picture the port is for is `view=2048x1152` (HD 2D tree,
16:9 3D, full 1280x720 screen, no bars), so `rr3-screen.txt` is back to
that, deployed and verified 5/5.

**AND A STALE CLAIM IN THE PREVIOUS ENTRY IS CORRECTED HERE.**  That entry
said the touch script's recorded pixel coordinates "still work unchanged" at
2048x1152, "the first time that has been true of any change to the view
size".  The measurement said otherwise: in q7 the script sat on the
full-screen event card and never reached a race (`msStartLights 0`).  The
coordinates are in the 1280x720 SCREEN space, but the guest lays its front
end out differently at a 16:9 view, so the same pixel lands on a different
widget.  The script has to be re-measured against the HD frames before an
automated round can drive into a race again; the guest's own red glove
cursor shows where it wants to be pressed (measured in the q7 frames at
about (762, 138)).

### 8. Still open

* **The car has to complete a lap.**  Everything the port needs to READ is
  proven; what is missing is a car that drives.  The port's autopilot steers
  with a magnitude of 0.05 and can only flip its sign when the progress rate
  goes negative, which cannot recover a car that is stopped against a
  barrier.
* **The touch script has to be re-measured for the HD layout**, or made
  screen-driven (the port currently identifies only the base class
  `N2cc9CloudcellE`, not the concrete screen).

## 2026-10-28 (night, part 4): the car could not turn -- four defects in the drive loop

### 1. What the 2026-10-28 q8 race actually was

The value store was fixed (the previous entry), the race started (the run
summary says `start lights 7`, `race screen 10`, `race marker: HUD string 10`),
and the guest's own race clock ran from 0 to 620,402 ms.  The car drove
**491.68 m** of a **7,056 m** lap and then stopped dead.

The unit is the guest's own: its HUD label "Spline Distance (metres)" at ELF
0xb35814 multiplies `m_nTotalSplineDistanceTravelled` by 1/256, so
`total = 125,870` is 491.68 m and `m_nLapDistance = 1,806,336` is
**7,056.0 m exactly** -- the lap length, constant from the first sample of
the race.  The track is **Spa-Francorchamps** (`Tracks/spa/...`).

So the car reached **6.97 %** of the lap and ran out of road at the end of
the start/finish straight.  It did not finish a lap, and the guest's own
"Total Laps" member (`RuleSet+0x60`) never moved off `0xFFFFFFFF` -- which
is the guest agreeing, independently of the port.

### 2. FOUR defects, all measured

The diagnosis was done twice, independently: once by me from the archived
log, and once by a sub-agent that re-implemented `updateDrive()` and
compared it with the log -- **713 of 713 samples match on steer, mag, sign
and best, 0 mismatches**.  Both readings agree on the outcome; the
sub-agent's mechanism is the sharper one.

**(a) THE STEERING FLOOR WAS A NUDGE.**  The magnitude sweep's lower clamp
was 0.05, i.e. **5 % of the steering axis**.  The guest's own controller
state confirms it was really transmitted: `ax[0] = +-0.050` with button 0
(the throttle) held down.  A car that cannot turn runs straight into the
first corner's barrier.

**(b) THE SERVO'S ANCHOR WAS NEVER SET.**  `g_drive.bestMagnitude` is
zero-initialised, and the branch that would have assigned it
(`if (rate > 0 && rate > bestRate)`) was **dead code**: `bestRate` was
raised to the running maximum on EVERY frame, so at the sweep tick the
comparison was always false.  The "step back to the best magnitude" branch
therefore wrote **0** into the magnitude, and the 0.05 clamp pinned it there
for the rest of the race.  Measured duty cycle: `mag == 0.050` on **43.4 %**
of the samples, `|steer| <= 0.11` on **67.5 %**, and the autopilot never once
asked for more than **41 %** of lock.

**(c) "STUCK" WAS READ AS "GOING THE WRONG WAY".**  The sign (the direction
AROUND THE track) flipped when the progress rate went below -0.0001.  A car
grinding against a barrier jitters the telemetry by a unit or two, so the
port read that jitter as "backwards" and reversed the direction around the
circuit: **24 sign flips** in one race, while the car never moved.

**(d) THE STALL DETECTOR COULD NOT FIRE, AND THE RECOVERY COULD NOT WORK.**
`stalls` stayed **0** across 713 samples and 18,270 stuck frames, because the
test was "did the value change AT ALL" and a jammed car jitters.  And even
when it did fire, the recovery could not have freed the car: the port held
the **throttle down in every frame**, so throttle+brake was just braking and
the guest never reversed -- the guest's own message is "hold the brake to
reverse", which only reverses when the throttle is let go.

### 3. The fix (one file, four edits) plus the throttle

* `bestMagnitude` is initialised with the configured base.
* The running maximum is folded in **only at the sweep tick**, so the
  branch that RECORDS a magnitude can fire at all.
* The floor is the configured base (0.30), not 0.05: "never steer less than
  the configured base".
* Progress is a **DISTANCE**, not a nudge: the threshold is 256 units = one
  metre in the guest's own 1/256 m units (one metre per 240 frames is half
  a kilometre per hour), so a jammed car registers as a stall.
* The recovery no longer flips the sign (a stuck car is not a wrong-way car),
  holds the brake for 240 frames (~7.3 s; 45 frames is a tap), **releases
  the throttle** so the guest can actually reverse, and holds the opposite
  lock (0.60) while it does.
* The race-state log gained `thr=` and `brk=`, so the next round can SEE
  whether the recovery really let go of the throttle.

### 4. What "Total Laps" is, from the guest's own code

The member at `RuleSet_RaceMetrics+0x60` is written in exactly one place
besides the constructor: **`RuleSet_RaceMetrics::vfunc[7]` (ELF 0xb35f54)**,
an event handler.

```
0xb35fb4  add  x0, x19, #0x60
0xb35fc4  bl   0xb36448     ; the SATURATING ACCUMULATOR: value + delta, clamped at INT_MAX
...
0xb35fdc  bl   0xb5741c     ; the registry
0xb35fe0  ldr  x1, [x19, #0x60]
0xb35fe4  bl   0x991d1c     ; read the current value
0xb35fec  sub  w27, x24, #1  ; value - 1
0xb3601c  mov  w8, #0x80000000
0xb36024  csel w2, w24, w27, eq   ; keep INT_MIN as it is, otherwise value - 1
0xb36030  bl   0x992518     ; insert the new value under a NEW id
0xb3603c  bl   0x9926c4     ; supersede the old id
0xb36044  str  x25, [x19, #0x60]  ; the slot gets the new id
```

So the member is a **counter**: +1 on one event, -1 on another, and the
constructor (ELF 0xf71650) registers it with the value the ruleset supplied.
**It is expected to MOVE during a race, and in q8 it never moved at all** --
which is a second, independent confirmation that no lap event ever fired.

### 5. The harness learned to choose the layout

`tools/Deploy-RR3CardScripts.ps1 -ScreenView 1280x720|2048x1152` rewrites
the three size lines of `rr3-screen.txt` on a COPY, so the repository's file
(the HD one, which is what the port ships) is never modified and the layout
under test is always a parameter rather than an uncommitted edit.  q10 is
therefore a clean A/B against q8: same layout, same touch script, same
drive config -- the only difference is the drive loop.

### 6. What q10 has to show

1. `mag=` must stop parking at 0.050 (it should span 0.30..0.95).
2. `stalls=` must become non-zero the first time the car stops.
3. The peak `total=` must exceed **125,870 units (491.68 m)**.
4. The guest's own `ax[0]` in `phase=input-door` must show real values
   (>= 0.30) and `btn[2]` must appear when the recovery runs.
5. `laps=` must leave `0xFFFFFFFF`.

If the peak still stalls near 491 m with a real steer, the controller needs
the lateral-offset read: a bang-bang servo on one scalar cannot follow a
circuit's curvature, and NONE of the six members the port resolves carries a
lateral term or a heading.  That read is the next piece of work, and the
guest's own spline projection (the code behind "Spline Distance (metres)") is
where to look.

## 2026-10-28 (night, part 5): the four fixes measured, and the two that the measurement found

### 1. q10: the four drive-loop fixes, all of them confirmed by the log

Same 1280x720 layout, same touch script, same drive config as q8; the only
difference is the drive loop.  Archive `20261028-223606-eden-q10`.

| what q8 did | what q10 did |
|---|---|
| `mag` parked at 0.050 on **43.4 %** of the samples | **0 %** -- and the sweep now EXPLORES (0.300 .. 0.950) |
| the guest's own axis 0 was **+-0.050** | up to **0.600** (the recovery's opposite lock) |
| **24** sign flips while the car sat still | **0** spurious flips before the stall |
| `stalls` stayed **0** for 18,270 stuck frames | **12**, and the recovery ran: **89 samples with the brake held and the throttle released** |
| the car sat against the barrier for 555 s | the recovery fired and the car **reversed 30 m back down the track** |

So the servo is alive, the stall detector fires, the throttle is really
released, and the guest really reverses.  Four instrument-level facts, not
inferences.

### 2. And the measurement found TWO MORE defects, in the recovery itself

The q10 log around the peak is the whole story:

```
f=18810  total=125,840  rate= 5.575  steer= 0.300  flips=0 stalls=0 1/0
f=18900  total=125,842  rate= 0.392  steer= 0.300  flips=0 stalls=0 1/0
f=19020  total=125,357  rate=-4.042  steer= 0.600  flips=1 stalls=1 0/1   <- the recovery starts
f=19050  total=123,632  rate= 0.000  steer= 0.600  flips=1 stalls=1 0/1
f=19080  total=120,562  rate= 0.000  steer= 0.600  flips=1 stalls=1 0/1
```

**(a) THE RECOVERY TRIGGERS THE "WRONG WAY ROUND" FLIP.**  The recovery
reverses the car ON PURPOSE, so the distance falls and the rate goes
negative -- that is the recovery working, not the car going the wrong way
round.  The flip rule read it as the latter, the controller drove the wrong
way round the circuit, and the distance fell from 125,357 to 103,119 with
the stall counter running to 8.  **The sign is now not flipped while the
brake is held.**

**(b) THE STALL REPEATED, AND THE CAR REVERSED ITS WAY DOWN THE TRACK.**
The progress reference stayed up at the pre-stall value, so after the
recovery backed the car 30 m off the barrier the car had to regain every
metre before it counted as progress; the stall fired again every 240 frames
and reversed the car again.  **The reference now follows the car down
while the brake is held.**

### 3. The car reached the same place in both runs -- and the direction was wrong

Both q8 and q10 stopped at **125,840 - 125,870 units = 491.6 m** of a
7,056 m lap.  That is not a coincidence and not the track's length: the
port's own measurement of 2026-10-24 says, of this circuit, "the start
straight bends LEFT and a car that does not steer runs straight off the
right-hand edge", and the run that launched with `lx=+0.6` ended up on the
**RIGHT** runoff.  So a POSITIVE axis 0 steers RIGHT.

**The autopilot was running with sign=+1 -- steering RIGHT on a straight
that bends LEFT.**  That is the whole reason it left the road at 491 m, and
the reason the rate collapsed from 100.3 to 6.5 within 60 m in q10.  The
drive config's sign is now **-1**, and the comment in the file says why.

### 4. What "Total Laps" is, from the guest's own code

The member at `RuleSet_RaceMetrics+0x60` is written in one place besides
the constructor: **`RuleSet_RaceMetrics::vfunc[7]` (ELF 0xb35f54)**, an
event handler.

```
0xb35fb4  add  x0, x19, #0x60
0xb35fc4  bl   0xb36448     ; the SATURATING ACCUMULATOR: value + delta, clamped at INT_MAX
...
0xb35fe4  bl   0x991d1c     ; read the current value
0xb35fec  sub  w27, x24, #1  ; value - 1
0xb3601c  mov  w8, #0x80000000
0xb36024  csel w2, w24, w27, eq   ; keep INT_MIN as it is, otherwise value - 1
0xb36030  bl   0x992518     ; insert the new value under a NEW id
0xb3603c  bl   0x9926c4     ; supersede the old id
0xb36044  str  x25, [x19, #0x60]  ; the slot gets the new id
```

So the member is a **counter**: +1 on one event, -1 on another, and the
constructor (ELF 0xf71650) registers it with the value the ruleset supplied.
**It is expected to MOVE during a race, and in q8 it never moved at all** --
a second, independent confirmation that no lap event ever fired.

### 5. Still open

* **The car still cannot complete a lap**, and the reason is now precise:
  a bang-bang controller on ONE scalar (the progress rate) cannot follow a
  circuit's curvature, and NONE of the six members the port resolves
  carries a lateral term or a heading.  The guest's own spline projection
  -- the code behind "Spline Distance (metres)" -- is where the missing
  signal lives, and that is the next piece of work.
* The sign is now measured rather than guessed, so the next round tests one
  hypothesis: that steering LEFT is what keeps the car on Spa's straight.

## 2026-10-28 (night, part 6): the direction was measured, and the floor was too big

### 1. Three rounds, one conclusion

| run | steering | peak distance | progress rate | stalls | laps |
|---|---|---|---|---|---|
| q8 | 0.050 (right) | 125,870 u = **491.6 m** | up to **129 u/frame** (49 km/h) | 0 (never fired) | -1 |
| q10 | 0.300 (right) | 125,842 u = **491.6 m** | **6.5 u/frame** (5 km/h) | 12 | -1 |
| q11 | 0.300 (left) | 84,018 u = 328.2 m | **6.5 u/frame** | 0 | -1 |

**A CONSTANT 0.30 ANGLE IS WORSE THAN 0.05, NOT BETTER.**  A constant
angle is a constant circle; the car scrubbed along at 5 km/h instead of
driving at 49.  The floor of 0.30 that the 2026-10-28 fix introduced was
wrong, and the log said so twice (q10 and q11, identical 6.5 units/frame
with opposite signs).  The floor is now the CONFIGURED base again, and the
drive config's base is **0.05**, with the reason written into the file.

### 2. The direction is measured, not guessed

The port's own measurement of 2026-10-24, of this circuit: "the start
straight bends LEFT and a car that does not steer runs straight off the
right-hand edge", and the run that launched with `lx=+0.6` ended up on the
**RIGHT** runoff -- so a POSITIVE axis 0 steers RIGHT.  The autopilot was
steering RIGHT on a straight that bends LEFT, which is why it left the road
at 491 m.  `sign` is now **-1**.

### 3. What the rate-only servo can and cannot do

Three rounds say the same thing: with ONE scalar (the progress rate along
the spline) the servo's best strategy is "go as straight as possible",
because that is what maximises the rate, and a straight line leaves the
road at the first corner.  The servo cannot learn to turn, because the
signal it has cannot distinguish "the road curves and I am not following
it" from "I am off the road and the road is straight".

**The missing ingredient is a lateral signal, and the guest HAS one.**  The
guest's own member registry names it: **`m_acrossAngle`** (a float at
**+0x1b0** of a per-car physics-state object), with `m_upAngle` at
+0x1a8, `m_nCurrentNode` at +0x24, `m_nPrevNode` at +0x20 and `m_vPosition`
at +0x30/+0x34/+0x38.  The guest even has an `IsOnRacingLine` debug panel
(ELF 0xc31960) and a `TrackSegmentDesc::RenderImGui(R3Car const&, TrackSpline
const&)`, and the projection itself is `snapToClosestNode` (ELF 0xd05e24,
called from 0x9fbb18) with its writer at 0xc2d8dc.

So the next piece of work is not a controller change: it is **reading
`m_acrossAngle`**.  Until the port can see the lateral error, the autopilot
can only go straight.

### 4. The lap counter is READ, and that is settled

`m_nTotalLaps` (`RuleSet_RaceMetrics+0x60`) is a counter written only by
`RuleSet_RaceMetrics::vfunc[7]` (ELF 0xb35f54): +1 through the saturating
accumulator 0xb36448, or a rewrite as `value-1` under a freshly minted id.
It is expected to MOVE during a race, and in q8 it never moved at all
across 717 samples -- an independent confirmation that no lap event fired.
The port reads it correctly (717/717 lookups `ok=1`, the guest's own
checksum agreeing every time); the game has simply not awarded a lap yet.

## 2026-10-29: the glove cursor, and the lateral distance the guest already keeps

### 1. q12 was not a test of anything -- the touch script is timing-fragile

q12 was meant to test base=0.05 with sign=-1.  It tested nothing: the guest
never reached the race.  The run summary has `start lights 0` and `race
screen 0`, while q10 and q11 with the SAME script had 7 and 10.  The hops in
`rr3-touch.txt` are anchored to FRAME NUMBERS, so when the guest loads
slower than the round that measured them, every hop lands on the wrong
widget and the guest spends the whole round in the front end.

**⇒ THE TOUCH SCRIPT IS THE FRAGILE PART, NOT THE PORT.**  And the fix is
already in the guest: it draws a **red glove cursor on the control it wants
pressed**, and it only draws it while it is waiting for that press.

### 2. tools/follow_glove.py -- click where the glove is

The guest's glove cursor is the answer to "where do I press", at any view
size and at any moment.  The tool differences the port's own framebuffer
captures (the PRESENTED screen, which is the coordinate space the touch
file expects) and finds the glove: a colour mask cannot separate it from
the red car behind it, but the glove ANIMATES, so it is the thing that MOVES
between two frames of the same screen.

No port change is needed: the touch control file is APPEND-ONLY and the
port injects every new line exactly once (see
`native-switch/include/rr3/guest_touch.hpp`), and the port's frame counter
is the one the guest log's `phase=race-state ... f=` lines carry.

**POSITIVE CONTROL** (the detector over the archived q9 frames): 17 of 33
consecutive frame pairs carried a glove, every time at (756..779, 123..134).
That is the same answer the earlier manual measurement gave (about (762,
126)).

`Deploy-RR3CardScripts.ps1 -TouchMode glove` deploys a COMMENTS-ONLY touch
file: the file is on the card, so the port's touch control owns the touch
path, but nothing is injected until the host appends a line.

### 3. The lateral distance the guest ALREADY keeps

The guest's own member registry contains `m_acrossAngle` -- and a sub-agent
**overturned the premise that it is the lateral offset**: it is a hinge
angle of a car body part (`CarBodyPart_Swinging`, the RTTI name), in
DEGREES, integrated by a spring-damper (angular acceleration = torque /
m_fMomentOfInertia, clamped to +-360), and its target is a HEADING error,
not a distance.  Three independent proofs: the integrator's own arithmetic
(ELF 0x157be30-0x157c094), the dot product with the node direction
(0x157bfb4-0x157bfe8), and the member set itself (angle, momentum) x2 plus
mass/length/inertia.

**What the guest DOES keep, and the port can now read, is the pair of
distances in the per-car track state.**  The chain, each step proved by the
guest's own code:

| step | offset | proving ELF address |
|---|---|---|
| RuleSet_RaceMetrics (the port already resolves it) | | vtable AP 0x1e4b378 |
| the spline-node structure | `+0x20` | **0xb35e60** `str x0,[x19,#0x20]` |
| the per-car track state | `*(that)+0x288` | **0xb35ce8** `ldr x8,[x8,#0x288]` |
| distance A (int32, scale 1/32) | `+0x1a8` | **0xb35cf4** `scvtf s1,s1; fmul s1,s2,s1; fcvtzs w26,s1,#8` |
| distance B (int32, scale 1/4) | `+0x1c8` | **0xb35cf0** `fcvtzs w27,s0` |

WHICH is which is NOT established, and the two scales are only an
inference, so both are now logged RAW next to the port's own spline
distance: `along=`, `across=`, `tsok=`.  One race names them -- the one that
grows with the port's own distance is the one along the track, and the
other is the lateral term the autopilot has been missing for three rounds.

The port reads them with its own safe reads and writes nothing.

### 4. There is no current-speed member

Scanned ALL 1091 `m_*` member strings: the only speed members are
`m_nTopGameSpeed` (a MAXIMUM, useless as a speed), `m_fDollySpeed`,
`m_fPedestalSpeed`, `m_nMinCarVelocitySwitchTime`, `m_Start_Vel` and
`m_End_Vel`.  **No live speed is available**, which is why the stall
detector has to work off distance.

## 2026-10-29: the guest was waiting for an answer nobody could deliver, and the lateral signal exists

### 1. Two rounds froze, and the harness called them "alive"

q13 and q15 both froze with the guest log at **1.5 MB and not one frame
rendered**, while Eden's CPU climbed a steady 0.5-0.6 core.  The watchdog
read that as "the guest is alive, not wedged" -- and the round then sat there
for twenty minutes doing nothing.

**THE FRAME CAPTURES ARE THE LIVENESS INSTRUMENT.**  The port writes one
when it renders, so "no new capture" is the ground truth for "the guest is
not drawing anything".  The watchdog now takes it into account: a quiet log
with a rising CPU AND a new capture is a live but slow guest (the case the
rule was written for), and a quiet log with a rising CPU and NO new capture
is a BLOCKED guest, which now ends the round with that reason.

### 2. WHY it was blocked: the answer was in a queue with no drain

The guest's Cloudcell HTTP answers are DEFERRED: `http_post()` enqueues the
work with `cloudcell_defer()` and something has to drain the queue later.
The drain was the alert-pump host thread -- a raw libnx thread, so the OS
schedules it whether or not the guest is making progress -- and it was
started from `alertAnswerPositiveImpl()`, i.e. only once the guest had
raised a DIALOG.

**The guest's cloud sync happens BEFORE any dialog.**  So on a round where no
alert comes first, the thread does not exist yet, the deferred answer to
POST https://connect-dev-ss.cloudcell.com:443/CC_Sync.php sits in the queue
with nobody to take it, and the guest waits for it forever.  MEASURED, and it
is a clean difference:

| round | outcome | "cloudcell_pump: site=alert-pump" lines |
|---|---|---|
| q11 | reached the race | **44** |
| q13 | froze, 1.5 MB, no frames | **0** |
| q15 | froze, 1.5 MB, no frames | **0** |

Not one "alert-pump" line at all in the frozen rounds: the thread was never
created.  The guest's own HTTP stack was never the problem.

**THE FIX**: the pump is started at the PORT's startup (`startAlertPumpEarly()`
in main.cpp), before the guest does anything.  Same thread, same 80 ms tick,
same `cloudcell_pump()` call; only its starting point moves earlier, and
earlier is exactly what this failure needs.

**AND `nogo=1` WAS TRIED AND REJECTED.**  The card's own rr3-gfx.conf
documents this symptom ("the guest's main thread is parked in libnx send()
... with no socket stack there is nothing to block on"), and it did remove
the hang -- but the next round then stalled on the loading screen with the
guest alive and the log growing, so the block was somewhere else, as that
file's own caveat warned.  It is back to 0.  The configuration now lives in
the repository (tools/rr3-gfx.conf) and the deploy script installs it, so a
round can be reproduced from the tree.

### 3. THE LATERAL SIGNAL: the chain, and what the two numbers really are

A sub-agent resolved the chain from the object the port ALREADY has, with
three independent ELF sites and the port's own vtable value as the positive
control:

```
base = RuleSet_RaceMetrics            <- the port resolves this today
step 1: read word at base+0x20   -> CAR         ELF 0xb35c94 / 0xb35dd0
step 2: read word at CAR+0x288   -> CAR_STATE   ELF 0xb35ce8 / 0xb35dd8
final: CAR_STATE+0x1a8 = the SPLINE NODE INDEX  (int)
       CAR_STATE+0x1c8 = the DISTANCE TRAVELLED  (int)
```

and the cross-checks are independent: the writer 0xc2d888
(`ldr x8,[x0,#0x288]` then `str w4,[x8,#0x1a8]`), the projection helper
`snapToClosestNode` 0xd05e44 (`ldr x9,[x8,#0x288]` then `ldr w9,[x9,#0x1a8]`),
and 0xb29264/0xb29284.  The progress formula at 0xb35dac combines them as
`[+0x60]*[+0x48] + node + travelled`, which is what says +0x1a8 is the node
and +0x1c8 the distance.

**⇒ THE PORT'S TWO NEW LOG FIELDS ARE THE NODE INDEX AND THE DISTANCE**,
and they are logged raw, next to the port's own spline distance, so one
race names them.

### 4. `m_acrossAngle` is NOT the racing-line offset (the premise was wrong)

The guest's own member registry names `m_acrossAngle`, and it is tempting to
read that as "the angle across the racing line".  It is not: the class that
registers it is **`CarBodyPart_Swinging`** (RTTI typeinfo 0x1eeb3b0, vtable
address point 0x1eeb158, slot[3] = 0x157c2a8), i.e. a SWINGING CAR BODY
PART, and its member set is a rigid body: `m_upAngle` (+0x1a8),
`m_momentum` (+0x1ac), `m_acrossAngle` (+0x1b0), `m_fMass`, `m_fLength`,
`m_fMomentOfInertia`, `m_distTravelled` (+0x1c8).  "across" is the ROLL
axis, and the integrator (0x157be30-0x157c094) works in degrees: the limit
is 360.0 (0x43b40000), the dead zone is 0.001, and the result is converted
from radians with `*180.0/pi` (0x43340000 / 0x4049fdb).

Three independent proofs: the integrator's own arithmetic, the member set,
and the fact that the guest's `IsOnRacingLine` panel (0xc31960) is only an
ImGui label that never touches +0x1b0.

⇒ **THE RACING-LINE OFFSET IS NOT STORED; IT IS COMPUTED**, in the projection
(snapToClosestNode 0xd05e24 / 0x9a7688).  But the inputs are reachable:
the car's own debug object carries **CAR+0x24 = the current spline node**
and **CAR+0x30/+0x34/+0x38 = the car's world position**, and the track's
spline is in the volume (Tracks/<track>/...spline).  So the port can compute
the lateral error and the heading error itself, and steer by them.

⇒ **AND THERE IS NO LIVE SPEED ANYWHERE IN THE GUEST**: all 1091 `m_*`
member names were scanned; the only speed members are `m_nTopGameSpeed` (a
MAXIMUM), `m_fDollySpeed`, `m_fPedestalSpeed`, `m_nMinCarVelocitySwitchTime`,
`m_Start_Vel` and `m_End_Vel`.  That is why the stall detector has to work
off distance.

## 2026-10-29 (part 2): the steering signal was in the guest all along -- aim at ITS target

### 1. The measurement that started it

The guest's own debug object registers, in one function (ELF 0xd052e0), the
names of the things a car knows about its own place on the track:

```
0xd052e0  ldr w1, [x19, #0x20]   "m_nPrevNode"
0xd052f0  ldr w1, [x19, #0x24]   "m_nCurrentNode"
0xd05304  add x20, x19, #0x50     "m_nIdealNodeJump"
0xd05328  ldr w1, [x19, #0x28]   "m_nCamNum"
0xd05338  ldr w1, [x19, #0x2c]   "m_savedCameraIndex"
0xd05348  ldr w1, [x19, #0x30]   "x"   0xd05368  ldr w1, [x19, #0x34]   "y"    >  m_vPosition
0xd05388  ldr w1, [x19, #0x38]   "z"   /
0xd053c0  ldr w1, [x19, #0x3c]   "x"   0xd053d8  ldr w1, [x19, #0x40]   "y"    >  m_vLastKnownTarget
0xd053f0  ldr w1, [x19, #0x44]   "z"   /
```

And the copy that fills it (ELF 0xd05184) shows where the values come from:
a 68-byte-per-record array (the base at x19+0x158, the index at x19+0x2c,
the stride 0x44) -- and the record's own layout:

```
+0x08  m_nCurrentNode          +0x0c  m_nCamNum
+0x10  the position's x,y      +0x18  the position's z
+0x1c or +0x28  the target's x,y + z
```

⇒ **THE CAR HAS ITS WORLD POSITION AND THE POINT IT IS AIMING AT.**
All 32-bit integers in the guest's 1/256 metre unit.

### 2. What that buys: a pure-pursuit controller

The port was steering on ONE scalar -- the progress rate along the spline --
and three measured rounds (q8, q10, q11) agree on why that fails: a single
scalar cannot say "the road curves and I am not following it" apart from "I
am off the road", so the controller's best strategy is always "go as
straight as possible", and the car leaves the road at 491 m of a 7,056 m lap.

The guest's own AI drives each car towards its m_vLastKnownTarget, so the
port can drive by the same pair:

```
heading = the car's direction of travel, from the difference of two
          position samples (this object carries no heading of its own);
desired = the direction from the car to the target;
error   = the signed angle from the heading to the desired direction;
axis    = clamp(error / steergain, -1, +1).
```

A positive axis 0 steers RIGHT (MEASURED, .build/rr3-pad-mirror.txt: lx=+0.6
at the launch ended up on the right runoff), and the world frame's
handedness is the ONE thing in this that is not measured, so the sign is a
config knob (`steersign`) and the log carries `perr=` -- **one round settles
it**.  The magnitude servo stays as the fallback for when there is no
target.

### 3. The touch script's fragility is gone too

The guest draws a **red glove cursor** on the control it wants pressed, so
"where do I press" is a question the guest answers.  `tools/follow_glove.py`
differences the port's own framebuffer captures -- the PRESENTED screen,
which is the coordinate space the touch file expects -- and finds the glove:
a colour mask cannot separate it from the red car behind it, but the glove
ANIMATES, so it is the thing that MOVES between two frames of the same
screen.

MEASURED in q17, at the SHIPPING 2048x1152 layout: the watcher appended 42
events and **the port injected all 42** into the guest (the counts match
exactly), and the guest walked from the loading screen to the
**EventMapScreen** on its own.  The legacy script's problems -- pixel
coordinates tied to one layout, hops anchored to frame numbers that miss
when the guest loads slower -- are gone: the glove is at (762,126) on the
event card and at (1120,45) on another screen, and the tool follows it.

The deploy script grew `-TouchMode glove` for this: it writes a
COMMENTS-ONLY touch file, so the port's touch control owns the touch path
but injects nothing until the host appends a line.


# 2026-10-29 -- THE GUEST REACHED A RACE AGAIN, AND WHY IT HAD STOPPED

## q19 was not a port regression: another project killed our emulator

The run tagged q19 died after 112 seconds and its archive was worthless.
The cause was NOT in this port:

* E:\AI\ani-switch\scripts\eden_stress_run.ps1 runs its own Eden on the
  SAME E:\Eden installation and, before every run, executes
  `Get-Process -Name 'eden','eden-cli','Eden','Eden-cli' | Kill` --
  which matches our emulator by PROCESS NAME and killed it at 01:28:28.
* Eden's log is ONE file per installation and Eden TRUNCATES it at
  startup, so the second instance to start DESTROYS the first one's log.
  Both files in the q19 archive (eden_log.txt.tail and
  eden_log.txt.old.txt) say "Loading aniswitch" and mention "rr3" ZERO
  times: they are the other project's log, and the harness archived them
  as if they were ours.

So a whole round was lost to an environment collision, and the harness
reported it as "eden exited on its own", which reads exactly like a port
regression.

THE FIX (tools/Setup-IsolatedEden.ps1, tools/Test-OnEden.ps1):

* a PRIVATE Eden at E:\Eden-rr3: the executables under a name the other
  project does not kill and its Resolve-EdenRoot does not find
  (eden-rr3.exe), its OWN user\ directory (log, crash dumps, NAND,
  config) and its OWN COPY of the SD card.  The card is copied rather
  than linked, because the guest WRITES to it.
* the harness now READS the title the archived emulator log says it
  loaded and says so in the archive, so a foreign log can never be
  mistaken for the run's own.  q21 reported
  "edenlog : the archived emulator log is ours (loaded 'RR3', rr3 mentions: 11003)".
* the harness warns at launch when another Eden is alive, and says whether
  it is on the SAME installation (which can kill this run and rotate this
  run's log) or a different one.

## Why the guest stopped reaching a race at all: a MODAL, not a code change

Every archive from q12 (2026-10-29 00:02) onwards failed to reach a
race, while q8, q10 and q11 (all on 2026-10-28) did.  The two runs are
IDENTICAL up to guest ordinal 2500: the same touch injections at the same
frames, the same 1280x720 view, the same touch channel state
(valid=1, enabled=1), the same asset lists, the same sync traffic.

The difference is ONE MODAL.  q12's last frame (frame-012000.png) shows
the guest sitting in the M$ DAILY REWARDS CALENDAR with its own red glove
on the orange COLLECT word; q11's frame at the same point
(frame-001400.png) shows the MAIN MENU, with no calendar at all.  On
2026-10-28 the daily reward had already been collected, so the calendar
did not open; after midnight it opened again and the guest never got past
it.  It sat in that modal for the whole run and every later hop landed on
the calendar instead of the menu.  The guest's own log says it plainly:
q12 never queued the asset update sync that in q11 brought the track's
asset lists down and led to the race.

THE HARD-CODED COORDINATE MOVED WITH THE DATE.  Measured off the real
frame with the orange-button mask (r>230, 150<g<215, b<60):

    2026-10-27 (archive f1k)   the COLLECT word at x 373..533, y 202..242
                                -> the point (453, 222) was used
    2026-10-29 (archive q12)   the COLLECT word at x 565..725, y 202..242
                                -> (453, 222) is NOT orange: blank
                                   background, 112 px left of the button

The vertical position did not move; the horizontal one did, because the
calendar lays the word over the middle column of a day grid whose width
depends on the month.  One hard-coded point is a coin toss.

THE FIX: hop 1 clicks THREE candidate columns (453, 549, 645) over a
window in which the calendar can plausibly be up, and every other hop is
repeated over a window too, so a hop that lands on the wrong control is
retried.  The extra clicks are harmless: they land on the calendar's own
day circles, which is a no-op.

## The touch queue was silently dropping events

The whole script is read in ONE poll, so all 48 events are queued
together -- and the queue held 64 entries.  On overflow the code
advanced the CONSUMED counter past the lines it had not queued, so those
lines were never re-read and never delivered: the script was truncated
with nothing to show for it but one "event queue full" line.  The queue
is now 256 entries, and the overflow path advances the counter only over
the lines that were actually queued, so an oversized script is DELAYED
rather than cut.

## q21: the guest got into a race, on the private instance

Archive 20261029-174557-eden-rr3-q21-isolated-calendar-fix, 1509 s, run
to the wall-clock deadline with nothing interfering:

    start lights (StartLightsScreen)   7
    race screen (InGameScreen)        10
    pre-race (PreRaceScreen)            4
    track load finished                1   ("Total Levelloading: 21832")
    HUD string (race marker)           10
    objective string (race marker)     15
    touch injections                   48   (0 overflow, 0 badline)
    QueueAssetsUpdateSync / ConsentToDownload   2 / 2

And the port read the guest's OWN race state for the first time in a race
that it could steer:

    metrics=1  mres=0x3f  cls=27RuleSetModifier_RaceMetrics
    total=171997 (673 m)  speed=11808 (46 km/h)  rate=-11.96
    drive=1  steer=-0.600  mag=0.170  stalls=2  flips=1

The screenshot .build/showcase/q21-in-race-brands-hatch.png shows the
guest's own F1 car on the Brands Hatch circuit, in position 7, with the
guest's "hold the brake to reverse" message on screen.

## What is STILL wrong, and where it is: the member slots are NOT descriptors

q21 reported "mres=0x3f" (all six members resolved: the rotating ids and
the value store are fine) and "ident=0x0" (the name test failed for all
six).  A test that fails for EVERY slot cannot be a name mismatch: it
means the two words the check reads are not the pair it thinks they are.

MEASURED from the port's own race-raw dump, with the per-word byte swap
undone (the probe prints each DWORD with "%08x", so the hex of a group of
four bytes is reversed relative to memory):

    +0x00  73 65 72 69 65 73 2f 71 75 61 6e 74 69 63 66 69  series/quanticfi
    +0x10  72 65 5f 66 65 72 72 61 72 69 5f 32 39 36 5f 67  re_ferrari_296_g
    +0x20  74 62 5f 74 74 63 2f 73 70 72 69 74 65 73 2e 61  tb_ttc/sprites.a
    +0x30  6e 64 72 6f 69 64 2e 61 74 6c 61 73 00 64 73 2e  ndroid.atlas.ds.
    +0x40  7a 00 ff ff ff ff ff f1 31 00 00 00 00 00 00 00  z.......1.......

So the object the probe dumps begins with the STRING
"series/quanticfire_ferrari_296_gtb_ttc/sprites.android.atlas" -- an ASSET
PATH, not a member descriptor -- and the port's member offsets
(0x08, 0x20, 0x38, 0x50, 0x68, 0x88) land INSIDE that string and in the
pointer words after it.

That explains all three remaining zeros at once:

* "ident=0x0" -- the words read at slot+0x08 and slot+0x10 are ASCII of
  the asset path, so "hash ^ (len^hash)" is not a length.
* "perr=0.0000" and "pur=0" -- the pursuit needs the car's position and
  its target, which come from the same wrong offsets, so the error is
  identically zero and the pure-pursuit controller never engages.
* "laps=4294967295" -- the lap counter is read as the same noise.

WHAT THE NEXT ROUND MUST MEASURE: the descriptor layout of the object the
guest's OWN registration builds (ELF 0xb35a30, the registration calls at
0xb35a5c..0xb35ba0), and where the value-store SLOT for each member
really is.  The port must not read a member descriptor at an offset that
has not been measured on a LIVE object.

NOTE: this also means the offsets the port uses for the CAR CHAIN
(CAR+0x20 -> CAR_STATE, CAR_STATE+0x1a8 -> node id, +0x1c8 -> distance)
must be re-measured the same way, since the object that was dumped is
the one the probe thinks is CAR_STATE+0x2c8.


## The rest of q21, read field by field

The last race-state line of q21, every field the port reports:

    metrics   1        active    0        nodes     1        keyoff    1
    cls       27RuleSetModifier_RaceMetrics             mres      0x3f
    laps      4294967295         lapF     -1.000
    lapd      1101824            lapdF    1101824.000
    prev      4294461080
    total     171941             totalF   171941.000
    speed     11808              speedF   11808.000
    time      911163             rate     0.033333
    prog      171941.000
    car       0x213e929770       cnode    0          cprev     0
    cpos      2,33,0             ctgt     0,0,0
    snode     290                sdist    13508      tsok      1
    perr      0.0000             pur      0          ident     0x0
    drive     1                  steer    -0.600     mag       0.230
    sign      1                  best     0.000000   flips     1
    stalls    126                thr      0          brk       1

WHAT IS ALREADY RIGHT, AND IT IS THE PART THAT MATTERS:

* `mres=0x3f` -- all six member VALUES resolve out of the guest's value
  store, and the guest's own class name is confirmed:
  `27RuleSetModifier_RaceMetrics`.  The port is reading the right OBJECT
  and the right MEMBERS.
* `total=171941` (672 m travelled), `lapd=1101824` (a 4304 m lap),
  `snode=290`, `sdist=13508` -- the guest's OWN progress numbers, read
  from the guest, not inferred.
* `laps=4294967295` is `-1` as an unsigned 32-bit value, and
  `lapF=-1.000` says the same thing in the float reading: **the lap
  counter is at its INITIAL value because the car had covered 672 m of a
  4304 m lap and had not crossed the line.**  So the counter is not
  broken; the run simply did not last long enough to complete a lap.
  The sign of this measurement is the opposite of what the archive's
  "laps left 0xFFFFFFFF" phrasing suggested.

WHAT IS STILL WRONG, AND IT IS ALL IN THE CAR CHAIN:

* `cnode=0`, `cpos=2,33,0`, `ctgt=0,0,0` -- the car's node index, world
  position and aim target are all ZERO or noise.  These come from
  `fillCarState()`, which reads them off the CAR object at +0x20/+0x24
  (the node indices) and +0x30..+0x38 (the position) and +0x3c..+0x44
  (the target).  The port's raw dump shows that the object it dumps at
  that address holds POINTERS in its first 0x48 bytes, so those offsets
  are not the fields the port thinks they are.
* `perr=0.0000` and `pur=0` follow directly: the pure-pursuit error is
  computed from the position and the target, and both are zero, so the
  error is identically zero and the pursuit branch never engages.  The
  sign of the world frame (the `steersign` knob) therefore cannot be
  measured until the position is read from the right place.
* `ident=0x0` -- the name test, whose reason is measured above.
* `stalls=126` with `thr=0 brk=1` -- the servo spent most of the race
  braking.  With no position to steer towards, the rate-only fallback
  servo had nothing to work with, which is the same root cause.

SO THE NEXT ROUND HAS ONE JOB: find where the CAR's node index, world
position and aim target REALLY are, on a LIVE object, the same way the
member slots were found.  Everything downstream -- the pursuit error, its
sign, the lap counter leaving its initial value -- depends on that one
measurement.

The port must not read a field at an offset that has not been measured on
a live object.  Where the measurement is missing, the port says so
instead of reporting a number it cannot back.


## The causal chain q21 closed: ONE wrong offset explains every symptom

Sampling q21's race-state line by line shows the car did not merely
"fail to steer" - it drove 513 m and then froze, and every number agrees on
why:

    f        total(m)   speed      thr  brk   steer    stalls  flips
    5700     0          4294967295 1   0     0.000   0       0
    7530     513        11808      1   0    -0.050   0       0
    9360     674        11808      0   1    -0.600   4       1
    11190    672        11808      0   1    -0.600   11      1
    ...      672        11808      0   1    -0.600   ...     1
    38640    672        11808      0   1    -0.600   125     1

* the car accelerates, covers 513 m and then 674 m, and from there
  `total` NEVER MOVES AGAIN (672 m for the rest of the race),
* `speed` is FROZEN at 11808 (46 km/h) - the guest's own speed value
  stopped changing, which is what a car against a barrier looks like,
* the port holds `brk=1` and `thr=0` the whole time, and `steer` is
  pinned at -0.600 with `mag=0.230`,
* `stalls` climbs to 126 and `flips` stays at 1.

WHY: the pursuit error is `perr=0.0000` and `pur=0` in EVERY sample, and
the position it would be computed from is `cpos=2,33,0` with `ctgt=0,0,0`
and `cnode=0`.  With no position and no target there is no error, so the
pure-pursuit branch never engages and the rate-only fallback servo has
nothing to steer towards: it winds the axis up to its floor and holds it.
The car drives into the barrier and the guest's own counters freeze.

So the port's single wrong assumption - that the car's world position and
aim target live at CAR+0x30.. and CAR+0x3c.. - is not a cosmetic
mismatch.  It is the reason the car cannot be driven, and therefore the
reason the steering sign, the lap counter and the pursuit error have
never been measurable.

THE FIX IS A MEASUREMENT, NOT AN OFFSET CHANGE.  The port now dumps the
CAR object's own bytes (phase=car-raw, 0x120 bytes, every 600 frames) in
the same format the metrics modifier is dumped, so the next round reads
the layout off a LIVE object instead of guessing it again.


## q22: the calendar moved AGAIN, so the coordinate cannot be fixed

The run tagged q22 (archive 20261030-210709-eden-rr3-q22-carraw, 1512 s, on
the private instance, nothing interfering) did NOT reach a race, and its
last frame says why: the guest was in the daily-rewards calendar again, and
this time the orange COLLECT word was at x 720..940 -- the FOURTH day
circle, not the third.

So the word has now been measured in three places:

    2026-10-27   x 373..533   the script clicked (453, 222)
    2026-10-29   x 565..725   (453, 222) was 112 px of blank background
    2026-10-30   x 720..940   and the glove pointed at the fourth circle

Three positions in three days, and the third one is not even the same
column.  The calendar lays its word over the day grid, whose width
depends on the month, and the selected day moves too.  **A hard-coded
coordinate cannot be repaired -- it can only be replaced.**

The replacement is to read the coordinate instead of knowing it: the guest
draws a RED GLOVE on the control it wants pressed, and the port already
has the pixels in hand (it reads them back for the PNG capture).  So the
port now finds the glove in the picture it just drew and clicks THAT
(native-switch/source/glove_detector.cpp, wired into the frame loop in
guest_view_probe.cpp, phase=glove-click).

THE DETECTOR'S FIRST RULE WAS WRONG, AND A TEST CAUGHT IT.  A colour mask
alone cannot tell the glove from a red car, so the rule was "the red
cluster that MOVED between two frames".  Measured over the q21 archive's
104 captured frames, that rule fires on 6 of the 16 race-screen frame
pairs: it would click a red car at 46 km/h and throw the car off the
track, which is the failure the port already has once per race.

What the measurements showed:

  * the SHAPE does not separate them: the glove's box is 44x41, 48x40,
    83x51 and the car's is 227x172, 219x103, 208x90, but their FILL
    ratios overlap (0.3..0.7 for both);
  * the WHITE STRIPES do not separate them either: the stripes are
    covered by the red mask, so "white inside the cluster" is 0 for every
    blob, real or false;
  * the SIZE does: a hand never exceeds about 90 pixels across and a car
    always does.

So the detector bounds the blob's WIDTH AND HEIGHT separately (110 px
each), not its area -- a car seen head-on is narrow but tall, and a hand is
small in both.

## The CAR chain: the chain is right, the OFFSETS belonged to the CAMERA

MEASURED 2026-10-30 by reading the guest's own code, and every offset
below is a guest instruction:

  RuleSet_RaceMetrics+0x20   the CAR          0xb35e60 `str x0,[x19,#0x20]`
  CAR+0x288                   the CAR_STATE    0xd05e44 `ldr x9,[x8,#0x288]`
  CAR+0x1a8                   the node index   0x109e258
  CAR+0xc8/+0xcc/+0xd0        the POSITION     0xc2d8ac / 0xc2d8a8
  CAR+0xfc/+0x104/+0x108      a second vector  0xc2d8c4
  CAR+0x49c                   the car's number 0xb35ca8 `ldr w1,[x9,#0x49c]`

The position is three int32 in the guest's 1/256 metre unit, and the
converter that produces it (0xf05600) NEGATES Y.

WHAT THE PORT WAS READING.  The offsets it used -- CAR+0x20/+0x24 for the
node indices and CAR+0x30..+0x38 for the position and CAR+0x3c..+0x44
for the target -- are the FOLLOW CAMERA's debug members, not the car's.
The guest's debug registration at 0xd052e0 registers m_nPrevNode (+0x20),
m_nCurrentNode (+0x24), m_nCamNum (+0x28), m_savedCameraIndex (+0x2c),
m_vPosition (+0x30..+0x38) and m_vLastKnownTarget (+0x3c..+0x44) on
the CAMERA CONTROLLER, and that controller copies its position and target
out of a 68-byte camera-preset table (0xd05184: the preset base at
x19+0x158, the index at x19+0x2c, the stride 0x44).  The port read a
camera's presets and called them the car's.

That is why q21's cpos was 2,33,0 and ctgt was 0,0,0: those are the
presets of a camera that had not loaded one yet.

The port also now reports `posok=1|0`, a PLAUSIBILITY GATE on the
position it read (|x|,|y| < 400000, |z| < 20000), and `cnum=`, the car's
own number, which the guest itself reads at 0xb35ca8.  A run that read
the wrong object now says so in the log instead of driving on noise.


## q23 and q24: two runs that died EARLY, and what they teach

Both runs were on the private instance with the new glove detector, and
both died before reaching the front end:

    q23   died at frame 3,   guest log frozen at 1.57 MB, CPU 0.5 cores
    q24   died at frame 16,  guest log frozen at 2.29 MB, CPU 0.5 cores

TWO FALSE LEADS, AND WHAT SETTLED THEM.

**LEAD ONE: "Unmapped Read64 @ 0x28".**  The Eden log was full of it, and
0x28 is what a null pointer plus an offset looks like, so it was the
obvious suspect for a new fault.  It is NOT new: the archived logs of the
runs BEFORE this change have it too -- 1170 times in q21 and 1390 times in
q22, against 102 in q23.  It is a long-standing condition of the port's
probes, not this change.  A fault that appears in every archive is a
standing condition, not a regression.

**LEAD TWO: "the diagnostic walked the guest's memory".**  The first
version asked the guest whether it was racing, by calling
raceStateResolveRuleSetRaceMetrics() from the frame loop, and that
resolver walks the guest's GameMode chain.  That was a real mistake and it
was removed: a diagnostic must not read the state of the thing it
measures.  It was NOT what killed these two runs, though, because q24 died
with it already gone.

**WHAT ACTUALLY KILLED THEM.**  The port's own readback line never
appeared at all:

    q21   61 readbacks, all from frame 30 onwards, every one of them
          reporting "lit=921600 of 921600"
    q23    0 readbacks
    q24    0 readbacks

The port reads the same back buffer with the same glReadPixels at frame 30
and it has never failed.  The glove loop read it on the FIRST frame
instead, and an early readback does not return -- it waits, because the
guest has not rendered anything yet.  So the glove loop HUNG THE GUEST on
its very first act.

**THE RULE, AND IT IS GENERAL.**  "The instrument's own first output line
never appeared" is not "the feature did not fire" -- it is "the run died
BEFORE that point".  A missing measurement is a LOCATION, not a
non-event.  And an instrument that hangs the thing it measures is worse
than no instrument, so the new code was scheduled on the schedule the port
already proved works: not before frame 30, and every 30 frames after.
## q25: the port clicked 144 times and the guest never moved -- the TAP had
## no DURATION

q25 ran on the private instance for 1500 seconds with the glove detector
wired in.  It produced 65 frames, 23 glove detections and 144 clicks, and
the guest ended the run in the SAME place it started: the main menu.

    start lights (StartLightsScreen)   0
    race screen (InGameScreen)         0
    Total Levelloading                 0
    EventMapScreen                   292   (the front end WAS up)

The guest was not stuck because it could not be reached.  It was stuck
because the clicks were not clicks.

**THE MEASUREMENT THAT SETTLED IT.**  Every archive in .build/eden-runs
that injected touches at all was swept for the gap between a down and its
up.  The separation is perfect:

    q21   24 taps, every down->up gap 40 frames   -> reached a race
    q25  144 taps, every down->up gap  0 frames   -> never advanced

Every archive that reached a race has same-frame = 0, and q25 -- the only
run with a same-frame tap -- is the only one that never moved.  The gaps
that appear in the archives are 1, 14..18, 20, 30, 36 and 40 frames; the
ones that reach a race are 1, 20 and 40.

The port's glove loop injected onTouchBeginJNI and onTouchEndJNI in the
SAME frame.  That is a zero-duration tap: the button's "pressed" state
never exists for one of the guest's own frames, and the guest's UI drops
it.  A tap is not a pair of coordinates; its SHAPE is part of the
contract.

**THE SECOND DEFECT, IN THE SAME PLACE.**  The guest's touch object
SHIPS with "a finger is down" set and with x = y = -1, and the guest's
engine (ELF 0x12b45f4) stores a new point ONLY when no finger is down.
So the first press of a run is swallowed unless that stale pointer is
released first.  Measured: q25's first press left the guest's own point
at (-1,-1); q21's first press, 900 frames in, landed.

**THE THIRD DEFECT: THE GLOVE WAS NOT THE TARGET.**  q25's last frame
shows the main menu.  The guest's red glove sits on the season-promo CAR
IMAGE at (465, 355) -- a decoration.  The actual way into a race was the
ORANGE "ENTER THE RACE" button at x 1090..1223, y 611..641, and the
guest does NOT draw a glove on it.  The same glove was still on that car
in q21 -- the archive that DID reach a race -- while the coordinate
script walked the menus underneath it.  So "find the glove" was never a
complete rule.

**WHAT CHANGED IN THE PORT.**

  * `include/rr3/ui_tap.hpp` + `source/ui_tap.cpp` -- a shared TAP
    primitive.  press() injects the Begin and schedules the End
    kTapHoldFrames (40) later; service() injects it when the frame
    arrives, on EVERY frame, so a tap cannot stay down.  Before the first
    press it releases the guest's shipped stale pointer.  Every path that
    presses a control goes through it, so no second path can grow its own
    idea of what a tap is.

  * `include/rr3/ui_target.hpp` + `source/ui_target.cpp` -- the CONTROL
    finder.  The glove is still one of the candidates, but the guest's
    accent-coloured controls are found as well: the orange band
    (r>230, 150<g<215, b<60) and the yellow band (r>225, g>215, b<90).
    A button's label is a row of glyphs with gaps between them, so the
    accent runs are MERGED (boxes within 8 pixels) before they are
    called a control, and the press point is the CENTROID of the accent
    pixels -- the middle of the box can be empty background between two
    characters, and can be off a skewed button entirely.  Measured on
    q25's last frame: the orange control is found at (1157, 626), which
    is on an accent pixel (rgb 255,167,16).

  * the frame loop -- the FEEDBACK LOOP.  A press that changes nothing
    leaves its own control exactly where it was; a press that worked does
    not.  So the port presses one candidate, watches the screen, and
    moves on to the next candidate when the screen did not move.  That is
    what lets it try more than one thing instead of hammering the same
    one for the whole run -- which is what q25 did, 144 times.

  * `native-switch/card/rr3-action.txt` -- a PIXEL-FREE route into a
    race, from the guest's own code.  The guest ships a QA framework of
    its own (the "Soaks"): `Soaks|QUEUE SOAK:RACE LOOP` (fn 0xa6e22c)
    pushes a test onto the guest's test queue, `Soaks|START SOAK`
    (fn 0xa6e1e8) runs the queue's last entry, and that entry builds a
    RACE REQUEST and hands it to the guest's normal race flow.  The
    order matters (START takes count-1).  The two QUEUE commands
    overwrite x0 before they use it, so they do not read the owner and
    the port now calls them with a zero argument.

**TWO AGENT FINDINGS THAT CORRECTED THE PREMISES.**

  * The guest's own `TouchStart: x = ... y = ...` log is NOT an echo of
    the injection.  Three runs with different injection sets printed
    three unrelated values, one of them with y = 759, which is outside
    the 1280x720 viewport.  The guest's touch logs are behind a debug
    flag, and their absence means nothing.  The acceptance signal that
    does mean something is the GUEST'S SCREEN changing.

  * The action table is TWO parallel tables -- function pointers in
    .data.rel.ro at 0x1e78b70 and names in .bss at 0x1f9d6d8 -- bound
    by index, not by hash.  `Assets|UPDATE EVENTS` (idx 100) is an EMPTY
    implementation: a bare `ret`.

**THE SCREEN REGISTRY, FOR THE ROUND AFTER THIS ONE.**  The guest's
screen manager registers 65 screens in a red-black tree at mgr+0xa0
whose root is at mgr+0xa8; the name is at node+0x20 and the instance at
node+0x38, so the port can ENUMERATE every registered screen instead of
guessing.  There is no virtual Enter -- each screen has its own
non-virtual one -- but mgr+0xb8 + i*0x30 is the screen stack and
0x14a7f90(mgr, name) switches screens by name.  The screens that matter
are EventMapScreen (mgr+0x2850), EventsScreen, SeriesScreen,
CarSelectScreen and DebugRaceSelectScreen.  A screen's CHILDREN are at
comp+0x138 with their names at comp+0x38 and their rectangle through
vtable slot 10, so the port can find a control BY NAME and stop hunting
pixels altogether.
## q26: the tap shape is fixed, and two MORE defects show up -- one of them
## a heuristic that measurement killed

q26 ran the new port (the shared tap primitive, the control finder, the
feedback loop) on the private instance for 1500 seconds.

WHAT THE PORT DID, MEASURED IN ITS OWN LOG:

    downs=7  ups=7   down->up gaps: min=40 max=40  same-frame=0

**Every tap was a real tap.**  q25's 144 taps had a zero-frame gap; these
seven have a forty-frame gap, which is the value every archive that
reached a race used.  The first press also shows the stale pointer being
released first (`phase=tap-stale-release ... down=0` immediately before
the Begin), and then the guest's own point is stored (`gx=881 gy=300
down=1`) -- so the first press of the run was NOT swallowed this time.

AND THE GUEST CONFIRMED ONE OF THEM IN ITS OWN WORDS:

    phase=guest-log-any ... MenuSelectHit: PRESS
    phase=guest-log-any ... TouchStart: x = 421 y = 356

`MenuSelectHit: PRESS` is the guest's own line for "the UI caught this
click" (guest ELF 0x13b8368), and it is paired with the TouchStart line
(0x13b855c).  ONE of the seven presses was consumed by the UI -- which is
the first time in this project that the guest has said so in its own
words.

AND THE FRONT END MOVED.  q26's frames show the main menu, then the event
picker, then the CAR SELECT screen ("选择赛车 / 越野赛", with the guest's
own red glove on the yellow car and an orange "start" button at the
bottom right).  q25 never left the main menu.

**BUT NO RACE, AND THE REASON IS A HEURISTIC THAT MEASUREMENT KILLED.**

    start lights (StartLightsScreen)   0
    race screen (InGameScreen)         0
    EventMapScreen                   270

The port pressed SEVEN times, in the first 900 frames, and then pressed
NOTHING for the next 4300.  The gate that stopped it was this one:

    const bool screenMoving = changed > (1280 * 720) / 5;   // 20 %
    if (!s_waitingFeedback && !screenMoving && !tapper.pending()) { ... }

which was there because "a click in a race IS A STEER".  The rule is
right and the SIGNAL is wrong.  MEASURED on q26's own captures:

    the car-select screen   68 .. 71 % of its pixels change between two
                            captures -- it is a 3D scene with a turning car
    a race screen           never drops below 57 %   (q21 archive)

**The two ranges overlap completely**, so no change-rate threshold can
tell a menu from a race.  A 3D car on the select screen looks exactly
like a race to that rule, and the port sat on the screen it had reached
and pressed nothing.

THE SIGNAL IS NOW THE GUEST'S OWN BOOKKEEPING.  The guest allocates a
RuleSet_RaceMetrics when a race starts, and the port's race-state probe
finds it.  The probe is pumped by the INPUT PUMP, on the frame-loop
thread, once per frame, so the frame loop can read the cached sample
without touching guest memory at all -- which matters, because the
on-demand resolver (raceStateResolveRuleSetRaceMetrics) is exactly what
froze q23 and q24.

**AND THE ACTION FILE'S "at=<frame>" WAS PARSED IN ONE ORDER ONLY.**
`native-switch/card/rr3-action.txt` asks the guest's OWN QA race
framework to start a race:

    at=5950 Soaks|CLEAR SOAKS
    at=6000 Character|Unlock All Tracks
    at=6100 Soaks|QUEUE SOAK:RACE LOOP
    at=6200 Soaks|START SOAK

and the port ran NONE of them: the parser cut everything from "at="
onwards out of the name, so a line written with the frame FIRST left an
empty name and the command was dropped SILENTLY.  The frame may now come
before or after the name, and a line that carries a frame but no name is
REPORTED (a command that never runs and never says so is
indistinguishable from a command that was never written).

The action pump also DEFERS a race command while the guest is racing,
for the same reason the UI driver does: the guest's own bookkeeping says
whether it is in a race, and a second race request in the middle of a race
is exactly the kind of thing that spoils a measurement.

**THE CARD'S ACTION FILE IS A PIXEL-FREE ROUTE INTO A RACE.**  The guest
ships a QA framework of its own -- the "Soaks".  `Soaks|QUEUE SOAK:RACE
LOOP` (fn 0xa6e22c) pushes a test onto the guest's own test queue,
`Soaks|START SOAK` (fn 0xa6e1e8) runs the queue's last entry, and that
entry builds a RACE REQUEST and hands it to the guest's normal race flow.
REVERSE ENGINEERED 2026-10-30: the two QUEUE commands overwrite x0
before they use it ("ldr x0,[x8,#0x78]"), so they do not read the owner
and the port calls them with a zero argument; and the ORDER MATTERS,
because START takes the queue's LAST entry (index = count-1).

**TWO AGENT FINDINGS THAT CORRECTED THE PREMISES.**

  * The guest's own `TouchStart: x = ... y = ...` log is NOT an echo of
    the injection.  Three runs with different injection sets printed three
    unrelated values, one of them with y = 759, which is outside the
    1280x720 viewport.  The guest's touch logs are behind a debug flag,
    and their absence means nothing.  The acceptance signal that does
    mean something is the GUEST'S SCREEN changing.

  * The action table is TWO parallel tables -- function pointers in
    .data.rel.ro at 0x1e78b70 and names in .bss at 0x1f9d6d8 -- bound by
    index, not by hash.  `Assets|UPDATE EVENTS` (idx 100) is an EMPTY
    implementation: a bare `ret`.

**AND THE ROAD TO THE NEXT LEVEL, FROM THE SAME REVERSE ENGINEERING.**
The guest's screen manager registers 65 screens in a red-black tree at
mgr+0xa0 whose root is at mgr+0xa8; the name is at node+0x20 and the
instance at node+0x38, so the port can ENUMERATE every registered screen
instead of guessing.  There is no virtual Enter -- each screen has its own
non-virtual one -- but mgr+0xb8 + i*0x30 is the screen stack and
0x14a7f90(mgr, name) switches screens by name.  The screens that matter
are EventMapScreen (mgr+0x2850), EventsScreen, SeriesScreen,
CarSelectScreen and DebugRaceSelectScreen.  A screen's CHILDREN are at
comp+0x138 with their names at comp+0x38 and their rectangle through
vtable slot 10, so the port can find a control BY NAME and stop hunting
pixels altogether.
## q27: the guest's own QA framework runs, and CRASHES the guest -- and the
## front end's animation breaks the feedback loop

q27 ran with the race gate fixed (the guest's own bookkeeping instead of
the change rate) and with the action file's parser fixed.

**THE ACTION FILE NOW WORKS, AND THE GUEST'S OWN AUTOMATION RAN.**

    phase=guest-action ... name="Soaks|CLEAR SOAKS" idx=226 fn=0x210986d218 arg=0x0 owner=0 calling=1 called=1
    phase=guest-action ... name="Character|Unlock All Tracks" idx=20 fn=0x21098662c8 arg=0x0 owner=0 calling=1 called=1
    phase=guest-action ... name="Soaks|QUEUE SOAK:RACE LOOP" idx=227 fn=0x210986d22c arg=0x0 owner=0 calling=1 called=1
    phase=guest-action ... name="Soaks|START SOAK" idx=224 fn=0x210986d1e8 arg=0x0 owner=0 calling=1 called=1
    phase=guest-action ... pending=4 all-done=1

Every function address is the one the reverse engineering predicted, and
every command was called with a ZERO argument and no owner -- which is
what the instruction-level analysis said, because the two QUEUE commands
overwrite x0 before they use it.

**AND THEN THE GUEST CRASHED.**  The harness ended the run at 663 s with
"the guest is blocked, not slow", and the guest had written its OWN
automation log to the card first:

    AUTOMATION LOG [Debug]:  Start Test SOAK TEST
    AUTOMATION LOG [Debug]:  Enter Scene 3
    AUTOMATION LOG [Debug]:  Scene changed from None to FrontEnd, memory usage: 0 KiB

**"memory usage: 0 KiB" IS THE WHOLE STORY.**  The soak framework needs a
CONFIGURATION string per test id (0..5) out of a runtime table, and in
this build that table is EMPTY: the race request has no data, the scene
comes up with nothing in it, and the guest dereferences a null pointer
(the emulator's log: Unmapped WriteExclusive64 @ 0x8).

So the Soaks route is real -- it reaches the guest's own QA machinery --
but it is **not usable until that configuration table is populated**, and
until then it takes the guest down.  A crashed guest measures nothing, so
the card's rr3-action.txt no longer asks for it: the file now carries the
harmless unlock and a note saying exactly why the rest is not there.  It
becomes the shortest route into a race the moment the table is filled.

**AND THE FEEDBACK LOOP'S SIGNAL WAS ALWAYS TRUE.**  The port pressed the
SAME control 45 times in a row -- the red glove on the main menu's promo
car, at (465, 355) -- and never moved on.  The success test was

    if (!pressedStillThere || screenJumped) { the press worked }

and `screenJumped` was "the screen changed by more than 12000 pixels".
MEASURED on q27's own log, the MAIN MENU changes 268510 pixels -- 37 % of
the screen -- between two readbacks, because the banner is an animation.
So "the screen changed" is ALWAYS true on the front end, the test was
always satisfied, and the tried list was cleared after every press.

**The screen's change rate cannot be a feedback signal in a UI that
animates.**  The signal is the CONTROL: a press that worked takes its
control away (the button is gone, or the guest's hint hand has moved),
and a press that did nothing leaves the control exactly where it was.
Twelve pixels of slack, because a control moves a little when it is
pressed.

**AND THE ACCENT-COLOURED CONTROLS ARE TRIED BEFORE THE GLOVE.**  On the
main menu the glove sits on the promo car -- a decoration -- while the way
into a race is the orange ENTER THE RACE button, which carries no glove at
all.  With the glove first, the port pressed the decoration and never
reached the button.

**THE LESSON, WHICH IS BIGGER THAN THE GAME.**  Every one of the four
defects in this round was a *signal* that looked like a fact:

  * "the tap happened" -- it happened, with zero duration, and the guest
    dropped it;
  * "the screen changed" -- it changed, by 37 %, and it said nothing;
  * "the screen is moving, so a race is running" -- it was moving, and it
    was a parked car on a menu;
  * "the command was called" -- it was called, and it crashed the guest.

  A signal has to be checked against the thing it is supposed to mean,
  and the check has to be a DIFFERENT instrument from the one that
  produced it.
## q29: the guest's own ORANGE back button was taken for the red glove, and
## the run oscillated between two screens

q29 ran with the accent-coloured controls tried before the glove and with
the feedback signal changed from "the screen changed" to "the control is
still there".

WHAT THE PORT DID, IN ITS OWN LOG:

    10 presses at (1157, 627)   kind=orange  area=2940  box=134x31
     9 presses at (31, 28)      kind=glove   area=943

and the guest's own touch state says the pipeline was never the problem:

    down x=1157 y=627 frame=1591 -> guest gx=1157 gy=627 down=1
    up   x=1157 y=627 frame=1631 -> guest gx=1157 gy=627 down=0

**The guest HAD the point on the "ENTER THE RACE" label, stored it, and
did not act.**  (1157, 627) is inside the label the coordinate script
measured (x 1113..1235, y 613..639), so the press was in the right place.

**WHAT ACTUALLY HAPPENED: THE RUN OSCILLATED.**  (31, 28) is the guest's
own ORANGE back button in the top-left corner.  The port pressed "ENTER THE
RACE", the guest went to the next screen, the port then pressed the back
button, the guest came back, and the two of them went round and round for
the whole run.

**AND THE RED MASK TAKES THAT ORANGE BUTTON.**  The glove's mask is
"r >= 140 and g, b <= r - 70", and an orange pixel is (240, 170, 30) --
and 170 IS 240 - 70, so the orange sits exactly on the mask's edge.

**THE FIX IS THE WARMTH, AND IT IS ONE MEASURED RULE:**

    the false positive (the orange back button)   g - b = 93 .. 147  (mean 139)
    the real glove (on the promo car)             g - b = -31        (mean)

A saturated red has its green and blue close together; an orange has its
green far above its blue.  "g <= b + 40" keeps **1071 of 1071** real
glove pixels and rejects **943 of 943** false ones.

**WHAT THIS ROUND ADDS TO THE LIST OF SIGNALS THAT LOOK LIKE FACTS.**
Five of them now, all in the port's own UI driver:

  * "the tap happened" -- it happened, with zero duration, and the guest
    dropped it;
  * "the screen changed" -- it changed, by 37 %, and it said nothing;
  * "the screen is moving, so a race is running" -- it was moving, and it
    was a parked car on a menu;
  * "the command was called" -- it was called, and it crashed the guest;
  * "the glove was found" -- a red blob was found, and it was the guest's
    own back button.

A detector that finds A RED BLOB has not found a glove.  The glove is a
glove because it is RED **AND NOT WARM**, and because the guest drew it
there.
## q30: THE CLOSED LOOP REACHED A RACE -- and the race hung on an asset
## that is not on the card

q30 is the run where the port's own UI driver took the guest all the way
into a race **without a single hard-coded coordinate**.

    start lights (StartLightsScreen)   4
    race screen (InGameScreen)        10
    race marker: HUD string           10
    EventMapScreen                   181

**THE PRESSES WERE ALL DIFFERENT.**  Ten presses, ten different targets,
in this order:

    glove  (123,546)   orange (840,226)   orange (634,547)
    orange (1157,627)  glove (381,404)    orange (894,494)
    orange (894,494)   orange (1234,694)   glove (553,326)
    glove (744,401)

(1157, 627) is the main menu's ENTER THE RACE; (894, 494) and (1234, 694)
are controls on the screens after it.  The port found them by READING the
picture -- the guest's red glove, and the accent-coloured controls -- and
it pressed them with real taps (every down->up gap 40 frames, none
same-frame).

**AND THE WARM-COLOUR RULE IS NOT A NICETY: IT IS WHAT MAKES THE GLOVE
FINDABLE ON TOP OF A BUTTON.**  The race ends with a modal panel -- a
driver's portrait and an orange CONTINUE button, with the guest's own red
glove sitting on it.  Measured on that exact frame:

    with the warm rule     the glove stands alone: 83x50 at (911,471)-(993,520)
    without the warm rule  the glove MERGES with the orange button into one
                           258x50 blob -- which the size bound then rejects

So without the rule the port cannot see the guest's own hint hand exactly
when the guest puts it on a button, which is the one time it matters most.

**AND THE RACE METRICS PROBE FOUND THE OBJECT FOR THE FIRST TIME:**

    phase=race-metrics ... rs=0x2136289cc0 ok=1 vptr=0x210a252378 vptrOk=1
                          reg=0x210a39e980 lapsOk=1 topspeedOk=1

The guest's RuleSet_RaceMetrics is now FOUND (the vptr is the one the
guest's own class has).  The member VALUES still read -1, so the value
store lookup is not resolved yet -- that is the next thing to fix.

**BUT THE GUEST THEN HUNG, AND IT WAS NOT THE PORT'S TOUCH.**  The port's
own log shows exactly what the guest was waiting for:

    phase=map-app   rr3veh2:/files/.depot/vehicles/2021_porsche_911_rsr/2021_porsche_911_rsr_sha.dxt.dds.z
    phase=stat-path rc=-1 errno=2  guest='sdmc:/switch/rr3-native-rehost/.depot/vehicles/2021_porsche_911_rsr/...'
    ... four candidate paths, all rc=-1 errno=2 (ENOENT) ...

**The guest picked a car whose assets are not on the card.**  The card has
the 2013 Porsche 911 RSR; the guest wants the 2021 one, and it waits for
that car's shader for ever.  The port's frame loop stopped with it (the
port's frame counter froze at 789, the harness's "the guest is blocked,
not slow").

**THE DATA IS THERE; THE DOWNLOAD NEVER HAPPENED.**  The volumes carry
it:

    core.romfs            128 x "2021_porsche_911_rsr"   42 x "porsche_911_rsr_sha"
    vehicles-modern.romfs   2 x "2021_porsche_911_rsr"    0 x "porsche_911_rsr_sha"
    vehicles-classic.romfs  2 x "2021_porsche_911_rsr"   30 x "2013_porsche_911_rsr"

but the card's .depot holds only the 2013 car and 82 vehicle asset
lists.  So the question is NOT "is the asset available" but "why did the
guest's own download manager not fetch it" -- and that is the next thing
to look at, with the asset-list rewrite (the port rewrites 386 asset
lists, 2988 entries) as the first suspect.

**WHAT THIS ROUND PROVED, IN ONE LINE.**  A port can drive a game's own
UI by reading what the game draws -- the guest's hint hand and its
accent-coloured controls -- and get all the way into a race with no
coordinates in the source at all.  What it cannot do yet is make the game
happy with the assets it wants.
## The picture is soft because of the GUEST'S OWN ATLAS FILTER -- and the
## screen size the port reports is the only lever

This is the answer to "the picture looks soft", and it came out of reading
the PS Vita port (see the q27/q28 section above for how).

**THE GUEST'S RULE** (cc::ImageResManager, ELF 0x10f8cf8): every folder in
the gui_assets listing is kept only if

    width <= screenW  OR  height <= screenH

against the screen size the port reports.  With 1280x720 the 2048x1536 tree
fails BOTH tests, so it is dropped BEFORE the sort and is never a
candidate.  The HD tree is on the card the whole time; the guest never
looks at it.

**MEASURED, 2026-10-31, archive 20261031-004255-eden-rr3-q28-hd-screen-only**
(default device profile, the only change is the reported screen size
2048x1152 instead of 1280x720):

    the paths the guest asked for, by volume
      rr3core   20261      the game's own data
      rr3veh0   10001      the classic vehicles
      rr3track   2486
      rr3gui0     750      <== the HD volume: gui_assets/2048x1536/
      rr3veh1     205
      rr3gui1      33      <== the SD volume: gui_assets/960x640/ and 480x320/

and the 750 are the whole front end, from the HD tree:

    rr3gui0:/files/.depot/gui_assets/2048x1536/common/sprites_0.dxt.dds.z
    rr3gui0:/files/.depot/gui_assets/2048x1536/button/sprites.android.atlas
    rr3gui0:/files/.depot/gui_assets/2048x1536/hud/sprites.android.atlas
    rr3gui0:/files/.depot/gui_assets/2048x1536/events/map/sprites.android.atlas
    rr3gui0:/files/.depot/gui_assets/2048x1536/round_hub/season_5/sprites.android.atlas
    rr3gui0:/files/.depot/gui_assets/2048x1536/landing_page/sprites.android.atlas
    rr3gui0:/files/.depot/gui_assets/2048x1536/daily_race/flag_bg/sprites_1.dxt.dds.z

At 1280x720 the same run asked the SD volume for its 960x640 tree and the
picture was a 1.33x upscale of every sprite and glyph.  THAT is "the
picture is soft", and the fix is one number.

**AND THE DEVICE PROFILE IS THE OTHER HALF -- AND IT IS A TRAP.**
The guest also reads `eds/<device>.plist`, and that file is encrypted with
a reversible RC4 variant (key a5 35 b3 b1 e8 43 e7 cf, the keystream at
ELF 0x13c5a08), so the port can ship any profile it likes
(tools/rr3-plist.py).  The guest's own list of what it reads from that
profile is 36 keys, all read natively:

    ALLOW_RETINA  GUI_IMAGE_AUTO_SCALE  HIGH_RES_MENU
    CAR_SHADOW_MAPS  EXTERIOR_SHADOWS_{MENU,OPPONENT,PLAYER}
    EXTERIOR_SELF_SHADOWS  CAR_DASH_SHADOWS  CAR_INTERIOR_SHADOW_MAPS
    STREAMING_CAR_SHADOW_MAPS  MENU_ANIMATED_SHADOWS
    DYNAMIC_CAR_REFLECTIONS  SPLIT_SCREEN_DYNAMIC_REFLECTIONS
    RENDER_CARS_IN_CUBEMAP  USE_ANISOTROPIC_FILTERING
    USE_FULL_SCREEN_EFFECTS  USE_PARTICLE_EFFECTS  USE_PAUSE_BLUR
    DEPTH_OF_FIELD_ALLOWED  CREATE_FULL_SCREEN_BUFFERS  REDUCED_PNG_BITDEPTH
    HAS_PHOTO_MODE  SUPPORT_EXTERNAL_SCREEN  FMOD_ZERO_VOLUME_VIRTUALISATION
    USE_FLAREBLOCK_LIST  FULL_OPPONENT_DAMAGE  BLEND_IMAGE_ANIMS
    CUSTOMISATION_{DECAL,LIVERY}_TEXTURE_SIZE  ENGINE_FONT_CACHE_SIZE
    PARTY_PLAY_MAX_AI_OPP(_NASCAR)  CAR_INTERIOR_LOD

and the volume ships the guest's own HD profile, AndroidXtraHigh.plist
(2048x1536, HIGH_RES_MENU=true, AA_LEVEL=4, FONT_PAGE_SIZE=2048,
MIPMAP_BIAS=-2.0, and twenty more switches AndroidHigh does not have).

**BUT SHIPPING IT KILLED THE RUN** (archive
20261031-003750-eden-rr3-q27-hd-profile): the guest died at 42 s, and the
log shows why -- it spent the whole run pulling .vsh/.fsh and RECOMPILING:

    rr3core:/files/.depot/Shaders/2dimage.vsh
    rr3core:/files/.depot/Shaders/rr2/frontend_blur.vsh
    rr3core:/files/.depot/Shaders/rr2/livery_bake.vsh

because changing the quality keys changes the SHADER SET.  That is the
guest's own warning ("Preparing the shaders for the new graphics
settings... only happens after changing the graphics and may take a
while"), and it is the price of the quality switches.

**⇒⇒ THE RULE THIS PRODUCES**:
  * the ATLAS choice is free -- it costs nothing but the reported screen
    size, and it is what the picture quality is made of;
  * the QUALITY switches are not free -- they invalidate the shader cache
    and cost a full recompile, so they may only be changed deliberately,
    and the profile must be kept CONSTANT from run to run.

⇒ The measurement that separates the two (q27 = profile + HD size, died at
42 s; q28 = default profile + HD size, alive and drawing at 312 s) is
also the lesson: change ONE thing at a time when one of the changes can
cost a recompile.
## THREE separate knobs, and only one of them is the picture quality

The PS Vita port sent the whole thing in the right direction, and the
measurement that came out of it is worth writing down on its own.

**THE THREE KNOBS ARE INDEPENDENT, AND THE GUEST USES THEM FOR DIFFERENT
THINGS.**  All three live in native-switch/card/rr3-screen.txt:

    platform=WxH   Platform.getScreenWidth / getScreenHeight
                  -> what the guest calls "Res base" (it prints it:
                     "Res base: 2048x1536")
                  -> THE GUEST'S ATLAS FILTER uses it:
                     width <= screenW OR height <= screenH
                     (cc::ImageResManager, ELF 0x10f8cf8)
    ui=WxH         CCUserInterfaceManager.getScreenWidth / getScreenHeight
                  -> the UI layout space
    view=WxH       the size the port tells the GUEST its view is
                  -> the viewport the guest DRAWS into, and the space the
                     touch coordinates live in

**MEASURED, and they do not agree by accident:**

    q28  platform=ui=view=2048x1152
         -> the guest printed "Res base: 2048x1536"?? no: it picked the
            2048x1536 ATLAS (750 paths from rr3gui0) and DREW the UI at
            2048x1152, so the capture -- a fixed 1280x720 readback -- was a
            four-times zoomed crop of the corner.
    q29  platform=ui=view=2048x1536
         -> the guest printed "Res base: 2048x1536" and its viewport was
            2048x1536 (the scissor log says viewport=0,0,2048,1536), but
            only 921600 of the 3145728 pixels were lit: the guest draws into
            an OFFSCREEN target of the view size and the port's readback
            reads the WINDOW, which is 1280x720 -- so it read the window and
            asked for more than the window has.
         -> and the frame loop made ZERO presses in 9120 frames: the
            detector was looking at the window, which at that moment held
            the presented (letterboxed) image, not the guest's picture.

**⇒⇒ THE COHERENT COMBINATION** is the one that keeps every space the
size of the output and only moves the ATLAS:

    platform=2048x1536   the guest's "Res base" -> the HD atlas is a
                         candidate -> the 2D UI is drawn from 2048x1536
                         textures instead of a 960x640 one
    ui=2048x1536         the layout space
    view=1280x720         the guest's viewport = the window, so the
                         readback, the touch coordinates and the picture
                         are all the same space

⇒ The UI is then drawn from the HD atlas into the 1280x720 window: sharp
(a downscale, not a 1.33x upscale), and correctly proportioned, because
the .gui layouts are resolution independent and only the textures change.

**⇒⇒ AND THIS IS THE GENERAL LESSON**: three numbers that LOOK like one
setting were three settings, and only one of them is the picture quality.
Every measurement that blamed "the resolution" was measuring the wrong
knob.  Read the guest's OWN log line for the number it actually uses
("Res base"), and find out which of the three the guest's filter compares
against, before changing any of them.
