# RR3 native rehost probe v8 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v8/rr3-native-rehost.nro`

The NRO embeds both `libRealRacing3.so` and the matching Android
`libc++_shared.so`; no separate SO files are required.

## Expected v8 gate

- `Bionic shim selftest=3ff expected=3ff`
- `Pthread bridge selftest=3ff expected=3ff`
- `libc++ shim selftest=ff expected=ff`
- Main: `resolver=259 table=37ac093e5c82d913`
- Main: `lookup sym=185/521 reloc=207/9198`
- Main: `lookup names=eac68cdec0838ac9`
- libc++: `lookup sym=154/6 reloc=155/7`
- libc++: `hash=5a0f86a4994217bd names=91591e75c02f690d matched=f29c03cd904d958c`

The six intentionally deferred libc++ names are `__cxa_atexit`,
`__cxa_finalize`, `__cxa_thread_atexit_impl`, `__sF`, `dl_iterate_phdr`, and
`syscall`. V8 still performs lookup and self-tests only: it does not patch the
guest image, retain executable DSO mappings, or call constructors.

Press PLUS to exit and photograph the self-test, main lookup, and libc++ lookup
blocks. Any count or hash mismatch blocks multi-DSO loading.
