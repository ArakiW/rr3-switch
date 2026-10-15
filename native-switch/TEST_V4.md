# RR3 native rehost probe v4 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v4/rr3-native-rehost.nro`

## New v4 gate

V4 links 121 real target addresses for low-risk C/newlib/libm functions such as memory, strings, numeric conversion, basic stdio, math, and time. It performs lookups only; it does not write these addresses into the game image.

Expected new lines:

- `resolver=121 table=ed88ffbeb612e9a2`
- `lookup sym=121/585 reloc=142/9263`
- `lookup names=c789bd33415c9b44`

All previous v3 counts and hashes must remain unchanged.

Press PLUS to exit and photograph the complete result. A mismatch means the binding table or target linker output differs; do not proceed to patching relocations.

