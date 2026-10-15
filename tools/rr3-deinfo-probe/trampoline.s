// rr3_deinfo_trampoline.s
//
// The AArch64 trampoline for the container-teardown wedge at guest ELF VA
// 0xcd2aa0. Assembled OFFLINE; the resulting bytes are frozen into
// native-switch/source/rr3_deinfo_probe.cpp as kTrampolineCode.
//
// This file is NOT part of the Switch build (it lives under tools/, not
// native-switch/source/) precisely so that the NRO keeps one single, auditable
// copy of the code: the byte array, not a second assembly of the same text.
//
// Regenerate with tools/rr3-deinfo-probe/build-trampoline.py.
//
// WHY THERE ARE NO ADRP PAIRS AND NO `ret` ANY MORE
// -------------------------------------------------
// jitCreate() places this code 12.4 GB away from the guest site (measured:
// site 0x2106a2baa0, trampoline 0x23e8cc1000). A single `B` reaches +/-128 MiB
// and ADRP+ADD reaches +/-4 GiB, so the previous design was unreachable twice
// over and deinfo-install failed closed on every run. Every address that is not
// PC-relative within this blob is therefore reached through a 64-bit literal
// loaded by `ldr xN, =const`: the literal load itself only has to span this
// 344-byte blob, and the 64-bit constant it holds has no range limit at all.
// There is deliberately not one distance-dependent reloc left in the file.
//
// Contract this file exists to satisfy:
//   * entered from guest 0xcd2aa0 via the 16-byte LDR-literal + BR stub that
//     install() writes (see rr3_deinfo_probe.cpp), which is position
//     independent and can cross any distance;
//   * replays the FOUR guest instructions the stub eats -- 0xcd2aa0, 0xcd2aa4,
//     0xcd2aa8, 0xcd2aac -- with byte-identical encodings, except that the
//     `cbz x20, 0xcd2b0c` at 0xcd2aa4 is re-expressed as an absolute transfer,
//     because a PC-relative displacement cannot mean the same thing 12.4 GB
//     from where it was written;
//   * resumes the guest at 0xcd2ab0 (site+16), again by absolute transfer;
//   * touches no C code, takes no lock, and never touches FS/TLS.
//   * preserves every GPR x0-x30, sp, the full 128 bits of v0-v15, and NZCV,
//     except x16/x17/x18 -- see "SCRATCH REGISTERS" below.

	.arch armv8-a
	.text
	.align 4

	.global rr3DeInfoTrampoline
	.type rr3DeInfoTrampoline, %function
rr3DeInfoTrampoline:

// ---------------------------------------------------------------- save phase
// Frame is 512 bytes. sp stays 16-byte aligned (512 = 32*16), which the
// SP-alignment check requires before any SP-relative access below.
//
//   sp'+  0 .. 240 : x0 .. x30
//   sp'+248        : NZCV          (31 GPRs cannot be stp-paired evenly)
//   sp'+256 .. 511 : q0 .. q15     (full 128 bits, not just d)
//
// q0-q15 rather than d0-d15 on purpose: a `stp dN` store ZEROES bits [127:64]
// of the V register. Saving low halves only and then writing dN back would
// destroy the guest's high halves, which is a corruption bug this file exists
// to avoid. Full-width save/restore costs 128 extra bytes of frame.

	sub	sp, sp, #512
	stp	x0,  x1,  [sp, #0]
	stp	x2,  x3,  [sp, #16]
	stp	x4,  x5,  [sp, #32]
	stp	x6,  x7,  [sp, #48]
	stp	x8,  x9,  [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30,     [sp, #240]
	mrs	x16, nzcv
	str	x16,     [sp, #248]
	stp	q0,  q1,  [sp, #256]
	stp	q2,  q3,  [sp, #288]
	stp	q4,  q5,  [sp, #320]
	stp	q6,  q7,  [sp, #352]
	stp	q8,  q9,  [sp, #384]
	stp	q10, q11, [sp, #416]
	stp	q12, q13, [sp, #448]
	stp	q14, q15, [sp, #480]

// ------------------------------------------------------------- capture phase
// x29 is a live AAPCS frame pointer here: the containing function
// 0xcd1014..0xcd3648 establishes it with `add x29, sp, #0x10` at 0xcd1030 and
// never writes it again. The two loop cursors are that frame's own locals:
//   [x29-0x48] = the sentinel the walk compares against (guest 0xcd2aa0)
//   [x29-0x40] = the node currently being examined   (guest 0xcd2aa8)

	ldr	x29, [sp, #232]		// guest frame pointer
	ldur	x9,  [x29, #-0x48]		// B
	ldur	x10, [x29, #-0x40]		// E

	ldr	x16, litB		// RELOC64 g_rr3DeInfoB
	ldr	x17, litE		// RELOC64 g_rr3DeInfoE
	ldr	x18, litSeq		// RELOC64 g_rr3DeInfoSeq

// The pair must be fully visible before the counter moves; the reader pairs
// its `ldar` on the counter with the release store below, so the only ordering
// that matters is data-then-counter, which is what this order gives.
	str	x9,  [x16]			// B
	str	x10, [x17]			// E
	ldr	w9,  [x18]
	add	w9,  w9, #1
	stlr	w9,  [x18]			// release: publishes the pair

// ------------------------------------------------------------- restore phase
	ldp	x0,  x1,  [sp, #0]
	ldp	x2,  x3,  [sp, #16]
	ldp	x4,  x5,  [sp, #32]
	ldp	x6,  x7,  [sp, #48]
	ldp	x8,  x9,  [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30,     [sp, #240]
	ldp	q0,  q1,  [sp, #256]
	ldp	q2,  q3,  [sp, #288]
	ldp	q4,  q5,  [sp, #320]
	ldp	q6,  q7,  [sp, #352]
	ldp	q8,  q9,  [sp, #384]
	ldp	q10, q11, [sp, #416]
	ldp	q12, q13, [sp, #448]
	ldp	q14, q15, [sp, #480]

// ------------------------------------------------- replay of the eaten window
// The 16-byte stub overwrote guest 0xcd2aa0..0xcd2ab0, i.e. these four words:
//
//   0xcd2aa0  f85b83b4  ldur x20, [x29, #-0x48]
//   0xcd2aa4  b4000354  cbz   x20, 0xcd2b0c
//   0xcd2aa8  f85c03b5  ldur x21, [x29, #-0x40]
//   0xcd2aac  eb1402bf  cmp   x21, x20
//
// Three of them are position independent and are replayed with byte-identical
// encodings, so the two loads and the compare are provably the guest's own
// instructions. The fourth is not: `cbz` is PC-relative, so the same 19-bit
// displacement at this address would land somewhere in the middle of nowhere.
// It is re-expressed below as the absolute transfer it means -- branch to
// 0xcd2b0c when x20 == 0, otherwise fall through -- which is the same
// predicate with no distance limit.
//
// SCRATCH REGISTERS. x16, x17 and x18 are clobbered by this block after the
// reload above, and the replay does not restore them. That is safe here for a
// reason that was measured, not assumed: disassembling the whole guest function
// 0xcd1014..0xcd3648 yields ZERO mentions of x16, x17 or x18. They are the
// linker's intra-procedure-call temporaries and this code never uses them. Any
// future change to the replay must re-check that scan before relying on it.
//
// NZCV, AND WHY THE RESTORE IS WHERE IT IS. Deciding the cbz needs a compare,
// and a compare sets flags. Left alone, the cbz-taken path would hand the guest
// 0xcd2b0c the flags of `cmp x20, #0` instead of the flags the guest was
// actually carrying -- a silent corruption of somebody else's code, on a path
// that then runs a long way without any instruction that happens to read them.
// So the saved NZCV is restored AFTER the select has consumed the compare's
// flags and BEFORE the branch that leaves, which fixes both paths at once:
//   * cbz taken  -> the guest keeps exactly the NZCV it walked in with, because
//                   the replayed `cmp x21, x20` below is never reached;
//   * fall-through -> the replayed `cmp x21, x20` is the last flag-setter, so
//                   the guest's `b.ne` at 0xcd2ab0 sees precisely the flags the
//                   unpatched guest would have produced.
// `sp` is still this trampoline's own frame at the `ldr [sp,#248]`, which is why
// `add sp` sits after the `msr` rather than at the top of the block.

	ldur	x20, [x29, #-0x48]		// == 0xf85b83b4, guest 0xcd2aa0
	ldr	x16, litCbz			// RELOC64 guest 0xcd2b0c
	adr	x17, 1f			// the cbz fall-through, blob-relative
	cmp	x20, #0			// the cbz predicate; flags die at the msr
	csel	x16, x16, x17, eq		// eq (x20==0) -> 0xcd2b0c, ne -> below
	ldr	x17, [sp, #248]		// the NZCV saved on entry; x17 is
					// reusable now, x16 is NOT -- it holds
					// the chosen target for the br below
	msr	nzcv, x17			// give the guest its own flags back
	add	sp, sp, #512			// no S suffix: flags untouched
	ldr	x17, litResume			// RELOC64 guest 0xcd2ab0 (site+16)
	br	x16				// absolute; PC-relative would not survive
1:	ldur	x21, [x29, #-0x40]		// == 0xf85c03b5, guest 0xcd2aa8
	cmp	x21, x20			// == 0xeb1402bf, guest 0xcd2aac
	br	x17				// absolute resume at 0xcd2ab0

// Load-bearing padding, never executed: it exists only so the literal pool
// below lands on an 8-byte boundary without the assembler inserting filler
// bytes the freeze-and-diff check would have to explain. 12 instructions puts
// the pool at +4.
	nop

// ------------------------------------------------------------- literal pool
// install() overwrites these five quadwords with 64-bit runtime addresses. Each
// slot carries a placeholder that install() asserts before writing, so a
// re-assembly that moves or resizes a slot makes install() refuse rather than
// stamp an address into the middle of an instruction.
	.balign 8
litB:	.quad	0x00000000c0de0001		// &g_rr3DeInfoB
litE:	.quad	0x00000000c0de0002		// &g_rr3DeInfoE
litSeq:	.quad	0x00000000c0de0003		// &g_rr3DeInfoSeq
litCbz:	.quad	0x00000000c0de0004		// guest ELF VA 0xcd2b0c
litResume:	.quad	0x00000000c0de0005	// guest ELF VA 0xcd2ab0

	.size rr3DeInfoTrampoline, .-rr3DeInfoTrampoline
