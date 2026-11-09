# Real Racing 3 → Nintendo Switch: Open Issues & Handoff

> Snapshot: 2026-11-04 (emulator: Eden — runs q135 … q144)
> Repo: `https://github.com/ArakiW/rr3-switch`
> Machine-readable companions: `docs/FEATURE-MATRIX.md` (feature matrix),
> `PORT_PROGRESS.md` (chronological log), `tools/README-hardware-test.md` (device install & test guide)

---

## 0. Bottom line

**The console has now got as far as the front end.** Five hardware crashes
have been located and fixed, each one of the same family: the port touched
something (a guest pointer, a mapped page) without checking that it was still
valid. The fifth one, at 19:45 on 2026-11-03, is fixed in the build described
below.

**And on 2026-11-04 the port ran a REAL RACE for 4,200 frames** (q142) — the
first time any archived round has got that far. That round is what turned the
remaining work from guesswork into measurement, and it moved the whole
diagnosis:

* **The race starts on its own** — the port's own red-glove follower presses
  the race button at (1105, 695), which is the button the guest scissors every
  frame as `glScissor x=994 y=667 w=187 h=53`. The touch script was never the
  problem.
* **The pursuit's error was not "a bit off": it swung to ±3 rad (180°)** in
  63 of 141 samples, while the car was **2 m from the line** with the target
  **20 m ahead**. That is only possible if the car's direction of travel had
  reversed — and the cause was in the port: the side of the line the
  look-ahead point was taken from was **re-decided every frame** from the
  car's instantaneous heading, so a momentary reversal moved the target to the
  other side of the car, the controller saw two flips at once and slammed full
  lock, and the car drove backwards (6,581 units of progress lost between two
  samples). **Fixed**: the direction is now **locked for the race** and decided
  once from a windowed motion vector, and the throttle is let go when the car
  is facing the wrong way (§2.10). Unit-tested: 22 checks, all ok.
* **"3–4.5 fps" was the menu phase, not the game.** Every healthy round uses
  **1.39–1.56 cores of CPU** and the frame rate still varies 4.9× — so the
  difference is **waiting**, not computing. q142, which spent most of its time
  in the race, did **11.44 fps with a smaller log** (§2.12). The CPU is flat,
  so the next lever is the wait, and the guest-thread census — the port's own
  instrument that touches the guest's threads every 4 s, with a kill switch
  that **has never been run** — is being A/B'd now.
* **The port's own log was 31 % repetitive** (33,643 `glyph-face` + 31,315
  `map-app` lines, 504 lines/second, one `fflush` each) because both dedup
  tables were smaller than the number of distinct keys. Both are fixed
  (§2.11). The effect on the frame rate is **not yet measured** — the port's
  probes have already been excluded twice by measurement, so this is stated as
  a suspicion and not as a result.

**⇒ ⇒ AND THE MOST IMPORTANT FINDING OF THE ROUND CAME FROM THE USER, NOT
FROM THE CODE**: *"you never actually let me take over the controls, so of
course you cannot complete a lap — that is not the car being stuck; those are
two completely different directions."*

**⇒ ⇒ HE WAS RIGHT, AND THE CODE CONFIRMED IT.** Every round this project
has ever run measured **the port's own autopilot**, because the card's
`autopilot = 1` and the closed loop owned the steering, the throttle and the
brake. Worse:

* **`autopilot = 0` did not switch anything** — the setting only governed
  the physical-controller path, and the script path was overridden by the
  loop with no mode check at all (MEASURED: the control round, card set to
  `autopilot = 0` with the old code, still shows `drive engaged 201/208`);
* **the shipped package ships the experiment script**, and while a script is
  active the physical controller is ignored *entirely* — so **on a console
  the player's own controller was dead**, and the script held the throttle
  down from frame 600;
* **the scripted path had no instrument at all**, which is why the question
  could not be answered from the logs.

All three are fixed (§2.14), and the first round with the fix shows the
script's own axis reaching the guest with `loop_steered=0`.

**⇒ THE PORT'S OWN OVERHEAD IS NOT THE BLACK SCREEN.** MEASURED (q146, a new
per-frame instrument on the existing census line: `tt` the whole frame, `tr`
the guest's render call, `tp` the port's own work, `ts` the present):

| | menu phase | race phase |
|---|---|---|
| the port's own work | **0.1 %** (median 1 ms) | **1.0 %** (median 0 ms) |
| the guest's render | 99.0 % | 90.2 % |
| the present | 0.9 % | 8.8 % |

⇒ The menu's slowness on Eden is **the guest's own ARM64 code being
emulated** — both the guest and the port's Mesa are ARM64 binaries. **⇒
Eden's 4.8 fps says nothing about the console**, and the console's black
screen is much more likely to be the **boot phase**, which the port does
control.

**Three things are left before delivery**, one of which only the user can do:

1. **Install the newest NRO on the console and boot** (§4.1) — the crash
   fixes are only proven on the emulator so far, and the new build carries
   the per-frame instrument that turns the console's black screen into a
   directly answerable question.
2. **Drive it** — with the input path fixed, the player's controller owns
   the car. The lap counter is the autopilot's problem (§2.14), not the
   port's.
3. **The splash overflow** (§4.2) — the chain is measured down to the
   engine's own matrix; what is left is which matrix it sets up for the 2D
   pass.

**What this round added, measured:**

* the release archive was **stale** (1480 bytes off the package) and the
  packager now fingerprints it (§2.8);
* the first ten black seconds were **2.6 MB of unnecessary writes**, and the
  pass is now 2.33x faster with the three costs separated for the first time
  (§4.3);
* the port's `renderToTexture()` did not scale the glyphs the way the guest's
  Java says it must (§2.7) — a real divergence, now fixed, and **measured to
  be unused**: the guest never calls it;
* **"laps = -1" is not a counter**: the car drove 5.08 % of a lap because
  the pursuit was aiming **behind** the car and the line fit could pick the
  **mirror** of the track's centre line (§2.9), and because the direction
  was re-decided **every frame** (§2.10). Both fixed and unit-tested, and
  the car now reaches **10.35 %** (§2.10);
* **the player could not drive at all** (§2.14) — the single most important
  fix of the round, and it came from the user pointing out that the tests
  had never handed the controls over;
* the **census** (the port's own instrument that samples the guest's threads
  every 4 s) is **not** the frame-rate lever but costs 9 % of the CPU (§2.12);
* the track's spline holds a **racing line** the port was not following, and
  the port now measures how far the car is from it (§2.13).

---

## 1. Status table

| # | Issue | State | Evidence |
|---|---|---|---|
| 1 | Console crash on launch (black screen → Atmosphère "software closed") | **Fixed, awaiting device** | §2.1–§2.6; five crashes, all the same family |
| 2 | UI missing characters (icons became letters, and too small) | **Fixed, measured** | q124: icons drawn at the game's own size |
| 3 | Hang (120 s) after opening the icon font | **Fixed, measured** | q123 hang → q124 clean, A/B'd |
| 4 | Font cache had only 6 slots (silently dropped faces) | **Fixed** | source + contract test |
| 5 | `substituted=` log field was always 1 (measured nothing) | **Fixed** | q124 log |
| 6 | 10,546 loose files on the card; slow to copy / slow to scan | **Fixed** | 10,546 → 186 files, 9.58 GiB |
| 7 | Installer exit code reported failure; mini profile deleted the manifest | **Fixed** | previous round |
| 8 | Mapping file truncated at 8 KiB (9,021 B → 8,191 B) | **Fixed** | previous round |
| 9 | The release archive was stale (1480 B off the package) | **Fixed, measured** | §2.8; fingerprint + rebuild + byte-for-byte join check |
| 10 | `renderToTexture` did not scale the glyphs (and had no log line) | **Fixed, measured** | §2.7; q133: 0 calls, so no pixel changed — a latent fix |
| 11 | Splash title overflows the screen (1398 px of the line off-screen) | **Characterised, not fixed** | §2.7 / §4.2; the engine's own matrix overshoots by 1.525× |
| 12 | First 10 s of black screen before the guest appears | **Largely fixed** | §4.3; q134: 6,628 ms → 2,843 ms, 2.6 MB no longer written |
| 11 | Pure pursuit cannot hold the line (lap count −1) | **Root cause found and fixed, twice** | §2.9 + §2.10; layer 1: the look-ahead was behind the car (up to 210/210) and the fit could pick the mirror. Layer 2 (found in a REAL race, q142): the side of the line was re-decided every frame, so the target jumped to the car's other side and the controller drove it backwards. Both fixed, unit-tested (22 checks) |
| 12 | The game never starts a race | **Fixed — and it was never the touch script** | §2.10; the port's own red-glove follower presses the race button at (1105,695), the rectangle the guest scissors every frame. q142 ran 4,200 frames of race |
| 13 | The black screen (the guest's 83-frame load at 3–4.5 fps) | **Not solved; the rate is a PHASE measurement** | §2.12; 3–4.5 fps is the MENU phase, the race phase does 11.44 fps, and the CPU is flat at 1.39–1.56 cores in every round ⇒ the difference is waiting. The census A/B is running |
| 14 | The port's own log was 31 % repetitive (504 lines/s, one fflush each) | **Fixed; the frame-rate effect NOT measured** | §2.11; the `glyph-face` dedup table held 8 (face,size) pairs and the guest asks for 7 faces at dozens of sizes, and `map-app` had no dedup at all |
| 15 | Deliverables (app zip / full package / 4 volumes) + commit | **Rebuilt, and stale again** | §4.5; the NRO in the package, the app zip and on the card are the same file — but every build since has made them stale, so the rebuild has to be the LAST step |

---

## 2. Issues located and fixed

### 2.1 Console crash: a surfaceless early EGL context is illegal on the console's NVN driver

**Symptom**: black screen on the console; ~10 s later the system reports
"the software was closed because an error occurred".

**Localisation (by byte count, not by feel)**

| | Console log | Emulator log |
|---|---|---|
| Size at the crash | 292,710 B / 1,175 lines | 295,222 B at t=10 s |
| Difference | — | **0.8%** |

⇒ The console died at the **same point** as the emulator at t=10 s. Line-by-line
diff: both logs agree up to `phase=guest-frame-selftest verdict=SELFTEST_PASS`
(which was the console's last line); the emulator's next lines are
`phase=census-arm` → `phase=gl-ctx-early`.

⇒ **The console never made a single EGL call.** The cause is
`ensureEarlyGuestGlContext()` at
`native-switch/source/guest_view_probe.cpp:9904`, which calls
`eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)` — a
**surfaceless context**. That is legal on Mesa's software rasteriser and a
**kernel fault on the console's NVN driver**.

**Fix (remove the reason the probe exists, rather than making it survive)**

* `native-switch/source/gfx/gfx_gles.cpp` — `__wrap_glGetString` answers
  **from the host** when `eglGetCurrentContext() == EGL_NO_CONTEXT`:
  `GL_VERSION = "OpenGL ES 3.2 Mesa 20.1.0-rc3"`,
  `GL_RENDERER = "PowerVR Rogue GE8320 (Mesa 20.1.0-rc3)"`,
  `GL_VENDOR = "Imagination Technologies"`, GLSL `OpenGL ES GLSL ES 3.20`.
  ⚠️ The null backend's `kGlVersion` must **not** be used here: it says
  `"OpenGL ES 2.0"`, and `major = 2` makes the guest's GL assert guard at
  `0x14851e8` (`major < 3`) **silently return for the rest of the run**.
* `native-switch/source/gfx/gfx_backend.cpp` — `earlyGlContext` default
  **true → false** (`glctxearly=1` restores the old behaviour for an A/B).

**Measured (q121, archive `20261102-172615-eden-rr3-q121-no-early-gl`)**

```
phase=gl-ctx-early   ... disabled by conf
phase=view-egl-init-begin -> view-egl-init-ok -> view-egl-surface-ok
   -> view-egl-context-ok -> view-egl-current-ok
```

⇒ the first EGL call is now the guest's own windowed surface, which is what the
NVN driver supports.

---

### 2.2 Missing characters: the icon font was being drawn as a text face (main fix this round)

**Reported symptom**: the top bar's `MS 500,000` / `RS 22,141,500` — which should
be the game's **currency icons** — came out as letters, and the wrong size.

**An older conclusion was falsified first** (it was written into the source
comments and `PORT_PROGRESS.md`; measurement now disproves it):

* ❌ Old claim: *"the guest lays out a whole English paragraph with
  `r3_symbols.ttf` (the tips text `If you want to race in the real world…`), and
  the port draws it **2.048× too big** (upem 2048 ÷ 1000)"* → **wrong on every
  count**.
* ✅ Measured: the tips paragraph uses `ConduitITCPro-Medium.otf`, opened **1:1**
  and laid out **1:1**; **2,812 text draws per run, zero substitutions**.
* **Why the old claim was wrong**: it was read out of a *"six log lines after a
  marker line"* window. **Six lines is not a request boundary** — the window ran
  into the **next** request and attributed its font to the previous one.
  The correct boundary is
  `jni-utf163` (the string) → `cc-dispatch … init` → `glyph-face` (primary face)
  → `glyph-face` (fallback) → `glyph-vector init` (the result).

**Paired by request boundary, the truth is**:

* The guest asks for `r3_symbols.ttf` **18 times per run**, and **every single
  one of them is a one-character draw**;
* those characters are **not letters, they are the game's icon art** (render the
  font to a sheet and it is obvious): `U+0029` a trophy, `U+002C` a stack of
  cash, `U+0031` the game's own devil logo, `U+0111` the RR3 logo, `U+0056` a
  shock absorber. The contour counts say it too: `'o'` has **54 contours** and an
  ink box of 1.671×0.985 em — that is a drawing, not a letter.
* The measured code points and sizes:

  | Code point | Size | What it is (measured from the rendered font) |
  |---|---|---|
  | U+0031 | 175 / 122 | the game logo |
  | U+0030 | 87 | wide art |
  | U+0038 / U+0039 | 52 / 35 | art |
  | U+0056 | 105 | shock absorber |
  | U+0068 / U+0069 / U+006A | 52 / 52 / 35 | art |
  | U+007B / U+007C / U+007D | 35 | brace art |
  | U+00BD | 52 | art |
  | U+00C5 / U+00C7 | 52,35 / 35 | art |
  | U+00CB | 17 | wide coin bar |
  | U+010E | 35 | house + car |

* The port used to answer those requests with `ConduitITCPro-Regular`, so
  `U+00CB` came out as a K with an accent, `U+0111` as a d with a stroke and
  `U+0056` as a plain `V`; **and every icon was 2.3–5.8× too small** (the icon
  art is 1.00–6.22 em wide; the letter that replaced it is 0.21–0.52 em):

  | Draw | With the substitution (wrong) | With the real icon font (right) |
  |---|---|---|
  | U+00CB @17 px | 6 × 13 px | **104 × 17 px** |
  | U+0031 @175 px | 58 × 112 px | **343 × 487 px** |
  | U+0056 @105 px | 47 × 68 px | **110 × 97 px** |

  ⇒ **"missing characters" and "the icons are the wrong size" are the same bug.**

**⚠️ Two files share that name, and they are not the same font:**

| Source | Size | sha256 (first 16) | Note |
|---|---|---|---|
| `rr3core:/files/.depot/r3_symbols.ttf` | **125,764 B** | `86AA06830C1EBA9D` | **the one the guest really maps** ✔ |
| `sdmc:/…/.depot/r3_symbols.ttf` | 72,468 B | `1EE7223E3E5B11A8` | the card's loose copy — **a different font** ✘ |

They disagree on the glyph at several code points ⇒ **the build gates on the
size** and refuses the wrong one (a wrong icon font looks like a broken UI and is
very hard to trace back to the file).

**Changes made this round**

| File | Change |
|---|---|
| `native-switch/source/jni_compat/classes/glyph_vector.cpp` | `kGameFaces` gains `{"r3_symbols.ttf", "romfs:/ui/r3_symbols.ttf"}`; `kNonTextFaces` loses it (only `r3_decals.ttf` stays); the source comments are rewritten from the measurements |
| `tools/Build-NativeSwitch.ps1` | stages the icon font into the romfs and **checks it against 125,764 B**; a mismatch is refused and printed |
| `native-switch/source/gfx/gfx_backend.cpp` + `include/rr3/gfx_backend.hpp` | new `iconfont=0/1` switch (default **1**) so the same binary can A/B |
| `native-switch/tests/test_font_faces.py` | contract rewritten: the icon font must be opened **under its own name**, the 16 icon code points must all have glyphs in it, their ink must really be wider than Conduit's (otherwise the change did nothing), and the build must carry the size gate |

---

### 2.3 Hang after opening the icon font: the guest's `sscanf` has no bound

**Symptom**: q123 (`iconfont=1`) — the log stopped at **1.62 MB / 4 frames**, and
no new frame appeared for 120 s.

**A/B (same binary, same card, one key in the conf file changed)**

| Run | Configuration | Result |
|---|---|---|
| q123 | `iconfont=1` (icon font on) | **hang**: log stops at 1.62 MB, 4 frames |
| q123b | `iconfont=0` (old behaviour) | clean: **3,000 frames / 202,361 log lines / 305 s** |
| q124 | `iconfont=1` + bounded `sscanf` | clean: **3,120 frames / 35 MB of log / 305 s** |

⇒ The icon font is the **trigger**, but it stepped on an **older** bug.

**Where it hangs**: the port's sampler reports `pc_img=host-nro pc_off=0x5ea1c0`;
resolving that offset against the host NRO's symbol table gives
**`__svfscanf_r`** (newlib's `sscanf`) — and both samples landed inside it.
⇒ **the guest thread is stuck inside the host libc's `sscanf`.**

**Why**: `symbol_resolver.cpp` has `RR3_BIND(sscanf)` — bound straight to the
host's newlib, so it walks guest memory with **no bound at all**. This is the
same shape as q89's hang in `_strtoul_l` (that round bounded
`strtol/strtoul/strtod`; **`sscanf` was the one that was missed**).

**Fix** (`native-switch/source/guest_strto.cpp`,
`native-switch/include/rr3/guest_strto.hpp`, `native-switch/source/symbol_resolver.cpp`):
new `guestSscanf` copies **both the input string and the format string** out
under a readability check (512-byte cap) and the host parser only ever sees the
copies. **The log line is written before the parse** — a diagnostic that runs
after the call is worthless when the call is the thing that never returns.

**The log kept the scene** (1,005 lines):

```
phase=guest-scanf-bounded ... input=truncated in_len=512 fmt=ok fmt_len=16
   in_head=4646545F464F4E545F4249474E554D424552533C3139350D0A4646545F ...
   fmt_text=[%[A-Z0-9_],%d]
```

⇒ that input is the guest's **font/shader definition file**
(`#SEC:MAX_IMAGES:24`, `FFT_FONT_BIGNUMBERS,195`, …) and the format is a
**scanset**, `%[A-Z0-9_]`. `in_len=512` means **there is no NUL in those 512
bytes** ⇒ newlib's scanset keeps matching across unterminated memory until it
runs into the heap's end — that was the 120 s.

⇒ The bounded copy caps the scan at 512 bytes **without changing the answer**
(the token itself is far shorter than the window), which is the same rule the
rest of `guest_strto` follows: **a bound must not change the answer**.

---

### 2.4 The font cache had only 6 slots (faces were dropped silently)

`glyph_vector.cpp` had `kFaceSlots = 6`, while the guest names **14** faces
(`Conduit` ×4, `Eurostile` ×7, `HelveticaNeueBold`, `LCD`, `minion_pro_italic`,
`myriadp0`) **plus the CJK fallback plus the icon font = 16**.

**A short cache does not fail loudly**: `openFace()` returns `nullptr` and the
text is **silently** drawn with the Conduit fallback — the UI quietly loses the
face the guest asked for. ⇒ **now 16.**

### 2.5 The `substituted=` log field was always 1

It compared the **guest's card path** against the **romfs path**, which never
match, so every line claimed a substitution — including the lines where the
guest's own face was opened verbatim. **A field that is always 1 measures
nothing.** ⇒ it now compares **file names**; in the q124 log
`face=romfs:/ui/r3_symbols.ttf substituted=0` ✔

### 2.6 The fifth console crash (19:45): a null pointer in the guest, handed over by the port

The console now gets as far as the front end: the guest's image is mapped
(`dlgate: bias=0x18361f3000`), EGL completes, and the Cloudcell stack is live
(`CC: [1] POST request [https://connect-dev-ss.cloudcell.com:443/time]`).

**The crash** (`crash-000001a77b06589b.txt`)

    handler=0x87d3f19c0   pc=0x1837ed7370   lr=0x87d40de54
    far=0x28   esr=0x92000005   (data abort, translation fault, level 1)

`pc` is inside the guest image (0x18361F3000 + 0x1AE4370) and `far` is exactly
**NULL + 0x28** — the guest dereferenced a null pointer. `lr` is in the host,
and `addr2line` against the matching `.elf` (base = handler −
`nm(__libnx_exception_handler)` = 0x87d36f000, lr offset 0x9ee54) says
`http_post(…){lambda()#1}` → **`deliverServerTime`** → `operator()`.

So the fault is inside the delivery of the `/time` answer, and the last host
line before it is the port's own probe:

    phase=cc-probe ordinal=370149934272 ... worker unreadable

The `cc-srvtime-body` line that follows every successful body delivery is
**absent**, so the fault is in the header callback.

**Why the port said "unreadable"** — and this is the part that explains every
"the console and the emulator disagree" in the HTTP area:

| | Console | Eden |
|---|---|---|
| guest image bias | 0x18361F3000 | 0x2106880000 |
| guest worker object | 0x562EA7C4C0 | 0x2113D42280 |
| distance above the image | **259 GiB** (0x4E2CC44C0) | 122 MB |

`guestReadable()` only accepted `[bias − 4 GiB, bias + 4 GiB)`, so the port
declared a perfectly good guest object unreadable — **and then handed that
pointer to the guest's native `headerCallback` anyway.** On Eden the same
worker is inside the window and the probe prints its vtable.

The host's own addresses are **below** the image on both machines (host
objects 0x87e1147b0 and 0x80e209c0, host stack 0x6d20958940) and the guest's
heap is **above** it, so a symmetric window cannot tell them apart.

**The fix**

1. `guestReadable()`'s window is now **asymmetric**: the lower bound stays at
   4 GiB (it is what keeps the probe out of the host's address space — archive
   20261021-072048-eden-srvtime4) and the upper bound is 512 GiB, which
   covers the guest's heap on the console.
2. Every hand-off of `req->callback` to the guest now goes through
   `ccprobe::guestWorkerAlive()`: the worker must still be readable, or the
   delivery is skipped and the refusal is logged as `cc-worker-dead`.
   Seven call sites: the auth answer, the sync answer, the asset answer, the
   /time body, the /time header, the /time completion and the error callback.

**THE A/B THAT CLOSES THE CHAIN** (same card, same emulator, one key changed)

| Build | `iconfont` | Result |
|---|---|---|
| `aa3e2ed0` | 0 (old behaviour) | **3,000 frames / 305 s** — clean |
| `f118e3b8` | 1 (icon font on) | **hang**: 4 frames, log stops at 1.62 MB |
| `249c9f02` (this build) | 1 (icon font on) | **1,560 PNGs / 305 s** — clean |

⇒ the icon font is what exposed the crash, and the worker gate + the
asymmetric window are what fixed it. The shipping configuration (icon font
ON) is the one that is measured clean.

Gate: BUILD 0, INPUTS 0, SYMBOLS 0, tests **39/39**.

---

### 2.7 The splash title overflows the screen — and the port's `renderToTexture` never scaled the glyphs

**This is the "Version 1" / "font size is wrong" family. It reproduces on the
emulator, so it needs no console to investigate.**

#### What is on the screen

The splash's big line is **guest text**, not artwork:

```
jni-utf163  … text=Player ID: CCDI123456789 / CCRR123456789      (40 characters)
glyph-face  req=…/ConduitITCPro-Bold.otf  size=140.0  substituted=0
glyph-vector method=init textLen=40 size=140.0 boundsW=2421.0 boundsH=125.0
glyph-vector method=createTexture boundsW=2421.0 texId=260 texW=4096 texH=128
```

The port measures the string at **2421 px**, and the font's ground truth agrees
with it (`tools/conduit_width_truth.py`: 17,383 font units at upem 1000 ⇒
**2433.6 px** at size 140; the 0.5 % difference is the ink box vs the advance).
**⇒ the port measures correctly.**

Pixel measurement of the rendered frame (`tools/splash_measure.py`, q132
`frame-000120.png`, 1280×720):

```
band y 271..391 (h=121)  x 257..1279  ->  TOUCHES THE RIGHT EDGE (column 1279)
```

The title's ink starts at x=257 and is cut off at the screen edge. 1023 px of
the 2421 px string are visible ⇒ **~1398 px of the line are off-screen.**

#### Where the engine's idea and the port's idea disagree

The engine put the line's left edge at 257. A string of width *w* centred on a
1280-px screen starts at `(1280 − w)/2`, so the engine believes the line is
**~766 px** wide. The same string measured at **size 42 is 719 px** — and the
guest *also* asks for that size (`glyph-vector … size=42.0 boundsW=719.0`).
The 2421/766 ratio is **3.16**, and 140/42 is **3.33**.

So the engine laid the line out for the *small* variant and the port drew the
*large* one, or something scaled by ~1/3.2 was expected and did not happen.

#### The port's bug

`native-switch/source/jni_compat/classes/glyph_vector.cpp`, `gv_renderToTexture()`.

The guest's own Java is the contract (`reverse/java/sources/com/firemint/
realracing/GlyphVector.java`):

```java
public boolean renderToTexture(int i, int i2, int i3, float f) {
    …
    float fWidth = (i2 - (this.m_bounds.width() * f)) * 0.5f;
    canvas.translate(fWidth - (rect.left * f),
                     ((f2 - (rect.height() * f)) * 0.5f) - (this.m_bounds.top * f));
    canvas.scale(f, f);                 // <-- the glyphs are scaled by f
    canvas.drawText(this.m_text, 0.0f, 0.0f, this.m_paint);
}
```

The port used `scale` **only** to compute the origin and then rasterised the
glyphs at their full size:

```cpp
const float originX = (w - facade->boundsW * scale) * 0.5f;   // origin used scale
rasterizeLine(face, …, state->widthScale, originX, originY, …); // glyphs did NOT
```

⇒ **text drawn through that path came out 1/f too big** — exactly the 3.2×
disagreement above. Two further divergences from the Java were in the same
function: the port centred by the **ascent** instead of the **ink box**, and it
put the **ink-left** where the Java puts the **pen**.

Worse, **that path had no log line at all** (`renderToTexture` never reached
`guestViewWideLog`), so a wrong answer there was invisible — which is why the
icon-font investigation had to walk the whole chain before landing here.

**Fixed 2026-11-03** (`gv_renderToTexture`): the glyphs are rendered at
`textSize * f`, the ink box is centred horizontally and vertically, and the pen
is placed at `inkLeft + offsetX * f`. A `glyph-rtt` log line now records every
call (`texId, w, h, scale, size, boundsW/H, inkW/H, penX, baselineY, text`), so
the path is observable for the first time.

**MEASURED (q133): the guest NEVER CALLS `renderToTexture`** — 0 `glyph-rtt`
lines in a 23.5 MB log, and the same 218/214 `text-quad` lines with the same
33/35 over-wide quads in q132 and q133. **⇒ this fix is a latent contract
correction, NOT the cause of the splash overflow, and it did not change a
single pixel on screen.** It is recorded here because it was a real divergence
from the guest's Java, and because the reason it took so long to find is the
one worth keeping: *the path had no log line, so it was invisible.*

**⇒ The remaining lead (measured, not guessed)**: the overflow is in the
engine's own matrix, not in the text. The `text-quad` probe records the
vertex data and the resulting NDC:

```
a_Position: v0=(0,1022) … v2=(0,0)                 <- the full-screen quad
a_Position: v0=(1021,472) … v2=(0,0)
  ndc= v0=(1.2168,-1.0000) … v2=(-1.2168,-1.0000)  rect=-138,0,1557,720
```

⇒ a quad whose vertices are 0..1021 px wide comes out **1557 px** wide and
**starts 138 px off the left edge** — the guest's own matrix overshoots the
1280-px viewport by 1.525x, symmetrically. Other quads in the same frame map
1:1 (`a_Position 257..731` → `ndc -0.5984..0.1422`), so the engine uses two
different matrices. The next reading is which matrix the guest sets up for the
2D UI pass, and what it derives it from.

#### Ruled out with measurements (so they are not re-derived)

| Candidate | Measurement | Verdict |
|---|---|---|
| The icon font | q123b (`iconfont=0`) and q132 (`iconfont=1`) produce the **identical** `size=140 … boundsW=2421.0` | not the cause |
| A wrong font FILE | the romfs copy and the guest's own APK copy of `ConduitITCPro-Bold.otf` are the **same 130,400 bytes, same sha256** (`CABB8BE8…`) | not the cause |
| The screen size | the guest prints `Res base: 1280x720` and `nScreenSize = 1280x720` — the same as the framebuffer | not the cause |
| The device profile | **q61 already measured this**: writing `EXPECTED_SCREEN_*` changed the profile (`changed=2 written=1`) and the font sizes **did not move**; the sizes come from a fixed table (14/28/42/56/70/84/98/140) | not the cause |
| Another face | `tools/face_width_scan.py` measured the string in all 18 staged faces at size 140: the narrowest is 2367 px, none is ~766 | not the cause |

**⇒ The device profile's `EXPECTED_SCREEN_WIDTH = 480` / `HEIGHT = 320` are a
red herring for the font size** (q61), even though they are what the game's
`AndroidHigh.plist` really contains. The profile is built by the port's own
settings layer (`settings-profile … base=rr3core:/files/.depot/eds/
AndroidHigh.plist … bytes=6808/6808 changed=1`), and the guest's own dump
confirms it reads `EXPECTED_SCREEN_WIDTH = 480`.

### 2.8 The release archive was stale: `pack-zip.ps1` reused an existing zip

`tools/…/.build/pack-zip.ps1` had `if (Test-Path $outZip) { … } else { zip }`:
**an existing archive was reused without checking it against the package.** The
2026-11-03 repack shipped an archive that was **1480 bytes** off the package:

```
README-hardware-test.md   zip=27,062   pkg=29,969   (+2,907)
switch/rr3-gfx.conf       zip=3,975    pkg=2,548    (−1,427)
```

The size gate caught it (`the archive holds 11459500064 bytes but the package
holds 11459501544`), which is how it was found — but the failure came **after**
a whole repack cycle.

**Fixed 2026-11-03**: the archive is now fingerprinted against the package it
was built from (file list + sizes + timestamps, hashed) and a mismatching
archive is **rebuilt** rather than reused; the fingerprint is written only after
a successful build, so an interrupted run leaves none and the next run rebuilds.
`pack-zip.ps1` also takes a `-Stamp` now (its paths were hard-wired to
20261102, which is how a repack could write next to the previous day's
deliverables), and `.build/run-repack.ps1` chains the whole deliverable set in
one go: archive → parts → app zip → join check.

---

## 3. Conclusions that were falsified (recorded so they are not re-derived)

| Old conclusion | Measurement | Why it was wrong |
|---|---|---|
| "The guest lays out a whole English paragraph with the symbol font, so the port draws it 2.048× too big" | The paragraph uses Conduit 1:1; 2,812 text draws per run, zero substitutions | a log window that crossed a request boundary ⇒ misattribution |
| "q122's blank dialog was caused by the symbol font" | q122 opened the card's **72,468-byte** file, which is a **different font** | same name, different content |
| "The icons are half the size because upem 2048 vs 1000" | A ratio does not describe a size error; the real cause was the **face being swapped** (different glyphs entirely) | a ratio mistaken for a size error |
| "The splash title is 1455 px wide because the symbol font's advance is 10.4 em at size 140" | The string is drawn with **ConduitITCPro-Bold**; the font's real advance for it is 2433.6 px at size 140 (`conduit_width_truth.py`). 1455 px is the width of a *different* element | a number from another measurement |
| "The splash line is drawn too big because the device profile says EXPECTED_SCREEN 480×320" | **q61 already measured this**: the profile was rewritten (`changed=2 written=1`) and the font sizes did not move. The sizes come from a fixed table | a plausible cause that was already falsified |
| "The splash artwork is scaled too wide" | The overflowing line is **guest text** (`Player ID: CCDI123456789 / CCRR123456789`), measured with a texture id, a bounds width and a pixel band | the artwork was blamed for a text bug |
| "The port's text probe caps the frame rate (a linear scan per draw)" | The scan was real (up to 4096 entries x ~90,000 draws a frame) and is now a hash, but **q137 with the fix did not get faster** (q134 960 / q136 840 / q137 — frames per 306 s). The PNG capture was measured too, and it is not the cost either | a plausible cause, measured and falsified |
| "The game got 6x slower" | The October round that showed 23 fps spent most of its 1206 s **in the race**; the November rounds spend it in the **menu**. Different phases, so the numbers are not comparable | two different phases compared as if they were the same |
| "The game runs at 3–4.5 fps" | That was the **menu** phase. MEASURED 2026-11-04 (q142): the same build, the same card and the same ~1.44 cores of CPU produced **4,680 frames in 409 s = 11.44 fps**, because most of that round was **in the race**. The CPU cores are the same in both phases (1.44–1.56 in every healthy round), so the phase difference is a difference in **waiting**, not in computing | one phase's rate was quoted as the game's rate |
| "The pursuit fix is verified — the last sample's error was 0.0391 rad" | A **single** sample is not a measurement. MEASURED (q142): over the 141 samples the error ranged from **−3.05 to +3.03 rad**, and **63 of 141** samples were more than 1 rad off. The good sample was the tail of a series that had been oscillating | one sample read as the distribution |
| "The port's own probes cost the frame rate (glyph-face / map-app)" | The volume is real (33,643 + 31,315 lines = **31 %** of a 45 MB log, one `fflush` each) but **no A/B has been run** on the log-volume cut yet, and the port's probes were **already excluded** by two measured A/Bs (PNG capture, glyph-texture hash) | a plausible cause, not yet a measurement — stated as a suspicion, not a result |

---

### 2.10 The look-ahead point moved to the OTHER SIDE of the car, because the direction was re-decided every frame

**Measured 2026-11-04 (q142, archive `20261104-145617-eden-rr3-q142-corners`).**
This is the first round in which the game ran a **real race for thousands of
frames**, so the pursuit could finally be measured instead of inferred.

**The race started on its own.** MEASURED: the port's own red-glove follower
pressed the race button —

```
phase=ui-press … kind=orange x=1105 y=695 area=8180 w=326 h=45   (frame 420)
```

— which is the button the guest scissors every frame as
`glScissor x=994 y=667 w=187 h=53`. **⇒ The race-start path was never the
touch script.** (The script's own frame-450 tap was still on the card in that
round, which is why the earlier rounds appeared to start "by accident".)

**What the pursuit did over the race** (141 samples, frames 570..4770):

| Measure | Value |
|---|---|
| frames of race | 4,200 |
| distance covered | 91,810 spline units = **5.08 %** of a lap (the lap is 1,806,336) |
| pursuit error `perr` | **−3.05 … +3.03 rad** (63 of 141 samples worse than 1 rad) |
| lateral residual `lres` | ≤ **2.00 m** — the car was ON the line |
| distance to the look-ahead point | 16–43 m |
| best / worst | `best=3.98`, `stalls=6`, `flips=3` |

**⇒ The car was on the line and the target was 20 m away, and the error was
still 180°.** A car 2 m from the line with a target 20 m ahead cannot have a
180° error: the angle is bounded by `atan(2/20) ≈ 6°`. **⇒ The only way to get
180° is that the car's own direction of travel had reversed.**

**And the reason is in the source.** `raceLineTarget()` decided which side of
the line to take the look-ahead point from **on every single frame**, using
the car's instantaneous heading:

```cpp
int step = 1;
if (headingX != 0.0 || headingY != 0.0) {
    ...
    if (backward > forward) { step = -1; }   // <- re-decided EVERY frame
}
```

⇒ the closed loop, all of it visible in q142's samples:

1. the car is nudged and its direction of travel reverses for a moment;
2. `step` flips, so **the look-ahead point jumps to the other side of the
   car**;
3. the error the controller sees is now the sum of two flips (the heading's
   and the target's) — the samples go `+0.44 → −3.05 → +2.99 → −2.73 rad`
   from one 30-frame sample to the next;
4. the controller saturates the steering (`steer = ±1.0`, full lock) and the
   throttle is on;
5. the car swings past the target, **drives backwards** — q142: it LOST
   6,581 units of progress between frames 2010 and 2040 — which reverses the
   heading, which repeats step 1.

**⇒ The track does not change direction while the car drives on it, so the
decision belongs to the RACE, not to the frame.**

**Fixed (this round), in three parts:**

1. **The direction is locked.** `raceLineTarget()` takes the direction as an
   in/out argument: 0 means "not decided yet", and a non-zero value is held
   for the whole race. The caller owns the lock, so the function stays free
   of hidden state and the host self-test can exercise it directly.
2. **The one decision is made from a WINDOWED motion vector** — the car's
   displacement over the whole 48-sample position ring — not from the
   instantaneous heading. A single-frame wobble cannot reverse it. The lock
   is reset when the track changes (`splineGeneration`).
3. **The throttle is let go when the error is more than ~86°.** A car that is
   spun or facing back down the track has to turn around, and full throttle at
   full lock turns a turn-around into a spin. The brake stays off, because the
   guest reverses only when the brake is held **and** the throttle is let go —
   and a car that reverses here is the loop above.

**Unit test** (`tests/host/race_line_selftest.cpp`, 22 checks, all ok): three
new ones prove the lock — *the direction is decided and reported back*, *the
lock is not changed by a reversed heading*, and *a locked race keeps the
target on the same side of the car*.

**⇒ MEASURED, in the next round (q143, the same build with the census off):**

| Measure | q142 (before) | q143 (after) |
|---|---|---|
| samples with the target behind the car (\|perr\| > 1 rad) | **63 of 141 = 45 %** | **21 of 160 = 13 %** |
| distance covered | 91,810 units = **5.08 %** of a lap | 156,244 units = **8.65 %** |
| frames of race | 4,200 | **4,770** |
| stalls | 6 | 4 |

⇒ **The lock works**: the look-ahead point stopped jumping to the other side
of the car, and the car drove 70 % further around the lap. **And it is not
enough**: the car still does not finish a lap.

**⇒ AND THE NEXT BOTTLENECK IS NOW VISIBLE, and it is not the pursuit.**
MEASURED (q143): the car reached its best at **frame ~1,830** (161,757 units
= 8.95 %) and then **stopped for good**. From frame ~2,700 to the end of the
round (2,700 frames) the guest's own **spline node index stood at 319** and
the distance moved by **10 units per 30 frames** — about 1.3 mm a frame, with
the throttle on and the pursuit's error small and steady. **The car is pinned
against a barrier**, and the port's recovery (which fires 4 times in the whole
round) does not free it.

⇒ The pursuit's error is now good enough (13 % of samples off, and the car is
2 m from the line), so the next thing to fix is the **stuck car**: the
recovery needs an EXIT rather than a reference-point reset, and the stall
detector has to notice a car that is pinned while the pursuit keeps asking it
to turn.

**⇒ FIXED AND MEASURED (q145, the next build).** The discriminator is the
car's own **POSITION**, not the progress meter: the progress metric has a
one-metre granularity, so a car crawling at less than a metre a second and a
car pressed against a barrier look exactly the same to it. A crawling car's
position changes; a pinned car's does not. So the test is *"the world
position has not moved by a metre for 1,200 frames"*, and it also requires
that the car has **already made progress in this race** — otherwise the start
sequence's hold at the grid is a pinned car too. The recovery then fires at
most once per 1,200 frames, so it cannot become a recovery loop.

| Measure | q143 (pinned) | q145 (fixed) |
|---|---|---|
| frames of race | 4,770 | **5,760** |
| distance covered | 8.65 % of a lap | **10.35 %** |
| recoveries fired | 4 | **10** |
| frames in the round | 5,410 | **6,240** |

⇒ **The car is no longer pinned.** The progress curve used to flat-line at
92,871 for 2,700 frames; it now oscillates and climbs
(93,872 → 130,177 → 177,324 → 162,471 → 186,921), i.e. the car gets free and
drives on.

**⇒ BUT THE LAP COUNTER IS STILL −1, AND NOW THE REASON IS CLEAR.** The car
is at 10.35 % of a lap and spends **41.5 % of the race in a recovery** — the
brake was held in **80 of 193 samples** and the recovery fired **10 times**.
At the rate it makes progress when it is NOT stuck (178 units a frame) a lap
is ~10,000 frames; at the rate it actually manages it is ~56,000 frames.
**⇒ The next lever is not the pursuit
any more, it is the quality of the line following** — the look-ahead distance
(20 m), the steering gain, and how much of the track the car can actually
hold. The car is on the line to within 2 m, but it is still hitting the
barriers.

### 2.11 The port's own log was 31 % repetitive, and the deduplication tables were too small

**Measured 2026-11-04 (q142).** The guest-view log was 45.2 MB / 205,983 lines
in 409 s — **504 lines a second**, each one an `fprintf` plus an `fflush` on
the emulated card (the log writer does not `fsync`; that was moved off the
card in an earlier round). The volume is not the game's: it is the port's own
probes.

| Phase | Lines | Share |
|---|---|---|
| `glyph-face` | 33,643 | 16 % |
| `map-app` | 31,315 | 15 % |
| `jni-void` + `cc-dispatch` + `jni-utf163` | 50,878 | 25 % |

**Both of the big two were the same line repeated**, and both dedup tables
were too small for the number of distinct keys:

* `glyph-face` kept **8** (face, size) pairs. The guest names **7 faces at
  dozens of sizes each**, so the table thrashed and the dedup never fired:
  16,823 of the 33,643 lines were the same CJK bootstrap face at some size.
* `map-app` had **no** dedup at all: the guest resolves the same paths over
  and over.

**Fixed**: the `glyph-face` table is now 4096 slots keyed by a 64-bit hash of
the (face, size) pair, and `map-app` gets a 2048-slot hash set of the paths
already reported. The information is kept (every distinct pair and path is
still logged once); the repetition is gone.

**⇒ What this has NOT yet proved**: that the log volume was costing frame
rate. The port's probes have already been excluded twice by measurement (the
PNG capture, the glyph-texture hash), and the log writer does not `fsync`. The
honest statement is that this is a **large, cheap, safe reduction of the
port's own overhead**, and its effect on the frame rate is **not yet
measured**.

### 2.12 The frame rate is a PHASE measurement, and the CPU is the same in both phases

**Measured 2026-11-04, over every archived round of 2026-11-03/04** (one
instrument: `run-summary.txt` + `eden-cpu-series.csv`, so every round is read
the same way):

| Round | Wall | Frames | fps | CPU cores | ms CPU/frame | Log |
|---|---|---|---|---|---|---|
| q132 | 306 s | 1,560 | 5.10 | 1.47 | 276 | 49.1 MB |
| q133 | 306 s | 720 | 2.35 | 1.50 | 613 | 47.3 MB |
| q134 | 306 s | 960 | 3.14 | 1.47 | 450 | 51.8 MB |
| q136 | 307 s | 840 | 2.74 | 1.49 | 523 | 51.9 MB |
| q137 | 306 s | 1,080 | 3.53 | 1.54 | 419 | 52.8 MB |
| q138 | 612 s | 2,760 | 4.51 | 1.44 | 314 | 130.3 MB |
| q140 | 610 s | 2,640 | 4.33 | 1.39 | 316 | 124.9 MB |
| q141 | 606 s | 2,280 | 3.76 | 1.56 | 407 | 114.3 MB |
| **q142** | **409 s** | **4,680** | **11.44** | **1.44** | **122** | **43.1 MB** |

**⇒ Every healthy round uses 1.39–1.56 cores of CPU, and the frame rate
varies 4.9× across the same builds.** Since the CPU is flat, the extra time
per frame is **waiting**, not computing. And q142 — the one round that spent
most of its time **in a race** — is 3× faster than the menu rounds with a
**smaller** log (43.1 MB vs 51.8 MB).

**⇒ Two conclusions, one of them a correction:**

* the black screen's "3–4.5 fps" is the **menu** phase; the game in a race
  does 11.4 fps on Eden;
* **any frame-rate comparison has to name its phase.** The October 23 fps and
  the November 3.9 fps were the same mistake twice.

**⇒ The next measurement is a paired A/B on the one switch that was never
tried**: the guest-thread census, which sweeps and samples the guest's threads
every 4 s and has a kill switch (`census=off` in
`sdmc:/switch/rr3-census.conf`) that **has never been run**. The census
A/B is running with the pursuit fix in the same build, as two rounds that
differ in that file and in nothing else.

**⇒ MEASURED (the pair is in: q143 census=off, q144 census=on — the same
build `330cd54d`, the same card, the same scripts, 405 s each):**

| | census=off | census=on | difference |
|---|---|---|---|
| frames | 5,410 | 5,481 | **+1.3 %** |
| log bytes | 34,085,625 | 35,856,215 | +5.2 % |
| CPU cores | 1.299 | 1.416 | **+9.0 %** |
| CPU seconds | 513.3 | 559.5 | +9.0 % |

**⇒ THE CENSUS IS NOT THE FRAME-RATE LEVER.** +1.3 % of frames is inside the
noise, and it does not perturb the driving either (the share of samples with
the look-ahead point behind the car was 13 % with the census off and 14 %
with it on). What it does cost is **~9 % of the CPU** and ~5 % of the log —
worth switching off for the console, not worth calling a fix.

**⇒ AND THE PNG CAPTURE IS EXCLUDED A THIRD TIME, AND CLEANLY.** The probe
conf says `pngfrom=1300`, and q143's race started at frame 630 — so the
**menu** phase, the slow phase, captured **no PNG at all**, and the menu
still ran at 4.8 fps.

**⇒ THE PHASE SPLIT, measured from the two new rounds** (the race starts at
the frame the guest reports the RaceMetrics object):

| Phase | Frame rate on Eden |
|---|---|
| menu (the front end) | **4.8 fps** |
| race | **15–17 fps** |

⇒ **⇒ The next instrument is a per-frame HOST TIMESTAMP**, because the frame
rate is now known to be phase-dependent while the CPU is flat, and nothing
in the logs says *what* the frame is waiting for. The three candidates that
remain are the guest's own 2D UI work (1,200 controls in the tree, 82 face
opens a second, 76 asset resolutions a second), the port's per-frame control
walk, and the host's own present path.

**⇒ ⇒ AND THE INSTRUMENT ANSWERED IT (q146).** The port's per-frame census
line now carries the frame's three costs — `tt` (the whole frame), `tr` (the
guest's render call), `tp` (the port's own work) and `ts` (the present,
`eglSwapBuffers`) — on the **existing** line, so the instrument costs no
extra log writes and cannot change the thing it measures.

| | menu phase (630 frames) | race phase (6,278 frames) |
|---|---|---|
| the whole frame | mean 546 ms, **median 107 ms** | mean 43 ms, **median 29 ms** |
| of which: the guest's render | **99.0 %** | **90.2 %** |
| of which: **the port's own work** | **0.1 %** (median 1 ms) | **1.0 %** (median 0 ms) |
| of which: the present | 0.9 % | 8.8 % |
| implied frame rate (median) | **9.3 fps** | **34.5 fps** |

**⇒ THE PORT IS NOT WHAT MAKES THE MENU SLOW.** Its own per-frame work is
0.6 ms out of a 546 ms frame. And the frame time is not spread out: **37 of
1,767 frames took more than a second, and those 37 frames are 69 % of the
run's frame time** (271.3 s of 395.9 s). The longest was 49.9 s — and it
holds 502 string-to-number conversions, 49 path resolutions and 15 stats:
**the guest loading and parsing assets inside its own render call**. The
port's own work in that frame was **1 ms**.

**⇒ AND IT IS NOT THE PORT'S FILESYSTEM LAYER EITHER**: the bulk-read path
served the 4.2 MB base asset list in **10 ms** (421 MB/s), and the whole
run's bulk reads were 5 calls and **25 ms**.

**⇒ ⇒ SO THE MENU'S SLOWNESS ON EDEN IS THE GUEST'S OWN ARM64 CODE BEING
EMULATED.** Both the guest *and* the port's Mesa are ARM64 binaries, so on
Eden both of them run through an x86 emulator; on a console they are native.
**⇒ Eden's 4.8 fps is a statement about Eden, not a forecast of the console.**
The console's black screen is therefore much more likely to be the **boot
phase** — which the port does control — than the frame rate. The boot
phase's own cost is already measured: the asset-list rewrite is 0 bytes
written and 2,843 ms, and what is left is reading 1235 lists and 386 cached
files on *every* boot, which a cache key could skip entirely.

**⇒ ⇒ AND THE INSTRUMENT IS WHAT THE CONSOLE NEEDS.** With `tt/tr/ts/tp` in
the console's own log, the console's black screen becomes a DIRECTLY
answerable question: is the long frame the guest's own work or the port's?
Eden could not answer that, because Eden's guest is emulated and the
console's is not.

### 2.13 The track's spline holds a RACING LINE, and the port is following the wrong one

**Measured 2026-11-04 (Spa, from the port's own `spline-open` line):**

```
phase=spline-open … chunks=3
  | centre_spline nodes=441 loop=112270.0 spanX=27466 spanY=28797
  | start_spline   nodes=882 loop=112030.5 spanX=27439 spanY=28797
  | ai_spline      nodes=882 loop=111197.5 spanX=27299 spanY=28921
```

**⇒ The file holds THREE lines, and the port follows the wrong one.**
`centre_spline` is the geometric middle of the track. `ai_spline` is the line
the AI cars drive — the **racing line** — and it is **67.0 m shorter a lap**
(7,016.9 m → 6,949.8 m), because it cuts the corners.

**How far apart are they** (`tools/spline_chunks.py`, the same container
format the port decodes):

| | metres |
|---|---|
| median | **2.92 m** |
| p90 | **4.62 m** |
| max | **8.25 m** |

**⇒ And the car is on the CENTRE line, not the racing line.** The port's own
`lres` (the lateral residual to the line it is following) is **≤ 2.01 m** in
every sample of q145 — so the car is on the centre line, and the centre line
is 2–8 m from the racing line. A car following the centre line drives on the
**outside** of every corner.

**⇒ That is a candidate explanation for the ten barrier hits a race** — and
the car spends **41.5 %** of the race in a recovery (the brake held in 80 of
193 samples). But it is a **hypothesis**, and it was measured wrong once
already: a first attempt to re-derive the transform in Python gave a residual
of **19.22 m** where the port's own fit gives **≤ 2.01 m**, so that
analysis's numbers were void and were thrown away rather than reported.

**⇒ So the port now MEASURES it instead of arguing about it.** Two
instruments, both pure instrumentation, no change to the driving:

* **`rres`** — the car's distance from the `ai_spline` chunk, in the same
  frame and with the same transform as the line the car IS following
  (`raceLineDistanceToChunk()` in `source/race_line.cpp`). If `rres` sits at
  2–8 m while the car is running, it is on the wrong line and switching the
  chunk is the fix. If `rres` is near zero, it is already on the racing line
  and the barrier hits have some other cause.
* **the frame's three costs** — `tt` (the whole frame), `tr` (the guest's
  render call), `tp` (the port's own work) and `ts` (the present,
  `eglSwapBuffers`), on the existing per-frame census line, so the
  instrument costs no extra log writes.

**⇒ MEASURED (q146, the port's own `rres`):**

```
frame=5640  line=1  lres=1.97  rres=10.50
frame=5670  line=1  lres=1.97  rres= 9.81
frame=5700  line=1  lres=1.97  rres= 9.17
frame=5730  line=1  lres=1.97  rres=11.38
```

⇒ **The car was 1.97 m from the centre line and 9–11 m from the racing
line.** It was on the wrong line, and the distance is bigger than the
centre↔racing deviation measured in the stored frame (2.92 m median),
because the car's path also contains its reversals and its excursions off
the line, so its *instantaneous* distance from the centre line is larger
than the fit's mean residual. **⇒ The switch is worth making.**

**⇒ THE SWITCHING IS NOT A ONE-LINE CHANGE, and the reason is the fit.** The
fit aligns the centre line onto the car's path, so the moment the car drives
the racing line the fit would follow it there and **pull the racing line
with it** — the two would swap places and the car would drive the centre
line again, offset by up to 8 m. The transform is a property of the TRACK,
not of where the car is, so:

* the **fit** keeps using `centre_spline` — it is the reference that says
  where the track IS;
* the **target** moves to `ai_spline` when the file has one;
* the **transform is LOCKED** once the fit settles (the same variant and an
  offset that moved less than a metre), and released when the track changes.

**⇒ MEASURED (q147, with the switch on):** `rres` went from 9–11 m to
**0.32 m** — the car really was on the racing line — and the time spent
reversing fell from **41.5 % to 13.7 %** of the race (three times fewer
barrier hits). **BUT the net lap progress went from 10.35 % to 5.14 %**,
because the pursuit's error became a constant **0.7 rad (40°)**: a 20 m
look-ahead overshoots the racing line's sharper apexes, so the car crabbed
along the line instead of driving it.

**⇒ ⇒ SO THE SWITCH IS REVERTED for now, and the reason is NOT that the
racing line is wrong.** It is that the look-ahead has to be tuned for the
racing line before the switch is worth having — and that tuning is the
AUTOPILOT's problem, which is a different problem (§2.14). The `rres`
instrument stays, so the autopilot's line quality is measurable from now on.

### 2.14 THE AUTOPILOT AND THE PORT ARE TWO DIFFERENT QUESTIONS

**This is the most important correction of the round, and it came from the
user, not from the code.**

Every round so far ran with **`autopilot = 1`** in the card's
`rr3-gamepad.txt`, which means:

```
auto   the port's own loop owns the throttle, the brake and the steering
       axis -- the human's inputs are OVERWRITTEN before they reach the
       guest
assist the human owns every control they are touching, and the loop fills
       in only the ones they are leaving alone
manual the port does not touch the car at all
```

**⇒ ⇒ SO EVERY MEASUREMENT OF "THE LAP COUNTER" SO FAR HAS BEEN A
MEASUREMENT OF THE AUTOPILOT'S DRIVING.** The car could not complete a lap
because the port's own pure-pursuit controller cannot drive a lap — which is
a quality problem in ONE optional feature, and says nothing about whether
the port is PLAYABLE. The two were being measured as if they were one thing.

**⇒ AND THE HUMAN PATH HAS NEVER BEEN PROVEN TO MOVE THE CAR AT ALL.** The
port has the mapping (`rr3-gamepad.txt`: 22 bindings, the guest's own button
and axis ordinals) and the scripted pad (`rr3-pad.txt`, the timeline that
injects `lx=`, `rt=`, `b=` at given frames), and the shipped pad script
holds the accelerator from frame 600 — but with `autopilot = 1` the guest's
steering axis was overwritten by the loop every single frame. **Nobody ever
checked whether the human's stick reaches the car.**

**⇒ ⇒ SO THE MEASUREMENT IS NOW THE HUMAN ONE**, and it is three separate
observations in one round (`autopilot = 0`, the card's pad script becomes a
weave that holds the accelerator and alternates the steering axis):

| # | Question | Instrument |
|---|---|---|
| 1 | does the human's input reach the guest at all? | the port's `gamepad-state` line: `lx` (what was asked for) against `guest_lx` (what the guest got) |
| 2 | does the human's throttle reach the car? | the guest's own car speed rising while BTN_A is held |
| 3 | does the human's steering reach the car? | the car's path curving when `lx` changes |

**⇒ ⇒ AND IF THEY DO, THE SHIPPED DEFAULT CHANGES.** A game port in which
the port drives the car and ignores the human is not a playable port, so
`autopilot = 0` (the human drives) is the shipping default and `auto` is the
opt-in attract mode — **but only after the human path is measured**, because
shipping `manual` on an unproven input path would make the console
unplayable in a different way.

⇒ The new files: `native-switch/card/rr3-pad-manual.txt` (the weave),
`.build/deploy-manual-control.ps1` (the card switch, with a backup so it can
be undone) and `.build/run-manual-round.ps1` (the gate, the switch and the
round, in that order).

**⇒ ⇒ AND THE CODE SAYS THE SAME THING, IN THREE PLACES.** The port has TWO
input paths, and the split was not where anyone would guess:

```cpp
if (scriptDrives) {
    // THE SCRIPT DRIVES THE GUEST.  The physical pad is ignored ONLY
    // while nobody is using it.
    ...
} else {
    // THE PHYSICAL CONTROLLER, THROUGH THE MAPPING.
    // libnx -> canonical inputs -> the mapping on the card -> the guest
    ...
}
```

**THE THREE DEFECTS, ALL FOUND IN THE SAME PLACE:**

1. **`autopilot` governed only the PHYSICAL path.** The script path was
   overridden by the closed loop no matter what the setting said:

   ```cpp
   if (driving) {
       scripted.leftX = drive.steer;                 // the script's own axis
       if (drive.throttle) scripted.buttons |= (1u << kBtnA);
       if (drive.brake)    scripted.buttons |= (1u << kBtnX);
   }
   ```

   ⇒ **With `autopilot = 0` the port still wrote its own steering into the
   script's axis and forced its own throttle.** The setting was a switch that
   did not switch anything on the path the experiments actually used.

2. **THE SHIPPED PACKAGE SHIPS THE EXPERIMENT SCRIPT, AND IT BLOCKED THE
   PLAYER.** `rr3-pad.txt` is in the release tree, and while a script is
   active the physical controller is ignored *entirely*. ⇒ **On a console
   with the shipped package, the player's own controller was dead.** The
   script owned every button and every axis.

3. **THE SCRIPTED PATH HAD NO INSTRUMENT.** `logGamepadState()` — the line
   that reports what was asked for against what the guest got — was only
   called on the physical path. ⇒ A round whose script drove the guest had
   **no instrument at all**, which is exactly why "did the human's input
   reach the car?" could not be answered.

**⇒ ALL THREE ARE FIXED:**

* the `autopilot` mode now governs **both** paths: `auto` the loop owns the
  car, `assist` the human wins on every control they are actually using,
  `manual` the port does not touch the car at all;
* **the script yields to the human**: it drives only while nobody is using a
  physical controller — a stick off its deadzone, a trigger pulled, a button
  held. Inside an emulator there is no controller, so every experiment keeps
  working exactly as before; on a console the player's stick takes over the
  moment it moves;
* the scripted path has **its own instrument** (`scripted-state`): what the
  script asked for, what the guest got, whether the closed loop overrode the
  steering, and whether a physical controller took over.

**⇒ ⇒ AND MEASURED (q149, the card at `autopilot = 0`, the weave script
holding the accelerator and alternating the steering axis):**

```
f= 300 mode=0 lx_script=0.000 lx_guest=0.000 btn=0x0000 loop_steered=0 phys=0
f= 600 mode=0 lx_script=0.000 lx_guest=0.000 btn=0x0001 loop_steered=0 phys=0
f= 900 mode=0 lx_script=0.500 lx_guest=0.500 btn=0x0001 loop_steered=0 phys=0
...                                     (24 consecutive samples, all the same)
f=7200 mode=0 lx_script=0.000 lx_guest=0.000 btn=0x0001 loop_steered=0 phys=0
```

⇒ **In every one of the 24 samples the axis the script asked for is the axis
the guest got, the accelerator is the accelerator, and the closed loop did
NOT overwrite either.** Under the old code that value was the autopilot's
steering and nothing in the log said so.

**⇒ ⇒ AND THE CAR ANSWERED.** The guest's own distance and speed, with the
human's input as the only source of control:

```
f=750  total=   632  speed= 1750   (the accelerator took effect)
f=780  total=  9116  speed= 5840
f=960  total= 20910  speed= 5879
f=1290 total= 58924  speed= 6464
```

and the step between samples **alternates between ~1.5 m and ~23–33 m** — the
car is accelerating on the straights and scrubbing on the turns, i.e. the
**steering axis is reaching the car and turning it**.

⇒ **⇒ THE PORT IS PLAYABLE. The lap counter's −1 was the AUTOPILOT's
quality problem, and the autopilot is an attract mode, not the game.**

**⇒ ⇒ SO THE SHIPPING DEFAULT CHANGES.** The release tree now ships
`autopilot = 0` (the player owns the car) and a `rr3-pad.txt` with **no
events** — the console's file must not hold the accelerator, because a held
accelerator takes the throttle away from the player again. The experiment
scripts (`rr3-pad-manual.txt`, the weave, and `rr3-pad-autopilot.txt`, the
confirm-then-throttle one) stay in the repository and are NOT in the
packager's list.

**⇒ AND THE SHIPPED DEFAULT IS ONLY HONEST ONCE THE PATH IS MEASURED**,
which is what q149 did.

---

### 2.9 "laps = -1" is not a counter: the look-ahead point was BEHIND the car

**This is the biggest functional finding of the round, and it is measured.**

### What the counter actually says

The guest's own `RuleSet_RaceMetrics` object reports the lap counter, and the
port reads it. MEASURED in the best archived round (q11, 1206 s, 27,720
frames):

```
phase=race-metrics … ok=1 lapsOk=1 lapsWhy=none … laps=-1 topspeed=7742
```

⇒ **the guest itself reports `laps = -1`, at 77 m/s (279 km/h).** The port is
not miscounting; the car never crosses the line.

**THE SCALE IS CALIBRATED** from three rounds that agree exactly: one lap is
**1,806,336 spline units** (`lapd=` is the same in q8, q10 and q11). The car's
best distance ever was **208,864 units = 11.6 % of a lap** (q11); most rounds
stopped at **125,841 units = 7.0 %**.

**⇒ The real symptom is "the car drives 7-12 % of a lap and then stops", not
"the counter is wrong".** A census of all 153 archived rounds
(`tools/lap_census.py`) found **no round that ever reported a completed lap**.

### Why the car stops: the pursuit was aiming BACKWARDS

The port's pure-pursuit controller aims at a look-ahead point on the fitted
racing line (`raceLineTarget`). The engine's own error — the signed angle
from the car's heading to that point — is logged per sample. MEASURED over
the archived rounds (`tools/pursuit_convergence.py`):

| round | pursuit ON | target BEHIND the car (error ≈ 180°) |
|---|---|---|
| q119 | 210 | **210 (100 %)** |
| q121 | 113 | 71 (62.8 %) |
| q116 | 133 | 21 (15.8 %) |
| q115 | 4 | 3 (75 %) |

⇒ **the look-ahead point was behind the car, so the controller steered the
wrong way, the car left the track at the first corner, and the recovery
deadlock (no progress → reverse → no heading → no pursuit) took over.**

**WHY THE TARGET WAS BEHIND: the residual cannot tell a curve from its mirror
image.** The fit searches 8 axis variants × a scale grid and keeps the
smallest residual; over the 48 metres of the car's own path, a curve and its
reflection agree to within centimetres, so the MIRROR can win. And the fit
says nothing about the DIRECTION, because a closed loop traversed backwards
has the same shape. The fitted variant also **changed mid-run** (q121 saw six
different values in one round), and every change moved the look-ahead point
to the mirror image: a full-lock steering kick in the wrong direction.

⇒ Two defects, both fixed:

1. **The look-ahead direction** — `raceLineTarget()` walked the spline's node
   order, which does not necessarily run the way the car drives. It now takes
   the car's heading and walks the way the car is going. The unit test
   (`tests/host/race_line_selftest.cpp`) proves it: the same call with the
   opposite heading puts the target on the opposite side of the car.
2. **The variant** — the fit now applies a **handedness test**: the car's own
   path is part of the line, so the line bends the way the car bends, and a
   mirror bends the other way. The mirrored candidates are dropped. If the
   test rejects everything, the unfiltered winner is used — the line is then
   exactly as good as it was before, and no worse.

**⇒ The next measurement is the one that decides it**: the pursuit's error
should sit near zero instead of at 180°, and the car's distance should pass
12 % of a lap.

### A measurement that was WRONG and is recorded so it is not repeated

A first version of the convergence analysis compared the MAGNITUDE of the
error and concluded "the steering makes it worse" — and that conclusion was
an artefact of the **angle wrapping at ±π**. The wrap-aware fit
(`tools/pursuit_slope.py`) gives `d(error) = −0.340 × steer` (R² = 0.166) for
the same data: the sign was RIGHT all along. The wrap is why the raw
comparison said the opposite.

### What blocks the measurement: the race never starts

MEASURED (q135, 611 s, 2400 frames): the game goes splash → menu → race
intro cinematic → **"select car"** → black → back to the menu. The race
metrics object is never found, so there is no race to count laps in.

**THE COORDINATE WAS NEVER WRONG.** MEASURED (q138): the guest scissors the
race button every frame as `glScissor x=994 y=667 w=187 h=53`, so the orange
button is at (994..1181, 667..720), centre about (1087, 693). The script's
coordinate (1114, 698) is inside it, and the tap was delivered and **accepted**
by the guest (`touch-inject … guest{gx=1114 gy=698 down=1}`).

**THE FRAME WAS WRONG, and that is the whole story.** The taps are frame
indexed and the race button's tap sits at script frame **5500**; the menu
phase runs at about **3–4.5 fps**, so a 600 s round only reaches frame ~2700
and the script's last entry (frame 13440) needs about **80 minutes**. The tap
that starts the race therefore never fired.

⇒ Fixed by putting the tap where the button is actually up. The screen
timeline (the `ui-ctl` lines) shows the game **cycling** through the
car-select screen — 360..660, 780..1080, 1380..1470, 1890..2160, 2340..2580 —
with gaps where no screen is up at all, and a tap in a gap does nothing. The
shipped script has **one** tap, at frame 450: MEASURED (q138) the race starts on
its own at about frame 510, so a later tap would land in the race screen and
press whatever is there.

**⇒ The taps that land DURING a race are not harmless.** The round with the
taps spread over the whole run (q139) stopped at 600 frames, while the same
build with one tap (q138, 2760 frames) and the same taps without the pursuit
fix (q140, 2640 frames) both ran to the end.

**⇒ AND THE RACE DID START, ONCE, FOR THE FIRST TIME** (q138, frame 510):

```
phase=race-state … gm_ok=1 metrics=1
    cls=27RuleSetModifier_RaceMetrics vptr_ok=1 mres=0x3f mfail=none
    laps=4294967295 lapd=1314816 snode=433 thr=1
```

⇒ the guest's own RaceMetrics object was found and its members resolved, for
the first time in any archived round, and the throttle was on. **But that
sample has `line=0 lres=0 perr=0 pur=0 drive=0`** — the line is not fitted
yet and the pursuit has not taken over. **⇒ The pursuit fix is unit-tested but
NOT yet measured in a race.** That is the next measurement, and it needs a
round where the race runs for more than a few hundred frames.

**⇒ THE PRINCIPLED WAY, still open**: the port can press a control by the id
the guest itself gives it (`rr3-ui.txt`, no frame budget). The race button's
id has not been read out yet, because **the `ui-ctl` log line is truncated** —
it carries 1193 controls and only 44 fit, so the button was never in the log.

---

## 4. Still open / not started (in priority order)

### 4.1 P0 — Verify the crash fixes on the console (needs the user)

**The console has been sent five logs so far and every one of them found a
crash** (§2.1–§2.6). The newest build has all five fixes in it; it has been
run on the emulator (q132, gate 39/39) but **not on the console**.

**Install the newest NRO and boot.** The build output is
`native-switch/rr3-native-rehost.nro`; the hash of the build that went to the
emulator card was `249c9f02…`. **Keep the `.elf` of every NRO you send to the
console** — the crash reports only resolve against the exact build.

Please send back:

1. `sdmc:/switch/rr3-native-rehost.guest-view.log`
2. **`sdmc:/switch/rr3-native-rehost.crash-early.txt`** — the early-crash
   register dump (it is what found crash #5)
3. the **newest** `sdmc:/atmosphere/crash_reports/*.log`, if one was written
4. what was on screen and roughly after how many seconds it closed
5. how it was launched (Album applet / holding R on a game / a forwarder) —
   that decides the heap size

**How to read the result**

| Last log line | Meaning |
|---|---|
| `cc-worker-dead` | the guest had already released the worker; the delivery was dropped instead of faulting — the fix worked |
| `cc-srvtime: header delivered, completed with status 200` | the /time answer landed; the guest is past the 19:45 crash |
| `view-egl-surface-ok` → `context-ok` → `current-ok` | the EGL sequence is fine (it has been fine since crash #4) |
| anything else | the `crash-early.txt` dump plus `addr2line` against the matching `.elf` names the function directly |

**The recipe that works** (it is how crash #5 was located):

```bash
B=.tools/devkitPro/msys64/opt/devkitpro/devkitA64/bin
# base = handler - (nm address of __libnx_exception_handler); offset = pc - base
$B/aarch64-none-elf-nm -C native-switch/rr3-native-rehost.elf | grep " __libnx_exception_handler"
$B/aarch64-none-elf-addr2line -f -C -i -e native-switch/rr3-native-rehost.elf 0x<offset>
```

> The earlier console logs ended at
> `phase=guest-frame-selftest verdict=SELFTEST_PASS`, and `teardown-repair.log`
> ended at `verdict=TEARDOWN_REPAIR_PASS` — the startup self-tests themselves
> passed every time. The crashes were always **after** the self-tests.

### 4.2 P1 — The splash's "Version 1" is clipped off the right edge

**Re-characterised on 2026-11-03 — the full chain is in §2.7.** The line that
overflows is **guest text**, not artwork:

```
"Player ID: CCDI123456789 / CCRR123456789"   (40 characters, Conduit Bold, size 140)
```

⇒ the port measures it at 2421 px (ground truth 2433.6 px — the port is right),
the engine puts its left edge at 257, and the 1280-px screen cuts it at the
right edge with ~1398 px of the line off-screen.

The **"Version 1" wordmark itself** is still not in any `text=` line and not in
the port's source, so *that* string does come out of the splash artwork — but
the artwork is not the cause of the overflow: the neighbouring guest text
overflows in exactly the same way, and it is the guest text that was measured.

**What was fixed**: the port's `renderToTexture()` did not scale the glyphs by
the guest's scale factor, so text drawn through that path came out 1/f too big
(§2.7). That path had **no log line at all**, which is why this took a whole
chain of measurements to find.

**What is still open**: whether the splash line's *size request* (140, the
largest entry of the guest's fixed table 14/28/42/56/70/84/98/140) is what the
game intends on a 1280-px screen. The guest asks for the same string at
**size 42** as well (719 px — that one fits), and the engine's own layout put
the large one where a ~766-px line would go. The next reading is the
`glyph-rtt` log line and the `text-quad` geometry of the two variants; if the
engine still draws the large one after the `renderToTexture` fix, the
remaining lever is the **view size** (`view=WxH` in `rr3-screen.txt`), which
also moves the guest's layout and therefore the touch script's pixel
coordinates.

### 4.3 P2 — The first 10 seconds are black

**Largely fixed on 2026-11-03 (q134) — see the numbers below.** What was left is
the port's own Chinese loading screen, which is off (`nofb=1`) because it fights
the guest's EGL surface for the same NWindow. Enabling it requires the
framebuffer and the guest's surface to stop fighting — **separate piece of
work**.

**The guest's own asset-list rewrite was the black screen's real cost.**
MEASURED on Eden (q132):

```
assetlist-rewrite-all-begin  lists=1235 overrides=8649
assetlist-rewrite-all        lists_touched=386 lists_written=386
  entries_rewritten=2992 bytes=2610656 ms=6628
```

386 of 1235 lists are rewritten, 2.6 MB is written, and the write is **a
temporary file, a remove and a rename per list** — three filesystem metadata
operations on a slow card. On the console that is 33..41 s of black screen.

**Fixed (q134): the pass now compares the content it would write with the
content the card already has, and writes nothing when they are the same.** The
content is deterministic, so after the first boot it always is. Measured on
Eden, same card, same build otherwise:

```
assetlist-rewrite-all  lists_touched=386 lists_written=0 lists_unchanged=386
  bytes=0 bytes_skipped=2610656 ms=2843
  read_ms=564 rewrite_ms=5 write_ms=0
```

⇒ **6,628 ms → 2,843 ms on Eden (2.33x), and 2,610,656 bytes are no longer
written.** The three costs are now separated in the log for the first time:
**reading the 1235 source lists is 564 ms, rewriting them is 5 ms, and the
writing was the other ~6,000 ms.** On the console, where the write is 5..6x
slower than Eden's, that is the bulk of the 33..41 s.

**What is left**: the pass still reads 1235 lists and 386 cached files on every
boot (~2.8 s on Eden). The GUI base list already has a cache key for exactly
this (`assetlist-rewrite-gui … cached=1 key=…`); the same key could cover the
whole pass and skip it entirely on an unchanged card.

### 4.4 P3 — Pure pursuit cannot hold the line (lap count −1)

**Re-characterised twice — §2.9 (layer 1) and §2.10 (layer 2, found in a real
race). It is NOT a gain to tune.**

The symptom is that the car drives a few per cent of a lap and then stops; the
guest's own lap counter says `-1` because the car never crosses the line.

| Layer | Defect | Status |
|---|---|---|
| 1 | the look-ahead point was **behind the car** (up to 210 of 210 samples) | fixed — the direction follows the car's heading |
| 1 | the line fit could pick the **mirror** of the centre line (the residual cannot tell a curve from its reflection) | fixed — the fit now applies a handedness test, with a fallback to the unfiltered winner |
| 2 | the side of the line the target was taken from was **re-decided every frame** from the car's instantaneous heading | fixed — the direction is **locked for the race** and decided once from a windowed motion vector |

**MEASURED in the first real race (q142, 4,200 frames of race):** the car
reached **5.08 %** of a lap and stopped; the pursuit error swung between
**−3.05 and +3.03 rad** in 63 of 141 samples; the lateral residual was
**≤ 2.00 m** and the target was **16–43 m** away. That combination is only
possible if the car's direction of travel had reversed — and the port was
making it reverse (§2.10).

**⇒ The measurement that decides it is running**: `perr` should stay inside
±1 rad for the whole race instead of swinging to ±3, and the distance should
pass the 5.08 % it stopped at.

**MEASURED (q143, the next round):** the look-ahead point stopped jumping to
the other side of the car — **45 % → 13 %** of the samples had the target
behind it — and the car drove **70 % further** (5.08 % → 8.65 % of a lap).
**⇒ The pursuit is no longer the bottleneck.**

**⇒ THE BOTTLENECK WAS A STUCK CAR, and it is fixed (q145).** MEASURED (q143):
the car reached its best at frame ~1,830 and then stopped for good. From frame
~2,700 to the end of the round the guest's own **spline node index stood at
319** and the distance moved by **10 units per 30 frames** (~1.3 mm a frame),
with the throttle on and the pursuit's error small and steady. The car was
**pinned against a barrier**, and the recovery — which fires 4 times in the
whole round — did not free it, because the stall test does not recover a car
that is **on the racing line** (the rule from q117: "a car on the line that
is not advancing is being HELD or it is CRAWLING"), and the pinned car's
lateral residual was 2.07 m — inside the 3 m exemption.

**FIXED (q145)**: the discriminator is the car's own **position**, not the
progress meter. MEASURED: the race grew from 4,770 to **5,760 frames**, the
distance from 8.65 % to **10.35 %** of a lap, and the recoveries from 4 to
**10** — the car gets free and drives on instead of flat-lining.

⇒ **The lap counter is still −1, and now the reason is clear**: the car
spends **41.5 % of the race in a recovery** (the brake held in 80 of 193
samples; the recovery fired 10 times). At the rate it makes
progress when it is not stuck (178 units a frame) a lap is ~10,000 frames; at
the rate it actually manages, ~56,000. **⇒ The next lever is the quality of
the line following** — the look-ahead distance (20 m), the steering gain, and
how much of the track the car can hold. The car is on the line to within 2 m
and still hits the barriers.

⇒ The **recovery needs an EXIT**, not a reference-point reset — resetting
makes the car spin on the spot (already noted, and now the blocking item).

Known directions (**none done yet**):

* `steergain` / look-ahead point: steering gain and the aim point need to be
  tuned per corner;

### 4.5 P4 — Deliverables and commit (**rebuilt on 2026-11-04**)

| Artefact | Size | Notes |
|---|---|---|
| `dist/rr3-switch-20261104/` | 190 files / 10.67 GiB | the card tree is **185 files / 9.58 GiB** |
| `dist/rr3-switch-app-20261104.zip` | 45.4 MiB | app only, 18 entries |
| `dist/rr3-switch-20261104.zip` | 7.89 GiB | 237 entries = 190 files + 46 directories |
| `dist/parts-20261104/*.001…004` | 2 + 2 + 2 + 1.89 GiB | concatenated = the archive **byte for byte** |

* **The NRO in the package, in the app zip and on the emulator card are all
  the same file** — `5515158395016D65…` — and `.build/rebuild-deliverables.ps1`
  PROVES it (it hashes all three and refuses to finish if they differ). The
  build is **byte-reproducible**: the gate rebuilds it from the same tree and
  gets the same sha256.
* **The parts join to the archive byte for byte**, sha256
  `a6cb20b470ba70c50b3f17777f3ff2986b00339f1732bed9419629816c547ece` for
  both the joined parts and the archive.
* **THE SHIPPING INPUTS ARE CHECKED, NOT ASSUMED.** The rebuild refuses to
  finish unless the package's `rr3-gamepad.txt` and `rr3-pad.txt` are
  byte-for-byte the repository's copies, the gamepad file says
  `autopilot = 0` — **the player owns the car** — and the pad file has **no
  event lines**, because a shipped script that holds the accelerator takes the
  throttle away from the player.
* **THE PACKAGER USED TO REVERSE ITS OWN RULE.** Its comment said the
  control scripts come from the repository, but its ORDER copied the card's
  copies over them — and the card carried the *experiment* pad script, so
  the release tree would have shipped a script that holds the throttle. The
  order is fixed (the repository's copies go last) and the check above stops
  it from coming back.
* **THE JOIN CHECK WAS PROVING THE WRONG DELIVERABLE, and that is fixed.**
  The packers took a `-Stamp` argument and `join_check.py` did not: its paths
  had 20261103 written into them, so the 20261104 rebuild proved that the OLD
  parts joined to the OLD archive and said nothing about the ones it had just
  written. The checker now takes the stamp, derives every path from it, and
  refuses to run when the parts or the archive it was asked about are not
  there.
* **The archive fingerprint caught a partial zip TWICE** in this round: the
  build was interrupted once (leaving a 2.69 GiB fragment of a 7.89 GiB
  archive) and the packer reported "existing zip: 2.69 GiB is STALE (package
  changed) -- rebuilding" and rebuilt it. The fingerprint is only written
  after a successful build, so an interrupted run never leaves one.
* The 20261103 deliverables were removed (they carried an older NRO).
* **`git commit` must use `-F <file>`** — `-m` and `-F` are mutually
  exclusive.
* **THE BUILD IS BYTE-REPRODUCIBLE.** MEASURED (2026-11-04): the gate rebuilds
  the NRO from the same tree and gets **the same sha256**
  (`5515158395016D65…`). That is what makes the four-way cross-proof
  (§4.5) meaningful: the NRO in the package, in the app zip, on the card and
  in the build output are not four copies of "something like the build", they
  are the same file, and any rebuild from the same commit reproduces it.

---

## 5. Numbers at a glance

| Metric | Value | Source |
|---|---|---|
| Faces the guest names | 16 (14 + CJK + icon) | `rr3-font-report.json` + log |
| Symbol-font requests / run | **18, every one a single character** | q123b log |
| Text draws / run (zero substitutions) | 2,812 | q123b log |
| Icon size error with the substitution | 2.3–5.8× too small | `font_truth.py` / `icon_change.py` |
| Two files named `r3_symbols.ttf` | 125,764 B (right) / 72,468 B (wrong) | sha256 `86AA0683…` / `1EE7223E…` |
| `sscanf` calls / run | 1,069 (1,005 of them with a truncated input) | q124 log |
| q123 (hang) | 4 frames / log stops at 1.62 MB | `20261103-011044-…q123-icon-font` |
| q123b (control, clean) | 3,000 frames / 305 s / 202,361 lines | `20261103-011940-…q123b-iconfont-off` |
| q124 (after the fix, clean) | 3,120 frames / 305 s / 35 MB | `20261103-012508-…q124-iconfont-scanfbounded` |
| Files on the card | **186** (was 10,546) | `make-package-v2.ps1` |
| Size on the card | **9.58 GiB** (was 11.21 GiB) | same |
| Files actually read in a full run | 133 (.depot 92 + cache 30 + doc 10 + .nomedia 1) | read-chain measurement |
| Splash line: guest text | `"Player ID: CCDI123456789 / CCRR123456789"`, 40 chars | q133 log |
| … its size request | 140 (the largest entry of the fixed table 14/28/42/56/70/84/98/140) | q133 log |
| … the port's measurement | 2421 px | q133 log |
| … the font's ground truth | 2433.6 px (17,383 units at upem 1000) | `tools/conduit_width_truth.py` |
| … on screen | ink x 257..1279, right edge touched ⇒ ~1398 px off-screen | `tools/splash_measure.py` |
| … what the engine thought | ~766 px (it centred that width) | from the ink's left edge |
| The engine's own matrix | 0..1021 px of vertices → 1557 px on screen (−138..1419) | `text-quad` probe |
| Asset-list rewrite, before | 386 of 1235 lists, 2,610,656 B written, 6,628 ms | q132 log |
| Asset-list rewrite, after | **0 written, 386 unchanged, 2,843 ms** (read 564 / rewrite 5 / write 0) | q134 log |
| `renderToTexture` calls by the guest | **0** (in a 23.5 MB log) | q133 log |
| One lap | **1,806,336** spline units (`lapd=`, identical in 3 rounds) | q8 / q10 / q11 |
| The car's best distance ever | 208,864 units = **11.6 % of a lap** | q11 |
| Rounds that ever completed a lap | **0 of 153** | `tools/lap_census.py` |
| The guest's own lap reading | `laps=-1` with `ok=1 lapsWhy=none` | q11 race-metrics |
| The pursuit's target BEHIND the car | up to **210 of 210** samples | `tools/pursuit_convergence.py` |
| The fitted line's variant changing mid-run | 6 different values in one round | q121 |
| The guest's load finishes at | **frame 83** (⇒ the black screen is 83 frames) | `tools/load_split.py` |
| The menu phase's frame rate | **3–4.5 fps** — and that is the MENU phase, not the game (§2.12) | q135 / q138 / q140 / q141 |
| The race phase's frame rate | **11.44 fps** (4,680 frames / 409 s) | q142 |
| CPU cores, every healthy round | **1.39–1.56** — flat, so the frame-rate spread is WAITING | q132..q142 |
| The touch script's race-start tap | **withdrawn** — the port's own red-glove follower presses the race button at (1105, 695) | q142 `ui-press` |
| A race, end to end | **4,200 frames** of race, 141 pursuit samples | q142 |
| … how far the car got | 91,810 units = **5.08 %** of a lap | q142 `tools/race_progress.py` |
| … the pursuit error over that race | **−3.05 … +3.03 rad**, 63 of 141 samples worse than 1 rad | q142 `tools/race_progress.py` |
| … the lateral residual | ≤ **2.00 m** (the car WAS on the line) | q142 |
| … progress lost to the loop | **6,581 units** between frames 2010 and 2040 — the car drove backwards | q142 |
| The guest-view log, old build | 45.2 MB / **205,983 lines** / 409 s = 504 lines a second | q142 |
| … of which repetitive | `glyph-face` 33,643 + `map-app` 31,315 = **31 %** | q142 |
| … `glyph-face` dedup table | **8** slots for 7 faces × dozens of sizes ⇒ thrashing | q142 |
| The guest-view log, after the dedup fix | 34.1 MB / **165,107 lines** at the same wall clock (**−20 %**) | q143 |
| The census A/B (the switch that had never been run) | frames **+1.3 %** (noise), CPU **+9.0 %**, log **+5.2 %** ⇒ **not** the frame-rate lever | q143 vs q144 |
| The menu phase's frame rate | **4.8 fps** (the race phase does 15–17 fps) | q143 |
| The PNG capture in the menu phase | **none** (`pngfrom=1300`, the race starts at frame 630) ⇒ the PNG capture is excluded again, cleanly | q143 |
| The pursuit fix, measured | target behind the car **45 % → 13 %**, distance **5.08 % → 8.65 %** of a lap | q142 → q143 |
| The pinned-car fix, measured | race frames **4,770 → 5,760**, distance **8.65 % → 10.35 %**, recoveries **4 → 10** | q143 → q145 |
| The car's best distance ever | **186,921 units = 10.35 %** of a lap | q145 |
| The race button's rectangle | `(994,667,187,53)` — the guest scissors it every frame | q138 |
| The first round where the guest's RaceMetrics was found | q138 frame 510: `gm_ok=1 metrics=1 vptr_ok=1 thr=1` | q138 |
| Rounds that stopped early because a tap landed in the race | q139 (600 frames) vs q138 2760 / q140 2640 | q138–q140 |
| The port's own glyph-texture lookup | was a linear scan of up to 4096 entries **per draw** (~90,000 draws a frame) | `gfx_text_probe.cpp` |

---

## 6. Reproducing this

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

# Build (NEVER pipe or redirect: the stderr "find: romfs" note aborts the script)
& 'E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1'

# Gate (39 tests / symbol resolution)
& 'E:\AI\RealRacing3-switch\.build\gate.ps1'

# Deploy NRO + romfs to the emulator card
& 'E:\AI\RealRacing3-switch\.build\deploy-card-romfs.ps1'

# Run
& 'E:\AI\RealRacing3-switch\tools\Test-OnEden.ps1' `
   -EmulatorRoot 'E:\Eden-rr3' -EmulatorExe eden-rr3.exe -EmulatorName eden-rr3 `
   -Seconds 300 -Tag q125-xxx

# Font contract test
python 'E:\AI\RealRacing3-switch\native-switch\tests\test_font_faces.py'

# The three measuring sticks (all read-only)
python 'E:\AI\RealRacing3-switch\.build\font_truth.py'      # metrics of both faces
python 'E:\AI\RealRacing3-switch\.build\face_pairing.py'    # pair faces by request boundary
python 'E:\AI\RealRacing3-switch\.build\nro_sym.py' 0x5ea1c0 # host PC offset -> symbol
```

**Switches** (`sdmc:/switch/rr3-gfx.conf`)

| Key | Default | Effect |
|---|---|---|
| `iconfont` | 1 | 0 = draw the icon font's characters with a text face (old behaviour, for A/B) |
| `glctxearly` | 0 | 1 = restore "build an EGL context before the guest's early init" |
| `nofb` | 1 | the port's own Chinese loading screen (off, because it fights the guest's surface for the NWindow) |

---

## 7. Handoff notes for whoever picks this up

* **Never** pipe or redirect `tools\Build-NativeSwitch.ps1` or `.build\gate.ps1`.
  The stderr note `find: 'romfs': No such file or directory` is expected and is
  treated as an error when redirected.
* `git commit` must use `-F <file>`; `-m` and `-F` are mutually exclusive.
* EA-owned material (the guest binaries, decompiled Java, UI assets, game text,
  FMOD, the three data packs, the romfs volumes, the decrypted plist, generated
  index JSON) **must never enter the repository**. `dist/` and `.build/` are
  gitignored; `native-switch/romfs/` and `analysis/rr3-ui-source/` are
  gitignored too.
* Line endings: new files are LF-only. `native-switch/source/symbol_resolver.cpp`
  and `guest_view_probe.cpp` are CRLF (patch them with a Python script using
  `newline=''`, not with a line-oriented editor); `race_state_probe.cpp`,
  `race_metrics_probe.cpp` and `glyph_vector.cpp` are LF.
* One Eden run at a time — check
  `Get-Process -Name eden,eden-cli,eden-rr3,eden-rr3-cli` first.
* Deletions on Windows: `Remove-Item` (recursive or single file) and `rm` with a
  variable are blocked by policy. Use `rm -- "<absolute literal path>"` (it goes
  through mavis-trash and is recoverable).
* When a conclusion is about the guest, the log field it rests on must be a value
  **read out of the guest**, not inferred.
* A hang and a regression look identical from the inside ⇒ **re-run the same
  binary** to tell them apart before changing code.
---

## 8. The 2026-11-09 console freeze: the port's log stopped mid-frame

### What happened

The console froze / black-screened with the v0.22.5 build and had to be
power-cycled.  The port stopped writing its log entirely.

### CONFIRMED from the device logs

* **The guest started.**  All four DSOs loaded, 181 constructors ran, and the
  guest logged `VIEW CHANGED 1280x720`.  So not a loader, JNI or DSO problem.
* **The framebuffers are FINE.**  Seven were created and every one reported
  `status=0x8cd5` -- that is `GL_FRAMEBUFFER_COMPLETE`, not an error.
  `guest_view_trace.cpp:1125` calls `glCheckFramebufferStatus(GL_FRAMEBUFFER)`.
  I first read `0x8cd5` as `EGL_BAD_MATCH` and the code corrected me: the FBO
  path was never the suspect.
* **The texture sizes are normal.**  The `ordinal` field packs
  `(width << 16) | height`: 1280x720 and 640x360.
* **NOT ONE FRAME WAS RENDERED.**  The log never contains `view-render-enter`,
  `view-gl-finish-enter`, `view-swap-enter`, `view-render-ret` or
  `guest-frame-census`.  The host called the guest's `onViewChangedJNI`
  (`guest_view_probe.cpp:2902`) and never got back.
* **The freeze happened 33.5 minutes after the console booted, and the log
  covers the last 2.8 minutes of it.**  `boot_ms` is
  `armGetSystemTick()*1000/freq`, a wall-clock stamp in MILLISECONDS since
  boot; the values in the log run 1842279 -> 2009900, i.e. 1842.3 s ->
  2009.9 s, a window of 167.6 s.  So the console was powered on, the game
  was launched 30.7 minutes later, and the port froze 33.5 minutes after the
  boot, 2.8 minutes into its own life.
  **The two numbers are easy to confuse, and the confusion points at a
  completely different bug**: reading the millisecond value as seconds says
  "the port idled for half an hour and then froze", which would point at a
  timeout or a stalled asset; reading it as minutes says "the port worked
  for 168 s and then froze", which is what the evidence supports.
  The reaper's own `waited_ns` reaches 93 s, longer than the 168 s window,
  because that counter accumulates across waits and is reported per slice --
  it measures the reaper's cumulative patience, not the process age.
* **The port died parked in a condition-variable wait.**  The progress file's
  last line is `phase=view-sync-blocked-cond-wait ordinal=53`, which is
  `guestViewTraceSyncBlocked("cond-wait", 53)` from `guestPthreadCondWait`.
* **THE INSTRUMENT THAT COULD HAVE NAMED THE OBJECT WAS OFF.**  The card
  carried `sdmc:/switch/rr3-census.conf` containing `census=off`, left over
  from an emulator round.  `censusArm()` returns immediately in that case, so
  the log holds no `census-arm` line and no `census-wait` line -- and
  `census-wait` is exactly the line that prints
  `obj/waits/signals/deficit/wait_caller`.

### The analysis

The guest's `onViewChangedJNI` parks a worker thread on a condition variable
that nothing ever signals, and the host waits for it forever.  The port could
not say WHICH condition variable, because the census that names it was
switched off on the card, and the census is periodic anyway, so a freeze
before the first sweep leaves nothing even when it is on.

This is the project's own lesson arriving from the other direction: **a path
without an instrument is a path that cannot be explained.**

### The fix (all three in `native-switch/source/pthread_bridge.cpp`)

1. **The waiter writes its own ledger.**  `condWaitLedgerOf()` reads the
   condvar's own counters (waits, signals, deficit, last waiter) under the
   registry lock, and `guestPthreadCondWait` writes them out the first time it
   times out, every 30 s after that, and once at the budget.  It needs no
   census and cannot be switched off, so a dead wait is self-describing.
2. **The wait is bounded.**  After 120 s it gives up -- and it gives up by
   returning **0**, the same answer a spurious wake-up gives, NOT
   `ETIMEDOUT`.  POSIX says `pthread_cond_wait` returns 0 and nothing else;
   `ETIMEDOUT` belongs to `pthread_cond_timedwait`, libc++'s
   `std::condition_variable::wait` throws `std::system_error` on a non-zero
   return, and a guest that checks the return code can assert -- so returning
   an error from a wait that merely ran out of patience would turn a slow
   guest into a CRASHING one.  The guest's own `while (!ready) cond_wait()`
   loop re-evaluates its predicate and comes back for another round, which is
   what it does after any spurious wake-up.  An unbounded wait turned a guest
   bug into a silent wedge with no end and no answer.
3. **The signal is not lost.**  A host `pthread_cond_t` has no memory, so a
   signal that arrives while nobody is blocked is gone.  The bridge counts
   the signals itself (`ObjectSlot::signalEpoch`) and the waiter checks the
   epoch before every wait: if the epoch moved, the signal was for THIS
   waiter and the wait returns immediately instead of blocking again.  See
   the q150 section below for the measurement that made this necessary.
4. **A census that is off says so.**  `censusArm()` now writes one line:
   `census-arm ... SKIPPED reason=census-disabled`.  "No census line" and
   "the census broke" look identical in a log; now they do not.  The line is
   written ONCE, because `censusArm()` is called from `resolveHostSymbol` for
   every symbol the guest linker resolves.

### The card was dirty (measured)

`E:\Eden-rr3\user\sdmc\switch` carried, from the emulator rounds:

```
rr3-census.conf                        census=off
rr3-probe.conf                         pngfrom=1300 | pngto=5200 | pngevery=20 | pngmax=250
rr3-native-rehost.crash-*.txt          143 files
rr3-native-rehost.guest-view.log       40,159,486 bytes
rr3-native-rehost.teardown-repair.log  5,488,499 bytes
```

45.6 MB of debris plus the census kill switch.  The release package ships none
of it, so it was never going to be overwritten -- it SURVIVED, and the console
inherited every file the emulator rounds had left behind.

`.build/clean-card-residue.ps1` reports and removes exactly this set and puts
`census=on` back.  **Run it before a console round.**

### q150: the instrument proved itself, and it named the shape of the bug

Round q150 (Eden, `census=off` on the card, exactly the console's state) ran
the new build and produced the first line that ever named the object the
guest was waiting on:

```
phase=cond-wait-ledger ordinal=120000 obj=0x210fc01900 known=1 waited_ms=120000
    waits=1 signals=1 deficit_at_entry=0 wait_caller=0x2109028a30
    budget_ms=120000 outcome=GIVING-UP
phase=cond-wait-abandoned ordinal=120000 reason=no-signal-within-budget
```

**`waits=1 signals=1` IS THE WHOLE STORY.**  The condition variable WAS
signalled -- once -- and the waiter still ran out the clock.  So the freeze
is not "nobody signalled it": it is **"the signal did not reach the waiter"**.

That is a well-known shape: a host `pthread_cond_t` has NO memory.  A signal
that arrives while nobody is blocked is gone.  The guest's producer
signalled, the waiter was not yet inside the wait (or had just come out of
it), and the notification was dropped on the floor.  The guest then sat in
the wait forever with nothing else to wake it.

⇒ The bridge now counts the signals itself (`ObjectSlot::signalEpoch`), and
the waiter checks the epoch before every wait: if the epoch moved since the
last look, the signal was for THIS waiter and the wait returns immediately
instead of blocking again.  The bridge's condvar now behaves like a
condition variable with a predicate, which is what the guest's
`while (!ready) cond_wait(&c, &m);` loop assumes it is talking to.

**AND THE GIVE-UP RETURNS 0, NOT ETIMEDOUT.**  POSIX says `pthread_cond_wait`
returns 0 and nothing else; `ETIMEDOUT` belongs to `pthread_cond_timedwait`.
libc++'s `std::condition_variable::wait` throws `std::system_error` on a
non-zero return, and a guest that checks the return code can assert.  So
returning an error from a wait that merely ran out of patience would turn a
slow guest into a CRASHING one.  The wait gives up by returning 0 -- the
same answer a spurious wake-up gives -- and the guest's own predicate loop
re-evaluates and comes back for another round.

(That correction came from the Antigravity CLI review of this change, which
caught the `ETIMEDOUT` return as a real defect in the first draft.  The
review also confirmed that the ledger read must happen BEFORE the first
wait, because `pthread_cond_timedwait` re-acquires the guest's host mutex on
return, and taking `gRegistryLock` while holding it walks into the AB-BA
shape that already exists between those two locks in this file.)

### q151: the epoch works, and it was also wrong the first time

Round q151 (Eden, `census=off`) ran the epoch build and produced 1,048,302
`cond-wait-ledger` lines -- every one of them `outcome=SIGNAL-SEEN` -- and
the guest rendered 1481 frames instead of the 532 q150 reached before it
stopped.

**The epoch is what made the difference**, and the numbers say why: on ONE
condition variable (`obj=0x210fa33d20`), `waits` climbed to 1,048,302 while
`signals` stayed at **1**.  So the guest was waiting on a condition variable
that had been signalled exactly once, ever -- and every one of those waits
was answered immediately instead of blocking.

**AND THAT WAS A BUG IN MY OWN FIRST VERSION.**  The epoch comparison was

```c
if (ledger.signalEpoch != 0 && ledger.signalEpoch != lastSeenEpoch) { ... }
```

with `lastSeenEpoch` initialised to 0 and never set to the epoch observed on
entry.  So on the very first pass `signalEpoch` was 1 (one historical signal
from before this guest ever waited) and `lastSeenEpoch` was 0, and the wait
concluded "a signal arrived" -- WITHOUT EVER WAITING.  The guest was not
unblocked because the lost signal was fixed; it was unblocked because the
wait stopped waiting at all.

The fix is the BASELINE: the epoch observed BEFORE the first wait is what the
waiter compares against, so a signal that was already delivered before the
waiter arrived is not mistaken for a fresh one.  A signal that arrives while
the waiter is blocked wakes it directly (the host condvar does that), and a
signal that arrives in the gap between one wait and the next is caught by the
epoch on the next pass.

**THE OTHER MEASUREMENT FROM q151 IS ABOUT THE LOG, NOT THE DEADLOCK.**  One
line per wait grew the guest log to 186 MB in four minutes -- 864,728 waits,
864,728 lines -- and the log became a second way for the card to fill up.
The ledger therefore has a TOTAL budget for the process (64 lines), not a
budget per wait: the first N are written, the rest are counted silently, and
the moment the budget is spent one line says so.  A real wedge needs one
line; a busy-but-healthy waiter needs none.

### q152: the fix holds, measured

Round q152 (Eden, `census=off` on the card) ran the corrected build -- the
epoch baseline taken on entry, and the ledger's total line budget.  The
three rounds together are the measurement:

| | q150 | q151 | q152 |
|---|---|---|---|
| frames rendered | 532 (stopped) | 1481 | **3762** |
| screenshots captured | 5 | - | **84** |
| ledger lines written | 5 | 1,048,302 | **12** |
| `outcome=SIGNAL-SEEN` | 0 | 1,048,302 | **0** |
| `outcome=GIVING-UP` | 1 | 0 | **2** |
| guest log size | 15.9 MB | 186 MB | **24 MB** |

**THE LEDGER NOW READS CORRECTLY.**  Every line says the same thing:

```
cond-wait-ledger ... obj=0x211002f780 known=1 waited_ms=1000  waits=1 signals=1
    deficit_at_entry=0 epoch=1 wait_caller=0x210940aa30 outcome=still-waiting
cond-wait-ledger ... waited_ms=30000 waits=1 signals=1 ... outcome=still-waiting
cond-wait-ledger ... waited_ms=60000 waits=1 signals=1 ... outcome=still-waiting
cond-wait-ledger ... waited_ms=90000 waits=1 signals=1 ... outcome=still-waiting
cond-wait-ledger ... waited_ms=120000 waits=1 signals=1 ... outcome=GIVING-UP
cond-wait-abandoned ... reason=no-signal-within-budget returned=0_like_a_spurious_wakeup
```

`waits` climbs, `signals` stays at 1, `epoch` stays at 1.  So the condition
variable was signalled exactly ONCE, ever, and that signal was already old
when the waiter arrived -- which is why the epoch baseline does not mistake
it for a fresh one.  The wait REALLY blocked (waited_ms 1 -> 30 -> 60 -> 90
-> 120), the budget expired, the port gave up and returned 0, and the guest
kept going: **3762 frames**.

**THE SIGNAL LOSS IS FIXED, AND THE FALSE-SIGNAL IS GONE.**  q151 reported
1,048,302 `SIGNAL-SEEN` because the baseline was 0 and the epoch was 1, so
every wait concluded "a signal arrived" without ever waiting.  q152 reports
ZERO, and the wait demonstrably blocked for 120 s.  The difference between
"the wait returned immediately" and "the wait really waited" is exactly the
`waited_ms` field, which is why that field is in the line.

**AND THE LEDGER BUDGET HOLDS.**  12 lines for a run that did 3 waits of up
to 120 s each, against q151's 1,048,302.  The guest log is 24 MB instead of
186 MB.

### TWO CORRECTIONS TO WHAT WAS WRITTEN BEFORE

**1. The freeze was 33.5 MINUTES after the console booted, not 33 seconds.**
`boot_ms` is `armGetSystemTick()*1000/freq` -- MILLISECONDS since boot.  The
values in the log run 1842279 -> 2009900, i.e. 1842.3 s -> 2009.9 s.  So:

* the console was powered on,
* the game was launched 30.7 minutes later,
* the port froze 33.5 minutes after the boot, 2.8 minutes into its own life.

The millisecond value read as seconds says "the port idled for half an hour
and then froze", which would point at a timeout or a stalled asset.  Read as
minutes it says "the port worked for 168 s and then froze", which is what the
evidence supports.  The two readings point at completely different bugs, so
the unit was checked by arithmetic rather than by reading.

**2. The uncommitted "clean loader" work IS in the published NRO.**  The
shipped build came from the working tree, and the ELF carries both
`resolvePortAssetPath` and `selectGameLibrary` (verified by scanning the
symbol table of `native-switch/rr3-native-rehost.elf`).

It is INERT in the default build, and that is worth being precise about:
`resolvePortAssetPath` is a FALLBACK chain -- RomFS first, then the card --
and the default build's RomFS still holds the files, so the chain resolves
inside the RomFS exactly as before.  Only the `-CleanLoader` build (which
leaves the EA files out of the RomFS) takes the second branch.  So the
published binary behaves identically to the committed source; the difference
is that the binary contains more than the commit says it does, which is a
bookkeeping problem, not a behaviour one.

⇒ The clean-loader work is either finished and verified, or reverted.  It is
not left half in a release.

### The Antigravity CLI review of the no-context observer

The observer was reviewed adversarially by the Antigravity CLI
(`gemini-3.8-flash-high`), and it found three REAL defects in the first
version -- the kind that only a second pair of eyes catches:

1. **THE LOG PATH WAS NOT BOUNDED.**  The record array was capped at 32
   entries, but the code FELL THROUGH to the log write when the slot index
   went past the cap, so a busy thread could write a line per call anyway.
   That is the same failure as the 186 MB log, one layer down: the budget was
   on the records, not on the lines.  Fixed: the out-of-budget branch now
   returns, and the surplus is counted.
2. **THE TALLY LOST UPDATES.**  `store(recorded + 1)` after a relaxed LOAD is
   a read-modify-write, so two threads could both see the same value and one
   increment would vanish -- which would let the budget be overrun.  Fixed
   with `fetch_add`.
3. **RE-ENTRANCY.**  The observer writes to the card, and the card write runs
   under a non-recursive mutex; a GL call reached from inside that write
   would re-enter the observer and deadlock the thread.  Fixed with a
   thread-local guard.

It also extended the coverage from the 9 tracers the first version hooked
to **23**, adding the ones a worker thread is most likely to use --
`glCompressedTexImage2D` (texture decompression upload), `glDrawArrays`,
`glDrawElements`, `glGetString`, `glClear`, `glViewport`, `glScissor`,
`glUseProgram`, `glCheckFramebufferStatus` and the bind/gen family.

**WHAT IS STILL INVISIBLE, STATED PLAINLY.**  The observer hooks the
`guestViewTracedGl*` tracers.  The guest's OTHER GL imports -- about sixty
of them, bound with `RR3_BIND` in symbol_resolver.cpp, including
`glFinish`, `glBufferData`, `glUniform*` and `glEnable` -- reach the driver
directly and are NOT observed.  The dangerous ones (the draw calls, the
shader calls, the texture uploads) ARE observed, but the coverage is not
complete and the next device run could still show a context-less call on a
symbol this observer does not see.

The port still does not make the EGL context current on the guest's worker
threads.  If the device freeze turns out to be that, the fix is to give the
worker threads their own context or to serialise their GL work onto the
main thread -- a bigger change than an instrument.

### The clean loader hung, and the cause was a rule that lived in one file

The clean build is the one this project actually wants to publish: an NRO
that carries NO EA material at all, with the guest's own files found on the
card next to it.  It had never been RUN, and it hung.

MEASURED 2026-11-09, on Eden:

  * the clean build's RomFS holds three files -- the OFL bootstrap face,
    libc++ and the font licence -- and the NRO is 11,565,161 bytes instead
    of 53,694,961;
  * the guest's files WERE on the card, and they WERE found: the guest
    booted (`guest-boot: recovery=1 initActive=1 initDone=1 extData=1`), the
    view callbacks resolved, and the guest got as far as VIEW CHANGED;
  * and then the port stopped, with the log's last line
    `view-egl-config-ok` -- which is written at guest_view_probe.cpp:2594,
    immediately before `eglCreateWindowSurface` -- and 0 frames rendered.

THE CONTROL THAT PROVED IT WAS THE ROMFS AND NOT THE CODE: the same source,
rebuilt with the full RomFS, produced the NRO byte-for-byte identical to the
published one (d8d8e76a...) and ran normally through
view-egl-config-ok -> view-egl-surface-ok -> view-egl-context-ok ->
view-egl-current-ok and rendered 480 frames.  So the only variable was the
RomFS content.  A bisect then narrowed it: clean + the game's faces in the
RomFS rendered fine (480 frames, 21286 draws); clean without them hung.

THE CAUSE, IN TWO STEPS.

1. `nativeUiInit()` (native_ui.cpp) opened the faces STRAIGHT out of
   "romfs:/ui/ConduitITCPro-Regular.otf" and friends, with no card
   fallback.  In a clean RomFS those files are not there, so the open
   failed and `nativeUiInit()` returned false.  `glyph_vector.cpp` had the
   fallback chain; `native_ui.cpp` did not, because the two were written at
   different times.  **A rule that lives in one file is not a rule.**

2. A false `nativeUiInit()` sends main.cpp down its diagnostic path, which
   calls `consoleInit(nullptr)`.  libnx binds deko3d to
   `nwindowGetDefault()`, and `nativeUiExit()` closed the port's own
   framebuffer but NEVER called `consoleExit()`.  Mesa then tried to create
   an EGL surface on a window the console still owned, and blocked in the
   kernel's buffer queue -- which is why the hang looked like a GPU problem
   and was not one.

THE FIX, IN THREE PARTS.

  * Every RomFS path in native_ui.cpp now goes through
    `resolvePortAssetPath()`, so the port finds its files wherever they are.
  * A face that is not there is replaced by the OFL bootstrap face, which
    the clean build always carries (it is Noto CJK under the SIL OFL and
    this project owns it).  A missing game face can no longer take the
    whole port down the console path.
  * `main.cpp` now calls `consoleExit(nullptr)` before handing
    `nwindowGetDefault()` to Mesa, so the window is released whether or not
    the console was ever opened.

AND A BUG THE FIX ITSELF WOULD HAVE INTRODUCED: with the fallback, three of
the four face pointers can be the SAME FT_Face, and `nativeUiExit()` closed
each of them -- a double free, in a destructor path, which is exactly the
kind of crash that looks like "the console froze for no reason".  The exit
path now closes each distinct pointer once.

### STILL OPEN

* **Why did the console FREEZE HARD rather than just the app hang?**  The
  analysis explains a wedge, not a machine-level freeze.  The candidates
  being checked against the code: `glFinish()` at `guest_view_probe.cpp:3167`
  waiting on a stuck GPU queue; a guest thread spinning at a non-timesliced
  priority (0x2C is not time-sliced on Horizon); or a thread left PAUSED by
  the census sampler -- `svcSetThreadActivity` pauses guest threads, and a
  paused-but-never-resumed thread is a different failure from a parked one.
* **Which condition variable was it?**  The next build's `cond-wait-ledger`
  line answers this, and the answer is not in this archive.
* **What was the guest waiting for?**  The guest's own `onViewChangedJNI` is
  doing something that expects a worker thread to make progress.  That is
  guest behaviour, and it needs the ledger to identify it.

---

## 9. The pursuit's A/B measured nothing, and the look-ahead it was about to measure was wrong

Three separate faults, found on 2026-11-09, none of them in the pursuit's
own maths at first sight, and all three of them between the reader and the
number.

### 9.1 q163 did not measure the pursuit at all

The round the pursuit's first A/B was going to be read from produced this:

    phase=race-state ... metrics=0 ... rej=the-members-are-not-live
                               rejm=0x3f rejlapd=4294967295
    phase=race-state ... metrics=0 ... rej=not-found

`4294967295` as a uint32 is `-1.0f`, which is the guest's own "no lap yet"
value.  The port found the guest's `RuleSet_RaceMetrics` object and refused
it, because the object's lap distance had never become positive -- the car
had never driven.  The port's closed loop refuses to drive a not-live
metrics object, so the two conditions are circular:

    the car must move for the metrics to be live
    the port only drives when the metrics are live

⇒ The round measured NOTHING about the pursuit.  The old report printed
`progress: max=0.000` and stopped there, which reads like a result ("the
adaptive look-ahead is worthless") and is in fact a report of a round that
failed to start.  `pursuit-report.py` now answers three questions in this
order -- did the pursuit run at all, under which configuration, and then how
far did it get -- and exits non-zero with an explicit "THIS ROUND SAYS
NOTHING ABOUT THE PURSUIT" when the first answer is no.

The good rounds are the reference: q145 and q147 (2026-11-04) found the
object live at frame 540 and 570 respectively.

### 9.2 The harness could not run a different binary

An A/B needs the two halves to differ in ONE thing.  This one needed the two
halves to differ in the CONFIGURATION and to run the SAME binary, and the
harness could do neither:

  * `deploy-card-romfs.ps1` copies the CURRENT BUILD's NRO onto the card;
  * `Test-OnEden.ps1` copies the current build's NRO onto the card again,
    unless `-NoDeploy` is passed.

Both copies happen AFTER the experiment's own deploy.  So a round that put
an older NRO on the card ran the new NRO anyway, and the comparison was
silently worthless.  This was caught by reading the archive, not by the
harness: the archived `run-summary.txt` said the NRO was 53,694,961 bytes
when the round had been told to run a 53,690,865-byte one.

⇒ `run-pinned-round.ps1` grew a `-Nro` parameter that puts the chosen NRO on
the card AFTER both copies, passes `-NoDeploy` to `Test-OnEden.ps1`, and
then VERIFIES the card's sha256 against the chosen file and refuses to start
if they differ.  The A/B script resolves one NRO and hashes it once, and
every cell runs that file.

⇒ The general rule: **an experiment that cannot say which binary it ran is
not an experiment.**  The archive now carries the hash, the harness asserts
it, and the report says which configuration was in force.

### 9.3 The adaptive look-ahead returned the ANGLE, not the ARC

`curvatureLookAheadMetres()` in `native-switch/source/race_line.cpp` is
supposed to return the look-ahead distance.  The comment above it says what
that distance is:

    "the ARC LENGTH at which the accumulated heading change first exceeds the
     threshold is the corner's scale.  A straight never reaches the
     threshold, so the walk reaches the maximum look-ahead; a hairpin
     reaches it in a couple of metres."

The implementation walked the polyline and accumulated the heading change,
but it never accumulated the DISTANCE it walked.  It returned
`accumulated * 8.0` -- the ANGLE, scaled by a magic number.

The consequence is not a small error.  The angle at the point where the
threshold is crossed is always between 0.35 rad and roughly 0.5 rad, so
`accumulated * 8.0` is always between 2.8 and 4.0, and the clamp is
`[4.0, 20.0]`.  ⇒ **The old "adaptive" look-ahead was a constant 4 metres
everywhere.**  Measured on a synthetic track:

    straight, before a corner      4.00 m      (correct: 20.00 m)
    corner of radius 25 m          4.00 m      (correct: 10.00 m)
    corner of radius 50 m          4.00 m      (correct: 18.00 m)
    corner of radius 12 m          4.00 m      (correct:  5.99 m)

It was completely insensitive to the radius of the corner.  And it was
backwards where it mattered: the SHORTEST look-ahead was on the straights,
where the steering noise lives, and it did not shorten at all as a corner
came up -- which is the failure q147 measured (a long look-ahead into a
corner is a steering command tens of degrees wrong).

⇒ The law is now the arc length: the distance at which the road ahead has
turned by the threshold, clamped to `[4, 20]` metres.  On a constant-radius
corner of radius R that is `0.35 * R`, which is the closed form the comment
promised, and it is the distance that actually bounds the pursuit error.

⇒ `native-switch/tests/host/race_line_selftest.cpp` now pins the law with a
synthetic track of KNOWN shape: a straight, a corner of known radius, two
corners of different radius, and a straight that ends in a corner.  The
assertions are that a straight gets the maximum, that a corner gets
`0.35 * R` to within one sample of arc, that a tighter corner gets a
shorter look-ahead, and that the look-ahead shortens as a corner comes up.

  THE TEST WAS PROVED TO HAVE TEETH: the same self-test was compiled
  against the old law and FAILED five of the new assertions, with the
  numbers above.  A test that has never been seen to fail is not evidence.

### 9.4 The census is a variable too, and it was never controlled

The good rounds (2026-11-04) ran with the census ARMED; every 2026-11-09
round ran with `census=off` in `sdmc:/switch/rr3-census.conf`.  The census
is not only logging: it SAMPLES the guest by pausing its threads, which is
the part of the port that touches the guest every frame.  So "the code
changed" and "the census was off" were confounded, and the regression matrix
in `.build/run-regression-matrix.ps1` separates them.  All four cells, one
variable each, the same card and the same assets:

                   census ON                       census OFF
    Nov-4 NRO     m1   WORKS                      m3   FAILS
                   metrics live at frame 540       the guest never reaches the race
                   6960 frames in 406 s (17.1 fps) 5160 frames

    Nov-9 NRO     m2   FAILS                      q163 FAILS
                   the guest never reaches the race  metrics live NEVER
                   4920 frames                   2040 frames

**⇒ AND THEN THE 2x2 WAS FOUND TO BE CONFOUNDED, AND THE CONCLUSION CHANGED
AGAIN.**

Read the four cells as a table and one thing stands out: `m2`, `m3` and a
later round `v1` drew **EXACTLY 10,394 things in 400 s**.  Three different
builds and two different census settings, the same number, to the draw.
That is not a coincidence and it is not a race condition; it is the same
deterministic stop.

**⇒ THE GUEST PERSISTS ITS OWN STATE, AND THE ROUNDS RAN IN ORDER.**

`m1` was the FIRST round of the day and it was the only one that reached
the race.  It also ended a race, and the guest paid out a reward, and the
"you received 50 gold" popup went into the guest's SAVE DATA.  The popup
survives between rounds, so every round after `m1` started with the popup
already on screen and stopped there.

The archived frame is the proof, and it is one image:
`.build/eden-runs/20261109-155529-eden-rr3-m3-nov4-census-off/frames/frame-005200.png`
shows a modal panel with a gold coin, an empty text area and the guest's red
glove parked at its top right corner.  The guest's own log says what the
panel is, in the same round, in the same archive: `phase=jni-utf163 ...
text=<the bytes of "you received 50 gold">`.  (The mojibake in that line is
the TRACER's own decoding, not the game's: the frames render Chinese
correctly, so the port's text pipeline is fine and only the tracer's
UTF-16-to-text step is wrong.)

**⇒ WHY A HELD BUTTON CANNOT DISMISS IT.**  The autopilot pad script is a
state timeline, and its third line is:

    [at 600] lx=0 ly=0 rx=0 ry=0 lt=0 rt=0 b=0x001

which holds the guest's BTN_A down from frame 600 to the end of the round.
There is never a release, so the guest's UI never sees a BUTTON-DOWN
TRANSITION once the popup is up, and a modal dialog that waits for a press
waits forever.  (`m1` was unaffected because its popup appeared at the END
of its race, when nothing more was needed from the script.)

**⇒ SO THE 2x2'S CELLS WERE NOT COMPARABLE, and the FIRST TWO READINGS OF
IT WERE BOTH WRONG.**  The first reading blamed the November 9th code; the
third cell (m3) corrected that to "the census"; and the draw counts
corrected it again to "the guest's persisted state, and the order the
rounds ran in".  **A 2x2 in which the system's own persisted state is not
reset between the cells is not a 2x2.**  The harness now snapshots the
guest's save data and puts it back before every round
(`.build/guest-state.ps1 -Snapshot | -Restore`).

### 9.5 What the failing rounds say about the guest's condition variable

m2's own log names the object and says what happened to it:

    phase=cond-wait-ledger obj=0x2110251d00 known=1 waited_ms=120000
      waits=1 signals=0 deficit_at_entry=1 epoch=0
      wait_caller=0x2109648a30 budget_ms=120000 outcome=GIVING-UP
    phase=cond-wait-abandoned reason=no-signal-within-budget
      returned=0_like_a_spurious_wakeup
    ... and then waits=2, waits=3, waits=4, with signals STILL 0.

`signals=0` means the guest never signalled this object at all, and the
waiter was released only by the port's 120 s give-up, which returns 0 --
the same answer a spurious wake-up gives.  The guest's own `while (!ready)`
loop then re-entered the wait, forever.

Two faults in one function, and the November 9th build has both:

  1. **THE REGISTRY LOCK, ONCE PER SECOND.**  The November 9th version read
     the cond-var ledger at the top of every pass of the wait loop.  That
     read takes `gRegistryLock`, and it takes it WHILE THE GUEST'S OWN HOST
     MUTEX IS HELD.  The census sampler takes the same lock and holds it
     while it pauses and resumes EVERY guest thread (7,071 threads per
     sweep, seconds of wall clock).  So the guest's cond-wait thread
     stalled for whole seconds at a time while holding a mutex that other
     guest threads needed -- and the producer that was going to signal
     never ran, which is what `signals=0` says.  The November 4th version
     took the lock once per WAIT, on entry, and never again.  **The rule is
     now: one acquisition at entry, one on an actual wake-up, two at the
     report points.  Never one per second.**
  2. **A DROPPED SIGNAL.**  A host `pthread_cond_t` has no memory: a
     signal that arrives while nobody is blocked is gone, and the guest's
     `while (!ready) cond_wait()` cannot recover it, because the predicate
     was already checked.  The November 9th epoch was meant to catch that
     and could not: its baseline was taken at entry, so a signal that
     arrived between two one-second slices was counted by the bridge and
     never seen by the waiter.  **The bridge now keeps a PENDING count per
     condition variable: a signal that arrives with nobody waiting is
     remembered, and the next waiter consumes exactly one of them and
     returns without waiting at all.**

**⇒ AND THE CENSUS WAS COVERING FOR THE BUG.**  The census pauses and
resumes every guest thread every 500 ms, and that perturbation is enough to
close the lost-wakeup window most of the time -- so the guest ran "by
luck", at the price of 7,071 thread pauses per sweep.  Turning the census
off removed the accident and exposed the fault.  **A port must not depend
on a diagnostic instrument to accidentally fix its threading.**

### 9.6 The console freeze's cond-var, named

q163's new `cond-wait-ledger` line named the object the guest was waiting on,
which is the question the console freeze left open:

    phase=cond-wait-ledger obj=0x2110000660 known=1 waited_ms=120000
      waits=4 signals=1 deficit_at_entry=3 epoch=1
      wait_caller=0x2109423a30 budget_ms=120000 outcome=GIVING-UP
    phase=cond-wait-abandoned reason=no-signal-within-budget
      returned=0_like_a_spurious_wakeup

Four guest threads wait on ONE guest condition variable, it is signalled
exactly once, and the waiters are never released; the port gives up after
120 s and returns 0, the same answer a spurious wake-up gives, so the
guest's own predicate loop decides what to do next.  This is the shape of a
starved worker pool, and it is consistent with the race never starting.

**⇒ THIS IS THE SAME FAULT AS 9.5, IN THE OTHER DIRECTION.**  Here the
condvar was signalled ONCE for FOUR waiters; in m2 it was signalled ZERO
times.  Both are the same underlying thing: the host condvar has no
memory, so the bridge has to keep the signal itself, and until it does,
the guest's wake-ups are a matter of luck.

### 9.7 The pulse, and the first round that measured the pursuit

THE EXPERIMENT.  One variable: the pad script's confirm button is released
for a few frames every cycle instead of being held down for the whole
round.  Same build, same card, same census setting as the round that
failed.

    v3   the fixed build, census OFF, PULSE   metrics live at frame 1830
                                              6120 frames in 406 s
                                              start lights 7, race screen 10

**⇒ CONFIRMED: the held confirm button was what wedged the guest, and the
popup was what the confirm button could not dismiss.**

And v3 is the FIRST round that measured the pursuit with the new
instruments:

    lookahead (lam)   : {'adaptive': 205}     the configuration was in force
    la                : min 0.00  median 8.71  max 20.00
    progress          : last 7367
    raceline          : {'centre_spline': 145}

**⇒ THE ADAPTIVE LOOK-AHEAD IS MEASURING FOR THE FIRST TIME, AND IT IS
VARYING.**  The corrected law gives 20 m on a straight and shortens towards
the corner; the median of 8.71 m over the round is what that looks like.
The old law would have printed 4.00 m for every single sample.

### 9.8 The pursuit drove the car backwards, and the log now says why

v3's `dir=` and `mot=` fields (added in this same round, because a decision
that is made once and never revisited is invisible in a per-frame line
unless the line carries it) read:

    f=2010 .. 2580   dir=0   la=0.00   mot=0.000,0.000
    f=2610           dir=1   la=20.00  mot=0.807,-0.590
    f=2670           dir=1   la=20.00  mot=-1.000,0.015   total 58712 -> 51160
    f=2730           dir=1   la=4.00   steer=-1.000
    f=2760           dir=1   la=9.31   steer=1.000

Three things, and the third is the bug:

  1. **FOR 570 FRAMES THE PORT HAD NO TARGET AT ALL.**  `la=0.00` means
     `raceLineTargetAdaptive()` returned false: the car was more than 25 m
     from the fitted centre line, so the port was steering a car it could
     not see the line for.  `dir=0` for the same period: the direction
     lock is decided inside that same call, so it had not been decided
     either.  The lock was therefore decided at frame 2610 -- 780 frames
     after the race started.
  2. **THE CAR TURNED ROUND AT FRAME 2670**, and `total` (the car's signed
     position along the track) fell from 58712 to 51160 while `snode` went
     261 -> 259.
  3. **THE FLIP RECOVERY DID NOT CORRECT THE LOCK.**  The recovery at
     `race_state_probe.cpp:2428` flips `g_drive.sign` -- the steering sign
     -- when the rate stays negative.  It did not touch
     `gRaceLine.direction`, which is what places the look-ahead point.  So
     the target stayed on the side the car had LEFT, that is behind it, and
     the controller chased a point behind the car at full lock in both
     directions.

**⇒ THE FIX: the flip recovery now inverts the direction lock as well.**

This is NOT "re-deciding the direction every frame", which is what q142
measured and rejected: the flip carries a 600 frame hysteresis, fires only
on a sustained negative rate, and is excluded while the recovery is
deliberately reversing the car.  A car that is going the wrong way round
the track means the track's direction is the other way, so the lock has to
go with the sign.

### 9.9 There are TWO wedge screens, and the second one is not a button problem

Round v4 (2026-11-09) repeated v3 with ONE change -- the guest's save data
was restored before the round -- and it did NOT reach the race.  Its last
frame says why, and it is a different screen from the one that wedged m3:

    .build/eden-runs/20261109-162254-eden-rr3-v4-flipfix/frames/frame-005600.png
    -> the NASCAR event screen, with the guest's red glove on the event and
       a progress bar reading the progress at 0 %.

**=> THE GUEST WAS TRYING TO LOAD AN EVENT WHOSE ASSETS ARE NOT ON THE
CARD.**  The load never progresses, the screen never changes, and the race
never starts -- and no button press can fix that, because what is missing
is DATA and not a dismissal.

**=> SO THE TWO WEDGES ARE DIFFERENT, AND THE PULSE ONLY FIXES ONE.**

    m3   the confirm button held down      wedged on the REWARD POPUP
    v3   the confirm button pulsed         got past it, reached the race
    v4   the pulse AND the save restored   wedged on the EVENT LOAD, 0 %

**=> THE PULSE IS CONFIRMED BY m3 AGAINST v3: the script was the only thing
that changed, and the popup stopped wedging the guest.**  What v4 shows is
that the state restore is HARMFUL as implemented: it puts the guest back on
an event whose assets are not on the card.

**=> AND THE BIGGER POINT, WHICH IS NOT A PORT BUG AT ALL: the port cannot
reach a race unless the EVENT's data is on the card.**  That is a shipping
requirement -- the package has to carry the event data, or the user has to
have downloaded it -- and it is also why the "2x2" cells differed: the
guest's persisted state decides WHICH EVENT it lands on, and an event
whose assets are absent is a wedge that no input can clear.

=> The harness therefore has to do two separate things, and only one of
them is about input:

  * PULSE the confirm button, so a modal dialog cannot wedge the guest.
  * PUT THE GUEST ON AN EVENT WHOSE ASSETS ARE ON THE CARD, or the round
    measures the download screen instead of the pursuit.  The guest's
    current event is visible in the archived frames, so this is checkable
    rather than guessed.

**=> AND THE RULE THAT FALLS OUT OF IT, WHICH COSTS THE MOST TO LEARN: a
round whose start state is not controlled is not a measurement.**  Three
readings of the same 2x2 (the code, then the census, then the persisted
state) were all wrong, and each was wrong because the cells were not
comparable.  **The first thing to fix is the experiment, and only then the
thing being measured.**

### 9.10 The control ids the front-end driver needs, grounded in the guest's own files

The port can already read the guest's screen tree by control id
(`ui_control_probe.cpp`: `uiControlFind`, `uiScreenOpen`, and the current
screen's NAME in the sample).  What was missing was the TABLE, and the table
has to come from the guest's own .gui files, not from a screenshot.

`tools/gui_index.py` decrypts all 939 .gui files and indexes the 4,729
distinct `id_string`s.  Asked the right question, it answers exactly what
the driver needs:

    python tools\gui_index.py --out .build\gui_index.json
    python .build\gui-ids.py <id> [...]
    python .build\gui-ids.py --screens <name>

**THE EVENT MAP'S CARD IS `EventMapScreen_STREAM_GROUP_<GROUP>_card`**, one
per championship group, and every one of them carries the SAME pressable:

    MAP_EVENT_CARD_BTN              the pressable card, on EVERY group
    MAP_EVENT_CARD_STREAM           present only on the STREAMABLE groups
    MAP_EVENT_CARD_LOCKED           the event is locked
    MAP_EVENT_CARD_MANUFACTURER_FRAME
    MAP_EVENT_CARD_RACETEAM_FRAME

**AND THE STREAM FLAG IS THE DISCRIMINATOR.**  Measured from the index:

    stream=True   AMATEUR, ELITE, EXPERT, LEGEND, MASTER, PRO, PRO_AM,
                  career_group_card
    stream=False  NASCAR, ENDURANCE, ENDURANCE_PROTOTYPES,
                  ENDURANCE_GRAND_TOURING, EURO_TRACK_MASTERS, FORMULA_E,
                  SUPERCARS, SUPERTROFEO

**⇒ THE MANUFACTURER CUPS ARE THE ONES THAT WEDGE.**  They have no
streamable card, and they are exactly the groups whose assets are not on the
card: the guest's own log says the assets are fine ("Asset download skipped -
all files up to date!"), and the stuck screen's progress bar reads 0 %.

**⇒ SO THE TABLE IS SHORT AND IT IS EVIDENCE, NOT A GUESS:**

    on EventMapScreen, if MAP_EVENT_CARD_STREAM is present:
        press MAP_EVENT_CARD_BTN          -> a raceable event
    on EventMapScreen, if it is NOT present:
        the guest is on a manufacturer cup; go back and pick AMATEUR
    on Event_item, if EVENT_LOCKED is present:
        the event is locked; EVENT_LOCKED_BTN and
        EVENT_LOCKED_BTN_COST_CURRENCY are its cost, so do not press it

**⇒ AND THE ARCHIVES CONFIRM THE IDS.**  The screen name is in the probe's
own line, so an archive says which group the guest was on:

    v3 (raced)   ui-ctl-br screen=EventMapScreen ... MAP_EVENT_CARD_STREAM
                 ... MAP_EVENT_CARD_LOCKED ... ACTION_BTN
                 then screen=CarSelectScreen ... BTN_RIGHT_CAR
    v4 (wedged)  ui-ctl-br screen=EventMapScreen ... MAP_EVENT_CARD_MANUFACTURER_FRAME
                 ... MAP_EVENT_CARD_RACETEAM_FRAME ... ICON_GOLD
                 and NO MAP_EVENT_CARD_STREAM

**⇒ THE LESSON, WHICH IS THE SAME ONE THE WHOLE PROJECT KEEPS PAYING FOR: a
frame-based input script cannot express "press the button that means go",
and it cannot express "do not press anything on this screen".  It can only
press A a number of times, and the number of times that works is a
function of the frame rate and of how long the script runs.  The port
already has the guest's own vocabulary; the driver has to use it.**

### 9.11 The zero-code fix: hold the accelerator, and let the port navigate BY NAME

Two facts, measured on 2026-11-09, and together they are the whole fix.

**FACT ONE: A HELD BUTTON NEVER WALKS THE MAP; A PULSE DOES.**  The event
map is a scrolling world map ("EventMapScreen" is called "Stream Selection"
in the guest's own screen registry), so a button-down TRANSITION moves the
selection from one championship group to the next.

    m3, m1   the confirm button HELD      no transition, no walking
                                            the guest stays on the group it
                                            started on -- the streamable
                                            AMATEUR one -- and RACES
    v4       the confirm button PULSED    a transition, so the selection
                                            WALKED to the NASCAR card, whose
                                            assets are not on the card, and the
                                            guest sat on a 0 % progress bar

**FACT TWO: THE PORT CAN ALREADY PRESS A CONTROL BY NAME, AND NOTHING WAS
ASKING IT TO.**  `rr3-ui.txt` is a list of the guest's own control ids and
the port taps each one it finds on the current screen
(`ui_control_find` / `ui_control_next_request` in ui_control_probe.cpp,
called from guest_view_probe.cpp).  A request whose control is not on the
screen is SKIPPED, so one wrong id cannot block the others.  And the file
on the card asked for exactly one id:

    BTN_RACE

which the guest's own .gui files put on `MainMenu` and `RaceStartScreen`
and NOWHERE on the event map.  So the request was pressing nothing, and
the event map was navigated by the script's blind button count instead.

**⇒ THE FIX IS ONE CARD FILE, NOT A NEW BUILD:**

    rr3-ui.txt:
        MAP_EVENT_CARD_BTN      the pressable event card, on EVERY group
        BTN_RACE                the race start button
        BTN_OK, BTN_CONTINUE    the popup screens

`.build/deploy-ui-requests.ps1` writes it and installs the HELD pad script
beside it.

**⇒ AND ROUND v5, WITH NOTHING ELSE CHANGED, IS THE PROOF:**

    v5   the held script + the semantic requests, census off, the same build

        metrics live       149 samples, first at frame 1950
        start lights 7, race screen 10      (the normal counts)
        flips 1, stalls 0                   (v3 was flips 7, stalls 5)
        rres median 6.27 m                  (v3 was 9.60 m)

**⇒ TWO FIXES ARE IN THAT ONE NUMBER.**  The semantic requests are what put
the guest in a race that the pursuit can be measured in, and the flip fix --
which inverts the direction lock together with the steering sign, because a
car going the wrong way round the track means the track's direction is the
other way -- is what took the stalls from 5 to 0 and the flips from 7 to 1.

**⇒ AND WHAT IS STILL WRONG IS NOW MEASURABLE RATHER THAN MYSTERIOUS.**  The
lap progress still ends negative (the car still turns round at the end), and
the `dir=` and `mot=` fields are in the log, so the next question is a
measurement and not a theory.

### 9.12 What the dir= and mot= fields say about the pursuit's REAL first fault

Round v5 (2026-11-09) was the first round with the direction lock and the
windowed motion vector IN THE LOG, and they say something the earlier rounds
could not.

    f=2370 .. 2730   dir=0   la=0.00   mot=0.967,-0.255   steer=-0.05
    f=2760           dir=-1  la=4.47   mot=0.967,-0.255   perr=-0.46 steer=0.92
    f=2940           dir=-1  la=4.47   sdist=-20658        total 74964 -> 50141
    f=3030           dir=1   la=4.48   perr=+0.43         steer=-0.85  flips=1

**⇒ READ IT BACKWARDS, BECAUSE THE FAULT IS EARLIER THAN THE DIRECTION.**

  1. **FOR 360 FRAMES THE PORT HAD NO TARGET AT ALL** (`la=0.00` means
     `raceLineTargetAdaptive()` returned false).  It refuses to aim at a car
     more than 25 m from the fitted line, and the car was further out than
     that -- it had already left the road.  `total` grew by only 433 units in
     those 360 frames, so the car was barely moving: it was off the line,
     probably against a barrier, and the port was steering it with a
     0.05-sized correction because it had nothing to aim at.
     **⇒ THE RACE WAS LOST BEFORE THE PURSUIT EVER STEERED IT.**
  2. **THE DIRECTION LOCK WAS DECIDED AT f=2760, IN THE MIDDLE OF THAT
     MESS**, and it was decided as **-1**, while the car's own travel
     direction was `mot=0.967,-0.255`.  So the look-ahead point went on the
     OPPOSITE side of the car, the pursuit error became -0.46 rad, and the
     controller held full lock (0.92) into it.
  3. **THE CAR REVERSED** at f=2940: `sdist` went negative and `total` fell
     from 74964 to 50141.
  4. **THE FLIP RECOVERY CORRECTED IT** at f=3030: `flips=1` and the lock
     went back to `dir=1`, with the steering following.  **⇒ The fix in
     9.8 WORKS: the lock and the steering sign are now corrected together.**
     But it took the 600 frame hysteresis to do it, and by then the car was
     already going the wrong way.

**⇒ SO THERE ARE TWO FAULTS, AND THE SECOND ONE IS THE INTERESTING ONE.**

  * the pursuit does not engage early enough (the 25 m guard means the car
    is unguided through the opening of the race), and
  * the direction is decided ONCE, from a geometric comparison of the car's
    travel direction with the spline's node order, at a moment when the car
    is in a state where that comparison is not trustworthy.

**⇒ AND THE ROBUST ANSWER IS THE GUEST'S OWN TRUTH, NOT OUR GEOMETRY.**
The guest already knows which way round the track it is: it publishes the
car's SPLINE NODE (`snode`) and its signed position along the track
(`total`).  **So the direction round the track is measurable directly from
the guest's own numbers -- does `snode` increase or decrease as the car
moves -- instead of being inferred from a comparison between the car's
heading and the polyline's neighbours, which is a MIRROR-SENSITIVE
inference made at a moment when the car may be facing any direction at all.**

**⇒ AND THIS IS THE SAME RULE THE PROJECT KEEPS LEARNING, IN A NEW PLACE:**
the direction round a closed track cannot be told from a residual, and it
cannot be told from a single sample of a car that is out of control.  It has
to come from the thing that knows, and that thing is the guest.

### 9.13 The four-cell A/B, and the number that was wrong

**⇒ FIRST, THE INSTRUMENT WAS LYING AGAIN.**  The report printed the
guest's `prog` member under the name "progress".  The project's own record
says "The car's best distance ever | 186,921 units = 10.35 % of a lap |
q145", and q145's last race-state line reads `total=186921 ... prog=9799`.
**⇒ The lap progress is `total`, the guest's total spline distance, and the
number that matters is its MAXIMUM over the round.**  The report now prints
both, named, and `.build/lap-progress.py` prints the comparable table.

**⇒ THE FOUR CELLS (2026-11-09, the same build, the same card, the same
binary, the guest's save restored before each one):**

    cell                          best total   % of a lap   rres   flips  stalls
    fixed    + centre line          69,367        3.84 %     11.80     2       3
    adaptive + centre line          58,110        3.22 %      1.37     2       0
    fixed    + racing line          57,091        3.16 %      2.08     1       0
    adaptive + racing line          74,016        4.10 %      5.63     7       7
    ------------------------------------------------------------------
    q145 (2026-11-04, the reference) 186,921     10.35 %      0.32     7      10

**⇒ THREE THINGS COME OUT OF IT, AND ONLY THE FIRST IS COMFORTABLE.**

  1. **THE PURSUIT IS REPEATABLY MEASURABLE NOW.**  All four cells reached
     the race, with the normal 7 start lights and 10 race screens, the
     metrics object alive, and the port's closed loop driving.  That is the
     thing that was missing for a month, and it is worth more than any one
     of these four numbers.
  2. **ALL FOUR ARE BELOW THE REFERENCE, AND THE FIRST CELL HAS THE SAME
     CONFIGURATION AS q145.**  Centre line, constant 20 m, and it is 2.7
     times WORSE.  The only differences are the changes of 2026-11-09.  And
     the shape of the failure is in the log: at the end of that cell the
     car's progress rate is **2.76 units a frame against q145's 23.88**,
     with the steering **pinned at full lock (steer=1.000)**.  A pursuit
     that is saturated at full lock is not tracking the line.
  3. **THE ADAPTIVE LAW AND THE FIXED LAW BARELY DIFFER ON THIS TRACK.**
     `la` has median 20.00 m in ALL FOUR cells -- identical -- because most
     of the road ahead is straight and the law only shortens the look-ahead
     in the corners.  **⇒ This A/B could not have separated the two laws
     even if the pursuit were perfect, and it did not.**  The spread
     between the four cells (3.16 % to 4.10 %) is of the same order as
     the difference between them, with one round per cell.

**⇒ AND THE PRIME SUSPECT FOR (2) IS THE FLIP'S OWN CRITERION.**  The flip
recovery inverts the steering sign when the rate stays negative.  The rate
is a 120 frame window, and **the recovery reverses the car ON PURPOSE**, so
the recovery's own reverse drives the rate negative.  **⇒ The flip can
fire on the recovery's signal, and inverting the direction LOCK on that
signal puts the look-ahead point on the wrong side of the car -- the one
thing the lock exists to prevent.**  The lock inversion is therefore
DISABLED, with the reason recorded in the code, and the next round measures
whether the distance comes back.

**⇒ AND THE NEXT QUESTION AFTER THAT IS NOT THE LOOK-AHEAD LAW AT ALL.**  It
is the pursuit's ENGAGEMENT: the port refuses to aim at a car more than 25 m
from the fitted line, so the car spends the opening of the race unguided
(9.12), and the direction lock is decided from a geometric comparison at a
moment when the car is in no state to be compared.  The robust answer is the
guest's own signed `total`, which is already in the log.

### 9.14 The hypothesis was killed by its own experiment

The suspicion in 9.13 was that the direction-lock inversion -- added on
2026-11-09 -- was the regression, because the first A/B cell has the SAME
configuration as q145 and is 2.7 times worse.  So the inversion was turned
off and ONE cell was rerun, with everything else identical (same binary
except the one line, same card, the guest's save restored, the same
navigation).

    m6   the lock inversion OFF, centre line, constant 20 m
         best total 75,141 units = 4.16 % of a lap
    the same cell with the inversion ON
         best total 69,367 units = 3.84 % of a lap
    the four A/B cells, for scale
         3.16 % ... 4.10 %

**⇒ 4.16 % AGAINST 3.84 % IS INSIDE THE NOISE BAND OF THE FOUR CELLS, so
THE INVERSION IS NOT THE REGRESSION.**  The hypothesis was killed by the
experiment that was run to confirm it, and the inversion stays off until
its criterion can be rebuilt on something the recovery cannot poison.

**⇒ AND THE ONE REMAINING SHAPE IN THE LOG IS MORE INTERESTING THAN THE
ONE THAT WAS SUSPECTED.**

    m6   the best total is reached at FRAME 1890 -- 450 frames after the
         race started -- and the round then runs for another 5,000 frames
         with flips 6 and stalls 9 and no further progress.
    q145 the car makes steady progress for 5,760 frames at 32.5 units a
         frame and is still going at the end of the round.

And the race itself starts much later today: the metrics object goes live
at frame 1440, against frame 540 in q145, so the guest spends 900 frames
more in the front end before there is a race to drive.

**⇒ SO THE REMAINING DIFFERENCES BETWEEN q145 AND A TODAY'S CELL ARE THESE
FOUR, AND NONE OF THEM HAS BEEN ISOLATED YET:**

  1. the census: ON in q145, OFF in every 2026-11-09 round;
  2. the NAVIGATION: q145 had the blind A press and an rr3-ui.txt that
     asked for BTN_RACE -- a control that does not exist on the event map,
     so nothing was tapped; today the port TAPS MAP_EVENT_CARD_BTN, so it
     selects whatever card is under the glove, which may not be the card
     the blind press used, and the race starts 900 frames later;
  3. the cond-wait change (9.5);
  4. the clean loader (the asset fallback chain and the console release).

**⇒ THE CHEAPEST NEXT DISCRIMINATOR IS (2), and it costs no build: take
MAP_EVENT_CARD_BTN out of rr3-ui.txt, so the guest is navigated by the blind
A press exactly as it was in q145, and rerun the same cell.**  If the
distance comes back, the port's own navigation was changing which event
raced; if it does not, (1) is next.

**⇒ AND THE LESSON IS THE SAME ONE THE FLIP WAS, ONE LEVEL UP: the thing
that changed between two measurements is not necessarily the thing that
broke them, and the way to find out is to put the old thing back and
measure, not to reason about it.**

### 9.15 The port's own navigation is the variable, and the instrument lied AGAIN

**⇒ THE INSTRUMENT, THIRD TIME.**  `total` is SIGNED: the guest publishes the
car's position along the track and it goes negative when the car drives
backwards, so the raw uint32 WRAPS.  The comparison table was taking the max
of the UNSIGNED value, so a wrapped negative -- the car going backwards --
looked like the best result of the round.  It is now signed.

**⇒ AND THE ROUND THAT SETTLES THE NAVIGATION.**  The cheapest possible
discriminator: take MAP_EVENT_CARD_BTN out of rr3-ui.txt, so the port taps
NOTHING on the event map and the guest is navigated by the pad script's blind
confirm press, exactly as in q145.  Same build, same card, the guest's save
restored, same configuration (centre line, constant 20 m).

    q145  the reference                        race starts at frame  540
                                                  best total 186,921 = 10.35 %
    m7    blind navigation, lock inversion off   race starts at frame  540
                                                  best total     -2,728  (backwards)
    m6    the port taps MAP_EVENT_CARD_BTN       race starts at frame 1440
                                                  best total    75,141 = 4.16 %

**⇒ SO THE PORT'S OWN NAVIGATION IS A VARIABLE, AND IT CHANGED WHEN THE
RACE STARTED.**  With the blind navigation the race starts on the SAME
frame as q145 -- 540 -- and with the port tapping the card it starts 900
frames later.  The port's tap selects whatever card is under the guest's
red glove, which is not necessarily the card the blind press used.

**⇒ AND THE PURSUIT'S OWN FAULT IS NOW ISOLATED FROM THE NAVIGATION.**
In m7 the race starts exactly where q145's did, and the car still drove
backwards.  So the remaining distance from 10.35 % is NOT the navigation and
NOT the lock inversion (9.14).  What is left is the pursuit's own behaviour,
the census, the cond-wait change and the clean loader.

**⇒ ⇒ AND THE NEXT ROUND IS THE CENSUS, WITH THE RACE NOW STARTING ON THE
SAME FRAME AS q145:** the same blind navigation with the census ON.  If the
distance comes back, the census is the variable -- and that would be the
third time the census has been blamed and the second time it has been
wrong, so it will be worth a repeat round before it is believed.

### 9.16 A CORRECTION: the magenta car is NOT a path-resolution bug

**THIS CORRECTS SECTION 9.4's LAST PARAGRAPH, WHICH WAS WRONG.**

The earlier reading claimed that 45 asset-list entries could not be resolved
locally, so the manifest kept the server's digest for them, and that the
port therefore served the guest a digest for a file it did not have.  **That
count was a misreading of the log**: the counter grepped only the PATH field
of the "assetlist-entry" lines and ignored the rest of the line.  Those 45
lines are REWRITES THAT SUCCEEDED.

WHAT THE SAME LOG ACTUALLY SAYS:

    phase=assetlist-entry path=/assets_2048x1536/common/sprites.android.atlas
      md5=2ab3d810...->72bcfd205a917770eebb8a1a9e3dcbd0 size=39460
      source=rr3gui0:/files/.depot/gui_assets/2048x1536/common/sprites.android.atlas
    phase=asset-invariant path=... published=72bcfd20... served=72bcfd20...
      agree=1 found=1 src=rr3gui0:/files/.depot/gui_assets/2048x1536/common/...
    phase=cc-asset-req path=... want_md5=72bcfd20... digest_field=wire candidates=13
      tried=.depot/assets_2048x1536/common/sprites.android.atlas|files/.depot/...

and for the magenta car itself:

    phase=repair-candidate path=assets_2048x1536/events/map/featured/
      weekly_events/lexus_is_f350_catalunya/sprites.android.atlas
      resolved=1 digest=debbef8a79a15e0b2aeb475039e2beab
      card=.../.depot/gui_assets/2048x1536/.../sprites.android.atlas present=0
      source=.../.depot/assets_2048x1536/.../sprites.android.atlas

**⇒ THE FILE IS FOUND, THE DIGEST IS PUBLISHED, THE SERVED DIGEST MATCHES
THE PUBLISHED ONE (agree=1, found=1), AND THE GUEST ASKS FOR EXACTLY THAT
DIGEST.**  And no texture failed to LOAD: every "unable to load file" line
in the round was counted by extension and there is not one texture among
them.

**⇒ SO THE TEXTURE IS CORRECTLY SERVED AND THE FAULT IS DOWNSTREAM OF
THAT.**  What is left is the part the port cannot currently see:

  (a) THE TEXTURE UPLOAD IS REJECTED BY THE DRIVER.  The guest's atlases
      are "sprites_0.rgb.pvr.z" -- a COMPRESSED PVR texture.  The port
      serves the bytes; if the host driver cannot decode that format the
      upload fails and the material samples an incomplete texture.  The
      port traces the upload calls and COUNTS them; it never reads
      glGetError() afterwards.
  (b) THE SHADER NEVER BECOMES USABLE.  The port traces glCompileShader
      and glLinkProgram and counts the calls; it never reads
      GL_COMPILE_STATUS or GL_LINK_STATUS, and never reads the driver's
      info log.

**⇒ (a) AND (b) ARE THE TWO THINGS THE PORT CANNOT CURRENTLY TELL APART,
and that is the whole problem: the instrument does not exist.**  So the
work is an instrument first -- read the shader status and the upload error
after every guest call that can fail, and report only failures, with a
budget -- and a fix second, if the fault is findable from the source.

**⇒ AND THE LESSON, WHICH IS THE THIRD TIME THIS PROJECT HAS PAID IT IN
ONE WEEK: a count taken from a log is only as good as the fields that were
actually read.**  Three readings of the same rounds were wrong in a row --
the 2x2 (the cells were not comparable), the flip hypothesis (killed by its
own experiment), and this one (the counter read a field and ignored the rest
of the line).  **⇒ PRINT THE WHOLE LINE, NOT A FIELD OF IT, AND GREP FOR
WHAT THE LINE SAYS, NOT FOR A SUBSTRING OF IT.**

### 9.17 The magenta rival cars: block layout corruption vs format advertising and delivery

**THE MEASUREMENT PIXEL BY PIXEL (NOT A MISSING TEXTURE):**
In frame 2000 of the m9 run (`.build/eden-runs/20261109-202239-eden-rr3-m9-nov4-binary-todays-card/frames/frame-002000.png`)
and its 4x crop (`frame-002000.magenta-crop.png`), rival DTM cars (BMW M4 DTM,
Mercedes C63 DTM) render magenta with dark purple windows.
Measuring pixel values proves this is NOT a missing-texture colour:
zero pixels in the frame are exactly (255, 0, 255).
What actually appears on the car bodies:
  (255,4,255) x40, (192,4,192) x17, (221,4,229) x7, (187,7,204) x5, (255,7,255) x4, (190,29,220) x3
This represents a normal grey/white car livery where the green channel is zeroed
(white 255,255,255 -> 255,4,255; grey 192,192,192 -> 192,4,192), and window glass
becomes dark purple (60,60,60 -> 60,0,60). The texture is decoded, but decoded
with the wrong block layout.

**THE ASSET DATA AND DELIVERY ARE VERIFIED GOOD:**
- Source pack member `files/.depot/vehicles/2017_bmw_m4_dtm/livery/2017_bmw_m4_dtm_ext.dxt.dds.z`
  (266,228 bytes on disk) has 4-byte LE size 699,192 + zlib payload.
  It inflates to a valid 1024x1024 DDS with 11 mip levels and FourCC 'DXT1'
  (8 bytes per 4x4 block).
  CPU decode via Pillow and via `rr3/dxt_decode.hpp` produces a normal grey car
  (mean RGB 125, 122, 126) with ZERO green-zeroed pixels.
- The delivery log in m9 (`guest-view.log`) proves the guest received exactly these
  266,228 bytes (`phase=map-app`, `phase=stat-layout size=266228`, `phase=open-ok`).

**LOG CAP EXPLANATION FOR THE DXT5 TRACE ARTIFACT:**
Earlier logs in m9/m10 showed:
  `phase=ctexImage2D ordinal=16777472 target=00000000000083f3 data`
  `... (8 mip levels from 256x256 down to 2x2) ...`
Internal format 0x83F3 is `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT` (16 bytes per block).
The reason only 0x83F3 was logged is that `guestViewTracedGlCompressedTexImage2D`
had a hard cutoff `if (g_glStats.compressedTexImage2D <= 8)`. Those 8 calls belonged
to `vehicles/common/car_rotor_glow.dxt.dds.z`, which is genuinely 256x256 DXT5 on disk.
All subsequent texture uploads (including car liveries) were suppressed by the budget.

**THE THREE DISCRIMINATED HYPOTHESES:**
- H1: The candidate chain served a format-specific request with bytes of another format.
- H2: The port's asset list rewrite and the guest's format decision disagreed.
- H3: Mesa 20.1.0-rc3 on this target cannot decode S3TC or decodes DXT1 with green masked.

**THE DIAGNOSTIC INSTRUMENTS INTRODUCED:**
1. `graphics_probe.cpp` pixel readback:
   Stage 9 now performs `glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE)`
   and compares the GPU pixel against reference CPU decode of the exact same DDS block
   via `decodeDxt1Block` / `decodeDxt5Block`.
   Probes both GUI atlas (`sprites_0.dxt.dds.z`) and vehicle livery
   (`2017_bmw_m4_dtm_ext.dxt.dds.z`), logging:
   `phase=probe-dxt1 asset=... fourcc=... w=... h=... internal=... err=... got=(r,g,b) want=(r,g,b) match=0/1`
2. Linked compressed texture upload tracking:
   A ring buffer in `guest_filesystem.cpp` records served DDS assets with parsed FourCC,
   dimensions, and mip counts. `guestViewTracedGlCompressedTexImage2D` reports the
   recent served file alongside `internalformat` and `glGetError()` with a 32-line budget.
3. Software DXT fallback:
   `__wrap_glCompressedTexImage2D` in `gfx_gles.cpp` now supports translating S3TC uploads
   to RGBA8 on CPU via `decodeDxtImage` and `__real_glTexImage2D` when `RR3_SW_DXT=1` is set.
4. Defect A Npad heartbeat:
   Added `npad-heartbeat` diagnostic (every 5s and on slot/button/analog change) reporting
   `state=npad-none|npad-idle|npad-active`, connection type, buttons (hex), sticks, and
   autopilot injection. Raw HID fallback is silenced during Npad activity and polled at
   15-frame cadence.

**WHAT REMAINS UNVERIFIED:**
- The first Eden run with the new diagnostics will report whether Mesa's `probe-dxt1`
  returns `match=1` or `match=0` (confirming or ruling out H3).
- The `ctex-upload` log lines will report whether the guest passed 0x83F1 (DXT1) or
  0x83F3 (DXT5) when uploading vehicle liveries.

### 9.18 The magenta rivals, continued: the guest's format ORDER, and what the GPU does with the bytes

**THE FORMAT ORDER THE GUEST ASKS FOR (measured from the `path-req` lines):**
The guest asks for a texture by SPELLING, and it walks the list:
`..._sprites_0.rgb.pvr.z` -> `..._sprites_0.etc.dds.z` -> `..._sprites_0.dxt.dds.z`
-> `..._sprites_0.atc.dds.z`.  PVR first, then ETC1, then DXT, then ATC.  It
takes the first one that EXISTS.

**WHERE EACH SPELLING EXISTS ON THIS CARD:**
- The card's loose `.depot` (the original Android app's own files) carries the
  PVR and ETC1 spellings -- for example
  `.depot/assets_2048x1536/common/sprites_0.rgb.pvr.z` (1,021,253 B) and
  `.depot/vehicles/<model>/livery/<model>_ext_<colour>.etc.dds.z`.
- The SEVEN EXTERNAL VOLUMES were built from the DXT data pack (the pack whose
  name and sha256 are recorded as `source_archive` / `source_sha256` in
  `data/rr3-data-manifest.json`; the archive itself is a ~6.6 GB zip whose file
  name carries CJK characters and is therefore not quoted here), and they
  carry ONLY the DXT spelling: of the
  11,155 `*.dds.z` entries in `tracks` + `vehicles-mid`, every single one is
  `.dxt.dds.z`.

⇒ **So a car whose assets are in the loose `.depot` gets its PVR/ETC spelling
and renders correctly; a car that exists ONLY in the volumes falls through to
the DXT spelling.**  That is exactly the split the symptom shows: the 82 cars
the card has are fine, and the DTM rivals -- which the card does not have --
are magenta.

**THE GUEST'S OWN FORMAT DECISION IS CORRECT (measured, `ctex-upload` lines):**
```
phase=ctex-upload ordinal=1 ... internal=0x83f3(DXT5_RGBA) err=0x0 recent_file=".../vehicles/common/car_rotor_glow.dxt.dds.z" recent_fourcc=DXT5 recent_dim=256x256
phase=ctex-upload ordinal=10 ... internal=0x83f1(DXT1_RGBA) err=0x0 recent_file=".../vehicles/common/arms_norm.dxt.dds.z" recent_fourcc=DXT1 recent_dim=512x512
```
The guest uploads each texture with the internal format that matches the
FourCC inside the file it was given (DXT5 file -> 0x83F3, DXT1 file -> 0x83F1),
and every upload reports `err=0x0`.  ⇒ **H1 (the alias chain answered a
format-specific request with another format's bytes) and H2 (the asset-list
rewrite and the guest's format decision disagreed) are DEAD.**

⇒ **WHAT IS LEFT IS THE GPU'S OWN DECODE.**  The bytes are good (the CPU
decodes them into a normal grey car, mean RGB 125,122,126, zero green-zeroed
pixels) and the guest hands them over in the right format with no GL error --
and the result on screen is the same texture with its GREEN CHANNEL ZEROED.

**THE FIX, AND ITS STATE: `swdxt=1` IN `sdmc:/switch/rr3-gfx.conf`**
`swdxt=1` makes the port decode DXT1/DXT5 itself
(`include/rr3/dxt_decode.hpp`) and upload the pixels as plain RGBA, which
takes Mesa out of the S3TC decode entirely.  The decoder is proved by
`native-switch/tests/test_dxt_decode_contract.py` (27 checks, and the gate
runs it).  It is a conf key and not an environment variable because the port
runs as an NRO on a console, where there is no environment to set.
**IT IS OFF BY DEFAULT (`swdxt=0`), because the first version of it broke the
event card -- see 9.19.**  The mechanism is right (the read-back says the GPU
returns the CPU's own pixels unchanged) but the port cannot yet attribute an
upload to the file it served, and the safe form of this fix re-encodes at
SERVE time instead.

**THE INSTRUMENT, AND TWO WAYS IT WAS WRONG FIRST:**
`phase=dxt-readback` prints what the GPU returned for a texel next to what the
CPU decoded for the same texel.
1. It first reported `match=0` (the GPU disagrees) when the truth was "nothing
   was measured": this Mesa build answers `GL_FRAMEBUFFER_UNSUPPORTED`
   (0x8CD6) when a COMPRESSED texture is attached to a framebuffer, so the
   pixel came back (0,0,0).  The verdict is now THREE-VALUED: `1` agrees,
   `0` disagrees, `-1` NOT MEASURED, with the status and the GL error on the
   line.  **"NOT MEASURED" IS NOT "MEASURED AND WRONG".**
2. It then read the texture back the moment level 0 arrived, and a texture
   whose mip chain is incomplete SAMPLES BLACK.  The measurement is now
   DEFERRED until the next texture starts, at which point the previous one has
   its whole chain.

**THE CONTROLLER, MEASURED THE SAME WAY (Defect A):**`phase=npad-heartbeat ... state=npad-idle type=FullKey seen_press=0
buttons=0x0 autopilot_injected=0` -- the emulator DOES hand the guest a
Nintendo Pro Controller (a FullKey one), and it never changes because no host
pad is bound to it.  Eden's own configuration says the same thing:
`player_0_button_a="engine:keyboard,code:67"` and the same for every other
button of every player, and `enable_all_controllers=false`.  The pad itself is
fine: XInput slot 0 is CONNECTED.  So on the PC the work is in EDEN's
controller configuration, and the port's job is only to make the difference
visible in one log line -- see `docs/CONTROLLER-ON-PC.md`.

**THE A/B, RUN AGAINST THE SAME SAVED STATE (rounds `dxtab0` and `dxtab1`):**
Both rounds start from the SAME restored guest state (`.build/guest-state.ps1
-Restore -Tag dxtab0`), the same build, and differ in ONE thing: the `swdxt`
key.

- `dxtab0` (swdxt off, the GPU decodes the S3TC blocks): the texture arrives
  on screen with its green channel zeroed -- the m9 frame above, measured pixel
  by pixel.
- `dxtab1` (swdxt on, the port decodes the blocks and uploads RGBA): the
  read-back returns the CPU's own pixels, unchanged:
  ```
  phase=dxt-readback ordinal=2 ... internal=0x83f1(DXT1) w=512 h=512 status=0x8cd5 err=0x0 got=(129,255,129) want=(129,255,129) match=1 sw=1
  phase=dxt-readback ordinal=3 ... internal=0x83f1(DXT1) w=1024 h=1024 status=0x8cd5 err=0x0 got=(127,127,255) want=(127,127,255) match=1 sw=1
  ```
  `status=0x8CD5` is GL_FRAMEBUFFER_COMPLETE -- the texture is plain RGBA8
  here, which a framebuffer DOES accept, so this is a real verdict and not a
  "not measured" one.

⇒ **Same bytes, same game, same shader, one switch: the GPU's S3TC decode is
what turned a grey car into a magenta one.**

### 9.19 The software DXT path broke the event card, and why "the internal format says DXT" is not enough

**WHAT HAPPENED (measured, round `dxtab1`):** with `swdxt=1` the event card in
the main menu rendered as rainbow noise (989 pink pixels per frame against the
10-pixel noise floor of the billboard), while the SAME frame of round `dxtab0`
(`swdxt` off) shows the red car perfectly.  The difference between the two
rounds was one conf key, so the key was the cause.

**WHY, AND IT IS NOT A DECODER BUG:** the event-card atlas is not a DXT
texture at all.  In the pack it is
`files/.depot/gui_assets/2048x1536/events/event_cards/sprites_0.rgb.pvr.z` --
a PVR texture -- and the guest uploads it under an S3TC internal format.  The
software path took "the internal format is 0x83F1, so these bytes are DXT1"
and decoded PVR bytes as DXT blocks, which is what turns an image into noise.

⇒ **THE INTERNAL FORMAT PROVES NOTHING.  THE FILE DOES.**  The port already
knows what it handed over: the served-texture ring records the FourCC of every
texture it serves, and the software path now decodes ONLY when that record
says DXT1 (for 0x83F0/0x83F1) or DXT5 (for 0x83F3).  Anything else -- a PVR
atlas, an ETC1 texture, no record at all -- goes to the GPU exactly as before.

**WHY IT WAS MISSED TWICE, AND THE RULE:**
1. The trace that showed the uploads (`ctex-upload`) had a 32-line budget and
   it was spent entirely on the car's own textures, so the GUI atlas upload
   that broke was never recorded.  **A BUDGET ON THE OUTPUT HIDES THE CASES
   THAT MATTER; THE BUDGET HAS TO COVER THE DIFFERENT KINDS OF ASSET.**
2. The first A/B was run against a race, and the race looked FINE, so the
   corruption was not noticed until the menu was compared frame by frame.
   **A FIX THAT IS VERIFIED IN ONE SCENE IS NOT VERIFIED.**

⇒ **THE RULE THIS ADDS:** a port-side change to how a texture is delivered has
to be compared in EVERY kind of screen that uses a texture -- menu, race, HUD
-- and the delivery decision has to be made from the FILE the port served, not
from a parameter the guest passed.

**AND THE LINKAGE WAS NOT ENOUGH EITHER (round `dxtab2`):** the second
attempt decoded only when the served-texture ring said "DXT1/DXT5" for the
most recently served texture, and the event card was STILL rainbow noise.
The reason is the age of the record: the guest mmaps the GUI atlases at
startup and uploads them hundreds of frames later, so by the time the menu
draws, the ring holds some other texture entirely.  **"THE TEXTURE I SERVED
MOST RECENTLY" IS NOT "THE TEXTURE BEING UPLOADED".**

⇒ **SO THE SWITCH IS OFF BY DEFAULT (`swdxt=0`) AND THE FIX IS NOT YET
SHIPPED AS A DEFAULT.**  The mechanism is proved -- with the switch on, the
read-back says the GPU returns the CPU's own pixels unchanged
(`match=1 sw=1`, eight textures) -- but the port cannot yet tell WHICH upload
belongs to WHICH file it served.

⇒ **WHAT THE SAFE FIX LOOKS LIKE, AND WHY IT IS THE NEXT PIECE OF WORK:** do
not decide at upload time at all.  Re-encode AT SERVE TIME: when the port
serves a `.dds.z` whose DDS says DXT1/DXT5, decode it, write an UNCOMPRESSED
DDS header and the RGBA pixels, and publish the new size and md5 in the asset
list (the port already rewrites those).  The guest then reads the DDS header
out of the file it mapped, sees an uncompressed format, and uploads RGBA --
so the format cannot disagree with the bytes, because the bytes SAY so.  The
read-back stays as the instrument that proves it.

### 9.20 The grey-white HUD blocks: a file the game GENERATES, and nothing we have has it

**THE SYMPTOM:** parts of the HUD render as grey-white blocks.

**WHAT THE GUEST ACTUALLY ASKS FOR (measured, m9 guest-view.log):**
```
phase=stat-path ... rc=-1 errno=2 guest='sdmc:/switch/rr3-native-rehost/.depot/gui/processed/LocalStyles.gui'
                       primary='rr3core:/files/.depot/gui/processed/LocalStyles.gui' fallback='rr3core:/files/apk/res/gui/processed/LocalStyles.gui'
phase=guest-log-any ... LoadReadOnlyMappedFile failed getting full path: gui/processed/LocalStyles.gui
phase=guest-log-any ... Asset::LoadFile unable to load file: 'gui/processed/LocalStyles.gui'
```
and the same for `gui/processed/LocalStyles_480x320.gui`.

**WHERE IT IS NOT:**
- not in the card's loose `.depot/gui/` (338 raw `.gui` files, no LocalStyles),
- not in `core.romfs` (`gui/processed/` has 3,870 entries there, and the ones
  for the series DO exist: `gui/processed/limited_time_series/common/LocalStyles.gui`
  is served with size 1014),
- not in the DXT, ETC or ATC data packs (zero `LocalStyles` members).

**AND THE GUEST NEVER WRITES IT:** every `map-write` line in the archive is for
something else (`ttss.2.dat.bak`, `webimage_cache`, `asset_list_base.txtCache.txt`,
the `assets_2048x1536/...` directories).  So it is not a write that the port
blocks -- the file simply is not there.

⇒ **⇒ IT IS A FILE THE APP GENERATES IN ITS OWN FILES DIRECTORY ON FIRST RUN**
(the name says it: LOCAL styles, next to a pre-baked set for every series), and
every copy we have came from the APK or from a data pack, neither of which
carries it.  **THE ONLY PLACE IT EXISTS IS THE ANDROID DEVICE THAT HAS RUN
THE GAME.**

⇒ **WHAT CLOSES IT:** copy it off the phone, into the card, where the port's
third candidate (the same path on the SD card) will find it:
```
Android/data/com.ea.gp.fifteen15.fnhd/files/.depot/gui/processed/LocalStyles.gui
Android/data/com.ea.gp.fifteen15.fnhd/files/.depot/gui/processed/LocalStyles_480x320.gui
  ->  sdmc:/switch/rr3-native-rehost/.depot/gui/processed/
```
⇒ **THE LESSON:** a content set assembled from an APK and from data packs is
not the same as the content set a device has after the game has run on it.
**THE MISSING FILES ARE THE ONES THE APP GENERATES, AND ONLY THE DEVICE HAS
THEM.**

### 9.21 THE MAGENTA FIX: THE VOLUMES ARE BUILT FROM TWO PACKS, NOT ONE

**THE ROOT CAUSE, IN ONE LINE:** the guest asks for a texture by SPELLING and
walks the list `pvr -> etc -> dxt -> atc`, taking the first one that EXISTS.
The card's loose `.depot` carries the pvr/etc spellings for the 82 cars it
has; the seven external volumes were built from the DXT pack, which carries
ONLY the dxt spelling.  A car the card does not have therefore fell through
to dxt -- and Mesa's S3TC decode zeroes the green channel of that texture.
**⇒ THE MAGENTA IS NOT A BUG IN THE PORT.  IT IS THE WRONG PACK ON THE CARD.**

**WHY THE DXT PATH IS WRONG AND THE ETC PATH IS NOT (measured):**
- dxt: the guest uploads it with `internal=0x83f1/0x83f3` and it arrives on
  screen with its green channel zeroed (9.18).
- etc: the SAME guest uploads it with `internal=0x8d64` (GL_ETC1_RGB8_OES) and
  it renders correctly -- that is what the 82 cars on the card have been doing
  all along.

**THE FIRST ATTEMPT WAS WRONG, AND IT BROKE THE GAME.**  Rebuilding ALL SEVEN
volumes from the ETC pack does not work: round `etcv1` reported
```
front-end layout opened 0    main menu layouts open 0    EventMapScreen 0
start lights 0              race screen 0
```
against the DXT baseline's 4 / 20 / 193 / 7 / 10.  The packs are NOT
interchangeable -- the ETC pack carries 12,305 `gui_assets` entries against
the DXT pack's 12,653, and the front end cannot be built without them:
```
DXT: 50,608 members  gui=2,825  gui_assets=12,653  tracks=12,229  vehicles=17,732
ETC: 50,168 members  gui=2,825  gui_assets=12,305  tracks=12,233  vehicles=17,679
```

**THE FIX THAT WORKS IS A HYBRID:** the four volumes the FRONT END reads
(`core`, `gui-hd`, `gui-sd`, `tracks`) come from the DXT pack, and the three
volumes the CARS read (`vehicles-classic`, `vehicles-mid`, `vehicles-modern`)
come from the ETC pack.  The vehicle volumes carry the car's MESHES as well
as its textures, and the meshes are the same bytes in both packs -- only the
texture compression differs -- so swapping the whole volume keeps the
geometry and changes the format.

`.build/merge-hybrid-volumes.ps1` assembles it (every volume is taken from a
build that produced it, so the manifest's sha256 is the one that build
recorded, and the merge re-verifies it) and `.build/deploy-volumes.ps1` puts
it on the card one file at a time -- the card has no room for two copies --
with every sha256 checked against the manifest.

**MEASURED ON THE CARD WITH THE HYBRID (round `hybrid1`):**
```
front-end layout opened 4   main menu layouts open 20   EventMapScreen 194
start lights 7             race screen 10
```
against the DXT baseline's 4 / 20 / 193 / 7 / 10 -- the front end is
INDISTINGUISHABLE from the set that boots.
```
ctex-upload: 30 x ETC1 (0x8d64, recent_fourcc=ETC)   2 x S3TC
  the two S3TC uploads are the WATER NORMAL MAP from the card's own loose
  copy (.depot/Tracks/common/gnd/water/water_world_normal.dxt.dds.z), not a car
```
and the frame scan of the round finds no green-zeroed pixel at all: its
worst frame holds 590 pink pixels and they are (255,77,181) -- the
advertising billboard, whose green channel is 77 -- where the magenta rivals
were (255,4,255) and (192,4,192), a green channel of FOUR.

⇒ **EVERY car texture is uploaded as ETC1 now, so Mesa's broken S3TC decode
is out of the picture for the cars, and the front end is still the DXT one
that boots.**  The volumes are also smaller than the ones they replace
(gui-hd 1,443,906,568 against 1,663,784,980 bytes), so the card gained space.

**AND THE PORT-SIDE EXPERIMENT IS KEPT, OFF BY DEFAULT:** `swdxt=1` still
decodes DXT on the CPU (9.19 proved the mechanism and proved that the port
cannot attribute an upload to the file it served).  It is the tool to use if a
future pack ships a texture format the GPU cannot decode and there is no
alternative spelling of it in the pack.

**THE LESSON THIS COST, TWICE OVER:**
1. A fix verified in ONE scene is not verified.  The pure-ETC set looked
   perfect in the upload trace -- 32 ETC1 uploads and no S3TC at all -- and
   still did not reach the race.  **THE MEASUREMENT THAT MATTERS IS THE ONE
   THAT ANSWERS THE USER'S QUESTION: DID THE GAME COME UP?**
2. A card that carries a wrong file is worse than a card that carries none.
   The same thing happened with the HUD stand-in (9.20): the per-series
   `LocalStyles.gui` was put on the card as the top-level one, and the guest
   stopped reaching the race.  It was taken back off.  **AN EXPERIMENT ON THE
   CARD HAS TO BE REVERTED UNLESS IT IS VERIFIED IN THE GAME IT CHANGES.

### 9.22 The pursuit gave up on the racing line exactly when the car needed it

**THE MEASUREMENT (round `hybrid1`, 223 race-state samples, read out by the
port's own `.build/pursuit-trace.py`):**
```
    frame   total    perr  pur  steer   mag  dir flips stalls thr brk   la  ctdist
      630       0   0.000   0  0.000  0.00   0     0      0   1   0   0.0 13076.48
     1530   67927  -1.295   1  1.000  0.29   1     1      2   0   1  20.0    7.04
     3330   58576   0.000   0 -0.410  0.41   1     4      4   1   0   0.0 12031.40
     5430   87803   0.000   0  0.530  0.53   1     7      6   1   0   0.0 47014.71
    pursuit on 153/223 samples; flips=9 stalls=7; final total=89623 (3.0 %)
```
against the reference round's 186,921 (10.35 %).

**THE MECHANISM, IN THREE STEPS:**
1. `raceLineTargetAdaptive()` refused the racing line whenever the car was
   more than **25 m** from it.  That was the guard's job: a car far from the
   line is not on this track's line, and aiming at it would be aiming at
   noise.
2. But when the line was refused, THE SAMPLE KEPT THE GUEST'S OWN AIM
   TARGET, and that target is garbage when the car is off the racing line --
   the measured distances are 13,076 m, 12,031 m, 19,059 m and 47,014 m.  The
   port's own plausibility test then refused it too (`targetPlausible`), so
   the pursuit switched itself OFF (`pur=0`) and the recovery took the wheel
   with a full lock.
3. The recovery is a full-lock manoeuvre that ends in a spin or a stall
   (flips=9, stalls=7), and the car never came back: the pursuit could not
   come back either, because the line was still refused.

⇒ **THE 25 m GUARD WAS THE ONLY THING THAT COULD HAVE STEERED THE CAR BACK
ONTO THE TRACK, AND IT WAS THE THING THAT PREVENTED IT.**

**THE FIX:** the line keeps the car.  Between "on the line" (25 m) and "not on
this track's line at all" (250 m) the TARGET IS THE NEAREST POINT ON THE LINE
-- a look-ahead of zero -- so the car is pointed back onto the track instead
of at a point further along it that it cannot see from there.  The hard limit
is still there, so a car that is nowhere near this track's line is still
refused.

The host test has teeth: the old law answered "no target" for the car below,
and the new law answers with the nearest point on the line.
```
  a car 100 m off the line still gets a target               ok
  100 m off the line : target 99.91 m away, 62.59 m from the centre
  the look-ahead is zero for a car that is off the line      ok
  the target is about 100 m away -- the nearest point on the line ok
  the target lies ON the line                                ok
  a car 500 m off the line gets no target at all             ok
```

**THE RULE:** a guard that exists to keep the controller from steering on
noise must not be the same guard that stops it steering at all.  When the
"right" input goes bad, the fallback has to be the port's OWN model of the
track, not the guest's.

**WHAT THE FIRST ROUND OF THE FIX DID AND DID NOT SHOW (round `pursuit1`):**
It did not settle the question, and the log says why:
- the round ENDED at frame 2,162 (the reference rounds race to about 6,600),
  so the absolute progress (76,368) is not comparable with the 89,623 of the
  round before it;
- the last sample has `la=0.00`, which is the NEW branch (a car off the line
  gets a look-ahead of zero), and `ctdist=13215.19` -- so the FITTED LINE was
  itself 13 km from the car in that sample.  The port refused to aim at it
  (the hard limit is 250 m) and kept the guest's own target, which is why the
  pursuit was still off (`pur=0`).

⇒ **So the change cannot make things worse** -- a line that is 13 km away is
refused, not aimed at -- **but the layer UNDER it is the real one: the fitted
chunk does not follow the car.**  The line is fitted once, on the chunk that
was current then; when the car is on another part of the track the nearest
point of that line is kilometres away, and the port is left with nothing.

⇒ **THE NEXT PIECE OF WORK IS THE CHUNK, NOT THE GUARD:** the fitted chunk
has to follow the car -- the guest's own `snode` says which node the car is
on, and the chunk boundaries are in the spline -- so that "the nearest point
of the line" is always metres away and not kilometres.  Until that is done,
the fallback only covers the case where the car is a hundred metres off the
line it was fitted on, which is the spin-into-the-runoff case and not the
wrong-chunk case.**

⇒ **AND THE PORT-SIDE EXPERIMENT IS KEPT, OFF BY DEFAULT:** `swdxt=1` still
decodes DXT on the CPU (9.19 proved the mechanism and proved that the port
cannot attribute an upload to the file it served).  It is the tool to use if a
future pack ships a texture format the GPU cannot decode and there is no
alternative spelling of it in the pack.

