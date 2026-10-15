# RR3 native rehost probe v3 - real-device test

## Install

Replace the previous NRO with:

`dist/rr3-native-rehost-v3/rr3-native-rehost.nro`

The SO remains embedded. Recommended SD path:

`sdmc:/switch/rr3-native-rehost/rr3-native-rehost.nro`

## Expected screen

- `Probe v3: ELF + dynamic symbol audit`
- `source: romfs:/libRealRacing3.so`
- `relative=142661 pending=9405 invalid=0`
- `pre-reloc hash=1db1197623a71fdc`
- `symbols=706 max=830 internal-reloc=34`
- `weak=22 versioned=284 names=5b63bc22017abc04`
- `GL=142 EGL=3 FMOD=52 AR=19 pthread=33`
- `CXX=194 android=2 other=261`

V3 validates `DT_SYMTAB`, `DT_SYMENT`, `DT_STRTAB`, `DT_STRSZ`, optional `DT_VERSYM`, every referenced symbol index, every NUL-terminated name, and whether each relocation refers to an internal or undefined symbol. It still does not patch external addresses or execute constructors.

Press PLUS to exit and photograph the complete result.

## Failure interpretation

- `symbol, string, or version table is outside file-backed LOAD data`: stop before binding.
- `invalid relocation table, type, or target`: relocation contract mismatch.
- A count/hash mismatch: the embedded SO or parser behavior differs; do not continue to constructors.

