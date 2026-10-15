# RR3 native rehost probe v5 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v5/rr3-native-rehost.nro`

## New v5 gate

V5 adds real target addresses for:

- Bionic `__errno`;
- checked memory/string/printf fortify functions;
- `__strncpy_chk2`;
- `__stack_chk_fail`;
- `__android_log_print` and `__android_log_vprint`.

These functions are linked and discoverable but are not written into the game image or invoked by game code.

Expected new lines:

- `Bionic shim selftest=3ff expected=3ff`
- `resolver=134 table=9a518f373646ed51`
- `lookup sym=134/572 reloc=155/9250`
- `lookup names=169f327679cb5bd7`

All earlier ELF and symbol counts must remain unchanged. Press PLUS to exit and photograph the complete result.

Pthread remains deliberately unresolved because Android and newlib pthread object layouts cannot be assumed compatible.
