# Formal native UI status

Formal UI 0.20.0 is locally built as a 1280x720 double-buffered native
framebuffer interface. It uses the package's Conduit/Eurostile faces and the
Switch shared Simplified Chinese font. The package's `cn-Hans` catalog is the
first localization source, with English present as fallback. Recovery/no-repeat
passed; RR3 main constructors remain disabled.

Implemented:

- two-page player-facing Chinese graphical UI, with engineering pages hidden
  behind the L+R+X chord
- L/R and D-pad navigation; PLUS exits
- `--rr3-console` diagnostic fallback and developer-only X report export
- original `text_cn-Hans.txt`, `text_en.txt`, `langs.utf8`, and Latin fonts
  embedded in RomFS without rewriting
- original `MenuTransitions.xml` plus nine menu/garage `.banim` tracks embedded
  unchanged; source windows 0-40 and 0-90 are validated at startup
- dynamic activation gate and progress display
- NACP title, author and version metadata
- one-pass ModuleCode loader based on the nx-hbloader sequence
- Mesa EGL/GLES symbol provider
- socket service initialization
- guest semaphore and setjmp/longjmp bridges
- Bionic fortify, mmap, statfs, stdio objects and legacy ctype bridge
- ARCore unsupported-device behavior and disabled fuelmetrics API

Host-validated core session:

- four images, 11 disjoint final permission operations
- zero strong missing relocations
- zero missing DT_NEEDED entries
- 158,445 patched and verified relocation slots
- zero invalid relocations

Release validation completed:

1. Y schedules an hbloader self-restart with `--rr3-renderer`; the EGL-only
   NRO stage returns before `consoleInit`, then a later console-only NRO displays
   the argv-encoded result;
2. recovery markers gate guarded actions after interruption;
3. all four UI pages were device-validated in 0.18.0;
4. 0.18.1 fixes ModuleCode permissions by submitting 11 disjoint final ranges;
5. NACP, icon, RomFS, checksum and report schema are embedded and verified.
6. 0.18.3 runtime display hot-switch was removed after its device crash;
7. a static contract forbids multiple `consoleExit` calls, requires the only
   renderer probe call to precede `consoleInit`, and requires the Y path to use
   hbloader next-load rather than live display switching.
8. the EGL call order and static-library link order now match the official
   switch-examples GLES2 path.
9. Y and PLUS are gated by a reverse-order four-image ProcessCode unload;
   backing memory and reservations are released only after successful unmap.
10. 0.18.7 passed on device: clean PLUS return and unload-gated renderer NRO
    handoff no longer produce `hbl 0xD401`.
11. 0.19.0 inventories a deterministic 3/181 dependency/main constructor split,
    writes one durable runtime-base snapshot, and calls only the three dependency
    constructors under ZR.
12. clean exit runs five dependency finalizers before the proven four-image
    reverse unmap; RR3 main constructors remain gated.
13. 0.19.1 saves/restores TPIDR_EL0 and supplies a scoped 0x100-byte Android TLS
    block with a stable stack canary at offset 0x28 for constructors/finalizers.
14. Eden v0.2.1 portable emulator is installed for NRO regression; CLI reaches
    applet startup but currently exits in the emulator SelfController layer.
15. 0.20.0 replaces the default software console with a native framebuffer;
    console and Mesa remain isolated from each other by explicit NRO stages.
16. All eight host-side contracts pass, including NRO/NACP metadata, original
    localization/motion assets, framebuffer backend, TLS, cleanup, and startup
    planning.
