# RR3 native rehost probe v15 - process-code loader

V15 avoids the CodeMemory-object ceiling. Each DSO receives one
`svcMapProcessCodeMemory` mapping backed by its final relocated bytes. SVC 0x73
then applies guard, LOAD, and RELRO permissions to ModuleCode memory.

Copy only:

`dist/rr3-native-rehost-v15/rr3-native-rehost.nro`

Launch, wait, press **A**, and photograph the summary.

Expected new lines:

- `loader done=yes retain=yes RX=yes perm=yes ready=no invalid=0`
- `segmap tried=yes ok=yes mode=1 regions=7/7 fail=0`
- permission ranges `34/34`, failures `0`; the earlier CodeMemory-slave
  `p73fail=34` may remain recorded

Mode `1` is the process-code mapper. Mode `2` is the fallback hybrid mapper.
Any failure prints the existing `segdiag` tuple. No constructor runs while the
333 platform relocations remain.
