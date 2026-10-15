---
name: source-recompile-porting
description: Plan, audit, and execute native source-recompile ports of games or engines to Nintendo Switch or another native target when legitimate buildable source is available. Use for platform-boundary inventory, RHI design, asset cooking, ARM64 migration, validation, and packaging. Do not use for binary-only rehosting, emulation, or streaming wrappers.
---

# Source Recompile Porting

Treat a source-recompile port as an engine/platform migration, not a package conversion.

## Route gate

Before designing the port, establish which route the evidence supports:

- **Source recompile:** legitimate, buildable source covers the game core and required engine/platform layers. Continue with this skill.
- **Partial source:** source exists but critical renderer, middleware, or platform code is missing. Identify the missing boundary before promising a native build.
- **Binary only:** APK/ELF/EXE plus data but no buildable source. Do not imitate a source port. Route to binary rehosting, ABI shims, or clean-room reimplementation instead.
- **Streaming/emulation:** treat as a separate product choice and label it honestly.

Do not retrieve, use, or operationalize leaked or stolen source. A case study may inform architecture, but not authorize copying its code or assets.

## Core workflow

1. **Prove the baseline.** Record source provenance, branch/version, build system, supported original target, compiler, third-party licenses, and a reproducible original-platform build. Separate confirmed facts from claims and inference.
2. **Inventory platform boundaries.** Cover lifecycle, threads/TLS, filesystem, input, rendering, shaders, audio, networking, platform services, save data, architecture-specific code, and resource cooking.
3. **Choose the narrowest port seam.** Preserve game logic. Replace platform adapters and introduce or extend an RHI only where renderer coupling demands it. Avoid global rewrites before the boot path is understood.
4. **Build a boot corridor.** Bring up logging, filesystem, allocator/TLS, window/surface, input, renderer clear/present, assets, audio, then a minimal gameplay scene. Stub nonessential online, advertising, store, telemetry, and social features explicitly.
5. **Port resources deliberately.** Treat platform tags, compression formats, shader binaries, endianness, alignment, archive naming, and streaming budgets as a versioned asset pipeline. Renaming is acceptable only when the runtime contract and archive contents are verified.
6. **Optimize after correctness.** Establish stock-clock CPU/GPU/memory baselines before overclock testing. Measure frame time, streaming stalls, heap peaks, shader compilation, and thermal behavior.
7. **Package last.** Do not use NSP/XCI packaging as proof that the engine runs. Keep program, resources, save data, and optional LayeredFS overrides separately verifiable.
8. **Report evidence levels.** Distinguish static build success, emulator boot, real-device boot, menu, scene load, full gameplay, and performance acceptance.

## References

- Read [references/behavior-pattern.md](references/behavior-pattern.md) when planning or auditing a source-recompile port.
- Read [references/switch-native-reference.md](references/switch-native-reference.md) for Nintendo Switch target decisions, rendering backends, packaging, and validation.
- Read [references/gta5-switch-case-study.md](references/gta5-switch-case-study.md) only when the user asks about the GTA V-style behavior pattern or wants a comparable case study.
- Use [references/assessment-template.md](references/assessment-template.md) when producing a durable feasibility or status report.

## Bundle inspection

For a Switch release bundle, run the read-only inspector before making claims:

```bash
python scripts/inspect_switch_bundle.py <bundle-directory> --json <output.json>
```

It inventories files, parses only the outer PFS0 table of NSP files, detects Atmosphere LayeredFS layouts, checks RPF7 headers, and reads public graphics settings. It never decrypts NCA content or looks for keys.

## Deliverable standard

Lead with the route classification and the next concrete milestone. State what is compiled, what is merely generated, what ran on an emulator, and what ran on real hardware. Preserve authorization boundaries and do not equate unsigned packaging, signature bypass, or copied commercial assets with a completed port.
