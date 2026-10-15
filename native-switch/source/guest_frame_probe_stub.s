// guest_frame_probe_stub.s
//
// (The file is named _stub.s rather than guest_frame_probe.s because the
// Makefile derives object names from the base name, and guest_frame_probe.cpp
// already claims guest_frame_probe.o. Two sources with one base name link the
// same object twice and every symbol in them is reported as a multiple
// definition.)
//
// THE CALLER'S FRAME POINTER, CAPTURED ON THE GUEST'S OWN THREAD
// =============================================================
//
// The container-teardown wedge at guest ELF VA 0xcd2aa0 needs two words that
// live in the guest's own stack frame, and both routes to them are closed:
//
//   * host cross-thread sampling needs svcSetThreadActivity + svcGetThreadContext3,
//     and on Eden the first never returns and the second returns the READER's
//     own registers, not the target thread's;
//   * patching the guest's .text needs svcSetMemoryPermission to hand back an
//     executable page, and it never does -- the ladder bottoms out at Perm_Rw
//     and the X bit is gone for good.
//
// This file is the third route, and it needs no kernel call at all: read the
// value while the guest itself is the one calling.
//
// THE ABI FACT THIS WHOLE FILE EXISTS TO EXPLOIT
// ----------------------------------------------
// AAPCS64 makes x29 (the frame pointer) and x30 (the link register)
// CALLEE-SAVED. A callee may use them, but it must put them back exactly as
// it found them before it returns. The guest function [0xcd1014, 0xcd3648)
// therefore still has its OWN frame pointer live in x29 at every one of the
// `bl` sites inside it -- not just at 0xcd2b08, but at all 291 of them.
//
// And the guest does not rewrite x29 anywhere in between. Measured, not
// assumed: disassembling the whole 10 KiB function yields exactly three
// instructions that touch x29, and they are
//
//   0xcd1018  stp  x29, x30, [sp, #0x10]   save the CALLER's frame
//   0xcd1030  add  x29, sp, #0x10          establish OUR frame  <- the only write
//   0xcd3450  ldp  x29, x30, [sp, #0x10]   restore, on the way out
//
// So "x29 on entry to this stub" == "[0xcd1014, 0xcd3648)'s frame pointer",
// with no window in the middle where that could be untrue.
//
// WHY THIS IS ASSEMBLY AND NOT A C FUNCTION
// -----------------------------------------
// A C function cannot do it. By the time any C statement runs, the prologue
// has already executed `stp x29, x30, [sp, #-N]!` and `add x29, sp, #N`, so
// x29 is the C function's OWN frame pointer. The caller's is already gone.
// `__builtin_return_address(0)` still works for the return address, which is
// why the census ring can name a guest PC, but there is no equivalent for the
// frame pointer. The capture has to happen in the first few instructions,
// before x29 is written, and that is exactly what a hand-written stub is for.
//
// THE SEVEN INSTRUCTIONS THAT MATTER
// ----------------------------------
//   stp  x29, x30, [sp, #0x00]   saves the INCOMING x29 and x30 -- this is the
//                                capture. It runs before x29 is ever written,
//                                so [sp,#0x00] is the CALLER's frame pointer and
//                                [sp,#0x08] is the guest's return address.
//   mov  x3, x29                 hands the caller's frame pointer to the C side.
//   mov  x4, x30                 hands the guest's return address to the C side.
//   bl   guestViewTracedMemcpyBody
//   ldp  x29, x30, [sp, #0x00]   puts the caller's x29 and x30 back, so the
//                                guest resumes with its own frame intact.
//   ret                          returns through the RESTORED x30, i.e. to the
//                                guest instruction after its own `bl`.
//
// `sub sp` / `add sp` are 0x30 = 48 bytes, which keeps sp 16-byte aligned as
// AAPCS requires. x0-x2 (the memcpy arguments) are left untouched by the two
// `mov`s and are still live at the `bl`, which is why the body can be reached
// with a single call and no reloading.
//
// SCRATCH REGISTERS. x3 and x4 are the only registers this stub writes besides
// x29/x30, and both are caller-saved under AAPCS64, so a guest that kept
// something live in them across its `bl` was already broken. The C body is an
// ordinary AAPCS64 function and preserves x19-x28, sp and d8-d15 by the same
// rule, so the guest cannot observe this stub at all except through its
// return value.
//
// WHY THE SYMBOL IS `guestViewTracedMemcpy`
// -----------------------------------------
// Because the guest resolver's symbol table is frozen and already binds the
// import named "memcpy" to &guestViewTracedMemcpy. Defining that symbol HERE,
// in assembly, means the guest's PLT stub lands on this code with no change to
// the resolver, no new relocation, and no write to any guest page. The C
// implementation the resolver used to point at is still called; it just moved
// one function down, to guestViewTracedMemcpyBody.

	.section .text.rr3_guest_frame_probe, "ax", %progbits
	.align 4

	.global	guestViewTracedMemcpy
	.type	guestViewTracedMemcpy, %function
guestViewTracedMemcpy:
	bti	c
	sub	sp, sp, #0x30
	stp	x29, x30, [sp, #0x00]		// the capture: caller's x29 and x30
	stp	x0,  x1,  [sp, #0x10]		// memcpy arguments, for the epilogue
	str	x2,      [sp, #0x20]
	mov	x3, x29				// x29 is still the caller's here
	mov	x4, x30				// x30 is the guest's return address
	bl	guestViewTracedMemcpyBody
	ldp	x29, x30, [sp, #0x00]		// give the guest its frame back
	add	sp, sp, #0x30			// no S: nothing here reads the flags
	ret
	.size	guestViewTracedMemcpy, .-guestViewTracedMemcpy
