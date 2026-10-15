# rr3 DeInfo probe — container-teardown loop trampoline

Probe for the two cursor words of the guest's container-teardown loop, which have
never been readable. One 4-byte patch at guest ELF VA `0xcd2aa0` redirects into a
70-instruction hand-assembled trampoline that copies the words out and steps a
release counter.

**Nothing here is installed by the build.** `rr3_deinfo_probe.cpp` compiles and
links into `rr3_deinfo_probe.o`, but nothing references it yet, so `--gc-sections`
drops it from the NRO. It becomes live only when a caller is added.

| File | Role |
| --- | --- |
| `native-switch/source/rr3_deinfo_probe.cpp` | the frozen byte array + install / uninstall / drain |
| `native-switch/include/rr3/rr3_deinfo_probe.hpp` | public API |
| `tools/rr3-deinfo-probe/trampoline.s` | the assembly the bytes came from |
| `tools/rr3-deinfo-probe/build-trampoline.py` | assemble, disassemble, assert, emit the array |
| `tools/rr3-deinfo-probe/test_trampoline.py` | semantic harness + positive controls |

Regenerate and re-verify with:

```
python tools/rr3-deinfo-probe/build-trampoline.py
python tools/rr3-deinfo-probe/test_trampoline.py
```

## What is actually at the patch site

Read out of the guest ELF, not taken from a brief:

```
0x00cd2a9c  0x940f33df  bl    #0x109fa18          ; the guest's logger
0x00cd2aa0  0xf85b83b4  ldur  x20, [x29, #-0x48] ; <-- the patch site
0x00cd2aa4  0xb4000354  cbz   x20, #0xcd2b0c
0x00cd2aa8  0xf85c03b5  ldur  x21, [x29, #-0x40]
0x00cd2aac  0xeb1402bf  cmp   x21, x20
...
0x00cd2afc  0x17fffff0  b     #0xcd2abc          ; inner back edge
0x00cd2b00  0xf85b83a0  ldur  x0,  [x29, #-0x48]
0x00cd2b04  0xf81c03b4  stur  x20, [x29, #-0x40]
0x00cd2b08  0x94451412  bl    #0x1e17b50         ; operator delete
```

Three corrections to the task premises, all of which change what the probe
should do:

1. **The offset is `-0x48`, not `+0x48`.** `0xf85b83b4` is `ldur x20, [x29,#-0x48]`.
   The word is right; the decode sign was wrong. This is *good* news — it means
   the word the guest loads into `x20` is the very slot the probe captures, so
   captured `b` and post-replay `x20` are the same value by construction. That
   makes the capture a free self-check on the patch landing at the right offset.

2. **The inner loop, not this span, is what iterates.** `0xcd2aa0..0xcd2b08` has
   exactly one backward branch, `0xcd2afc → 0xcd2abc`, and it walks `x23` forward
   by `0x30` until `x23 == x20`. The site is re-entered because it sits inside the
   enclosing 1893-instruction cycle `[0xcd15ec..0xcd35ec]`, closed by `b` at
   `0xcd35ec`. The counter will therefore tick, but the probe is observing an
   *outer* loop's cursor, not an inner one.

3. **`x29` is trustworthy here**, which is what makes the whole probe possible.
   In function `[0xcd1014, 0xcd3648)` the AAPCS prologue does
   `add x29, sp, #0x10` at `0xcd1030` and **never writes `x29` again** across the
   1699 instructions to the patch site. `[x29-0x48]` and `[x29-0x40]` are real
   locals of that frame. `[x29-0x48]` is read-only inside the function (it arrives
   as an argument); `[x29-0x40]` is the node cursor and is overwritten with `x20`
   at `0xcd2b04`.

Also worth knowing: the NZCV the `cbz` at `0xcd2aa4` would have seen is the one
from `bl` at `0xcd2a9c`, and at this site it is **dead** — nothing between
`0xcd2aa0` and the `cmp` at `0xcd2aac` reads flags. It is preserved anyway.

## The trampoline

70 instructions, 280 bytes. Entered by a single unconditional `B`, returns to
`0xcd2aa4`. Calls nothing, takes nothing, never touches FS/TLS: a C call from
here re-enters the bridge with the guest mid-teardown, and the filesystem wrapper
is the thing that must not re-enter.

```
 0x000  sub  sp, sp, #512
 0x004  stp  x0,  x1,  [sp]
 ...    16x stp  x0..x29                    ; x0-x28 paired, x29 second
 0x040  str  x30,     [sp, #240]
 0x044  mrs  x16, nzcv
 0x048  str  x16,     [sp, #248]
 0x04c  stp  q0,  q1,  [sp, #256]
 ...    8x stp q0..q15                       ; FULL 128 bits, not d
 0x06c  ldr  x29, [sp, #232]                ; the guest's frame pointer
 0x070  ldur x9,  [x29, #-0x48]             ; B
 0x074  ldur x10, [x29, #-0x40]             ; E
 0x078  adrp x16 / 0x07c add x16            ; RELOC g_rr3DeInfoB
 0x080  adrp x17 / 0x084 add x17            ; RELOC g_rr3DeInfoE
 0x088  adrp x18 / 0x08c add x18            ; RELOC g_rr3DeInfoSeq
 0x090  str  x9,  [x16]                     ; B first
 0x094  str  x10, [x17]                     ; E second
 0x098  ldr  w9,  [x18]
 0x09c  add  w9,  w9, #1
 0x0a0  stlr w9,  [x18]                     ; release: publishes the pair
 0x0a4  ldr  x16, [sp, #248]
 0x0a8  msr  nzcv, x16
 0x0ac  ldp  x0,  x1,  [sp]
 ...    16x ldp x0..x29, ldr x30
 0x0ec  ldp  q0,  q1,  [sp, #256]
 ...    8x ldp q0..q15
 0x10c  ldur x20, [x29, #-0x48]             ; THE REPLAY == 0xf85b83b4
 0x110  add  sp, sp, #512
 0x114  ret
```

Four orderings in there are load-bearing and easy to get wrong:

- **The replay is last-but-two, after the GPR reload.** The reload restores the
  guest's old `x20`; the replay is what overwrites it. Put the replay before the
  reload and `x20` comes back with the guest's stale value.
- **`msr nzcv` sits before the reload**, because the reload reuses `x16`. Load
  NZCV afterwards and you need a register that is no longer yours.
- **`q`, not `d`.** `stp dN` zeroes bits `[127:64]` of the V register. Saving low
  halves only, and then writing `dN` back on restore, would destroy the guest's
  high halves — a corruption bug introduced by the fix for another one.
- **`add sp` and `ret` carry no `S`**, so NZCV reaches the guest intact.

### Register / flag table

Frame is `sp - 512`. `sp` is restored by `add sp, sp, #512`, so it needs no slot.

| State | Saved at | Restored at |
| --- | --- | --- |
| `x0`-`x28` | `[sp+0]` .. `[sp+224]` (16x `stp`) | `[sp+0]` .. `[sp+224]` (16x `ldp`) |
| `x29` | `[sp+232]` (paired with `x28`) | `[sp+232]` (paired with `x28`) |
| `x30` | `[sp+240]` (`str`) | `[sp+240]` (`ldr`) |
| NZCV | `[sp+248]` (`mrs x16, nzcv`) | `[sp+248]` → `msr nzcv, x16` |
| `v0`-`v15` (full 128 b) | `[sp+256]` .. `[sp+496]` (8x `stp q`) | same (8x `ldp q`) |
| `sp` | — | `add sp, sp, #512` |

Clobbered, deliberately: `x9`, `x10`, `x16`, `x17`, `x18` (scratch), and `x20`
(overwritten by the replay, which the original instruction overwrote too).

**Honest note on what the save/restore actually buys.** The trampoline's own body
clobbers nothing outside that scratch set. So every save/restore pair above is
currently dead weight — it is insurance against a future edit to the trampoline,
not a repair of an existing bug. `build-trampoline.py` proves the sets match; it
cannot prove the restores are load-bearing, because by construction they are not.
`test_trampoline.py`'s positive controls delete the *saves*, not the restores, for
exactly this reason: a deleted restore is invisible, and a test that passes either
way proves nothing.

## Memory model

Writer: `str` B, `str` E, `stlr` counter. Reader: `ldar` counter, then `ldr` B and
E. That is the whole synchronisation story. There is no mutex anywhere, and it
cannot observe a fresh counter paired with a half-written pair.

`g_rr3DeInfoSeq` is incremented with a plain load / add / `stlr`, not an atomic
increment. Two concurrent writers could lose a count. That is deliberate and
acceptable: the design assumes one writer — the guest thread that owns this code
path — and a lost increment can only cost a sample, never produce a torn pair.
ARMv8.0 (this build's `-march`) has no LSE, so a real atomic increment would mean
an `ldxr`/`stxr` loop inside the trampoline, which is more surface on a live code
path than the failure mode is worth.

## Install

`rr3DeInfoInstall(runtimeVa, &report)` — idempotent.

1. Reject a misaligned site.
2. **Fail closed** unless the 4 bytes still read `0xf85b83b4`. A mismatch means
   the rebase moved or the build differs; patching anyway would write a branch
   over live guest code.
3. `jitCreate(&jit, 0x1000)` — the relocated trampoline must be both writable and
   executable, and Switch/Eden cannot produce RWX in one mapping. libnx's JIT
   gives an RW alias to write through and an RX alias to branch to, which is the
   pattern `jit_probe.cpp` already uses.
4. Copy the frozen array into the RW alias; rewrite the three `adrp`/`add` pairs
   to the slot addresses, after checking each sentinel word still matches the
   assembler output.
5. `armDCacheFlush(rw)`, `armICacheInvalidate(rx)`, `jitTransitionToExecutable`.
6. `encodeBranch(site, jitRxAddr)` — computed from the **actual runtime** address.
   A single `B` reaches ±128 MiB. Out of range is reported as
   `kDeInfoBranchOutOfRange`; the patch is **not** widened into instructions
   nobody has analysed.
7. `svcSetMemoryPermission(page, 0x1000, Perm_Rw)`.
8. Write the `B`; `armDCacheFlush(site, 4)`; `armICacheInvalidate(site, 4)`.
9. `svcSetMemoryPermission(page, 0x1000, Perm_Rx)`.

The I-cache invalidate is **required**, not defensive. The guest may already have
`0xf85b83b4` in the I-cache for that line; without the invalidate it would keep
executing the original instruction forever, with no visible cause.

### Failure path

Every step before 7 is reversible with a `jitClose`. At step 8 the bytes are live,
so if step 9 fails the original word is written back, flushed, invalidated, and
the page is reprotected, and the function returns `false` with
`kDeInfoInstallRestored`. A half-installed patch is never left in the guest.

### Deviation from the brief: the restore permission is `Perm_Rx`, not `Perm_R`

`Perm_Rx = Perm_R | Perm_X = 0x5`, `Perm_R = 0x1` (libnx `kernel/svc.h`), and
`svc.h` documents the call as *"Perm_X is not allowed … This can be used to move
back and forth between Perm_None, Perm_R and Perm_Rw"* — i.e. **it will not hand
back X**. Meanwhile `permissionForFlags` (`multi_dso_linker.cpp:1908`) maps
executable segments to `Perm_Rx`, so guest `.text` *is* `Perm_Rx`.

Restoring `Perm_R` would therefore leave the page the guest is executing on
non-executable. `Perm_Rx` strictly dominates: a no-op if the kernel kept the X bit
sticky, a repair if it did not. `report.permRestoreMode` records which path ran.

The same question applies to the already-measured `guesttext-perm-result` probe
(`guest_view_probe.cpp:3019`): it restored `Perm_R` and called that "leave no
trace". Whether that run's guest `.text` remained executable is **not** established
by that measurement. It has not been changed here — it is outside this task's
scope — but it should not be cited as evidence that the RW round trip is benign.

`guestCodeWritableAddress()` (`multi_dso_linker.cpp:279`) would be the cleaner
route if it worked, but `guest_view_probe.cpp:2983` records that this guest goes
through `mapNormalSegmentedRange`, so the table walks back empty and there is no
owner alias to use.

## Verification

**No AArch64 code has been executed.** This machine is x86-64 Windows; there is no
qemu-aarch64 and no aarch64 emulator, and devkitA64 is a bare-metal cross
toolchain. What was actually run:

| Check | How | Result |
| --- | --- | --- |
| Patch-site bytes | read from `libRealRacing3.so` with pyelftools | `0xf85b83b4` at `0xcd2aa0` |
| Encoding / disassembly | `aarch64-none-elf-as` + `objdump` | 70 insns, 280 bytes |
| Replay word | assembler output vs guest word | `0xf85b83b4` == `0xf85b83b4` |
| Frozen array not hand-edited | `build-trampoline.py` byte-compares the `.cpp` array against the assembler | identical |
| Save/restore sets, NZCV, no calls/branches, store ordering | `build-trampoline.py` static assertions | pass |
| Semantics of the shipped bytes | `test_trampoline.py`: capstone-decoded interpreter over the relocated bytes | pass |
| Positive controls | 8 deliberate mutations, all must fail | all 8 fire |
| Build | `tools\Build-NativeSwitch.ps1` | exit 0, no warnings |
| Contract test | `tools\Test-RR3Inputs.ps1` | exit 0 |

The interpreter in `test_trampoline.py` is a **semantic model, not a CPU**. It
decodes with capstone (an independent implementation, so the bytes are checked
through a second opinion rather than the arithmetic that produced them) and
executes against a sparse memory model and a register file. It does not model
caches, the pipeline, speculative load, or the icache/dc split.

### Not verified

- Actual execution on Switch/Eden. Every "the guest sees X" claim above is an
  argument from the instruction semantics, not an observation.
- Whether `svcSetMemoryPermission` keeps the X bit sticky across a `Perm_Rw`
  round trip. `Perm_Rx` is used precisely because this is unknown.
- Whether the trampoline is within `B` range of the guest page. Depends on the
  JIT allocator and the guest image base; reported at install time as
  `report.branchDelta`, not assumed.
- The release/acquire ordering is asserted **statically** (instruction order),
  not dynamically. A sequential interpreter cannot demonstrate a reordering bug;
  that would need two threads.
