"""Append section 9.44: the NSP package, and what the new console dump says.

WHY: three things happened that belong together in one section, because
they change what the NEXT console run means:

  1. the app now ships as an NSP, and the packaging has two traps in it
     that are worth writing down;
  2. the console dump the user attached carries the answer to the SPAL
     question in one field -- fault_tpidr_el0;
  3. the controlled round settled the race-count question: the state was
     NOT the variable.
"""
import io
import sys

SECTION = """
### 9.44 The NSP package, the TPIDR_EL0 reading, and what the controlled round settled

#### 9.44.1 THE APP NOW SHIPS AS AN NSP

An NSP installs as a TITLE -- through hbloader or an NSP installer -- and
launches from the Home menu.  An NRO has to be launched by a homebrew menu
every time, and the user has to put the file in the right place by hand.

**THE PACKAGING IS THREE STEPS AND TWO TRAPS.**

    elf2nso   <elf>          -> exefs/main       (the NSO)
    npdmtool  <descriptor>   -> exefs/main.npdm  (the meta)
    <pfs0 writer>            -> rr3-native-rehost.nsp

*TRAP ONE: libnx's own `%.nsp` rule builds the PFS0 from the DIRECTORY
`exefs`.*  `build_pfs0` packs only the TOP LEVEL of its input directory --
it prints "Directories aren't supported, skipping..." for a subdirectory
and does not recurse into it.  So that rule's `.nsp` contains the entries
`main` and `main.npdm`: that is an EXEFS IMAGE, not an NSP.  An NSP's
entries have to be named `exefs/main` and `exefs/main.npdm`, i.e. the
directory layout has to live INSIDE the container.  The container is
therefore written by `tools/make-pfs0.py`, whose format was read back off
`build_pfs0`'s own output (header 0x10 bytes, then count * 0x18 entries of
{u64 offset, u64 size, u32 string offset, u32 parent}, then the string
table, then the data).

*TRAP TWO: npdmtool cannot describe a RomFS section.*  Its descriptor has
no `icon`, no `nacp` and no `romfs` key -- each one was tried and each one
produced a byte-identical 848-byte meta.  So an NSP built this way has no
RomFS and no icon.

**⇒ ⇒ AND THE MISSING ROMFS DOES NOT MATTER, BECAUSE THE PORT ALREADY HAS A
FALLBACK FOR IT.**  Every port asset is read through `resolvePortAssetPath()`,
which tries `romfs:/X` first and falls back to
`sdmc:/switch/rr3-native-rehost/X`; and the guest's own libraries
(`libRealRacing3.so`, `libc++_shared.so`, `libfmodex.so`) and the UI faces
are ALREADY on the card there.  The same fallback is why the game library
is found without a RomFS: `selectGameLibrary()` falls through to
`sdmc:/switch/rr3-native-rehost/libRealRacing3.so`.

**⇒ ⇒ AND THE PACKAGE IS CHECKED, NOT ASSUMED.**  `Pack-NativeSwitchNsp.ps1`
reads the container back after writing it and refuses to report success
unless the two entries are named `exefs/main` and `exefs/main.npdm`.  So
"the build said it worked" can never hide a container whose entries are
the exefs image's.

**⇒ ⇒ WHAT THE NSP DOES NOT CARRY, AND STILL HAS TO BE COPIED: the game's
own data -- the seven volumes and the .depot tree, about 11.8 GiB.**  That
has never been in the executable and it is not in the NSP either; it goes
onto the card as before.

#### 9.44.2 THE NEW CONSOLE DUMP ANSWERS THE SPAL QUESTION IN ONE FIELD

**MEASURED 2026-11-11, the dump the user attached after the run of
2026-11-10:**

    esr=0000000092000005
    pc=0000001abfbb96f4
    lr=0000001abfbb96f0
    far=00000059a492e5d0
    fault_tpidr_el0=000000018eff3e60
    sp=000000018eff39b0
    fp=000000018eff39e0

**=> THE EXCEPTION CLASS IS STILL SPAL** (0x24), and `pc = lr + 4` again.

**=> ⇒ ⇒ AND THE ONE FIELD THAT MATTERS IS `fault_tpidr_el0`: IT IS 0x18eff3e60,
AND THE STACK POINTER IS 0x18eff39b0.**  The thread pointer that names the
thread-local area -- the value every PAC key is derived from -- points
**1,200 bytes ABOVE THE STACK POINTER, INSIDE THE STACK FRAME.**

**=> ⇒ ⇒ ⇒ THAT IS THE PORT'S OWN FAKE TCB.**  `GuestTlsScope`
(`tls_probe.cpp`) fabricates a 256-byte block ON THE STACK and points
TPIDR_EL0 at it, and it is active across the whole guest-constructor
window (the loader's calls at `multi_dso_linker.cpp:2345`, `:2748`,
`:2910`).  On a console, TPIDR_EL0 must name a real thread-local area
whose PAC key material lives there; a stack address for TPIDR_EL0 means
the guest's `autiasp`/`paciasp` computes a PAC against the wrong key.

**=> ⇒ ⇒ ⇒ AND THE GUEST'S CODE IS PAC-COMPILED** (1328 `paciasp` + 1372
`autiasp` in `libc++_shared.so`, section 9.41), so the guest signs and
authenticates pointers on every function entry and exit.  **⇒ THE PORT'S
FAKE TLS IS THE MOST LIKELY CAUSE OF THE SPAL**, and it is a port-side
bug, not a guest bug.

**=> ⇒ ⇒ ⇒ ⇒ THE FIX IS NAMED AND SMALL: the port must not clobber
TPIDR_EL0.**  It has to save the host's value and restore it around the
guest-constructor window, or allocate a real thread-local block for the
guest instead of pointing at the stack.  The next console run is not
needed to find that; it is needed to confirm the fix.

#### 9.44.3 THE CONTROLLED ROUND SETTLED THE RACE-COUNT QUESTION

**THE QUESTION.**  The start lights went 7 (chunk1..chunk3) -> 19 (fps1) ->
27 (yield2) -> **1** (slim1, census2, aa0, fx0, view960) -> 2 (pcall1buf,
loaderdiag1).  The obvious reading is "the port regressed".

**THE EXPERIMENT.**  `run-pinned-round.ps1` SNAPSHOTS the guest's state and
never RESTORES it, so every round began from whatever the previous round
left behind -- and the guest advances to the next event after each race
and writes its popups into the save.  So the round `ctl1` restored
`.build/guest-state/yield2` (the state that produced the 27) and ran the
SAME binary as `loaderdiag1` for the same 400 seconds.

    round          state            binary      event  lights  race  hud  scanf
    yield2         inherited        A           300    27      36    0     0
    loaderdiag1    inherited        B (9543C130) 168    2       2   88   13385
    ctl1           RESTORED yield2  B (9543C130) 167    2       2   88   13385

**=> ⇒ ⇒ AND THE STATE IS NOT THE VARIABLE.**  ctl1 put the SAME binary
back on the state that produced 27, and it came back at 2/2 -- the same
numbers as the round that inherited a drifted save.  **⇒ THE COLLAPSE IS
NOT THE SAVE.**

**=> ⇒ ⇒ ⇒ SO IT IS THE OTHER HALF OF THE A/B: the binary, or the logging
switch that moved with it.**  `loaderdiag1` and `ctl1` ran with
`percall=1` (13,385 guest-scanf lines) and `yield2` ran with `percall=0`
(0).  **⇒ ⇒ ⇒ ⇒ ⇒ THE NEXT EXPERIMENT IS ONE VARIABLE: the SAME binary with
`percall=0`, and then with the path trace restored** -- because the path
trace went from 108,419 lines (105,150 of them asset paths) to 80 lines
when `logResolvedPathOnce()` was thinned to `gui/processed/` only, and
section 9.33 already recorded that the port's own front-end navigation
reads that trace.
"""


def main():
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.44 The NSP package" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended 9.44")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
