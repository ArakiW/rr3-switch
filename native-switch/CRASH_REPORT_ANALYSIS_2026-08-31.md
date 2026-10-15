# Crash-report analysis — 2026-08-31

Source directory: `E:/AI/RealRacing3-switch/crash_reports` (read-only copy).

## Confirmed 0.18.3 application crash

Reports `01788116325_05901c2b378bc000.log` and
`01788116387_0540f981db4f4000.log` contain:

- module `rr3-native-rehost`;
- module ID `402C3A943ED4E5BB4272DCE3DB070832156FFE5F`, which exactly
  matches the packaged 0.18.3 NRO;
- Result `0x2B59` = Module_Libnx, description 21 =
  `LibnxError_BadGfxDequeueBuffer`;
- PC `rr3-native-rehost + 0x3F0F6C`;
- LR `rr3-native-rehost + 0x3E7B2C`;
- stack contains `nwindowDequeueBuffer`-side graphics code.

Conclusion: the 0.18.3 console-to-EGL hot switch reached the native window
buffer dequeue path, but the NWindow state was not valid for a new graphics
consumer. This is not a guest ELF, relocation, or constructor failure.

## Confirmed hbloader return failure

The latest reports `01788118534`, `01788118702`, and `01788119662` contain:

- module `hbl`, module ID `638144AE946720B09A2B2BAF578D70E00F81147E`;
- Result `0xD401` = Kernel `InvalidMemoryState`;
- PC `hbl + 0x3918`, LR `hbl + 0x356C`;
- no loaded `rr3-native-rehost` module in the report.

The packaged NRO module IDs are:

- 0.18.3: `402C3A943ED4E5BB4272DCE3DB070832156FFE5F`;
- 0.18.4: `44EE226883BB82B6600054FB0582A7A99E68E284`;
- 0.18.5: `87D0C2B5A34952ECF0D29667559DC9B88A57FCA8`.

Conclusion: the visible 0.18.5 error occurs after control returns to hbloader,
not as an 0.18.5 application exception. `envSetNextLoad` reloads an NRO inside
the same hbloader process. The retained four-DSO ModuleCode mappings therefore
must be released before hbloader attempts its own NRO unmap/remap sequence.

## Implemented correction

0.18.7 adds an explicit reverse-order unload transaction:

- unmap every retained guest ProcessCode mapping;
- free backing memory only after successful unmap;
- release the corresponding virtmem reservation;
- clear loader images and dynamic-link scope only after all unmaps succeed;
- gate Y handoff and PLUS exit on a successful transaction;
- remain in the UI and show the first Result if cleanup fails.

Expected successful cleanup is `active/unmapped=4/4`, `reservations=4`,
`fail=0`, `rc=00000000`.
