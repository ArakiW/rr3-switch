# Emulator debug environment

Eden v0.2.1 Windows amd64 Clang PGO is installed as a portable tool at:

`E:/AI/RealRacing3-switch/.tools/emulators/eden-0.2.1`

Official release source:

`https://git.eden-emu.dev/eden-emu/eden/releases`

Downloaded archive SHA-256:

`6C1B53CE325170A026CC0F87098380027DC6170D94BA95F913AAB7596FD097CB`

No keys, firmware, or commercial content were downloaded or imported.

## Current result

`eden-cli.exe -g rr3-native-rehost.nro` recognizes and begins loading the
homebrew NRO, initializes Vulkan on an RTX 3080, audio, account, clock, the
Simplified Chinese system-font archive, and applet services, and creates a
60 FPS render window. The 0.20.0 regression still stops inside Eden's emulated
Applet SelfController at `SetOperationModeChangedNotification`, before the NRO
main function and framebuffer UI become automatable. This is unchanged from
0.19.1 and is not counted as a UI runtime pass.

0.20.0 includes an explicit `--rr3-auto-deps` argument for emulator-only
automation without virtual controller input. Ordinary Switch launches never
set it. Eden's program-argument path remains to be validated after its applet
startup limitation is resolved.

Emulator results are regression evidence only and never replace Switch hardware
validation for Horizon memory-state, NWindow, or TPIDR_EL0 behavior.
