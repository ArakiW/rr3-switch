# RR3 native rehost probe v6 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v6/rr3-native-rehost.nro`

## New v6 gate

V6 adds all 33 imported pthread names through a guest-object bridge. Android mutex/condition/rwlock/once/attribute memory is used only as an opaque key; real newlib objects live in fixed-capacity NRO registries. Android detach constants are translated explicitly.

Expected new lines:

- `Bionic shim selftest=3ff expected=3ff`
- `Pthread bridge selftest=3ff expected=3ff`
- `resolver=167 table=0b5b4522fdb57f09`
- `lookup sym=167/539 reloc=188/9217`
- `lookup names=8e9c95d40d07e9cb`

The pthread self-test covers recursive mutex, condition signal/broadcast, rwlock read/write, once, TLS key, thread create/join, attr translation, and self/equal. Game relocations are still not patched.

Press PLUS to exit and photograph the complete result. A hang before the ELF block is a pthread bridge failure; power-cycle only if PLUS no longer responds.

