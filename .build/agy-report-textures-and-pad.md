# Diagnostic Report: Controller Plumb & Texture Format Investigation

**Date**: 2026-11-09  
**Target**: EA Real Racing 3 (Android guest re-hosted on Nintendo Switch Horizon OS, libnx + Mesa 20.1.0-rc3)  
**Workspace**: `E:\AI\RealRacing3-switch`  
**Status**: Diagnostics & Fallback Hooks Implemented, Gate 40/40 Passing (`GATE OK`)

---

## 1. Defect A: Controller Heartbeat & Activity State Machine

### 1.1 The Problem
The port previously could not distinguish between Eden (or the host environment) never forwarding controller events versus reading the controller but doing nothing with it. Additionally, raw HID fallback polling risked competing with or spamming during Npad activity.

### 1.2 Implemented Solution
1. **Pure State Decision Contract (`rr3/gamepad.hpp`)**:
   - `NpadSlotSnapshot`: captures slot index, connection state, style set bitmask, button bitmask, analog sticks (`lx`, `ly`, `rx`, `ry`), triggers (`lt`, `rt`), `everPressed` latch, and `autopilotInjected` status.
   - `npadStyleName(uint32_t styleSet)`: maps connection style to human-readable names (`FullKey`, `Handheld`, `JoyDual`, `none`, etc.).
   - `isNpadSlotActive(const NpadSlotSnapshot& slot, float deadzone = 0.05f)`: pure function determining whether any input is deflected beyond deadzone or any button is pressed.
   - `npadSlotStateWord(const NpadSlotSnapshot& slot)` & `npadStateWord(const NpadSlotSnapshot* slots, size_t count)`: returns:
     - `"npad-none"`: controller slot disconnected
     - `"npad-idle"`: controller connected with zero button presses and sticks within deadzone
     - `"npad-active"`: controller connected with active button or stick/trigger deflection
2. **Host Unit Tests (`native-switch/tests/host/gamepad_selftest.cpp`)**:
   - Added `testNpadStateDecision()` testing style names, disconnected slot mapping, idle slot mapping, button activation, stick deflection, deadzone noise handling, and multi-slot aggregation.
   - Host test contract verified via `python native-switch/tests/test_gamepad_contract.py` (260/260 checks passed).
3. **Runtime Heartbeat Diagnostic (`native-switch/source/guest_input.cpp`)**:
   - `logNpadHeartbeat`: logs `npad-heartbeat` lines every 300 frames (~5s at 60 fps) and on significant slot/button/stick/trigger/autopilot change (throttled to at least 15 frames between change-driven lines).
   - Hard line budget: capped at 128 lines per run, emitting `budget exhausted (128 lines)` upon reaching the limit.
   - Format:
     ```
     phase=npad-heartbeat f=<frame> state=<npad-none|npad-idle|npad-active> type=<FullKey|Handheld|...> seen_press=<0|1> buttons=0x<hex> lx=<f> ly=<f> rx=<f> ry=<f> lt=<f> rt=<f> autopilot_injected=<0|1> trigger=<periodic|change>
     ```
4. **Raw HID Fallback Silencing**:
   - In `pumpGuestControllerInput`: raw HID fallback polling is suppressed while `npadInUse` is true. When Npad is idle, raw HID is polled at a 15-frame cadence (or every frame only when a raw HID device is actively deflected).
   - `logRawHidState` is silenced during Npad activity.

---

## 2. Defect B: Rival Cars Render Magenta (Green Channel Zeroed)

### 2.1 The Symptom & Pixel-by-Pixel Measurement
- Frame 2000 of m9 run (`.build/eden-runs/20261109-202239-eden-rr3-m9-nov4-binary-todays-card/frames/frame-002000.png`):
  - Affected cars: DTM rival cars (BMW M4 DTM, Mercedes C63 DTM) at Hockenheim. Player's car renders normally.
  - Zero pixels in the frame are pure magenta missing-texture `(255, 0, 255)`.
  - Measured pixel counts on car body:
    - `(255, 4, 255)` x40
    - `(192, 4, 192)` x17
    - `(221, 4, 229)` x7
    - `(187, 7, 204)` x5
    - `(255, 7, 255)` x4
    - `(190, 29, 220)` x3
  - Window glass renders as dark purple `(60, 0, 60)`.
  - Diagnosis: The green channel of normal grey/white textures `(192, 192, 192 -> 192, 4, 192)` and `(255, 255, 255 -> 255, 4, 255)` is zeroed out. The texture data is decoded, but decoded with an incorrect block layout or color mapping.

### 2.2 Asset Pack & Delivery Verification
- Inspected source asset pack `files/.depot/vehicles/2017_bmw_m4_dtm/livery/2017_bmw_m4_dtm_ext.dxt.dds.z` (266,228 bytes on disk):
  - 4-byte LE size header declares 699,192 bytes.
  - Inflates to a valid 1024x1024 DDS with 11 mip levels and FourCC `'DXT1'` (8 bytes per 4x4 block).
  - CPU decoding via `dxt_decode.hpp` produces a normal grey car livery (mean RGB 125, 122, 126) with zero green-zeroed pixels.
- Guest delivery log in m9 proves identical 266,228 bytes were served to the guest (`phase=map-app`, `phase=stat-layout size=266228`, `phase=open-ok`). The delivered bytes on disk are valid.

### 2.3 The Discovery Behind the DXT5 Trace Log Artifact
- Previous log excerpts showed 8 calls to `ctexImage2D` with internal format `0x83f3` (`GL_COMPRESSED_RGBA_S3TC_DXT5_EXT`):
  ```
  phase=ctexImage2D ordinal=16777472 target=00000000000083f3 data
  ... (8 mip levels from 256x256 down to 2x2) ...
  ```
- Detailed source inspection revealed `guestViewTracedGlCompressedTexImage2D` had an 8-call budget cutoff:
  `if (g_glStats.compressedTexImage2D <= 8) { ... }`
- Analysis of asset sizes revealed that those 8 calls were the mip chain for `vehicles/common/car_rotor_glow.dxt.dds.z`, which is genuinely 256x256 DXT5 on disk.
- All subsequent compressed texture uploads (including car liveries) were completely suppressed by the 8-call budget.

### 2.4 Hypotheses to Discriminate
- **H1**: Format-alias chain in `guest_filesystem.cpp` served a format-specific request with bytes of another format.
- **H2**: Port's asset list rewrite and guest's format decision disagreed due to device profile substitution.
- **H3**: Mesa 20.1.0-rc3 on Switch (tegra / nouveau) fails to decode S3TC or has a driver bug decoding DXT1 green channels in GLES.

### 2.5 Implemented Instrumentation & Fix Hooks
1. **Graphics Probe Readback & CPU Compare (`native-switch/source/graphics_probe.cpp`)**:
   - In stage 9: after `glDrawArrays` renders the compressed asset quad to the 1280x720 framebuffer, reads back the rendered center pixel via `glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE)`.
   - Simultaneously decodes the matching texel on CPU directly from the DDS payload using `rr3::decodeDxt1Block` / `rr3::decodeDxt5Block`.
   - Emits a budgeted diagnostic line:
     ```
     phase=probe-dxt1 asset=gui fourcc=DXT1 w=1024 h=1024 internal=0x83f1 err=0x0 got=(r,g,b) want=(r,g,b) match=1/0 path=...
     ```
   - Added a vehicle texture probe for `2017_bmw_m4_dtm_ext.dxt.dds.z` using the same draw, `glReadPixels`, CPU decode, and comparison sequence:
     ```
     phase=probe-dxt1 asset=veh fourcc=DXT1 w=1024 h=1024 internal=0x83f1 err=0x0 got=(r,g,b) want=(r,g,b) match=1/0 path=...
     ```
2. **Linked Texture Upload Tracker (`native-switch/source/guest_filesystem.cpp`, `rr3/guest_filesystem.hpp`)**:
   - Implemented a 16-entry ring buffer `rememberServedTexture` and `getRecentServedTexture` in `guest_filesystem.cpp`.
   - On every `.dds` or `.dds.z` open (`guestFopen` / `guestOpen`), parses the DDS header (inflating the container if `.dds.z`) to record the served file path, FourCC, dimensions, and mip levels.
3. **Traced Compressed Upload Logger (`native-switch/source/guest_view_trace.cpp`)**:
   - In `guestViewTracedGlCompressedTexImage2D`: reports each compressed texture upload linked with the most recently served file from the ring buffer.
   - Reports `glGetError()` even on success (`err=0x0`).
   - Hard budget: 32 lines (`phase=ctex-upload`), followed by `budget exhausted (32 lines)`.
4. **Software DXT Translation Hook (`native-switch/source/gfx/gfx_gles.cpp`)**:
   - In `__wrap_glCompressedTexImage2D`: added a translation hook that decodes S3TC DXT1/DXT5 blocks to RGBA8 on CPU via `rr3::decodeDxtImage` and uploads as uncompressed RGBA8 via `__real_glTexImage2D` when `RR3_SW_DXT=1` is set.

---

## 3. Verification & Gate Results

- Host test suite:
  - `python native-switch/tests/test_gamepad_contract.py`: **260 checks passed**
- Build:
  - `cmd /c powershell -ExecutionPolicy Bypass -File E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1`: **BUILD EXIT=0**
- Gate:
  - `cmd /c powershell -ExecutionPolicy Bypass -File E:\AI\RealRacing3-switch\.build\gate.ps1`:
    - `BUILD EXIT=0`
    - `INPUTS EXIT=0`
    - `TESTS: 40 run, 0 failed`
    - `38 resolved, 16 known-absent, 0 unexpected`
    - `SYMBOLS EXIT=0`
    - **`GATE OK`**
- Documentation integrity:
  - `docs/OPEN-ISSUES.md`: updated with subsection 9.17 (English only, CJK count = 0).

---

## 4. What Remains Unverified
- Direct emulator / hardware execution of the new binary in Eden:
  - The output of `phase=probe-dxt1 asset=veh ... match=...` will directly determine whether Mesa 20.1.0-rc3 decodes DXT1 correctly on this GPU (`match=1`) or produces corrupted green channels (`match=0`, H3).
  - The output of `phase=ctex-upload ... internal=... recent_file=... recent_fourcc=...` will show whether the guest passes `0x83f1` (DXT1) or `0x83f3` (DXT5) when uploading vehicle liveries.
