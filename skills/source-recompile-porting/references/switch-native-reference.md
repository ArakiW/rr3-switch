# Nintendo Switch native source-port reference

Read this reference only for a Nintendo Switch target.

## Toolchain route

Choose explicitly:

| Route | Appropriate when | Typical output | Boundary |
|---|---|---|---|
| Official Nintendo SDK | the developer has platform approval, rights, SDK access, and middleware licenses | official development/installable packages | NDA and certification apply |
| devkitPro/libnx | legal homebrew, research, and open-source ports | NRO | no official retail signing |

Do not use signature patches, forged tickets, or unofficial NSP/XCI packaging as a substitute for platform authorization.

## Rendering decision

### Existing standard API renderer

Use switch-mesa when the engine already depends on OpenGL and runtime GLSL behavior. It minimizes engine changes but adds driver size and overhead.

### New low-level RHI

Use deko3d for a homebrew-specific low-level backend when the engine can own resource lifetimes, command submission, synchronization, and build-time shader compilation. It is not a drop-in OpenGL implementation.

### Official backend

Use the official SDK graphics API only when authorized. Keep its implementation behind the engine RHI so gameplay and resource logic remain platform-neutral.

Before selecting a backend, inventory:

- runtime versus offline shader compilation;
- shader source language and reflection;
- resource binding model;
- render-pass and synchronization assumptions;
- compressed texture formats;
- compute, geometry, tessellation, and advanced blending use;
- readback, occlusion, and query behavior.

## Architecture and runtime

- Switch is AArch64, but source still needs target compiler, ABI, C++ runtime, TLS, exceptions, atomics, SIMD, and alignment review.
- Remove x86-only intrinsics or put them behind portable/vector backends.
- Validate worker affinity and synchronization on the available application cores.
- Treat application/app-mode memory limits as an explicit budget.
- Audit custom allocators and streaming heaps before reducing graphical quality.

## Platform adapters

Create narrow modules for:

- applet lifecycle and suspend/resume;
- Npad/Joy-Con and optional touch/gyro;
- RomFS/content and save-data mounts;
- sockets and offline failure behavior;
- audio output or licensed middleware;
- user selection, keyboard, error dialogs, and system UI;
- logging via console, nxlink, or a rotating SD log.

## Asset layout

For large ports, keep four hashes/manifests:

1. source assets;
2. cooked target assets;
3. program build;
4. deployment tree.

LayeredFS can be useful for private iteration and large data overrides, but it should not conceal which title and content root the program expects. Verify case, alignment, platform tags, update/DLC merges, texture formats, and stream chunk sizes.

## Validation ladder

1. cross-compile and inspect AArch64 output;
2. emulator smoke test, if useful;
3. real-hardware launch and clean exit;
4. stock-clock menu/scene test;
5. input, audio, save, suspend/resume;
6. deterministic gameplay capture;
7. CPU/GPU/memory/thermal profile;
8. long-run stability;
9. package/deployment reproducibility.

Never collapse these levels into a single “ported” status.

## Source port versus binary rehosting

With source, modify platform adapters and renderer abstractions, then rebuild. Without source, the problem changes to ELF loading, ABI/TLS compatibility, import shims, binary patching, or clean-room reimplementation. Do not apply source-port milestones to a stripped binary as if the same seams were available.
