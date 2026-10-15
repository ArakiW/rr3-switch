# Guest analysis tools

Scripts for working on the rehosted Real Racing 3 guest binary
(`reverse/native/arm64/libRealRacing3.so`, AArch64 PIE, `.symtab` stripped,
`.dynsym` intact) and for reading files out of the shipped data volumes.

They live here rather than in `.build/` because `.build/` is scratch and gets
cleaned. Each of these cost real debugging to get right; re-deriving them is
not free.

## `guestfde.py` — the ELF layer

Import it; do not fork it.

```python
import sys; sys.path.insert(0, r'E:\AI\RealRacing3-switch\tools\guest-analysis')
import guestfde as G

G._range(va)        # (start, end) exact function range, or None
G.insns(va)         # disassemble the whole function; n only ever SHRINKS it
G.calls(va)         # unique direct bl targets: [(call_site, target)]
G.back_branches(va) # backward branches, epilogue targets excluded
G.name(va)          # .dynsym name, or None
G.plt_name(va)      # PLT stub -> imported symbol, via the GOT slot it loads
G.sym(va)           # name(va) or plt_name(va)
```

**Two things here are not obvious and were each wrong before they were fixed:**

- **Function ranges come from `.eh_frame_hdr`'s binary-search table**, whose
  entries hold fully-decoded absolute `initial_location` values. Do not walk
  `.eh_frame` CIE/FDE records to re-derive them: this binary's `CIE_pointer`
  base is not resolvable the way the spec suggests, and
  `initial_location`'s pcrel base is `rec_body` (after the length field),
  not the length field itself. The `address_range` is an absolute sdata4 and
  needs no base at all, so the table plus that one field is sufficient. Real
  `fde_count` is **84636**.

- **PLT slots must be resolved through the GOT slot the stub loads**
  (`adrp x16,page ; ldr x17,[x16,#off] ; br x17`). Indexing `.rela.plt` by
  `plt_base + i*16` produces plausible but wrong names — it reported
  `poll` as `listen` and `usleep` as `powf`. Cross-checking against
  "well, `powf` in a render function is plausible" is not verification; that
  is a coincidence you selected after the fact.

## `scc.py` — the only correct "is there a loop here"

```python
python scc.py 0x12aea7c:render 0x1105160:state_machine
```

Tarjan over the function's own instructions, with `ret` treated as a
terminator with no successors and a tail-call `b` as a transfer out of the
function. Reports SCC count and the real cycles (size > 1, or a self-edge).

**Do not use `back_branches()` counts to decide whether a loop exists.** It
counts three different things as loops on this binary: branches to an
epilogue (a *return*), shared continuation points (a second pass), and
switch-table jumps. Measured on the render function: 8 "back edges" by that
count, **0 real cycles** by SCC. That gap produced a wrong root cause three
times.

`back_branches()` is still fine as a *lead* — it just cannot be the verdict.

## `romfs-read.py` — reading a file out of a data volume

```python
python romfs-read.py <volume.romfs> files/.depot/no3g.xml
```

Two details that are easy to get wrong and produce text that looks fine while
coming from the wrong place:

- Header fields `[3]` / `[7]` are already **byte offsets**; they are not
  record counts to multiply by 32.
- A file record is `struct "<IIQQII"` — 6 fields, 32 bytes. `data_offset` is
  relative to header field `[9]`, which is `0x200`.

This is the verified layout from `tools/verify_rr3_data_volumes.py`; follow it
rather than re-deriving.

## `fix-errno.py` — one-off, already applied

Rewrote guest-visible `errno = EXXX;` in the shim layer to the bionic
numbering now in `native-switch/source/bionic_errno.hpp`. Kept because the
same divergence will bite any new shim; re-run it after adding files.

The short version: newlib and bionic agree for errno 1..34 and diverge from
35 onward, because newlib carries its own `EL2NSYNC`/`EL3HLT`/`EL3RST` block.
`ENAMETOOLONG` is 91 in newlib and 36 in bionic; `ENOSYS` 88 vs 38;
`ENOTSUP` 134 vs 95; `ETIMEDOUT` 116 vs 110. The shims used all four.

## `find-4fc.py` — a one-shot field writer scan

Enumerates stores to a chosen struct offset across all of `.text`. It is a
shortlist generator, not a conclusion: a shared field offset produces hits in
unrelated types, and every hit still needs a reachability check.

## `start-elfva.py` — guest thread entry points, from the census log

Reads `guest-view.log`, recovers the guest image base by subtracting a
thread's already-reported `lr_elfva` from its runtime `lr`, and translates
every thread's `start=` pointer with it. Two independent concerns that both
looked unresolvable before this existed:

- a guest thread's `start` is a runtime pointer, not an ELF address, so it
  cannot be compared against anything in the disassembly until the base is
  known — and the base is only obtainable from the `lr`/`lr_elfva` pair on the
  same line;
- the census is the only place a thread's entry routine is recorded at all.

Sanity check: the base it derives must be identical across every ordinal in
the run, and `start - base` must land on a function start (equal to its own
FDE start). If either fails, the log means something other than it appears to.

## `ttf-cmap.py` — is this font actually usable

```python
python ttf-cmap.py            # run against .build/r3_symbols.ttf
```

Parses the sfnt table directory, checks that every table a rasteriser needs
is present, then walks the `cmap` format-4 segments and reports total codes
mapped plus a printable-ASCII coverage count.

Worth running before blaming a renderer for "text does not show up": a font
can be structurally perfect and still map nothing, and the two failures look
identical from the game side. This project had a note claiming
`r3_symbols.ttf` could not be extracted — the file was fine the whole time,
the recorded offset was out of range by one digit.

## `eden-tail.py` / `eden-faults.py` — reading the emulator's own log

Eden's log is one rolling buffer, not a per-run file, and on a wedged run it
is ~100 MB of `core/memory.cpp: Unmapped ...` lines. `eden-tail.py` seeks to
the last N MB and buckets by severity; `eden-faults.py` then counts distinct
fault addresses, finds the longest `+1` byte-scan run (a pointer that is
really text being walked), and clusters by 64 KiB.

The byte-scan run is the useful signal: a long monotonic `+1` read sequence
means something is iterating a string as if it were an address, which is a
data-corruption symptom and not a rendering one.


## `render-head.py` / `fsm-writers.py` — the frame state machine

```python
python render-head.py 0x12aea7c 50    # annotate the render entry's opening
python fsm-writers.py 0x12aea7c       # every read/write of activity+0x4a4..0x4e0
```

`render-head.py` marks `hasFocus` (+0x4d1), `frameCounter` (+0x4d4),
`frameFsm` (+0x4d8) and the two size pairs the entry compares. `fsm-writers.py`
lists stores in that whole range, which is how the cycle 0 -> 1..3 -> 4 -> 5 -> 0
was mapped.

Two readings that look like findings and are not:

- **`str w8, [x19, #0x4d8]` right after `ldr w8, [x19, #0x4d8]`** at 0x12aeba0
  is not a self-store. There is an `add w8, w8, #1` at 0x12aeba4 between them;
  a filter that prints only lines matching `[xN, #imm]` drops it and makes the
  increment look like a no-op.
- **The gate `0x4a8 != 0x4b0 || 0x4ac != 0x4b4` is not a per-frame gate.** Both
  pairs stay equal (the host passes one resolution forever), so `0x4d4` is
  cleared on every call and `bl 0x12ae290` is never reached -- but the actual
  per-frame work is `bl 0x12b4824` further down and it *does* run every call.
  It is a size-change debounce, not the thing that hangs.

The runtime counterpart is the `GATE` field on the `wedge: step=loop-top` line
in `guest-view.log`; see PORT_PROGRESS.md for the measured values.

## Where the wedge actually is

`0x12b4824` is 284 bytes and does little but call `0x1105160` at 0x12b48f0 --
unconditionally, every frame. `0x1105160` is 17832 bytes / 4458 instructions
whose only stdio calls are `fopen` / `fwrite` / `fclose`: **the save write, in
the per-frame function.** `bionicFwrite` in `libcxx_shims.cpp` now logs a short
write (requested vs returned, `ferror`, `errno`) because that is the one number
in this chain that has never been observed, and a short write is the standard
way a write turns into a retry loop.
## `xref-str.py` — who references a .rodata string

```python
python xref-str.py 0x47e0f8 [radius]
```

Finds every `adrp` + `add` pair that materialises a .rodata address and prints
the enclosing FDE. This is how `SaveManager.cpp:699` was traced to
`0x13a4468` and how the page-wide inventory came out: **91 directly referenced
strings on page `0x47e000` alone**.

**Two lessons, both paid for:**

- **ADRP is `Align(PC, 4096) + imm*4096`.** Using `pc + (imm << 12)` gives a
  result that is never page-aligned, so comparing it to a page always fails and
  the tool reports zero references for a page with 150 of them. The bug hid
  because the first cross-check applied `& ~0xFFF` to its own output, which
  rounded the wrong value back onto a plausible page and agreed with the broken
  number. **If two computations of one quantity disagree, find which is wrong
  before believing either — and notice when the "reliable" one is the one with
  a normalising step.**
- **Adjacency in .rodata means nothing.** The guest's fatal dialog string and
  `SaveManager.cpp:699` are 462 bytes apart in a packed page, which read like a
  causal link and was wrong. A string table groups by size and locality. Only a
  confirmed xref establishes a relationship; on this binary the dialog's own
  raise site is still unknown.

`FUN_0119f730(level, "file:line", "message")` is the logger, and its first
string argument is the full build path, e.g.
`E:\dev\builds\r3_update_a\source\src\Save\SaveManager.cpp:699`. The **second**
argument is the actual message and is the one worth reading — that is how
`SaveManager.cpp:699` turned out to be "You can not load a save game on a non
main thread", not the error being investigated.