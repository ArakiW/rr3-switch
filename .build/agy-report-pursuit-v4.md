# Investigation and Implementation Report: Pursuit and Recovery Decoupling (v4)

## 1. What Changed and Why

### H1. Pursuit vs. Recovery Steering Priority
- **Change**: In `native-switch/source/race_state_probe.cpp`, the steering authority logic in `updateDrive()` was restructured so that whenever `g_drive.brake` is active, the recovery's decisive steering angle (`-g_drive.sign * kDriveRecoveryMagnitude`) takes precedence, setting `g_drive.owner = "recovery"`. Pursuit steering is only evaluated when `g_drive.brake` is false.
- **Why (Reasoned from Measurement)**: In live log samples from round chunk2 (frames 3870–3960):
  ```
  f=3870 dir=1 mot=-1.000,-0.007 cchunk=4.80 perr= 1.5175 pur=1 total= 71949 ctdist=17.92 flips=2 stalls=5 steer=-1.000 mag=0.530 sign=-1 thr=0 brk=1
  f=3900 dir=1 mot=-1.000,-0.017 cchunk=5.14 perr= 1.5228 pur=1 total= 72387 ctdist=19.61 flips=2 stalls=5 steer=-1.000 mag=0.530 sign=-1 thr=0 brk=1
  f=3930 dir=1 mot=-1.000, 0.009 cchunk=2.51 perr= 1.6406 pur=1 total= 72850 ctdist=16.92 flips=2 stalls=5 steer=-1.000 mag=0.530 sign=-1 thr=0 brk=1
  f=3960 dir=1 mot=-0.997, 0.076 cchunk=2.88 perr= 1.6346 pur=1 total= 73309 ctdist=18.71 flips=2 stalls=5 steer=-1.000 mag=0.530 sign=-1 thr=0 brk=1
  ```
  Pursuit commanded full lock (`steer = -1.000`) concurrently with recovery braking (`brk = 1`, `thr = 0`). A pure-pursuit calculation aiming 18–20 m down the track is topologically invalid when the car is reversing. A brake combined with full steering lock causes a spin, holding the car in a circular orbit against the barrier. Scaling down pursuit magnitude would still command steering in the forward direction. Recovery's decisive angle (`-sign * kDriveRecoveryMagnitude`, which evaluates to `+0.600` when `sign = -1`) was specifically designed to back the car off the wall and swing its front end away from the barrier toward the track. Letting recovery's decisive angle win while active resolves this conflict.

### H2. Limit Cycle Prevention and Yielding to Recovery
- **Change**: Added detection for pure pursuit limit cycles. When pursuit is engaged with a steady perpendicular error (`|steerError| > kLimitCycleErrorRad` = 1.40 rad) and the along-track coordinate (`m.alongTrack`) has failed to advance by `kStuckAlongDistanceMetres` (2.0 m) for `kLimitCycleFrames` (240 frames), pursuit gives way to recovery. This triggers recovery (`g_brakeUntilFrame = frame + kDriveRecoveryFrames`, `g_drive.brake = true`, `g_drive.throttle = false`), increments `g_drive.stalls`, and resets the along-track anchor. Additionally, `pinned` stall detection directly triggers recovery instead of being gated by odometer `total` progress.
- **Why**: A pure pursuit at ~90° error (1.52–1.64 rad in chunk2) orbits the lookahead aim point in a limit cycle. Because creeping in circles caused odometer `total` to increase by ~450 units per 30 frames, progress stall detection based on `total` was repeatedly suppressed. Relying directly on along-track displacement (`m.alongTrack`) ensures that circular orbits without track progress are detected promptly, forcing pursuit to yield to recovery.

### H3. Recovery Reversal Completion and Forward Alignment
- **Change**: 
  1. While `g_drive.brake` is true during recovery, `s_haveHeading` is invalidated (`s_haveHeading = false`).
  2. In heading acquisition (`!g_drive.brake && hLen > 0.05`), a forward dot product check (`fwdDot = (hx*dx + hy*dy) / (hLen*dLen) > 0.0`) ensures that residual reverse motion or coasting cannot corrupt the forward heading.
  3. When recovery ends, `pursuit` is temporarily false until forward motion (> 0.05 m) is established. Steering follows `g_drive.sign * g_drive.magnitude` (down the track) under full forward throttle (`thr = 1`). Within 1–2 frames, the car accelerates forward, capturing a fresh heading pointing down the track, allowing pursuit to engage with low error and keeping throttle open.
- **Why**: In previous rounds, keeping the pre-recovery heading meant that upon brake release, pursuit immediately evaluated `|steerError| > 1.5`, cut throttle (`thr = 0`), and locked steering at `-1.000`. The car could never move forward from standstill to establish a new heading, deadlocking the car and running `stalls` up to 5. Resetting heading during recovery and protecting heading acquisition ensures the recovery successfully completes and points the car down the track.

### H4. Steering Ownership Instrumentation
- **Change**: Added `const char* owner{"none"}` to `struct RaceDriveState` in `native-switch/include/rr3/race_state_probe.hpp`. In `native-switch/source/race_state_probe.cpp`, assigned `owner` to `"recovery"`, `"pursuit"`, `"sweep"`, or `"disabled"`. Added `owner=%s` at the end of the `race-state` log format string and argument list for both active and inactive samples.
- **Why**: The log line now explicitly attributes which subsystem dictated steering in that sample. Placing `owner=%s` at the end ensures argument positions match exactly, avoiding displacement of preceding fields and eliminating `-Wformat=` compiler warnings.

### H5. Diagnostic Line Hard Budget
- **Change**: Added `logLimitCycleEvent` emitting to `race-limitcycle` with a line budget of `kMaxLimitCycleDiagLines = 16`. Upon the 16th emission, it outputs `"budget exhausted (16 lines)"`.
- **Why**: Ensures diagnostics do not flood guest log buffers, adhering to the repository's strict line-budget policy.

---

## 2. New Constants and Justifications

1. `kLimitCycleErrorRad = 1.40`
   - *Value*: 1.40 radians (~80.2°).
   - *Justification*: In round chunk2, `perr` sat at 1.5175..1.6406 rad (87° to 94°) at full lock. Normal racing-line tracking errors are under 0.5 rad (28.6°). An error > 1.40 rad indicates the car's heading is roughly perpendicular to the lookahead target vector, characteristic of an orbital limit cycle.

2. `kLimitCycleFrames = 240ULL`
   - *Value*: 240 frames (~7.3 s at the measured 30.35 ms/frame).
   - *Justification*: Matches `kDriveRecoveryFrames` (240 frames) and the progress stall timeout. If perpendicular pursuit error persists for 240 frames without advancing `kStuckAlongDistanceMetres` (2.0 m) along the track, the car is trapped orbiting the aim point and must yield to recovery.

3. `kMaxLimitCycleDiagLines = 16`
   - *Value*: 16 lines.
   - *Justification*: Standard bounded budget matching `kMaxFlipDiagLines` and `kMaxStuckDiagLines`.

---

## 3. Next Round Emulator Measurements

To prove or disprove the fix in the next emulator run, verify the following fields in the `race-state` log lines:

1. `owner`:
   - During recovery (`brk=1`), must read `owner=recovery`.
   - During forward tracking (`brk=0, thr=1`), must read `owner=pursuit`.
   - Never `owner=pursuit` while `brk=1`.

2. `steer`:
   - During `brk=1`, `steer` must NOT be full lock `-1.000` (or `+1.000`). It must equal `-sign * kDriveRecoveryMagnitude` (e.g. `+0.600` when `sign=-1`, or `-0.600` when `sign=+1`).

3. `perr`:
   - Must NOT remain frozen at ~1.52..1.64 rad indefinitely. Following recovery, as the car moves down the track, `perr` should fall toward 0.0–0.4 rad.

4. `thr` & `brk`:
   - When recovery ends, `brk` transitions from `1` to `0`, and `thr` transitions from `0` to `1`. `thr` must remain `1` (not immediately cut to `0` by `|perr| > 1.5`).

5. `along` & `stalls`:
   - `along` must increase as the car makes genuine track progress after recovery.
   - `stalls` counter must not continuously increment in a deadlock loop (previously reached `stalls=5`).

---

## 4. Build and Gate Results

### Build Command:
`cmd /c "powershell -ExecutionPolicy Bypass -File E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1 > E:\AI\RealRacing3-switch\.build\build-pursuit-v4.log 2>&1"`

**Quoted last lines of `build-pursuit-v4.log`:**
```
Done!
make: 进入目录“/e/AI/RealRacing3-switch/native-switch”
find: ‘romfs’: No such file or directory
built ... rr3-native-rehost.nro
make: 离开目录“/e/AI/RealRacing3-switch/native-switch”
Built: E:\AI\RealRacing3-switch\native-switch\rr3-native-rehost.nro
```
*Note*: Compiled with zero `-Wformat=` warnings and zero errors.

### Gate Command:
`cmd /c "powershell -ExecutionPolicy Bypass -File E:\AI\RealRacing3-switch\.build\gate.ps1 > E:\AI\RealRacing3-switch\.build\gate-pursuit-v4.log 2>&1"`

**Quoted last lines of `gate-pursuit-v4.log`:**
```
=== UNEXPECTED misses: 0 ===

=== files that are not valid utf-8: 8 ===
  app_NativeActivity.h       first bad byte at offset 102
  ea_DisplayAndroidDelegate.h first bad byte at offset 111
  ea_EAIO.h                  first bad byte at offset 92
  ea_EAMIO_StorageDirectory.h first bad byte at offset 105
  ea_EASPHandler.h           first bad byte at offset 99
  ea_GetAppDataDirectoryDelegate.h first bad byte at offset 116
  xt_Play.h                  first bad byte at offset 95
  xt_System.h                first bad byte at offset 84

38 resolved, 16 known-absent, 0 unexpected
SYMBOLS EXIT=0
GATE OK
```
*Summary*: Build Exit = 0, Inputs Exit = 0, Tests: 40 run, 0 failed, Symbols Exit = 0, `GATE OK`.

---

## 5. Items Not Verified

- Dynamic driving behavior on the Ryujinx emulator / physical Switch hardware could not be tested directly in this environment. Verification was conducted via host test suites (`race_line_selftest.exe`, 40 Python contract tests, symbol resolver, input preflight, and clean toolchain compilation).
