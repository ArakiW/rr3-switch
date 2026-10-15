# RR3 native rehost probe v17 - corrected process-code permissions

V17 keeps the successful seven `svcMapProcessCodeMemory` mappings and removes
the invalid whole-image `None -> None` transitions.

Copy only:

`dist/rr3-native-rehost-v17/rr3-native-rehost.nro`

Launch, wait, press **A**, and photograph the summary.

Expected:

- `loader done=yes retain=yes RX=yes perm=yes ready=no invalid=0`
- `perm ... ranges=27/27 fail=0 p73fail=34`
- `segmap tried=yes ok=yes mode=1 regions=7/7 fail=0`
- all relocation, RELRO, lifecycle, order and plan values unchanged

The 27 operations are 21 LOAD permissions plus six RELRO overrides. Gaps remain
the AliasCode mapping's initial None state. No constructor runs while 333 strong
platform relocations remain.
