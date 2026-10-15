# Formal native UI 0.20.0 - one-write consolidated validation

This is the first package intended for TF-card testing after the raw probe
series. One installation covers loader, UI, recovery and renderer checks.

## Install once

Copy once:

`dist/rr3-native-rehost-ui-0.20.0/rr3-native-rehost.nro`

to:

`sdmc:/switch/rr3-native-rehost/rr3-native-rehost.nro`

No external SO files are required.

## UI validation

1. Launch and confirm a 1280x720 graphical Chinese loading screen appears; the
   old terminal dashboard must not be the default.
2. Player mode exposes two pages. Use L/R to visit Status and Compatibility.
3. Status should show original Simplified Chinese as preferred and original
   menu motion tracks as retained.
4. Hold L+R and press X once to expose the two engineering pages.
5. Runtime should show all self-tests as OK, including Guest TPIDR_EL0.
6. Loader should show 158445 patched/verified and invalid 0.
7. Diagnostics should show the one-pass process-code result and deterministic
   plan `999a029e95729174`.
8. ModuleCode permission ranges should be `11/11`; LOAD and RELRO never overlap
   in the submitted range list.

## Dependency initialization validation

1. Open Loader page 3. The staged plan must show four images and dependency/main
   constructors `3/181`.
2. Runtime page must show `Guest TPIDR_EL0 scope [OK]`, expected `f`.
3. After preserving any prior crash report, press B once if Recovery is set.
4. Press ZR once. This is the only step that creates the small recovery snapshot.
5. On success, Loader shows attempted/preflight/complete/active =
   `yes/yes/yes/yes`, constructors `3/3`, stage `4`, code `0`.
6. Do not press ZR again. Press PLUS. Five dependency finalizers run, followed by
   the already-proven four-image reverse unmap; the marker is then deleted.
7. Launch the same NRO again. Overview must report a clean recovery state.

If a dependency constructor crashes, restart normally but do not press ZR. The
dashboard will show recovery mode. Preserve the latest Atmosphere report and
`sdmc:/switch/rr3-native-rehost.guarded-action.pending`, then press B to clear
the marker only after those files are copied.

## Existing renderer validation

Y retains the unload-gated isolated renderer path. It is optional in this
validation batch.

X report export is optional and is the only ordinary UI action that writes a
diagnostic report. RR3's 181 main constructors are not executed in 0.20.0.
