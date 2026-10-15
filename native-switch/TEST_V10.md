# RR3 native rehost probe v10 - loader-framework batch

V10 embeds and loads seven ARM64 DSOs in one dependency-aware session. It is a
single-copy concentrated test.

## Install and run

Copy only:

`dist/rr3-native-rehost-v10/rr3-native-rehost.nro`

Replace the old NRO, launch from hbmenu, wait for the detailed probe, then press
**A** once and photograph the compact loader-summary page. PLUS exits.

## Deterministic expected values

- Core tests: `B/P/L/C=3ff/3ff/ff/3f`.
- Loader tests: `D/S/F/R/G=7/7/7/f/3f`.
- Resolver: `270`, table `ed0e63ce1f6e2322`.
- libc++ remains `160/0` symbols and `162/0` relocations.
- Loader: `done=yes retain=yes RX=yes ready=no invalid=0`.
- Images: `7`, total `39680 KiB`.
- Segments `L/X/W/WX=21/7/13/0`; TLS `0`.
- Needed `E/V/M=8/32/0`.
- Relocations `R/I/H/X/W/M=161535/3027/658/8928/39/333`.
- Patch/verify `174187/174187`; unique `H/X/M=270/276/263`.
- RELRO `6/1702 KiB`, targets `166249`.
- Lifecycle `365/13/378`.
- Constructor/finalizer order hashes:
  `ecc9d4762c84fe52/8747b5bc4aee3bdc`.
- Resolution plan `9362828b4164dbfa`.

## Permission capability line

This line is intentionally device/environment dependent:

- Full path: `api=yes`, `ranges=34/34`, `fail=0`, `perm=yes`.
- Restricted hbloader path: `api=no`, `ranges=0/0`, `fail=0`, `perm=no`.
- `api=yes` with a non-zero failure count means the environment exposes SVC
  0x73 but rejects permission changes on CodeMemory aliases; report the exact
  line.

`ready=no` is expected in every case because 333 strong platform relocations
still lack providers. V10 does not execute any of the 365 constructors.
