# Probe v15 real-device result

Status: **process-code path failed, then the now-removed normal-map fallback
failed with InvalidMemoryRange**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/6a1a4e1a73cc30903ad5221451436b63.jpg`

Visible final diagnostic came from fallback mode 2:

- image `0`, region `0`, stage `14` (`svcMapMemory`)
- Result `0x0000dc01` = kernel module 1, description 110
  (`InvalidMemoryRange`)
- target still RX (`5`), expected R (`1`)

This confirms normal-map destinations cannot occupy the selected CodeMemory
address range. The fallback overwrote the earlier process-code error, so V16
removes fallback mode 2 and records source/target MemoryInfo plus the original
stage/Result from `svcMapProcessCodeMemory` or SVC 0x73.
