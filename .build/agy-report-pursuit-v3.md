# Autopilot Pursuit V3 Investigation and Remediation Report

## 1. What Changed and Why

### 1.1 Fix A - Aim Point Projection and Non-Zero Look-Ahead
* **Root Cause**: Previously, `raceLineTargetAdaptive()` identified only the nearest polyline node rather than the true orthogonal projection on the nearest segment. Furthermore, whenever the car drifted beyond `kOnLineDistanceMetres` (25 m), the look-ahead distance was forced to 0.0 m. Because track nodes are spaced ~8 m apart, the nearest node could fall up to ~4 m behind the car along the track direction. Aiming at that node with zero look-ahead caused the aim vector to point backwards (producing signed angular errors between +1.16 and -2.66 radians, up to 137 degrees off heading). This induced full-lock steering saturation, vehicle spin, and backwards travel.
* **Remediation**:
  * Implemented `projectOntoPolyline()` to compute the exact closest point on the closest line segment, together with an along-track arc-length coordinate measured in metres from node 0 in node order, wrapping at the closed loop length.
  * Formulated the aim point as `(projection arc coordinate + lookAhead)` along the locked track direction (`step`).
  * Replaced the zero look-ahead behavior off-line with a fixed moderate distance `kFarLookAheadMetres = 10.0` m (always non-zero), preserving `kOnLineDistanceMetres` (25 m) and `kFarFromLineLimitMetres` (250 m).
  * Maintained curvature-adaptive look-ahead for cars within 25 m of the line.
  * Exposed `raceLineProject()`, `raceLineArcDelta()`, `raceLineLoopLength()`, and optional projection/coordinate outputs in `raceLineTargetAdaptive()` in `native-switch/include/rr3/race_line.hpp` and `native-switch/source/race_line.cpp`.

### 1.2 Fix B - Along-Track Direction Flip Criterion and Restored Inversion
* **Root Cause**: The steering direction flip evaluated `rate`, a 120-frame smoothed rate derived from progress telemetry. When the stall recovery routine triggered, it intentionally reversed the vehicle for 240 frames (`kDriveRecoveryFrames`). The 120-frame rate filter was completely dominated by this recovery reversal, poisoning the flip detector. Consequently, direction-lock inversion was previously disabled with `if (false && gRaceLine.direction != 0)`.
* **Remediation**:
  * Replaced the smoothed rate criterion with a windowed along-track delta (`fdelta`) tracked over `kFlipWindowFrames = 360ULL` frames. Because 360 frames is 1.5x longer than `kDriveRecoveryFrames` (240 frames), a full recovery reversal cannot dominate the window, and at least 120 frames of forward driving are included.
  * Computed `fdelta` using a wrap-around aware along-track delta (`raceLineArcDelta()`) mapped through the locked race direction.
  * Maintained the `!g_drive.brake` guard and the 600-frame flip hysteresis window.
  * Restored direction-lock inversion by removing `if (false && ...)`: when `fdelta < kFlipDeltaThresholdMetres` (-5.0 m), `gRaceLine.direction` is inverted alongside `g_drive.sign`.

### 1.3 Fix C - Along-Track Stuck Detection
* **Root Cause**: The pinned/stuck test previously watched the vehicle's world Euclidean coordinate displacement (`dx*dx + dy*dy >= kPinnedDistance^2`, 1 m). Cars grinding against barriers jittered world positions by 1 m every few frames and were never flagged as stuck, whereas recovery reversals displaced world coordinates significantly, concealing the stall.
* **Remediation**:
  * Base the stuck discriminator on progress along the track (`m.alongTrack`). If a car fails to advance forward along the track by `kStuckAlongDistanceMetres` (2.0 m) over `kStuckAlongFrames` (600 frames = 10.0 s), it is declared pinned.
  * Kept the `g_sawProgress` guard so cars stationary at the starting grid during the start sequence are exempted.
  * Implemented Option B for recovery handling: the stuck test explicitly ignores frames where stall recovery is active (`g_drive.brake == true`), resetting the anchor timer. This prevents intentional recovery reversals from triggering or masking stuck states and gives a fresh 600-frame window once forward throttle resumes.

### 1.4 Fix D - Instrumentation and Line Budgets
* **Root Cause**: Earlier commits suffered from argument misalignments against format strings in `race_state_probe.cpp`, leading to compiler `-Wformat=` warnings and corrupted diagnostic fields (`dir`, `mot`, `cchunk`).
* **Remediation**:
  * Aligned the format string and argument list in `logSample()` in `race_state_probe.cpp` with 1-to-1 specifier-to-argument matching, fully eliminating `-Wformat=` warnings.
  * Added new fields to the `race-state` line: `along=%.2f` (arc-length coordinate in metres), `pdist=%.2f` (distance to projection in metres), and `fdelta=%.2f` (windowed along-track progress delta in metres).
  * Wrapped new event logging (`race-flip` and `race-stuck`) with hard budgets of 16 lines each, emitting a single `"budget exhausted (16 lines)"` notification when spent, following the pattern in `guest_view_trace.cpp`.

### 1.5 Fix E - Host Self-Tests
* **Root Cause**: Prior host self-tests lacked verification for non-zero look-ahead off-line, along-track coordinate monotonicity, and honest reporting of backwards-driving heading errors.
* **Remediation**:
  * Updated existing test 5 in `race_line_selftest.cpp` to verify that `lookAhead` equals `kFarLookAheadMetres` (10.0 m) rather than 0.0 m.
  * Added test cases E1 through E4 covering forward driving error bound (< 0.3 rad), backward driving error reporting (> 2.5 rad), 30 m off-line non-zero look-ahead and target distance, and along-track coordinate directionality.

---

## 2. New Constants and Justifications

| Constant | Value | File | Justification |
| :--- | :--- | :--- | :--- |
| `kFarLookAheadMetres` | `10.0` m | `race_line.hpp`, `race_line.cpp` | Moderate look-ahead distance used when the vehicle is between 25 m and 250 m off the line. It is halfway between `kMinLookAheadMetres` (4.0 m) and `kMaxLookAheadMetres` (20.0 m) and strictly exceeds the ~8 m spline node spacing, ensuring the aim point is always ahead along the track from the projection and steering back at an oblique angle. |
| `kFlipWindowFrames` | `360ULL` frames | `race_state_probe.cpp` | Time window for evaluating along-track progress for direction flipping. At 60 fps this is 6.0 s (1.5x `kDriveRecoveryFrames` of 240 frames). Even if a full 240-frame recovery reverse occurs, at least 120 frames of forward driving are included in the window, preventing recovery reversals from dominating the calculation. It is also safely within the 600-frame flip hysteresis. |
| `kFlipDeltaThresholdMetres` | `-5.0` m | `race_state_probe.cpp` | Net backwards progress threshold over 360 frames. Corresponds to -0.83 m/s (-3.0 km/h) sustained reverse motion, cleanly separating genuine reverse driving from barrier grinding jitter (+/- 0.5 to 2.0 m). |
| `kStuckAlongDistanceMetres` | `2.0` m | `race_state_probe.cpp` | Distance along track required to reset the stuck timer. 2.0 m is well above barrier grinding lateral/longitudinal jitter while small enough that even a car crawling at 0.2 m/s (~0.7 km/h) covers 2 m within 10 seconds. |
| `kStuckAlongFrames` | `600ULL` frames | `race_state_probe.cpp` | Duration of applied throttle without 2.0 m along-track progress before declaring the car stuck. 600 frames at 60 fps equals 10.0 seconds. |
| `kMaxFlipDiagLines` | `16` lines | `race_state_probe.cpp` | Hard line budget for `race-flip` event diagnostics in guest view log, preventing log flooding. |
| `kMaxStuckDiagLines` | `16` lines | `race_state_probe.cpp` | Hard line budget for `race-stuck` event diagnostics in guest view log, preventing log flooding. |

---

## 3. Added Self-Test Cases and Comparison with Old Behavior

### Case E1: Forward Car on Racing Line
* **Description**: A car on the line driving forward along the track node order with locked direction 1.
* **Test**: Verifies that the aim point is ahead along the track and that the angle between heading and aim point vector is under 0.3 rad (measured: 0.1087 rad on circle R=62.5 m).
* **Old Behavior**: The old implementation selected the nearest discrete node rather than segment projection, introducing angular discretization jitter depending on where the car was relative to the nearest node.

### Case E2: Backward Car on Racing Line
* **Description**: A car on the line driving backwards (heading reversed) with direction locked forward (1).
* **Test**: Verifies that the aim point's arc coordinate is ahead along the track (`arcDiff` = 20.0 m) and that the pursuit geometry honestly reports a large angular error (measured: -3.0329 rad, > 2.5 rad).
* **Old Behavior**: Under the old node-snapping logic and 0.0 m off-line look-ahead, vehicles nudged off-line aimed at nodes behind them, obscuring the wrong-way report and swinging errors into full-lock oscillations.

### Case E3: Car 30 m Off Racing Line
* **Description**: A car positioned 30 m off the racing line.
* **Test**: Verifies that `raceLineTargetAdaptive()` returns a valid target, that `lookAhead` is non-zero (10.0 m), and that the Euclidean distance from the car to the target is at least `lookAhead` and at least 30.0 m (measured: 32.39 m).
* **Old Behavior**: When distance exceeded `kOnLineDistanceMetres` (25 m), the old code forced look-ahead to 0.0 m and aimed at the nearest node. On Case E3, old code returned `lookAhead == 0.0` and distance to target was solely the node distance (~30 m), failing the requirement that distance to target be at least look-ahead and that look-ahead never be zero.

### Case E4: Monotonicity of Along-Track Coordinate
* **Description**: A car at node 0, advancing forward toward node 1, and stepping backward toward node N-1 across the loop wrap-around boundary.
* **Test**: Verifies that `raceLineArcDelta()` produces `deltaFwd > 0.0` (+3.11 m) and `deltaBwd < 0.0` (-3.07 m).
* **Old Behavior**: The old codebase possessed no segment projection or continuous arc-length coordinate API, relying exclusively on raw node indices.

---

## 4. Build and Gate Results

### Build Log (`build-pursuit-v3.log`)
Last lines:
```
linking rr3-native-rehost.elf
built ... rr3-native-rehost.nro
make: Leaving directory '/e/AI/RealRacing3-switch/native-switch'
Built: E:\AI\RealRacing3-switch\native-switch\rr3-native-rehost.nro
```
Build exited cleanly with code 0. Zero compiler warnings for `race_line.cpp` and `race_state_probe.cpp`, with `-Wformat=` completely clean.

### Gate Log (`gate-pursuit-v3.log`)
Last lines:
```
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
Summary from gate:
* Build: Exit 0
* Inputs: Exit 0
* Tests: 40 run, 0 failed
* Symbols: Exit 0
* Result: GATE OK

---

## 5. Verification Limits and Next Hardware Measurements

### Items Not Verifiable Offline
1. **Dynamic Track Rebound Physics**: Host tests verify synthetic polyline geometry, projection math, and mock heading rotations, but the full interaction of vehicle suspension, barrier collision restitution, and guest vehicle dynamics can only run under the Switch target emulator / hardware.
2. **Real-Track Telemetry Convergence**: While unit tests pass on circular splines, track splines with varying node density and complex chicanes (such as Spa or Silverstone) require runtime guest execution to observe full lap convergence.

### Measurements for Next Hardware Run
1. **Pursuit Error Range**: Confirm `perr` remains within [-0.5, +0.5] rad during normal racing and no longer swings into [-2.6, +2.6] rad saturation.
2. **Direction Inversion During Genuine Spins**: Confirm that if the car is turned around by a crash, `fdelta` smoothly drops below -5.0 m after recovery and flips both `sign` and `direction` without oscillating.
3. **Barrier Grinding Progress**: Verify that a vehicle crawling along barrier edges maintains `alongTrack` advance, avoiding false stall increments (`stalls` count should remain near zero on straights).
4. **Lap Completion Rate**: Compare lap progress (`total`) against the benchmark 186,921 units (10.35%) to confirm progression past previous stall bottlenecks.
