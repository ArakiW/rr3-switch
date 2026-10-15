# RR3 native rehost probe v2 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v2/rr3-native-rehost.nro`

Recommended SD path:

`sdmc:/switch/rr3-native-rehost/rr3-native-rehost.nro`

This NRO is **33,360,996 bytes**. It embeds `libRealRacing3.so`; do not copy a separate SO.

## Expected screen

The top should say:

`Probe v2: embedded ELF + dry loader`

Expected key results:

- `RomFS=00000000`
- `source: romfs:/libRealRacing3.so`
- `A64=yes DYN=yes`
- `rel=142661 abs64=8728 glob=202 jump=509 unsupported=0`
- `image=32544 KiB`
- `relative=142661 pending=9439 invalid=0`
- `pre-reloc hash=1db1197623a71fdc`

The dry loader copies all three PT_LOAD segments, zero-fills BSS, validates that dynamic and relocation tables are file-backed LOAD data, and applies only `R_AARCH64_RELATIVE`. It does not execute constructors or game code.

Press PLUS to exit. Photograph the complete result screen, especially the SD/RomFS line and the dry-load block.

## Failure interpretation

- `RomFS != 00000000`: embedded asset mount failed.
- `source: not found`: old/small NRO was copied instead of v2.
- `image allocation failed`: launch hbmenu in full application mode and retry.
- `invalid relocation range or target`: stop; do not proceed to constructors.
- Crash before the dry-load block: report the last visible line.

