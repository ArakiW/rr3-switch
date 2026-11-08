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
