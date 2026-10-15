# RR3 native rehost probe v14 - diagnosed segmented mapping

V14 preserves the v13 mapper, flushes owner contents before MapSlave, and adds
an exact failure tuple:

`image / region / stage / Result / actual permission / expected permission`.

Copy only:

`dist/rr3-native-rehost-v14/rr3-native-rehost.nro`

If mapping succeeds, press A and photograph the summary. Expected:

- `loader done=yes retain=yes RX=yes perm=yes ready=no invalid=0`
- `segmap tried=yes ok=yes regions=21/21 fail=0`

If it fails, photograph the detail page containing `segdiag`. Stage meanings:

- 1 reserve target, 2 backing allocation, 3 CreateCodeMemory
- 4 owner address, 5 MapOwner, 6 MapSlave
- 7 QueryMemory, 8 permission mismatch, 9 content mismatch
- 10 old-JIT cleanup, 11 relocation recheck, 12/13 page-plan validation

No constructor executes on failure or while the 333 platform relocations remain.
