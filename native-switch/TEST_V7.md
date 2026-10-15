# RR3 native rehost probe v7 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v7/rr3-native-rehost.nro`

The NRO now embeds both `libRealRacing3.so` and its matching Android `libc++_shared.so`.

## New v7 gate

Expected libc++ block:

- `image=1028 KiB rel=1428 pending=162 internal=2283 invalid=0`
- `symbols=160 weak=1 versioned=159 max=2436`
- `lookup sym=62/98 reloc=63/99`
- `hash=5a0f86a4994217bd names=91591e75c02f690d matched=da30bfee6f6c085c`

Both Bionic and pthread self-tests must remain `3ff`. V7 copies and relocates libc++ in a temporary dry image only; it does not retain executable mappings, cross-resolve `std::__ndk1`, or call constructors.

Press PLUS to exit and photograph the libc++ block. A count/hash mismatch blocks multi-DSO loading.

