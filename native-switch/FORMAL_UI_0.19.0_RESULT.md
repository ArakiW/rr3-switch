# Formal UI 0.19.0 preliminary device result

Device result:

- pressing ZR during the guarded dependency stage terminated the software;
- the durable marker survived the crash;
- the next normal launch entered Recovery state;
- dependency initialization was blocked while Recovery was active;
- the Loader page still reported the expected deterministic `3/181` split;
- RR3's 181 main constructors were not called.

This validates the recovery and no-repeat gates but fails the dependency
constructor batch.

The copied report `01788162015_05901c2b378bc000.log` provides exact attribution:

- 0.19.0 NRO module ID `1D24582172BD5135D5C257F6FEB6C04060B8C0CE`;
- guest module range `0x6E9B90000-0x6E9C7F000`, matching libc++;
- LR guest offset `0x943DC`, inside the sole libc++ constructor at `0x94398`;
- PC guest offset `0x92F4C`, `DoIOSInit + 0x28`;
- Data Abort at address `0x28` with register X23 equal to zero.

Disassembly at `0x92F48-0x92F4C` is `mrs x23, tpidr_el0; ldr x8,
[x23,#0x28]`. FMOD constructors were not reached. 0.19.1 installs a scoped
Android-compatible TPIDR_EL0 block with a stable canary at `+0x28` around guest
constructors/finalizers, then restores the original register.
