# RR3 native rehost probe v9 - concentrated real-device batch

V9 deliberately combines several gates so the NRO only needs to be copied to
the TF card once.

## Install and run

Replace the previous NRO with:

`dist/rr3-native-rehost-v9/rr3-native-rehost.nro`

Launch it from hbmenu and wait for the detailed probe to finish. Press **A** once
to open the compact batch-summary page, then photograph that single page. Press
PLUS to exit.

## Expected batch summary

- Basic JIT: `call=yes`, close result `00000000`.
- Image-sized JIT: `33572 KiB`, create/exec/close all `00000000`, both edges
  `yes/yes`.
- Self-tests: `B=3ff P=3ff L=ff C=3f D=7 S=7 F=7`.
- Resolver: `265`, table hash `6cdd4e315206951e`.
- libc++ lookup: `sym=160/0 reloc=162/0`.
- libc++ matched-name hash: `91591e75c02f690d`.
- Two-DSO link: `completed=yes retained=yes invalid=0`.
- Relocations: `rel=144089 internal=2317 patch=155658 verify=155658`.
- Main providers: `H/X/W/M=211/8842/37/315`.
- libc++ providers: `H/X/W/M=162/0/0/0`.
- Unique providers: `H/X/M=265/190/305`.
- Lifecycle inventory: `ctors=181+1 fini=4 verified=186`.
- Deterministic plan hash: `ecee08eba8dfe616`.

V9 writes and reads back relocation slots in two simultaneously retained heap
images. It does not run the 182 constructors. The large JIT test executes only
two controlled AArch64 `ret` instructions at the first and last edge of a
33,572-KiB allocation; it never jumps into game or libc++ code.
