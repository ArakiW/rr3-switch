# Probe v16 real-device result

Status: **process-code mapping succeeded; first permission operation was an
invalid redundant transition**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/dfb211f861022baf95285951fe1d365a.jpg`

Diagnostic:

- image `0`, stage `21`
- Result `0x0000d401` = Kernel InvalidMemoryState
- backing `type=5, perm=3, attr=0` (valid Heap RW)
- target `type=8, perm=0` (AliasCode already None)

`svcMapProcessCodeMemory` therefore succeeded. The error came from redundantly
setting the entire AliasCode mapping to None even though it is created as None.
V17 removes those seven reset calls. Gaps naturally remain None; only 21 LOAD
and six RELRO operations are needed.
