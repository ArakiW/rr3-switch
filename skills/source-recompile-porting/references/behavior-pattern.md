# Native source-recompile behavior pattern

Use this reference to turn an observed port into a reusable engineering workflow. The goal is to recover decisions and invariants, not to copy a particular project's code.

## Behavior model

### 1. Establish the route before touching packaging

Ask what actually executes:

- newly compiled target-native code;
- a foreign binary behind a compatibility layer;
- an emulator;
- a streaming client;
- a launcher or forwarder.

Evidence order:

1. source tree and build graph;
2. target-native executable headers and imports;
3. asset platform tags and shader formats;
4. runtime logs on real hardware;
5. public developer statements;
6. filenames, marketing language, and community guesses.

Never infer a source port merely from an installable NSP or a home-menu icon.

### 2. Freeze a reproducible source baseline

Record:

- exact branch/commit or release lineage;
- original supported platform and last known working build;
- compiler and language modes;
- generated code and asset-tool versions;
- middleware and platform licenses;
- build outputs and checksums.

If the original target cannot be built, distinguish source repair from target porting.

### 3. Draw the platform boundary map

Use one row per subsystem:

| Boundary | Original implementation | Target implementation | Evidence | First acceptance test |
|---|---|---|---|---|
| Lifecycle | Win32/Android/console | target app loop | call graph/build files | clean start/exit |
| Threads/TLS | platform runtime | target runtime | intrinsics/imports | worker + TLS test |
| Filesystem | original paths/archive API | target content/save layout | I/O sites | load one asset |
| Input | original controller API | target controller API | event mapping | full control matrix |
| Rendering | DirectX/OpenGL/console RHI | target RHI | renderer graph | clear + triangle + frame |
| Shaders | source/runtime bytecode | target compiler/translator | shader pipeline | one material |
| Audio | middleware/backend | licensed target backend or adapter | imports/licenses | music + SFX |
| Networking | sockets/platform services | target sockets/services | imports | controlled offline/online test |
| Save data | original profile paths | target save API | save schema | save/reload |

This map is the port's working contract. Update it as evidence changes.

### 4. Preserve logic, replace seams

Prefer this order:

1. platform abstraction layer;
2. allocator, jobs, atomics, and TLS;
3. filesystem and resource mount;
4. display and renderer backend;
5. input and audio;
6. online/platform services;
7. packaging.

Do not refactor gameplay systems while the platform seam is unstable. When a renderer is tightly coupled, introduce an RHI whose resource, command, synchronization, and presentation model matches the target rather than translating calls ad hoc throughout the engine.

### 5. Build the narrow boot corridor

Each milestone must end with observable proof:

1. target executable launches and logs;
2. allocator/TLS/thread smoke tests pass;
3. content root mounts and a known file hash matches;
4. renderer creates device/swapchain and presents a clear color;
5. one compiled shader/material renders;
6. input and audio work;
7. engine reaches menu;
8. one deterministic scene or race loads;
9. save/reload works;
10. longer gameplay and performance validation pass.

Avoid broad feature work until the previous corridor step is stable.

### 6. Treat resources as a separate product

The asset pipeline may require more work than the executable:

- platform directory/tag mapping;
- archive decryption only when authorized;
- texture compression selection;
- shader recompilation;
- alignment and page-size changes;
- filename hashing/case rules;
- stream chunk sizing;
- language/DLC/update manifest reconciliation.

Verify archive headers and representative decoded assets. A renamed platform tag is evidence of routing, not proof that the data is target-native.

### 7. Optimize with budgets, not folklore

Keep independent budgets for:

- main/game/render threads;
- frame CPU and GPU time;
- resident, streaming, and graphics heaps;
- I/O bandwidth and decompression;
- shader compilation and pipeline caches;
- temperature and power.

Overclocking is a diagnostic variable, not a completion criterion. Always retain a stock-clock profile.

### 8. Package only after runtime proof

Keep these concepts separate:

- developer/homebrew executable;
- installable program container;
- large data deployment;
- optional LayeredFS override;
- save data;
- official retail/game-card distribution.

An NSP may contain only the program/control/meta content while tens of gigabytes of assets live in a separate content tree. This is a deployment choice, not an engine technique.

## Reusable skill points

- Evidence-gated route selection.
- Baseline-first source control.
- Platform boundary mapping.
- RHI design instead of scattered graphics patches.
- Asset pipeline versioning and platform tagging.
- Boot-corridor milestone discipline.
- Real-device validation separated from emulator results.
- CPU/GPU/memory/streaming budget ownership.
- Packaging deferred until the runtime core is proven.
- Explicit legal and licensing boundary for source, middleware, assets, signatures, and online services.

## Failure patterns

- Calling an APK/EXE wrapper a source port.
- Rewriting gameplay before platform seams are stable.
- Treating successful compilation as boot proof.
- Treating emulator boot as hardware proof.
- Selecting a graphics API before inventorying shader behavior.
- Renaming assets without validating runtime platform contracts.
- Hiding missing platform services behind silent success stubs.
- Using overclocking to mask deadlocks, streaming stalls, or single-thread bottlenecks.
- Producing NSP/XCI packages before the engine runs.
- Learning from leaked source by copying implementation rather than extracting independent architecture principles.
