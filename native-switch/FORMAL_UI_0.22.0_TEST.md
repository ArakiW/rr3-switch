# Formal native UI 0.22.0 - guest-boot (stage 9) on-device test

First TF-card test of the RR3 main-image constructors and the first real
runtime entry point, `Java_com_firemint_realracing_MainActivity_onCreateJNI`,
called through the minimal host-side JNIEnv. Builds on the 0.21.0 dependency
gate in `FORMAL_UI_0.21.0_TEST.md`; do that test first if the dependency
constructors have not yet passed on this card.

## Install once

Copy the whole `switch` folder from
`dist/rr3-native-rehost-complete-0.22.0/` to the TF-card root so the final
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

The seven data volumes are unchanged from 0.21.0 (identical hashes); only the
NRO changes. Optional integrity check before ejecting: from the package root
run `sha256sum -c checksums.sha256` (expects all ten entries OK, including NRO
`292b75aa…f02838`).

## Preconditions (must hold before the new stage-9 step)

Complete the 0.21.0 checks first. In particular:

1. External data **7/7 volumes mounted**, `ready=yes`.
2. Runtime page `Guest filesystem VFS` = `fff`, `Guest TPIDR_EL0 scope` = `f`.
3. If Overview shows a recovery state from a prior interrupted run, preserve any
   Atmosphere report and
   `sdmc:/switch/rr3-native-rehost.guarded-action.pending`, then press **B** to
   clear the marker.
4. Press **ZR once** for the dependency constructors. The Loader page must show
   attempted/preflight/complete/active = `yes/yes/yes/yes`, constructors `3/3`.
   The stage-9 boot is gated on this being complete **and** still active in the
   same session (the marker stays armed until PLUS-exit), so run ZL without
   power-cycling in between.

## Guarded guest boot (new in 0.22.0)

This is the point of the test: run the RR3 main image's 181 init_array
constructors, then call `onCreateJNI`.

1. With the dependency gate complete+active (step 4 above), press **ZL once**.
   This re-arms the durable snapshot with action `guest-boot`, runs the 181 main
   constructors under `GuestTlsScope` with per-ordinal progress, and — only if
   they all complete — calls `onCreateJNI` with the minimal 232-slot JNIEnv and
   a non-null fake activity jobject.
2. On success the new **GUEST BOOT (STAGE 9)** block on Loader page 2 shows:
   - `Main ctors: attempted=yes preflight=yes complete=yes called=181/181 code=0`
   - `onCreateJNI: attempted=yes resolved=yes called=yes returned=yes code=0`
   - `JNI bridge: GetMethodID=<n> unhandled=<n> lastSlot=<n>`
3. Do **not** press ZL again. Press **PLUS** for the finalizers + reverse unmap
   + marker clear + clean return to hbmenu.
4. Relaunch. Overview must report a clean (non-recovery) state.

## Export the report

Hold **L+R** and press **X** once to enable developer mode, then release L+R and
press **X** alone to write
`/switch/rr3-native-rehost/loader-report.txt`. The stage-9 lines to read back:

```
main_init attempted=1 preflight=1 completed=1 active=1 stage=<s> code=0 called=181 planned=181 image=<i> ordinal=<o> target=<addr>
guest_entry attempted=1 resolved=1 called=1 returned=1 code=0 method_bindings=<n> unhandled=<n> last_slot=<n>
```

## Reading the outcome

- **`main_init called=181/181 completed=1`** — the full native init chain (4 core
  DSOs → 158445 relocs → deps libc++/Nimble/FMOD → 181 RR3 main ctors) has now
  run on device. If it stops short, `ordinal=` + `target=` name the faulting
  constructor; subtract the main image's snapshot base for the image-relative
  offset.
- **`guest_entry returned=1`** — `onCreateJNI` completed without a fault; it built
  the ~0x540-byte platform Activity object through the minimal JNIEnv.
- **`unhandled=<n>` / `last_slot=<n>`** — if `onCreateJNI` reached for a JNI slot
  the minimal table does not yet implement, `last_slot` is that slot index and
  `unhandled` counts how many such calls happened. That is the exact next slot to
  implement; a stub returns 0/null rather than crashing so the probe still
  returns.

### If it faults

Restart but do **not** press ZR/ZL (the durable marker forces Recovery). Preserve
the newest Atmosphere report and the `.guarded-action.pending` marker before
pressing B. Known risk: `onCreateJNI` may touch uninitialized device-config
globals (`DAT_02094f28`, the doc-path globals) if no constructor set them — this
is expected probe-discovery, and the per-ordinal + `last_slot` telemetry exists
to pinpoint it.

### Failure codes

`main_init code=`: 1 loader session inactive, 2 already initialized, 3/5 bad
target (not in an RX PT_LOAD), 4 TLS scope failed, 6 no main image.
`guest_entry code=`: 1 onCreateJNI export not found, 2 could not install guest
TLS.

## Automation

`--rr3-auto-boot` (argv) chains the dependency init and the guest boot at
startup, for a hands-free run; ordinary testing uses ZR then ZL.
