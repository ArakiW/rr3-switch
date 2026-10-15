# Formal native UI 0.21.0 - external-data on-device test

First TF-card test of the seven-volume external data layout plus the guarded
dependency-constructor path. Supersedes the 0.20.0 embedded-data procedure in
`FORMAL_UI_TEST.md`.

## Install once

Copy the whole `switch` folder from
`dist/rr3-native-rehost-complete-0.21.0/` to the TF-card root so the final
layout is:

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

All seven volumes are under 2 GiB and FAT32-safe. Optional integrity check
before ejecting: from the package root run `sha256sum -c checksums.sha256`
(expects all ten entries OK, including NRO `1c9c87eb…f73da5`).

## UI validation (player-facing)

1. Launch. Confirm a 1280x720 graphical Simplified Chinese screen appears — not
   the terminal dashboard.
2. Player mode exposes two pages; use L/R for Status and Compatibility.
3. Status should show original zh-Hans as preferred and original menu motion
   tracks as retained.
4. Hold L+R and press X once to reveal the engineering pages.

## External-data validation (new in 0.21.0)

1. On the Runtime page, `Guest filesystem VFS` must read `[OK]`, expected `fff`.
   This is the path-ABI self-test; anything other than `fff` means one of the
   content/save/cache routing rules failed and dependency init will stay gated.
2. On the Loader/Overview page confirm external data reports **7/7 volumes
   mounted** and `ready=yes`. If a sentinel is missing the dashboard names the
   offending volume — re-copy that `.romfs`.
3. If a volume fails to mount, the fallback is loose files under
   `/switch/rr3-native-rehost/data/loose/…`; not needed if the seven volumes
   mount.

## Runtime self-tests (must all pass before ZR)

Runtime page expected masks:

- Bionic fortify/log `3ff`, Guest pthread bridge `3ff`, libc++ shims `ff`,
  C++ lifecycle `3f`, futex/gettid `7`, Bionic stdio `7`, Loader rollback `f`,
  Program-header registry `7`, Guest dynamic linker `3f`, Optional AR/telemetry
  `f`, Guest semaphore bridge `3f`, Guest setjmp/longjmp `3`, Bionic misc/mmap
  `3f`.
- **Guest TPIDR_EL0 scope must read `f`** (install/canary/restore). This is the
  0.19.1 fix for the 0.19.0 `DoIOSInit+0x28` crash; do not press ZR if it is not
  `f`.
- Guest filesystem VFS `fff` (see above).

Loader page must show 4 images, `11/11` final permission ranges, 158445
patched/verified, invalid 0, and dependency/main constructors `3/181`.

## Guarded dependency constructors

This is the gate that failed on device at 0.19.0 and is the point of this test.

1. If Overview shows a recovery state from a prior interrupted run, first
   preserve any Atmosphere report and
   `sdmc:/switch/rr3-native-rehost.guarded-action.pending`, then press B to
   clear the marker.
2. Press **ZR once**. This writes the single durable snapshot and calls the
   three dependency constructors in order libc++(1) -> Nimble(0) -> FMOD(2)
   inside `GuestTlsScope`.
3. On success the Loader page shows
   attempted/preflight/complete/active = `yes/yes/yes/yes`, constructors `3/3`,
   stage `4`, code `0`.
4. Do **not** press ZR again. Press **PLUS**: five dependency finalizers run,
   then the proven four-image reverse ProcessCode unmap, then the marker is
   deleted and the app returns cleanly to hbmenu.
5. Relaunch. Overview must report a clean (non-recovery) state.

### If a constructor faults

Restart normally but do **not** press ZR (the durable marker will already have
forced Recovery). Then:

1. Preserve the newest Atmosphere report and the `.guarded-action.pending`
   marker (copy both off the card) before pressing B.
2. Attribute the fault: subtract the faulting image's runtime base (from the
   snapshot) from the report PC to get the image-relative offset.
   - libc++ `0x94398` region: the 0.19.0 signature. If it now passes the
     `+0x92f4c` TLS load, the fix worked.
   - FMOD `0x215fc` / `0x216b8`: see `FMOD_CONSTRUCTOR_ANALYSIS.md` — not
     expected to hit the TLS fault.
3. RR3's 181 main constructors are never called in 0.21.0 regardless of the
   dependency result.

## Optional

- **Y** retains the unload-gated isolated renderer stage; optional here.
- **X** (developer mode) exports a diagnostic report to
  `/switch/rr3-native-rehost/loader-report.txt`; the only ordinary UI action
  that writes.
