"""Rewrite section 9.41 (the version that got the attribution wrong) and
append section 9.42.

WHY: the first version of 9.41 said the console crash was a SPAL inside the
PORT'S OWN CODE, and put it in the seven-DSO loader.  That was wrong, and
the evidence is measured, not argued:

  * the port's NRO carries exactly TWO PAC/AUT instructions (both in
    libunwind's uw_update_context), so the port's own code has almost no
    pointer authentication to fail;
  * the NRO base implied by the crash pc is not page-aligned, and an NRO
    base must be;
  * the guest's libc++_shared.so carries 1328 paciasp and 1372 autiasp.

And 9.42 records what the 60 FPS question actually measured, plus three
things that did not look like what they were.
"""
import io
import sys

MARK = "### 9.41 The console crash"

SECTION_941 = """
### 9.41 The console crash: a SPAL, and the first version of this section got the attribution wrong

**MEASURED 2026-11-10 (the console run the user attached): crash-early.txt,
guest-view.log and teardown-repair.log.**

**=> THIS SECTION CORRECTS ITSELF.  The first version of it said the fault
was a pointer-authentication failure in the PORT'S OWN CODE and put it in
the seven-DSO loader.  That was wrong, and the reason it was wrong is
measurable.**

**=> AND THE FAULT IS A SPAL.**  `esr=0x92000005`; the exception class is
`(esr >> 26) & 0x3f` = **0x24 = SPAL**, a pointer-authentication failure.
The repository's own note read this ESR as "data abort, translation fault",
which is `0x4E000005` and NOT this value -- the wrong reading sent the
search after a translation fault instead of after a pointer authentication.

**=> ⇒ ⇒ AND THE FAULT IS NOT IN THE PORT'S OWN CODE.**  The port's NRO
carries **exactly TWO** PAC/AUT instructions, both in libunwind's
`uw_update_context` (file offsets 0x5c7e2c and 0x5c7e4c), in the current
ELF and in the crashed binary alike.  A SPAL needs a pointer-authentication
check to fail; the port's code has almost none.  And the NRO base implied
by the crash pc (`0x6d478dc508` / `0x6d478dc4e8`) is **not page-aligned**,
and an NRO base must be.  **⇒ THE pc IS IN A GUEST IMAGE**, mapped into
code memory by the loader -- not in the port.

**=> ⇒ ⇒ AND THE GUEST'S CODE IS POINTER-AUTHENTICATED.**  The APK's
`libc++_shared.so` carries **1328 `paciasp` and 1372 `autiasp`**
(`libRealRacing3.so`: 2; `libfmodex.so`: 0).  So the guest is
PAC-compiled code being run on a console that enforces PAC, by a port that
has no PAC context for it.

**=> ⇒ ⇒ AND "IT CRASHED IN THE LOADER" WAS ALSO UNSOUND.**  `appendGuestBootLine`
opens the log in APPEND mode, so the log's last line can be the PREVIOUS
session's tail: the loader's first line only appears once it starts
mapping.  So "the last line is `gl-ctx-early ... disabled by conf`, and the
next statement is `loadLinkElfSet()`" does not place the crash.

**=> AND TWO EARLIER READINGS WERE WRONG TOO:**
* `create=00000000 exec=00000000 close=00000000` on the JIT probe reads
  like three failures and is not: a libnx `Result` of 0 is SUCCESS, and the
  image-sized JIT probe passed all three steps.
* The teardown log is NOT the crash scene: every line is `HOP-TRACE` and the
  last is `pass=1 expected=1 checks_passed=38 checks_failed=0` -- the port's
  own stack-unwinding self-test, which PASSED.  The crash is after it.

**=> ⇒ ⇒ SO BOTH SIDES GET AN INSTRUMENT, AND THE NEXT CONSOLE RUN ANSWERS
THE QUESTION IN THE FILE THE USER ALREADY ATTACHES.**

1. **THE LOADER SAYS IT ITSELF, AND ITS LINES REACH THE CARD.**
   `multi_dso_linker.cpp` writes, before each segment mapping, the DSO, the
   segment, the offset, the size and the permission; on failure, the stage,
   the Result and the actual and expected permissions; and **a PAC census
   per DSO** (`pac=` and `aut=` counts over the image's own bytes), because
   whether the guest's pointer authentication comes along for the ride is
   the difference between "the loader broke" and "the guest is
   authenticated and the port is not".  The budget is 320 lines and the
   budget says so when it runs out.
   **⇒ AND EVERY ONE OF THOSE LINES IS FLUSHED IMMEDIATELY.**  The log's
   normal policy is 64 lines and a bulk flush, which is right for the frame
   loop and WRONG here: these lines are written once, at boot, and the thing
   they exist to explain is a crash -- so a crash inside the loader would
   have taken the loader's own evidence with it, which is exactly what
   happened on 2026-11-10.

2. **THE CRASH DUMP SAYS WHOSE CODE FAULTED.**  It now carries the decoded
   exception class (`ec=0x..`) and, for `pc` and `lr`, the attribution:
   the DSO and the offset, or `port-nro+0x...`, or `unmapped`.  One line
   each, written before the process goes down.

**=> ⇒ ⇒ ⇒ THE NEXT CONSOLE RUN NAMES THE FAILING DSO AND SEGMENT, SAYS
WHETHER THE GUEST'S CODE IS PAC-COMPILED, AND SAYS WHOSE CODE THE FAULTING
pc IS IN -- all in the guest-view log and the crash file the user already
attaches.**

**=> AND ONE MORE MEASUREMENT OUT OF THAT RUN: THERE WERE NO CENSUS LINES**,
because the crash happens before the frame loop starts -- so the console's
`tt=` (the 60 FPS number) is still unmeasured, and it can only be measured
after the loader survives.
"""


SECTION_942 = """
### 9.42 The 60 FPS number, and three things that did not look like what they were

**MEASURED 2026-11-10: the archived Eden rounds, the frame budget from the
census line, and a fresh 400-second round on the current build.**

#### 9.42.1 THE 60 FPS NUMBER: THE PORT'S OWN OVERHEAD IS ZERO

The census line carries the frame's three costs: `tt` (the whole frame),
`tr` (the guest's own render), `ts` (the swap) and `tp` (the port's own
overhead, `tt - tr - ts`).  The rounds split by phase -- a frame that is
waiting for an asset (`fs > 0`) is a LOADING frame, and averaging the two
phases describes neither.

    round            phase   n      tt p25  tt med  tt p75   tr med   tp med   jni med   tt <= 17 ms
    yield2          render  1668     29       66      118        37        0         7        222/1668  13%
    yield2          loading  678     54      168      452       157        0         8         24/678    4%
    pcall1buf        render  1503     32       51       92        40        0        11        156/1503  10%
    pcall1buf        loading  514     79      183      649       168        0        10         11/514    2%
    slim1            render  1877     30       40       95        37        0         8        197/1877  10%
    slim1            loading  501    104      181      808       168        0         8         17/501    3%

**=> ⇒ ⇒ `tp` -- THE PORT'S OWN COST -- IS ZERO IN EVERY ROUND AND EVERY
PHASE.**  The port adds nothing measurable to the frame.  The frame is the
guest's own render (`tr` = 37-40 ms) plus the emulator's own overhead.

**=> ⇒ ⇒ SO THE PORT SIDE OF THE 60 FPS QUESTION IS CLOSED, AND THE REST OF
THE QUESTION IS NOT.**  On Eden the guest renders in 37-40 ms; on a real
Maxwell GPU it will be much cheaper, and only a console measurement says by
how much.  **THE ONLY HONEST 60 FPS CRITERION IS THE `tt=` OF A CONSOLE
RUN**, and it is the same instrument the census already prints.  What the
port still owns is the guest's QUALITY SETTINGS (README 10.10), not the
frame's cost.

#### 9.42.2 THE RESOLUTION CACHE IS DEAD CODE, AND THE FILESYSTEM IS A QUARTER OF THE ROUND

The round's filesystem service cost **99,120 / 110,605 / 120,044 ms** in
slim1 / yield2 / pcall1buf -- a quarter of a 400-second round -- and in the
SAME rounds the resolution cache's counters never moved ONCE (max hit 0,
max miss 0, over every census line with `fs > 0`).

**=> ⇒ ⇒ THE CACHE IS ON THE WRONG FUNCTION.**  The 1024-entry cache is on
`resolveAssetLocalFile()`, the BOOT-time list builder.  The hot path is
`resolvePath()`, which the guest's own asset requests go through.  So the
cache has never once answered a request.

**=> ⇒ ⇒ AND UNTIL THE TIME IS SPLIT, "THE CACHE DOES NOT HELP" AND "THE
CACHE IS IN THE WRONG FUNCTION" ARE THE SAME SENTENCE WITH OPPOSITE FIXES.**
So the census now prints `fsr=` -- the resolution inside the service -- next
to `fs=`, and `fs - fsr` is the I/O.  One round says which.

#### 9.42.3 THE CENSUS PRINTED A FIELD THAT DOES NOT EXIST

The census line read `fs= fshit= fhit= fsmiss=`, and the arguments were
`fs, fshit, fshit, fsmiss`: **`fhit` printed `fshit`'s value**, because
there are only two filesystem counters, not three.  A reader could not
tell the two apart, and the cache's hit rate could not be computed from the
log at all.  The line is now `fs= fsr= fshit= fsmiss=` with the two real
counters.

#### 9.42.4 THE ROUNDS WERE NEVER CONTROLLED FOR THE GUEST'S OWN STATE

**THIS IS THE ONE THAT MATTERS FOR THE RACE COUNTS.**  The rounds' start
lights went 7 (chunk1..chunk3) -> 19 (fps1) -> 27 (yield2) -> **1** (slim1,
census2, aa0, fx0, view960) -> 2 (pcall1buf, and the fresh loaderdiag1
round).  The obvious reading is a regression in the port.

**=> ⇒ ⇒ BUT `run-pinned-round.ps1` SNAPSHOTS THE GUEST'S STATE AND NEVER
RESTORES IT.**  It takes a snapshot under the round's own name before the
round starts, so every round begins from whatever the PREVIOUS round left
behind.  And the guest persists: it advances to the NEXT EVENT after each
race, and it writes its popups into the save.  So each round inherited its
predecessor's progress -- which is the same trap the project has already
paid for once ("a 2x2 with one uncontrolled cell is not a 2x2").

**=> ⇒ ⇒ SO THE RACE-COUNT COLLAPSE CANNOT BE ATTRIBUTED TO CODE YET.**
The controlled experiment is now defined and cheap:
`.build/guest-state.ps1 -Restore -Tag yield2` (that snapshot exists, and it
is the state that produced 27 start lights and 36 race screens), then the
same 400 seconds on the same build.  If the numbers come back, the code did
not regress and the rounds were measuring the save.  If they do not, the
thinning at `logResolvedPathOnce()` is the suspect: the path trace went
from **108,419 lines (105,150 of them asset paths) in yield2 to 80 lines**
(census2, pcall1buf, loaderdiag1) because it is now thinned to
`gui/processed/` only.

#### 9.42.5 AND THE GUEST IS ASKING FOR A TRACK DETAIL MAP IN A SPELLING THAT IS NOT SHIPPED

The fresh round's guest shows an error: its own Java string is
**"加载轨道数据时出错，请重新安装！"** ("error loading track data, please
reinstall"), which reaches the port's log as GBK mojibake -- a port-side
log-encoding detail, not the game's own text.

**MEASURED: the guest asks for `detailmap_road_surface_norm_a` in four
spellings -- `.dxt.dds.z`, `.dxt.dds`, `.etc.dds.z`, `.etc.dds` -- across
three guest roots and two passes: 24 probes, every one of them ENOENT.**
Meanwhile the published asset list (`asset_list_track_common.txt`, both the
original and the port's rewritten copy, which agree) carries exactly ONE
entry for that leaf:

    /Tracks/common/detail/detailmap_road_surface_norm_a.rgb.pvr.z  84b9989df0a3e5cc7964844e56a9a263  522565  522565

and the card's loose tree carries exactly that file, 522,565 bytes.  Every
OTHER detailmap on the card is `.etc.dds.z`; this one, and only this one,
is `.rgb.pvr.z`.

**=> ⇒ ⇒ SO THE SHIPPED SPELLING AND THE PROBED SPELLINGS DISAGREE FOR THIS
ONE LEAF, AND THE PORT ANSWERS ENOENT FOR ALL FOUR.**  The whole round
logged **46,789 `stat-path` failures with errno=2**.

**=> ⇒ ⇒ THIS IS NOT YET ATTRIBUTED, AND IT IS NOT YET FIXED.**  What is
NOT the cause: the list is correct, the file is on the card, and the port
routes the path to the right volumes.  What is open: whether the guest
walks a format list that the port's published list is supposed to extend,
or whether this asset needs a format conversion the pipeline does not do.
The next instrument is the one that answers it: a count, per leaf, of the
spellings the guest asked for against the spellings the card holds.
"""


def main():
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    index = text.find(MARK)
    if index < 0:
        print("9.41 not found")
        return 1
    head = text[:index].rstrip("\n")
    text = head + "\n" + SECTION_941 + SECTION_942
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("rewrote 9.41 and appended 9.42")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
