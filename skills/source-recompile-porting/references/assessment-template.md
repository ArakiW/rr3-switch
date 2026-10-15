# Source-recompile port assessment template

## Decision

- Route: source recompile / partial source / binary only / emulation / streaming
- Evidence strength: confirmed / inferred / claimed
- First achievable target:
- Current blocker:

## Source baseline

- Source authority and license:
- Branch/version/commit:
- Original build command:
- Reproduced output and checksum:
- Missing generated code or tools:
- Middleware/platform licenses:

## Platform boundary matrix

| Subsystem | Original | Target | Status | Evidence | Next test |
|---|---|---|---|---|---|
| Lifecycle | | | | | |
| Threads/TLS | | | | | |
| Filesystem | | | | | |
| Input | | | | | |
| Rendering/RHI | | | | | |
| Shaders | | | | | |
| Audio | | | | | |
| Networking | | | | | |
| Save/profile | | | | | |
| Platform services | | | | | |

## Asset pipeline

- Source archive/version:
- Platform tags:
- Texture formats:
- Shader formats:
- Alignment/endian/case constraints:
- Update/DLC merge rules:
- Cooked manifest and representative decode proof:

## Boot corridor

| Milestone | Static build | Emulator | Real hardware | Evidence |
|---|---:|---:|---:|---|
| Launch/log | | | | |
| Allocator/TLS/thread | | | | |
| Content mount | | | | |
| Clear/present | | | | |
| First material | | | | |
| Input/audio | | | | |
| Menu | | | | |
| Deterministic scene | | | | |
| Save/reload | | | | |

## Performance and stability

- Stock clocks:
- CPU thread/frame budget:
- GPU frame budget:
- Resident/streaming/graphics heaps:
- I/O and decompression:
- Thermals/power:
- Long-run duration and failures:

## Packaging

- Development executable:
- Program container:
- Resource deployment:
- Save data:
- Optional overrides:
- Official distribution authority:

## Truthful status statement

State exactly what was generated, compiled, launched, rendered, played, and measured. List what remains unverified.
