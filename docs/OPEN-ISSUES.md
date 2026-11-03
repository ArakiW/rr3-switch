# Real Racing 3 → Nintendo Switch: Open Issues & Handoff

> Snapshot: 2026-11-03 02:00 (emulator: Eden — runs q123 / q123b / q124)
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

**The UI work from this round is also done and measured**: the guest's icon
font (`r3_symbols.ttf`) was being swapped for a text face, so **every icon
came out as a Latin letter and 2.3–5.8× too small**. Opening the real icon
font then exposed a **second, older bug: the guest's own `sscanf` had no
length bound**. With that bounded the game runs clean:
**q124 = 3,120 frames / 305 s / 35 MB of log, no hang, no regression.**

**Three things are left before delivery**, one of which only the user can do:

1. **Install the newest NRO on the console and boot** (§4.1) — the crash
   fixes are only proven on the emulator so far.
2. **Rebuild the deliverables and commit** (this round is uncommitted; `dist/`
   still carries the old NRO).
3. **Characterise the "Version 1" stretch** (see §4.2).

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
| 9 | Splash "Version 1" stretched 2.47×, right half clipped | **Needs characterisation** | §4.2 |
| 10 | First 10 s of black screen before the guest appears | **Not started** (separate work) | §4.3 |
| 11 | Pure pursuit cannot hold the line (lap count −1) | **Not started** | §4.4 |
| 12 | Deliverables (app zip / full package / 4 volumes) + commit | **Not started** | §4.5 |

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

Gate: BUILD 0, INPUTS 0, SYMBOLS 0, tests **39/39**.

---

### 3. Conclusions that were falsified (recorded so they are not re-derived)

| Old conclusion | Measurement | Why it was wrong |
|---|---|---|
| "The guest lays out a whole English paragraph with the symbol font, so the port draws it 2.048× too big" | The paragraph uses Conduit 1:1; 2,812 text draws per run, zero substitutions | a log window that crossed a request boundary ⇒ misattribution |
| "q122's blank dialog was caused by the symbol font" | q122 opened the card's **72,468-byte** file, which is a **different font** | same name, different content |
| "The icons are half the size because upem 2048 vs 1000" | A ratio does not describe a size error; the real cause was the **face being swapped** (different glyphs entirely) | a ratio mistaken for a size error |

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

### 4.2 P1 — The splash's "Version 1" is stretched 2.47×

**Measured (q123b, the `iconfont=0` control run)**

```
glyph-face   req=ConduitITCPro-Bold.otf  size=140.0     <- the guest names Conduit Bold
glyph-tex    texture=1024x128  inked=20618 of 131072 (15.7%) texId=254
glyph-vector createTexture  boundsW=588.0  boundsH=93.0
text-quad    a_Position: v0=(2048,430) v2=(593,430)      <- actually drawn 1455 px wide
```

⇒ the port measures **588 px**; the guest draws it **1455 px** wide (ratio
**2.474**). The screen is 1280 px, so **the right half is clipped** — which is
exactly what the frame shows.

**Clue**: the guest's own 1455 px ≈ the **symbol font's** advance sum
(≈10.4 em × 140 px = 1456 px) ⇒ **the guest's own metrics are computed with the
symbol font while the port computes them with Conduit Bold.** The next thing to
check is which face the port's `Font` class uses when it answers the guest's
`getFontMetrics()` / `getSize()`.

**Current state**: with the icon font open (q124) this element **disappears
entirely** (the splash area is empty). Neither state is right, but there is no
console feedback yet, so **nothing has been changed here** — it is deliberately
held until P0 comes back, so the direction is decided on device evidence rather
than on the emulator.

### 4.3 P2 — The first 10 seconds are black

The port's own Chinese loading screen is off (`nofb=1`) because it fights the
guest's EGL surface for the same NWindow. Enabling it requires the framebuffer
and the guest's surface to stop fighting — **separate piece of work**.

### 4.4 P3 — Pure pursuit cannot hold the line (lap count −1)

Known directions (**none done yet**):

* `steergain` / look-ahead point: steering gain and the aim point need to be
  tuned per corner;
* **the recovery logic needs an EXIT, not a reference-point reset** — resetting
  makes the car spin on the spot.

### 4.5 P4 — Deliverables and commit (**not done this round**)

* ⚠️ **This round's source changes are not committed** (`git status` lists 10
  modified files).
* ⚠️ **`dist/` still carries the old NRO** (`rr3-switch-app-20261102.zip`,
  45.26 MiB, NRO `fcdb4bee…`) and therefore **does not contain** the icon font
  or the bounded `sscanf`. Needs a rebuild: app zip (≈45 MiB) + full package
  (≈9.6 GiB / 186 files) + 4 split volumes.
* The gate `.build\gate.ps1` needs **one more run** after the `sscanf` change
  (the last `GATE OK` predates it).

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
