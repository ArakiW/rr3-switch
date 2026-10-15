# RR3 native rehost probe v11 - page-correct loader

V11 fixes the v10 internal gate: GNU RELRO is page-rounded and is now checked
against the complete mapped image rather than one PT_LOAD segment.

Copy only:

`dist/rr3-native-rehost-v11/rr3-native-rehost.nro`

Launch, wait for the seven-DSO loader, press **A** once, and photograph the
summary. PLUS exits.

Expected deterministic values remain:

- `tests core B/P/L/C=3ff/3ff/ff/3f`
- `tests loader D/S/F/R/G=7/7/7/f/3f`
- `resolver=270 table=ed0e63ce1f6e2322`
- `loader done=yes retain=yes RX=yes ready=no invalid=0`
- `images=7 total=39680 KiB seg L/X/W/WX=21/7/13/0`
- `needed E/V/M=8/32/0 TLS=0`
- `reloc R/I/H/X/W/M=161535/3027/658/8928/39/333`
- `patch/verify=174187/174187 unique H/X/M=270/276/263`
- `RELRO=6/1702 KiB targets=166249 lifecycle=365/13/378`
- order hashes `ecc9d4762c84fe52/8747b5bc4aee3bdc`
- `plan=9362828b4164dbfa`

Permission capability is device-dependent:

- full: `api=yes ranges=34/34 fail=0 perm=yes`
- restricted: `api=no ranges=0/0 fail=0 perm=no`
- if `api=yes` and failures are non-zero, photograph the exact line

`ready=no` remains intentional because 333 strong platform relocations are
unresolved. No constructor is executed.
