# Probe v14 real-device result

Status: **segmented algorithm reached the 11th CodeMemory object and hit a
kernel resource result**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/102758ad2f8c829f738b91a2fc42f66f.jpg`

Diagnostic:

- image `3` (`libarcore_sdk_c.so`)
- global region `10` (the 11th region)
- stage `3` (`svcCreateCodeMemory`)
- Result `0x0001ce01`, module `1`, description `231`
- ten previous regions succeeded

The mapping rules and content flush were therefore valid, but one CodeMemory
object per permission region exceeds the available kernel object/resource
budget. V15 uses seven `svcMapProcessCodeMemory` mappings, one per DSO, and then
applies the already available SVC 0x73 permissions to ModuleCode pages.
