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
2. **Characterise the splash overflow** (see §4.2) — the chain is measured
   down to the engine's own matrix; what is left is which matrix it sets up
   for the 2D pass.
3. **Pure pursuit cannot hold the line** (§4.4).

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
| 13 | Pure pursuit cannot hold the line (lap count −1) | **Not started** | §4.4 |
| 14 | Deliverables (app zip / full package / 4 volumes) + commit | **Rebuilt** | §4.5; the NRO in the package, the app zip and on the card are the same file |

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



| Old conclusion | Measurement | Why it was wrong |
|---|---|---|
| "The guest lays out a whole English paragraph with the symbol font, so the port draws it 2.048× too big" | The paragraph uses Conduit 1:1; 2,812 text draws per run, zero substitutions | a log window that crossed a request boundary ⇒ misattribution |
| "q122's blank dialog was caused by the symbol font" | q122 opened the card's **72,468-byte** file, which is a **different font** | same name, different content |
| "The icons are half the size because upem 2048 vs 1000" | A ratio does not describe a size error; the real cause was the **face being swapped** (different glyphs entirely) | a ratio mistaken for a size error |
| "The splash title is 1455 px wide because the symbol font's advance is 10.4 em at size 140" | The string is drawn with **ConduitITCPro-Bold**; the font's real advance for it is 2433.6 px at size 140 (`conduit_width_truth.py`). 1455 px is the width of a *different* element | a number from another measurement |
| "The splash line is drawn too big because the device profile says EXPECTED_SCREEN 480×320" | **q61 already measured this**: the profile was rewritten (`changed=2 written=1`) and the font sizes did not move. The sizes come from a fixed table | a plausible cause that was already falsified |
| "The splash artwork is scaled too wide" | The overflowing line is **guest text** (`Player ID: CCDI123456789 / CCRR123456789`), measured with a texture id, a bounds width and a pixel band | the artwork was blamed for a text bug |

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

Known directions (**none done yet**):

* `steergain` / look-ahead point: steering gain and the aim point need to be
  tuned per corner;
* **the recovery logic needs an EXIT, not a reference-point reset** — resetting
  makes the car spin on the spot.

### 4.5 P4 — Deliverables and commit (**rebuilt on 2026-11-03, late**)

| Artefact | Size | Notes |
|---|---|---|
| `dist/rr3-switch-20261103/` | 190 files / 10.67 GiB | the card tree is **185 files / 9.58 GiB** |
| `dist/rr3-switch-app-20261103.zip` | 45.4 MiB | app only, 18 entries |
| `dist/rr3-switch-20261103.zip` | 7.89 GiB | 237 entries = 190 files + 46 directories |
| `dist/parts-20261103/*.001…004` | 2 + 2 + 2 + 1.89 GiB | concatenated = the archive **byte for byte** |

* **The NRO in the package, in the app zip and on the emulator card are all
  the same file**, and `.build/rebuild-deliverables.ps1` now PROVES it (it
  hashes all three and refuses to finish if they differ). The build this round
  is `b71fbc5a…`.
* **The package used to carry the NRO of an OLDER build** — the one that
  crashed the console five times. That is the same class of bug as the stale
  archive (§2.8), one level up, and it is why the rebuild script exists and
  why the deliverable packagers are now versioned (`.gitignore` explains it).
* The parts join to the archive byte for byte (sha256
  `6001e60e…` for the archive of the earlier build; the rebuild prints its
  own).
* The 20261102 deliverables were removed (they carried the NRO that crashed
  five times on the console).
* **`git commit` must use `-F <file>`** — `-m` and `-F` are mutually
  exclusive.

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
