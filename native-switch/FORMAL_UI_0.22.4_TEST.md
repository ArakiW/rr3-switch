# Formal native UI 0.22.4 - Guest View EGL probe

This test validates the 0.22.4 Guest View discovery stage after the 0.22.3 JNI
lifecycle pass. The guest images and the EGL context are owned by the same NRO
process for this probe; the native framebuffer UI is closed while the callbacks
run and recreated after EGL teardown.

## Install

Copy the `switch` folder from `dist/rr3-native-rehost-complete-0.22.4/` to the
TF-card root. The final layout is:

```
/switch/rr3-native-rehost/rr3-native-rehost.nro
/switch/rr3-native-rehost/data/core.romfs
/switch/rr3-native-rehost/data/gui-hd.romfs
/switch/rr3-native-rehost/data/gui-sd.romfs
/switch/rr3-native-rehost/data/tracks.romfs
/switch/rr3-native-rehost/data/vehicles-classic.romfs
/switch/rr3-native-rehost/data/vehicles-mid.romfs
/switch/rr3-native-rehost/data/vehicles-modern.romfs
```

The package root contains `checksums.sha256`; all ten entries must report `OK`.

## Run the guarded probes

1. Launch the NRO and inspect the Overview/Runtime pages.
2. If Recovery is shown on the next launch, preserve the marker and any crash
   diagnostics. Only after the files are backed up and no guest initialization is
   active, press **B** once to clear the old guarded action.
3. Press **ZR** once. Confirm the dependency stage reaches `complete=yes`,
   `active=yes`, and `constructors=3/3`.
4. Press **ZL** once. Confirm JNI_OnLoad, all 181 main constructors,
   onCreateJNI, onStartJNI, and onResumeJNI return.
5. Press **ZL** a second time. The native framebuffer temporarily closes, the
   default `NWindow` is reacquired, and the same process creates EGL and calls,
   in order, `onViewCreatedJNI`, `onViewChangedJNI(1280,720,0,0)`, and
   `onViewRenderJNI(2,0)`. Before the guest callback, the probe performs the
   harmless `glGetString(GL_VERSION)` query both outside and inside
   `GuestTlsScope`; the guest-resolved version/vendor/renderer/extensions calls
   are traced only during this callback interval. The post-GL allocation trace
   records the enter/return pairs for the guest's distinctive `malloc(0x2800)`,
   `malloc(0x3e38)`, first vector-growth `malloc(0x240)`, `malloc(0x130)`,
   and the first and second vector-storage `malloc(0x40)` calls. The durable
   breadcrumb is updated before and after each boundary, so a power-off still
   identifies the last completed nested stage.
6. After the probe returns, confirm the dashboard has been restored. Loader
   page 2 shows the callback return flags, EGL stages, GL/EGL errors, and JNI
   unhandled-call counts for each view phase.
7. Press **PLUS** once to run finalizers, reverse-unmap the four images, clear
   the guarded marker, and return cleanly to hbmenu.

## Export the report

Hold **L+R** and tap **X** once to enable developer mode. Release **L+R**, then
press **X** alone. Retrieve:

```
/switch/rr3-native-rehost/loader-report.txt
```

Relevant lines:

```
guest_entry attempted=1 resolved=1 called=1 returned=1 code=0
guest_lifecycle start=1/1/1 resume=1/1/1
guest_view attempted=1 egl=1/1/1/1/1/1 callbacks=1/1/1 swap=1 mode=1 ...
guest_view_host_gl outside=1/1/1 inside=1/1/1 trace=<n>
guest_view_post_gl stage=<n> malloc=2800:<enter/return> 3e38:<enter/return>
                     240-first:<enter/return> 130:<enter/return>
                     40-first:<enter/return> 40-second:<enter/return>
jni_view_phase created=<n> changed=<n> render=<n>
guest_boot_progress=phase=guest-view-complete ordinal=3 ...
```

When the second ZL terminates the process, treat the durable breadcrumb as
the authoritative boundary from the interrupted process. The diagnostic
stages are:

```
1/2  GL_VERSION enter/return       3/4  GL_VENDOR enter/return
5/6  GL_RENDERER enter/return      7/8  GL_EXTENSIONS enter/return
9/10 malloc(0x2800) enter/return   11/12 malloc(0x3e38) enter/return
13/14 memset(0x1550) enter/return  15/16 memset(0x320) enter/return
17/18 memset(0xe98) enter/return    19/20 malloc(0x478) enter/return
21/22 memset(0xc01) enter/return    23/24 malloc(0x240) first enter/return
25/26 malloc(0x130) enter/return    27/28 malloc(0x40) first enter/return
29/30 malloc(0x40) second enter/return
31/32 first post-stage-30 memcpy enter/return
33/34 first free after that memcpy enter/return
35/37 stack-check failure classification (same TLS / TLS changed /
       same TLS with changed canary)
```

A breadcrumb at an `enter` stage means the real host operation was not yet
observed returning; a `return` stage means that allocation completed. The
`target=` field on an allocation enter is the guest call-site return address;
on an allocation return it is the returned host pointer. The first post-stage-30
memcpy uses stages 31/32 and the following free uses stages 33/34. A
stack-check failure uses stages 35-37: stage 35 means the Guest View trace saw
the same guest TLS and canary, stage 36 means TPIDR_EL0 changed, and stage 37
means TPIDR_EL0 was unchanged but the canary changed or could not be read. For
stack-check failure, the `ordinal=` field is its classification (35, 36, or 37),
and `target=` is the return address supplied by the `__stack_chk_fail` call. A
remaining nonzero JNI view count identifies the next platform-wrapper target.
If a callback fails, preserve the complete report and breadcrumb on the TF card;
do not press **B** or **PLUS** until a later launch is in Recovery with no guest
initialization active.

## Exception and shader diagnostics (exception-v1 / shader-v1)

Before the Guest View probe, the host creates a unique file:
`/switch/rr3-native-rehost.crash-<system-tick>.txt`. If the file cannot be
created/opened/flushed, the probe is blocked and the recovery marker remains.
Existing crash files are never overwritten. An `armed` file means capture was
prepared, not that an exception was captured.

The libnx exception handler records PC/LR/SP/FP/FAR/ESR/PSTATE, X0-X28,
TPIDR_EL0, and its own runtime address (for host ELF attribution), then calls
svcExitProcess. It does not resume guest execution or run guest finalizers.
The file is pre-opened; the handler uses a fixed buffer, raw FS write/flush
and SVCs instead of allocating or calling stdio. Capture is best-effort:
a debugger, fatal termination, a second fault, or FS failure can prevent it.
A successful capture may return directly to hbmenu rather than show an error.

After a crash, copy the newest crash file together with the same-run progress
and pending files before relaunching. Do not discard files containing only
`status=armed`; that result distinguishes preparation from capture.

Shader imports are traced for every call during the existing View probe:

- 38/39: glCreateShader enter/return (enter target is type, return target is ID).
- 40/41: glShaderSource enter/return (enter target is count, return target is ID).
- 42/43: glCompileShader enter/return.
- 44/45: glLinkProgram enter/return.

`gltrace=shader-v1`, `gl_caller`, and `gl_object` preserve the most recent shader
boundary context. Returning from compile/link does not prove GL compile/link
status is successful. Source pointers are passed through without diagnostic
dereferences. Allocator flags and copy gating now use an actual stage-event
bitset, not comparisons against the last numeric stage; later shader stages
cannot falsely mark earlier allocations as completed.

## Caller attribution

The `trace=callers-v1` breadcrumb suffix preserves `malloc40_caller`,
`copy_caller`, `dst`, `src`, `len`, `free_caller`, and `ptr` at stages 30-37.
All addresses and `len` are hexadecimal. Zero means not captured yet.
Caller fields are return addresses; subtract the matching image base from
that run's pending marker to locate the guest instruction. The copy/free
filters select by execution order, not by guest function. Stage 34 proves
those host operations returned, not that a particular guest helper returned.
Read the complete progress file, since the dashboard may truncate the line.

## Guest pthread TLS fix (pthread-tls-v1)

The exception-v1 hardware run captured RR3 +0x1c69ab8:
`ldr x8, [x23, #0x28]` immediately after `mrs x23, tpidr_el0`.
X23 and TPIDR_EL0 were zero, FAR was 0x28, and ESR was 0x92000006.
The guest worker entry +0x1c699d0 is supplied through pthread_create;
the previous bridge passed it straight to a host thread without guest TLS.
The concurrent probe breadcrumb reached glLinkProgram return (stage 45),
which is not evidence that GL caused this worker-thread fault.

The bridge now copies the start routine/argument into an owned context,
starts a host trampoline, frees that context, and installs a worker-local
GuestTlsScope before entering guest code. Normal returns go through
pthread_exit with the TLS frame still alive for thread-specific destructors.
Explicit guest pthread_exit has the same lifetime. Failed thread creation
frees the unconsumed context. The parent's TLS block is never shared.

The existing pthread self-test bit 6 now requires both normal return and
explicit exit, nonzero private worker TLS/canary, unchanged parent TLS, and
TLS still valid inside the key destructor. Expected aggregate remains 0x3ff.
This self-test runs on the Switch at startup; local contract tests do not
execute it. Confirm the Guest pthread bridge check passes before probes.

The breadcrumb enable flag is host thread_local, so newly working guest
threads cannot overwrite the probe thread's trace stream. Worker faults are
still captured by the process exception handler. The emitted host TLS helper
uses TPIDRRO_EL0, not the guest TPIDR_EL0 register.

This fix is built and locally checked, not yet device-validated. Keep collecting
crash/progress/pending if another failure appears; successful TLS setup does
not prove all guest threading, shutdown, shader, or gameplay behavior works.

## Termination and Render-tail diagnostics (termination-v1)

The current build is termination-v2: libc++'s `android_set_abort_message` shim
stores the message in a thread-local 1024-byte buffer. Termination records
include the bounded text between `abort_message_begin` and
`abort_message_end`; `prepareGuestCrashCapture()` clears the previous message
before each View attempt. This identifies messages such as libc++'s own fatal
reason when the guest requests `abort`. It remains diagnostic only: abort is
still performed and no error is suppressed. A non-empty message is not by
itself proof of the precise caller, so correlate it with caller and progress.


```text
RR3 termination-v1
status=termination-request
kind=abort|exit|raise|assert2|<fortify operation>
caller=<hex return address>
value=<hex exit status, signal, assertion line, or zero>
```

The recorder uses a local fixed buffer, SVC debug output and raw FS write/flush;
it does not reopen files or use stdio. `raise` may return, and exit may be an
ordinary requested exit: this record proves a request, not a CPU exception.
The caller may belong to a host helper rather than guest code. Subsequent
termination/exception writes can replace the record, and concurrent events or
FS failure can lose information. Termination records pad the 2 KiB slot with
spaces so shorter events do not retain stale register fields.
Direct host-library fatal paths outside these bindings are not intercepted.

Additional progress boundaries distinguish Render from its following host work:

- 46: onViewRenderJNI-return.
- 47/48: view-gl-finish-enter/return.
- 49/50: view-swap-enter/return (return target is swap boolean).
- 51: view-egl-release-enter.
- guest-view-complete / guest-view-swap-failed is now written after EGL release.

The old onViewRenderJNI breadcrumb alone did not establish a fault inside that
callback: glFinish, error queries and swap previously had no intervening record.
Keep the same ZR/ZL/ZL procedure and return crash/progress/pending together.
This build retains the pthread TLS fix; the new diagnostic behavior is pending
hardware validation and does not claim Render or gameplay success.

## Handoff quiesce fix (quiesce-v2)

Device round 7 stopped at `phase=handoff-quiesce-enter ordinal=80` with no
`handoff-quiesce-return`. `guestPthreadQuiesceAll()` joined running guest
threads (including the detached reaper) while the reaper was still blocked in
an infinite `condvarWait`; `gQuiesceRequested` was never set and the wake ran
only after those joins, so PLUS/Y could never leave the dashboard.

quiesce-v2 arms `gQuiesceRequested` first, wakes the reaper, joins the reaper
before any guest-worker join, and refuses new `pthread_create`/`detach` with
`EBUSY` once shutdown has started. PLUS after the View probe must write
`handoff-quiesce-return` and then `handoff-unload-return` (or report a real
unload failure) instead of hanging on enter.

A guest worker that never exits can still block the remaining joins; that is
a different failure and must not be conflated with the reaper deadlock.

## Handoff quiesce fix (quiesce-v3)

Device round 8 (loader-report): Guest View **completed** —
`callbacks=1/1/1 swap=1`, progress reached `view-probe-returned`. PLUS then
stopped again at `handoff-quiesce-enter` with `status=armed` only.

Root cause after the reaper fix: guest workers created during
`onViewChangedJNI` (and FMOD) run until process death. `::pthread_join` on
those never returns, so even a correct reaper-first quiesce still hangs.

quiesce-v3:

- Joins **only** the reaper. Never joins guest workers.
- Writes `handoff-quiesce-return`, then `handoff-quiesce-stuck` with the live
  thread count when workers remain.
- PLUS with live workers performs a **dirty exit**: skips finalizers/unmap,
  keeps the recovery marker, returns to hbmenu so the OS reclaims mappings.
  Next launch is Recovery (backup files first, then B to clear).
- Y (renderer reload) still refuses when workers remain — it must unload.
- Report adds `guest_pthreads live=N` and `frame_loop`/`frames`.

Expected PLUS breadcrumbs: `handoff-quiesce-enter` → `handoff-quiesce-return`
→ `handoff-quiesce-stuck` (if live>0) → `handoff-dirty-exit` →
`main-loop-exit` → `handoff-process-exit` (immediate `svcExitProcess`, no
nativeUiExit/unmount).

## Round 9 device result and quiesce-v4

Round 9 (`loader-report (9)` / progress `main-loop-exit`):

| Field | Value |
|---|---|
| guest_view | callbacks 1/1/1, swap=1, code=0 |
| frame_loop | entered, **frames=872** (~14s) |
| guest_pthreads | **live=6** |
| unload | attempted=0 (dirty path) |
| crash | armed only |

Operator notes: second ZL blacks out, then a **checkerboard** flashes; PLUS
leaves the frame loop to the dashboard; PLUS again to leave the app showed a
system error.

Interpretation:

- Checkerboard is **not** the RR3 menu. It is typical of an uninitialized or
  partially cleared GLES framebuffer while guest workers are still starting.
  Treat it as diagnostic output, not menu success.
- Frame-loop PLUS returning to the dashboard is intended.
- The leave-app error is the dirty-exit teardown race: after `main-loop-exit`
  the process still ran `nativeUiExit` / `unmountExternalData` / `return 0`
  while 6 guest workers were live.

quiesce-v4: on dirty exit, write `handoff-process-exit` and call
`svcExitProcess()` immediately. No finalizers, no UI teardown, no FS unmount.
OS reclaims everything; recovery marker stays.

## Automation

`--rr3-auto-boot` still chains dependency initialization and the guest boot at
startup. The Guest View probe is deliberately manual: normal validation is
**ZR**, **ZL**, **ZL**, report export, and **PLUS**.
