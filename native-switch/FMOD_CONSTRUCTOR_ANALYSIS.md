# FMOD constructor pre-staging analysis

Prepared 2026-09-02, before the 0.19.1+ on-device dependency-constructor test.
Purpose: if libc++ `DoIOSInit` passes under `GuestTlsScope`, the next two guest
constructors to run are FMOD's. This maps them ahead of time so any device
fault is already attributed rather than re-investigated from scratch.

All offsets are guest-image relative in `reverse/native/arm64/libfmodex.so`
(AArch64 ELF). Disassembled with the project-private
`aarch64-none-elf-objdump` (devkitA64 16.1.0).

## Constructor inventory

`.init_array` is section [14] at `0x135000`, size `0x10` = **two 8-byte
entries**, matching the deterministic audit (FMOD = 2 constructors). The two
targets are:

- `0x215FC`
- `0x216B8`

Order libc++(1) -> Nimble(0) -> FMOD(2) is unchanged from
`DEPENDENCY_CONSTRUCTOR_AUDIT.md`.

## TLS-fault risk assessment

The 0.19.0 crash was libc++ reading the Android stack guard from
`TPIDR_EL0 + 0x28` while TPIDR_EL0 was zero. The relevant question for FMOD is
whether either constructor (or anything they call in their prologue) performs
the same `mrs xN, tpidr_el0` read.

- Neither `0x215FC` nor `0x216B8` reads TPIDR_EL0 in its own body. Their
  prologues only touch `sp`, `adrp/add` literals, and store global state.
- Their direct `bl` targets in range are `__cxa_atexit` (guest-shimmed),
  FMOD-internal `0x38474`, `0x32e0c`, `0xb0120`, and `0x42c1c`. Spot-checking
  `0x38474` and its callees shows the same pattern — no `tpidr_el0` read in the
  reachable prologue path.

`libfmodex.so` does contain 28 `tpidr_el0` reads overall, but they are not on
the two constructors' immediate paths. Because the constructors run **inside**
`GuestTlsScope` (which installs a valid TPIDR_EL0 with a canary at +0x28 for
the whole lifecycle window — see `source/multi_dso_linker.cpp:2258`), even a
deeper stack-guard read would resolve against the scoped block rather than
faulting on zero. The register-collision safety (host libnx uses TPIDRRO_EL0
only) is covered separately and is not re-argued here.

Conclusion: **FMOD's two constructors are not expected to reproduce the 0.19.0
TLS fault.**

## Destructor registration

`0x215FC` calls `__cxa_atexit` (PLT `0x20cc0`) to register two destructors;
these resolve through the rehost shims:

- `symbol_resolver.cpp:448` `__cxa_atexit -> guestCxaAtexit`
- `symbol_resolver.cpp:449` `__cxa_finalize -> guestCxaFinalize`
- `symbol_resolver.cpp:450` `__cxa_thread_atexit_impl -> guestCxaThreadAtexitImpl`

So the destructors FMOD registers during construction are captured by the
rehost's own destructor table (`source/cxx_lifecycle.cpp`) and will be replayed
by `unloadLinkElfSet` -> `finalizeInitializedGuestImages` in reverse order on
PLUS/Y exit. This matches the audit's "1 finalizer" for FMOD plus the
`__cxa_atexit`-registered pair.

## What to capture if FMOD faults on device

If the device fault PC lands in `0x215FC..` or `0x216B8..`:

1. Preserve the Atmosphere report and
   `sdmc:/switch/rr3-native-rehost.guarded-action.pending` before pressing B.
2. Record PC/LR guest offsets. Subtract the FMOD runtime base from the loader
   snapshot to get the image-relative offset.
3. If the faulting instruction is a `tpidr_el0` load with a non-zero base, the
   issue is block *layout* (offset within the scoped TLS), not a missing TLS —
   different fix from 0.19.0.
4. If the fault is in `__cxa_atexit`/destructor registration, inspect the guest
   destructor table capacity (`kMaximumGlobalDestructors = 256` in
   `cxx_lifecycle.cpp`).

## Audio device policy (silent stub, never claims audible output)

Horizon has no Android audio device. FMOD guest threads that wait for
OpenSL/AAudio callbacks would deadlock. The rehost therefore provides:

1. **Silent OpenSL device** (`source/fmod_audio_stub.cpp`):
   `slCreateEngine` succeeds; buffer `Enqueue` discards samples immediately
   and fires the registered callback on the enqueuer thread so the mixer
   never blocks on hardware. `SetPlayState(PLAYING)` is recorded as
   `playRequested` only. **`audible` is always 0** — the stub never claims
   samples reached a speaker.
2. **Clear AAudio errors**: `AAudioStreamBuilder_create` / `openStream`
   return `AAUDIO_ERROR_UNAVAILABLE` (-899). No silent "stream started".
3. **Virtual DSOs**: `guestDlopen("libOpenSLES.so" | "libaaudio.so")` returns
   a stub handle; `dlsym` resolves the silent/error entry points.
4. **Bounded waits**: `guestSemWait` uses trywait + 500 ms slices with
   `guestViewTraceSyncBlocked("sem-wait")` breadcrumbs (same pattern as
   `guestPthreadCondWait`). FMOD workers therefore cannot hard-block without
   a durable report.

Self-test bits: `fmodAudioStubSelfTest` expected `0x3f`.
Report line: `fmod_audio silent=yes ... audible=0 ...`.

This does not change the dependency-constructor path: the two FMOD
constructors only initialize global state and still pass 3/3 on device.
Device open happens later, when (and if) the game calls `FMOD_System_Init`.
