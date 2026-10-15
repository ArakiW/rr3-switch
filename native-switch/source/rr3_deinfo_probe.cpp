// Container-teardown loop probe: hand-assembled trampoline + install plumbing.
//
// THE ENTRY JUMP IS THE INTERESTING PART, AND IT IS NOT A BRANCH. The guest
// site 0xcd2aa0 and the trampoline jitCreate() hands out are 12.4 GB apart
// (measured: site 0x2106a2baa0, trampoline 0x23e8cc1000). A single `B` reaches
// +/-128 MiB and ADRP+ADD reaches +/-4 GiB, so the previous 4-byte-branch design
// could never be reached and deinfo-install failed closed on every single run.
// AArch64 has no absolute branch, but `ldr x16, <literal>` + `br x16` is one:
// the literal load only has to span 8 bytes and the 64-bit constant it loads has
// no range limit at all. That is the whole fix, and it costs 16 bytes of window.
//
// WHICH IS WHY THE WINDOW IS 16 BYTES AND NOT 4. Those 16 bytes cover four
// guest instructions, not one:
//   0xcd2aa0  f85b83b4  ldur x20, [x29, #-0x48]
//   0xcd2aa4  b4000354  cbz   x20, 0xcd2b0c
//   0xcd2aa8  f85c03b5  ldur x21, [x29, #-0x40]
//   0xcd2aac  eb1402bf  cmp   x21, x20
// Three of them are position independent and the trampoline replays them with
// byte-identical encodings. The fourth cannot be replayed verbatim: `cbz` is
// PC-relative, so the same 19-bit displacement read from the trampoline would
// land 12.4 GB away from where it means. It is re-expressed as the absolute
// transfer it denotes. install() therefore checks all four words before it
// writes anything, and uninstall() puts all four back and reads them back.
//
// Why the trampoline is otherwise so defensive. The instruction it replaces
// sits in the middle of a live guest function, on the guest's own stack, reached
// through the bridge. The guest keeps x19-x23 live across the `bl` at 0xcd2a9c
// two instructions earlier, and the enclosing cycle is 8 KiB long, so
// essentially any register could be carrying something. So the trampoline saves
// and restores ALL of x0-x30, sp, the full 128 bits of v0-v15, and NZCV. It
// calls nothing, takes nothing, and never touches FS/TLS: a C call from here
// would re-enter the bridge while the guest thread is mid-teardown, and this
// project's filesystem wrapper is exactly the thing that must not be re-entered.
//
// The three registers the replay block does NOT restore are x16, x17 and x18.
// That is measured, not assumed: disassembling the whole guest function
// [0xcd1014, 0xcd3648) yields zero mentions of any of them. They are the
// linker's intra-procedure-call temporaries. tools/rr3-deinfo-probe re-checks
// that scan.
//
// Why the counter is a release store and the reader an acquire load. The
// trampoline writes B, then E, then bumps the counter with `stlr`. A frame-loop
// reader does `ldar` on the counter and only then reads B and E. That ordering
// is the entire synchronisation story: no mutex anywhere, and it cannot observe
// a new counter paired with a half-updated pair. A missed increment is possible
// but can only cost a sample, never corrupt one.

#include "rr3/rr3_deinfo_probe.hpp"

#include <switch.h>

#include <cstring>

namespace rr3 {
namespace {

// ---------------------------------------------------------------------------
// Capture slots. Preallocated, 64-byte aligned and mutually far apart so the
// trampoline's three stores never share a cache line with each other.
// The trampoline reaches these through 64-bit literals in its own pool, so the
// 12.4 GB between it and this module costs nothing; treat the names as
// documentation, not as something the .s refers to.
// ---------------------------------------------------------------------------
alignas(64) volatile uint64_t g_rr3DeInfoB = 0;
alignas(64) volatile uint64_t g_rr3DeInfoE = 0;
alignas(64) volatile uint32_t g_rr3DeInfoSeq = 0;

// ---------------------------------------------------------------------------
// The trampoline, byte for byte, as produced by
//   aarch64-none-elf-as tools/rr3-deinfo-probe/trampoline.s
//
// 0x000 sub  sp, sp, #512
// 0x004 stp  x0,  x1,  [sp]
// ...    16x stp x0..x29, str x30 @240
// 0x044 mrs  x16, nzcv
// 0x048 str  x16, [sp, #248]
// 0x04c ...   8x stp q0..q15          (full width: an stp dN would zero the
//                                      high halves of the V register)
// 0x06c ldr  x29, [sp, #232]
// 0x070 ldur x9,  [x29, #-0x48]       B
// 0x074 ldur x10, [x29, #-0x40]       E
// 0x078 ldr  x16, 0x130               RELOC64 g_rr3DeInfoB
// 0x07c ldr  x17, 0x138               RELOC64 g_rr3DeInfoE
// 0x080 ldr  x18, 0x140               RELOC64 g_rr3DeInfoSeq
// 0x084 str  x9,  [x16]
// 0x088 str  x10, [x17]
// 0x08c ldr  w9,  [x18]
// 0x090 add  w9,  w9, #1
// 0x094 stlr w9,  [x18]               release: publishes the pair
// 0x098 ...    15x ldp x0..x29, ldr x30
// 0x0cc ...    8x ldp q0..q15
// 0x0f8 ldur x20, [x29, #-0x48]       REPLAY +0,  == 0xf85b83b4
// 0x0fc ldr  x16, 0x148               RELOC64 guest 0xcd2b0c (the cbz target)
// 0x100 adr  x17, 0x120               the cbz fall-through, blob-relative
// 0x104 cmp  x20, #0                 the cbz predicate
// 0x108 csel x16, x17, x16, eq        consume the predicate
// 0x10c ldr  x17, [sp, #248]          the NZCV saved on entry
// 0x110 msr  nzcv, x17               give the guest its own flags back
// 0x114 add  sp, sp, #512
// 0x118 ldr  x17, 0x150               RELOC64 guest 0xcd2ab0 (resume)
// 0x11c br   x16                     absolute; PC-relative would not survive
// 0x120 ldur x21, [x29, #-0x40]       REPLAY +8,  == 0xf85c03b5
// 0x124 cmp  x21, x20                REPLAY +12, == 0xeb1402bf
// 0x128 br   x17                     absolute resume
// 0x12c nop                          load-bearing: aligns the pool to 8
// 0x130 .. literal pool, 5 x 8 bytes, placeholders 0x00000000c0de000N
//
// kTrampolineNotes, so a future reader does not have to re-derive them:
//   * 0x70 loads the same slot the replay at 0xf8 loads. So the captured `b`
//     and the value the guest ends up with in x20 are the SAME word by
//     construction -- a free cross-check that the patch is not off by a frame.
//   * the replay runs after the reload, because the reload restores the guest's
//     old x20 and the replay is what overwrites it. That ordering is the point
//     of putting the replay after the restore rather than before.
//   * deciding the replayed `cbz` needs a compare, and a compare sets flags.
//     The NZCV restore therefore sits AFTER the select has consumed those flags
//     and BEFORE the branch that leaves, which is the only ordering that leaves
//     the guest its own flags on BOTH paths: the cbz-taken path never reaches
//     the replayed `cmp x21, x20`, so the `msr` is what protects it.
//   * x16 holds the chosen target from the csel all the way to `br x16`, so it
//     must not be reused in between -- the NZCV carrier is x17, which the
//     fall-through `adr` is finished with by then. `add sp` sits after the
//     `msr` because the `msr` still needs this frame, and it sets no flags.
//   * there is deliberately not one ADRP in this blob. Everything non-local is a
//     64-bit literal, so nothing in here can fail because of where it landed.
// ---------------------------------------------------------------------------
alignas(16) const uint8_t kTrampolineCode[kDeInfoTrampolineSize] = {
    0xff, 0x03, 0x08, 0xd1, 0xe0, 0x07, 0x00, 0xa9, 0xe2, 0x0f, 0x01, 0xa9,
    0xe4, 0x17, 0x02, 0xa9, 0xe6, 0x1f, 0x03, 0xa9, 0xe8, 0x27, 0x04, 0xa9,
    0xea, 0x2f, 0x05, 0xa9, 0xec, 0x37, 0x06, 0xa9, 0xee, 0x3f, 0x07, 0xa9,
    0xf0, 0x47, 0x08, 0xa9, 0xf2, 0x4f, 0x09, 0xa9, 0xf4, 0x57, 0x0a, 0xa9,
    0xf6, 0x5f, 0x0b, 0xa9, 0xf8, 0x67, 0x0c, 0xa9, 0xfa, 0x6f, 0x0d, 0xa9,
    0xfc, 0x77, 0x0e, 0xa9, 0xfe, 0x7b, 0x00, 0xf9, 0x10, 0x42, 0x3b, 0xd5,
    0xf0, 0x7f, 0x00, 0xf9, 0xe0, 0x07, 0x08, 0xad, 0xe2, 0x0f, 0x09, 0xad,
    0xe4, 0x17, 0x0a, 0xad, 0xe6, 0x1f, 0x0b, 0xad, 0xe8, 0x27, 0x0c, 0xad,
    0xea, 0x2f, 0x0d, 0xad, 0xec, 0x37, 0x0e, 0xad, 0xee, 0x3f, 0x0f, 0xad,
    0xfd, 0x77, 0x40, 0xf9, 0xa9, 0x83, 0x5b, 0xf8, 0xaa, 0x03, 0x5c, 0xf8,
    0xd0, 0x05, 0x00, 0x58, 0xf1, 0x05, 0x00, 0x58, 0x12, 0x06, 0x00, 0x58,
    0x09, 0x02, 0x00, 0xf9, 0x2a, 0x02, 0x00, 0xf9, 0x49, 0x02, 0x40, 0xb9,
    0x29, 0x05, 0x00, 0x11, 0x49, 0xfe, 0x9f, 0x88, 0xe0, 0x07, 0x40, 0xa9,
    0xe2, 0x0f, 0x41, 0xa9, 0xe4, 0x17, 0x42, 0xa9, 0xe6, 0x1f, 0x43, 0xa9,
    0xe8, 0x27, 0x44, 0xa9, 0xea, 0x2f, 0x45, 0xa9, 0xec, 0x37, 0x46, 0xa9,
    0xee, 0x3f, 0x47, 0xa9, 0xf0, 0x47, 0x48, 0xa9, 0xf2, 0x4f, 0x49, 0xa9,
    0xf4, 0x57, 0x4a, 0xa9, 0xf6, 0x5f, 0x4b, 0xa9, 0xf8, 0x67, 0x4c, 0xa9,
    0xfa, 0x6f, 0x4d, 0xa9, 0xfc, 0x77, 0x4e, 0xa9, 0xfe, 0x7b, 0x40, 0xf9,
    0xe0, 0x07, 0x48, 0xad, 0xe2, 0x0f, 0x49, 0xad, 0xe4, 0x17, 0x4a, 0xad,
    0xe6, 0x1f, 0x4b, 0xad, 0xe8, 0x27, 0x4c, 0xad, 0xea, 0x2f, 0x4d, 0xad,
    0xec, 0x37, 0x4e, 0xad, 0xee, 0x3f, 0x4f, 0xad, 0xb4, 0x83, 0x5b, 0xf8,
    0x70, 0x02, 0x00, 0x58, 0x11, 0x01, 0x00, 0x10, 0x9f, 0x02, 0x00, 0xf1,
    0x10, 0x02, 0x91, 0x9a, 0xf1, 0x7f, 0x40, 0xf9, 0x11, 0x42, 0x1b, 0xd5,
    0xff, 0x03, 0x08, 0x91, 0xd1, 0x01, 0x00, 0x58, 0x00, 0x02, 0x1f, 0xd6,
    0xb5, 0x03, 0x5c, 0xf8, 0xbf, 0x02, 0x14, 0xeb, 0x20, 0x02, 0x1f, 0xd6,
    0x1f, 0x20, 0x03, 0xd5, 0x01, 0x00, 0xde, 0xc0, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0xde, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0xde, 0xc0,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0xde, 0xc0, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0xde, 0xc0, 0x00, 0x00, 0x00, 0x00,
};

// The five `ldr <x>, <literal>` instructions and the five 64-bit literal slots
// they read. install() asserts both the instruction encodings and the
// placeholders before it writes anything, so a re-assembly that moves or resizes
// a slot makes install() refuse rather than stamp a runtime address into the
// middle of an unrelated instruction.
struct LiteralSlot {
    uint32_t insnOffset;  // the `ldr xN, ...` word
    uint32_t insnWord;    // its exact encoding, sentinels for both halves
    uint32_t slotOffset;  // the 8 bytes it reads
    uint64_t placeholder; // what the assembler must have left there
};
constexpr LiteralSlot kLiteralSlots[5] = {
    {0x078, 0x580005d0u, 0x130, 0x00000000c0de0001ull},  // &g_rr3DeInfoB
    {0x07c, 0x580005f1u, 0x138, 0x00000000c0de0002ull},  // &g_rr3DeInfoE
    {0x080, 0x58000612u, 0x140, 0x00000000c0de0003ull},  // &g_rr3DeInfoSeq
    {0x0fc, 0x58000270u, 0x148, 0x00000000c0de0004ull},  // guest 0xcd2b0c
    {0x118, 0x580001d1u, 0x150, 0x00000000c0de0005ull},  // guest 0xcd2ab0
};
constexpr uintptr_t kCaptureTargets[3] = {
    reinterpret_cast<uintptr_t>(&g_rr3DeInfoB),
    reinterpret_cast<uintptr_t>(&g_rr3DeInfoE),
    reinterpret_cast<uintptr_t>(&g_rr3DeInfoSeq),
};

// The 16-byte stub install() writes into guest .text, as encodings rather than
// as bytes, so that no distance limit can creep back in unnoticed:
//
//   +0x0  ldr  x16, #8        0x58000050   load the 64-bit constant at +8
//   +0x4  br   x16            0xd61f0200   unconditional, any distance
//   +0x8  <8 bytes>                      the constant: the trampoline's address
//
// The constant sits AFTER the branch that consumes it, and that is what makes
// 16 bytes enough. The obvious spelling -- `ldr`, `b .+8`, `.dword`, `br` -- is
// five words and twenty bytes and does NOT fit the window at all. No skip branch
// is needed, because `br x16` leaves the window before +8 is ever fetched; the
// two literal words are read by the ldr, never executed.
//
// The code has to start at +0, because +0 is where the guest's own fetch lands.
// Putting the constant first and reaching back to it with a negative
// displacement looks tidier and is broken: the CPU would decode eight bytes of
// address as instructions on arrival.
//
// The ldr's literal displacement is a fixed +8, trivially inside the +/-1 MiB
// LDR-literal window. The 64-bit constant it reads has no range limit at all,
// which is the entire reason this stub exists.
constexpr uint32_t kStubLdrLiteralWord = 0x58000050u;  // ldr x16, #8
constexpr uint32_t kStubBrX16Word = 0xd61f0200u;       // br x16
constexpr uint32_t kStubBrBase = 0xd61f0000u;          // br Xn, bits 9:5 = Rn
constexpr int32_t kStubLiteralDispBytes = 8;            // ldr at +0 reads +8

static_assert(kStubLdrLiteralWord ==
                  (0x58000000u | ((static_cast<uint32_t>(kStubLiteralDispBytes / 4) & 0x7ffffu) << 5) | 16u),
              "ldr must read the constant at +8");
static_assert(kStubBrX16Word == (kStubBrBase | (16u << 5)), "tail must be br x16");

// 64-bit, and that is load-bearing rather than stylistic.
//
// This was `constexpr uint32_t kPageSize = 0x1000`. That made
// `guestSiteRuntimeVa & ~(kPageSize - 1)` evaluate with a 32-bit mask:
// `~(0xFFFu)` is unsigned int 0xFFFFF000, and the `&` then ZERO-EXTENDS it
// to 64 bits as 0x00000000FFFFF000. For the real site 0x210762baa0 that
// yields page 0x762b000 -- a 15-digit-short address that was never mapped --
// and svcSetMemoryPermission on it returned 0x0000d401 (stage 9).
//
// The reason it stayed invisible for a whole round: the report logged
// `siteVa`, the RAW runtime address, and never the masked page. So the log
// showed a perfectly correct 0x210762baa0 next to a permission failure, and
// read as "the page refuses". guest_view_probe.cpp did the same masking with
// `const uint64_t kPageSz = 4096ULL` and therefore always worked -- two files,
// same intent, one 32-bit and one 64-bit, and the difference looked like an
// emulator bug for a full 200-second round.
//
// Guest .text lives above 4 GiB here, so a 32-bit page mask cannot address it
// at all. Any mask on a guest address must be formed in 64 bits.
constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = ~(kPageSize - 1);

// Install state. Single-threaded by construction: the frame loop owns it, and
// the guest thread never looks at it.
struct InstallState {
    bool installed;
    uintptr_t siteVa;
    Jit jit;
};
InstallState g_state{};
uint32_t g_lastDrainedSeq = 0;

inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void write32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

inline uint64_t read64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline void write64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

// Acquire half of the release/acquire pair with the trampoline's `stlr`.
inline uint32_t acquireSeq() {
    uint32_t v;
    __asm__ __volatile__("ldar %w0, [%1]"
                         : "=r"(v)
                         : "r"(&g_rr3DeInfoSeq)
                         : "memory");
    return v;
}

// Build the 16-byte entry stub. Fills all four words and returns true, or
// returns false having written nothing.
//
// This replaces the old encodeBranch(), which could only express `B` and so
// could only say "no" once jitCreate() put the trampoline out of reach. There is
// no range check here to fail, because there is nothing left to check: the
// literal is 8 bytes away from its own load and the constant is 64 bits wide.
bool encodeEntryStub(uintptr_t trampVa, uint32_t stub[4]) {
    if ((trampVa & 3u) != 0) {
        return false;
    }
    stub[0] = kStubLdrLiteralWord;
    stub[1] = kStubBrX16Word;
    stub[2] = static_cast<uint32_t>(trampVa & 0xffffffffu);
    stub[3] = static_cast<uint32_t>((trampVa >> 32) & 0xffffffffu);
    return true;
}

// Decode the stub back out of memory and return the address it would branch to.
//
// This is the check that earns the right to touch guest .text. It does not just
// recognise two magic words: it decodes the ldr's literal displacement and
// proves it reaches the two words at the front of the stub, checks the tail is
// `br x16`, and only then hands back the 64-bit constant the `br` will fetch.
bool decodeEntryStub(const uint32_t stub[4], uintptr_t stubAddr,
                     uintptr_t* target) {
    if ((stubAddr & 3u) != 0) {
        return false;
    }
    // `br Xn`: bits 9:5 are Rn, so strip them and then demand Rn == x16.
    // Comparing the masked word against the full encoding would never match.
    if ((stub[1] & ~0x000003e0u) != kStubBrBase || ((stub[1] >> 5) & 0x1fu) != 16u) {
        return false;
    }
    // LDR (literal): imm19, signed, in units of instructions.
    if ((stub[0] & ~0x00ffffe0u) != (0x58000000u | 16u)) {
        return false;
    }
    int32_t ldrImm = static_cast<int32_t>((stub[0] >> 5) & 0x7ffffu);
    if ((ldrImm & 0x40000) != 0) {
        ldrImm -= 0x80000;
    }
    if (stubAddr + (static_cast<intptr_t>(ldrImm) << 2) !=
        stubAddr + kStubLiteralDispBytes) {
        return false;
    }
    const uintptr_t lo = stub[2];
    const uintptr_t hi = static_cast<uintptr_t>(stub[3]) << 32;
    *target = static_cast<uintptr_t>(lo | hi);
    return true;
}

// Stamp the five runtime addresses into the trampoline's literal pool. Every
// placeholder and every `ldr` encoding is checked first; nothing is written
// unless all ten checks pass.
bool patchLiterals(uint8_t* image, uintptr_t cbzTarget, uintptr_t resume) {
    uintptr_t wanted[5];
    for (size_t i = 0; i < 3; ++i) {
        wanted[i] = kCaptureTargets[i];
    }
    wanted[3] = cbzTarget;
    wanted[4] = resume;

    for (size_t i = 0; i < 5; ++i) {
        const LiteralSlot& s = kLiteralSlots[i];
        if (read32(image + s.insnOffset) != s.insnWord) {
            return false;
        }
        if (read64(image + s.slotOffset) != s.placeholder) {
            return false;
        }
    }
    for (size_t i = 0; i < 5; ++i) {
        write64(image + kLiteralSlots[i].slotOffset, wanted[i]);
    }
    return true;
}

// Which of the ten literal checks failed, so install() can say why instead of
// just "no". Pure predicate, no writes.
enum class LiteralFault : uint8_t { kNone, kInsn, kSlot };
LiteralFault literalFault(const uint8_t* image) {
    for (size_t i = 0; i < 5; ++i) {
        const LiteralSlot& s = kLiteralSlots[i];
        if (read32(image + s.insnOffset) != s.insnWord) {
            return LiteralFault::kInsn;
        }
        if (read64(image + s.slotOffset) != s.placeholder) {
            return LiteralFault::kSlot;
        }
    }
    return LiteralFault::kNone;
}

// Byte-for-byte: are these sixteen bytes the guest's own four words again?
// install() uses this on its rollback path and uninstall() uses it to prove it
// restored rather than merely intended to.
bool windowRestored(const uint8_t* window) {
    const uint8_t want[kDeInfoWindowBytes] = {
        0xb4, 0x83, 0x5b, 0xf8,  // 0xf85b83b4  ldur x20, [x29, #-0x48]
        0x54, 0x03, 0x00, 0xb4,  // 0xb4000354  cbz   x20, 0xcd2b0c
        0xb5, 0x03, 0x5c, 0xf8,  // 0xf85c03b5  ldur x21, [x29, #-0x40]
        0xbf, 0x02, 0x14, 0xeb,  // 0xeb1402bf  cmp   x21, x20
    };
    for (uint32_t i = 0; i < kDeInfoWindowBytes; ++i) {
        if (window[i] != want[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE PERMISSION LADDER, AND THE ONE MEASUREMENT THAT DECIDES IF IT WORKED.
//
// WHY THERE IS A LADDER. Perm_Rx is Perm_R|Perm_X == 0x5, and bit 2 of
// svcSetMemoryPermission's perm argument IS the execute bit. Two independent
// sources say the kernel refuses it. This SDK's own header, at svc.h:470:
// "Perm_X is not allowed. ... This can be used to move back and forth between
// Perm_None, Perm_R and Perm_Rw." And switchbrew's SVC page gives the code:
// SetMemoryPermission returns 0xD801, "Invalid permission specified. Valid
// permissions are ---, r-- and rw-". main.cpp:472 recorded the same 0xD801
// against svcSetProcessMemoryPermission for "any perm above 0x5, and 0x5 is
// exactly Perm_Rx", which is the same wall reached from the other syscall.
//
// The 2026-10-06 04:31 run then measured it. permRwRes=0 and
// permRwPreRes=0 -- both Perm_Rw requests were accepted -- and
// permRestoreMode=0 with stage=11, which is this ladder's FIRST rung being
// refused with the sixteen bytes already sitting in guest .text. The old code
// asked for a permission the kernel has never accepted, and then treated the
// refusal as an emergency.
//
// SO THE PAGE NEVER BECOMES Perm_Rx. It becomes whatever the kernel's own
// mapping already says once the read/write bits are dropped, and the only way
// to know what that is is to read it back rather than to assume it.
//
// WHICH IS WHAT svcQueryMemory IS FOR. libnx's MemoryInfo carries a `perm`
// field (svc.h:96) and svcQueryMemory is a plain unprivileged query, syscall
// 0x06, that takes no lock on this side. main.cpp:528 already reads
// info.perm and main.cpp:531 already computes `(info.perm & Perm_X)`, so this
// is established idiom here and not a new dependency. A run log from
// 2026-10-02 shows it returning perm=3 for a page svcMapMemory had just handed
// back, which is exactly Perm_Rw -- the field is populated and meaningful
// rather than aliasing some other member of the struct.
//
// The ladder therefore does not try to SET execute. It offers every request
// the kernel might conceivably accept, stops at the first one it accepts, and
// then MEASURES. Rung 4 is the one expected to work on real hardware, because
// Perm_R is legal and the execute bit is a property of the mapping rather than
// of this syscall -- but that last clause is a hypothesis, which is exactly
// why the measurement exists and why the install refuses to call itself a
// success without one.
//
// The ladder, in the order the run must report it.
//
// Rungs 1 and 2 are the same value spelled two ways, and 3 and 5 are the same
// value too. The duplication is deliberate. "The macro expanded differently
// from the explicit bit-or" and "the same request behaves differently the
// second time" each cost one logged rc to settle and are expensive to keep
// guessing about; a ladder that collapsed the duplicates would leave exactly
// that ambiguity behind.
//
// `Permission` is a plain enum, so `Perm_R | Perm_X` is an int and needs the
// cast. That is not a detail: it is the entire reason rung 2 is spelled
// differently from rung 1 in the first place, and it is the one place in this
// file where "the macro" and "the explicit bit-or" can genuinely differ.
struct PermRung {
    Permission perm;
};
constexpr PermRung kPermLadder[5] = {
    {Perm_Rx},                                   // 1. the macro, as the old code called it
    {static_cast<Permission>(Perm_R | Perm_X)},   // 2. spelled out
    {Perm_X},                                    // 3. execute and nothing else
    {Perm_R},                                    // 4a. the one the kernel is documented to take
    {Perm_X},                                    // 4b. only reached if 4a was refused
};
constexpr uint32_t kPermLadderSteps =
    static_cast<uint32_t>(sizeof(kPermLadder) / sizeof(kPermLadder[0]));

// Run the ladder until a rung is accepted, then read back what the page
// actually is. Every rung's Result goes into `rungs` whether it succeeded or
// not, `winner` gets the 1-based index of the accepted one (0 = none), and
// `observed` gets MemoryInfo.perm.
//
// The read-back happens even when nothing was accepted, and that is the point:
// a refused request leaves the page exactly as it was, so on a total failure
// the reading is still the truth about the state the rollback is about to
// inherit.
Result applyPagePerm(void* page, uint32_t* rungs, uint32_t* count,
                     uint32_t* winner, Result* queryRc, uint32_t* observed) {
    uint32_t last = 0;
    uint32_t tried = 0;
    uint32_t won = 0;
    for (uint32_t i = 0; i < kPermLadderSteps; ++i) {
        const Result rc = svcSetMemoryPermission(
            page, kPageSize, static_cast<uint32_t>(kPermLadder[i].perm));
        if (tried < kPermLadderSteps) {
            rungs[tried++] = static_cast<uint32_t>(rc);
        }
        last = static_cast<uint32_t>(rc);
        if (R_SUCCEEDED(rc)) {
            won = i + 1;
            break;
        }
    }
    *count = tried;
    *winner = won;
    MemoryInfo info{};
    u32 pageInfo = 0;
    const Result q =
        svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(page));
    *queryRc = static_cast<Result>(q);
    *observed = R_SUCCEEDED(q) ? info.perm : 0u;
    return static_cast<Result>(last);
}

// Read one address's MemoryInfo.perm. Pure query, no side effect, used to
// capture the page as it was before install() touched it so the log shows the
// whole round trip (before -> Perm_Rw -> after) rather than only the end.
uint32_t queryPagePerm(void* page, Result* queryRc) {
    MemoryInfo info{};
    u32 pageInfo = 0;
    const Result q =
        svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(page));
    *queryRc = static_cast<Result>(q);
    return R_SUCCEEDED(q) ? info.perm : 0u;
}

inline uint32_t permHasExec(uint32_t perm) {
    return (perm & static_cast<uint32_t>(Perm_X)) != 0 ? 1u : 0u;
}

}  // namespace

bool rr3DeInfoInstalled() { return g_state.installed; }

bool rr3DeInfoDrain(DeInfoSample* out) {
    const uint32_t seq = acquireSeq();
    if (seq == g_lastDrainedSeq) {
        return false;
    }
    // The acquire above orders these two loads after the trampoline's two
    // stores, so a fresh counter guarantees a complete pair.
    const uint64_t b = g_rr3DeInfoB;
    const uint64_t e = g_rr3DeInfoE;
    g_lastDrainedSeq = seq;
    if (out != nullptr) {
        out->b = b;
        out->e = e;
        out->seq = seq;
    }
    return true;
}

bool rr3DeInfoInstall(uintptr_t guestSiteRuntimeVa,
                      DeInfoInstallReport* report) {
    DeInfoInstallReport r{};
    r.siteVa = guestSiteRuntimeVa;

    auto publish = [&](bool ok) {
        if (report != nullptr) {
            *report = r;
        }
        return ok;
    };

    if (g_state.installed) {
        r.stage = kDeInfoAlreadyInstalled;
        r.alreadyInstalled = 1;
        r.installed = 1;
        r.siteVa = g_state.siteVa;
        r.trampolineVa = reinterpret_cast<uintptr_t>(jitGetRxAddr(&g_state.jit));
        r.branchDelta = static_cast<int64_t>(r.trampolineVa) -
                        static_cast<int64_t>(r.siteVa);
        return publish(true);
    }

    if ((guestSiteRuntimeVa & 3u) != 0) {
        r.stage = kDeInfoBadSiteAlignment;
        return publish(false);
    }

    // The window has to sit inside one page, because one Perm_Rw round covers
    // one page and a window straddling two would leave the second half
    // read-only. Checked before anything else is allocated.
    const uintptr_t windowFirst = guestSiteRuntimeVa;
    const uintptr_t windowLast = guestSiteRuntimeVa + kDeInfoWindowBytes - 1;
    if ((windowFirst >> 12) != (windowLast >> 12)) {
        r.stage = kDeInfoWindowStraddlesPage;
        return publish(false);
    }

    const uint8_t* const window = reinterpret_cast<const uint8_t*>(windowFirst);

    // If the window already holds our own stub, the probe is live but this
    // module's state was lost -- say so instead of reporting a mystery word
    // mismatch, because the operator's next move is completely different.
    if (read32(window) == kStubLdrLiteralWord &&
        read32(window + 4) == kStubBrX16Word) {
        r.stage = kDeInfoAlreadyPatched;
        return publish(false);
    }

    // Fail closed if the guest is not what we disassembled. All four words, not
    // just the first: the patch eats all four, and a site whose first word is
    // right but whose fourth is not would hand the guest back a window that
    // ends on someone else's instruction.
    uint32_t matched = 0;
    for (uint32_t i = 0; i < kDeInfoEatenWords; ++i) {
        if (read32(window + i * 4) != kDeInfoEatenWordsData[i]) {
            r.stage = (i == 0) ? kDeInfoSiteWordMismatch
                               : kDeInfoWindowWordMismatch;
            return publish(false);
        }
        ++matched;
    }
    r.windowVerified = matched;

    // Everything after this point is guest-code-free until the very last step.
    // If any check below fails, nothing in guest .text has been touched, so the
    // honest outcome is a closed install with an unchanged guest.

    const int64_t bias = static_cast<int64_t>(guestSiteRuntimeVa) -
                         static_cast<int64_t>(kDeInfoSiteElfVa);
    const uintptr_t cbzTarget =
        static_cast<uintptr_t>(static_cast<int64_t>(kDeInfoCbzTargetElfVa) + bias);
    const uintptr_t resume =
        static_cast<uintptr_t>(static_cast<int64_t>(kDeInfoResumeElfVa) + bias);
    r.cbzTargetVa = cbzTarget;
    r.resumeVa = resume;

    // ---- what the page looks like BEFORE we touch it -------------------
    //
    // One query, no side effect, and it is what makes the rest of the perm
    // story readable: permBefore -> Perm_Rw -> permPreFinal -> Perm_Rw ->
    // permFinal is the whole round trip in one log line. Without the "before"
    // reading there is no way to tell a page that was never executable from one
    // that we made non-executable, and those are very different bugs.
    void* const page = reinterpret_cast<void*>(guestSiteRuntimeVa & kPageMask);
    r.pageVa = static_cast<uint64_t>(guestSiteRuntimeVa & kPageMask);
    Result preQuery = 0;
    r.permBefore = queryPagePerm(page, &preQuery);
    r.permQueryResPre = static_cast<uint32_t>(preQuery);

    // ---- PRE-jitCreate permission probe --------------------------------
    //
    // Measured, not guessed. In the 2026-10-06 04:26 run this page's FIRST
    // svcSetMemoryPermission(Perm_Rw) -- the one down at the bottom of this
    // function, after jitCreate -- returned 0x0000d401, while the permission
    // probe in guest_view_probe.cpp ran the byte-identical call on the byte-
    // identical page moments LATER and returned rc=0. "Eden refuses a repeat
    // remap" was therefore falsified: the repeat is allowed, and it is the
    // first call that fails. The only thing between the two is jitCreate, so
    // call it once up here, before any of that, and let the run say which.
    //
    // The restore used to ask for Perm_Rx here, and that was a second, quieter
    // version of the same bug: Perm_Rx is never accepted, so on a console this
    // probe would have handed the guest a PERMANENTLY Perm_Rw .text page and
    // said nothing, because the Result was cast to void. It goes through the
    // same ladder the install uses, and it is also the earliest measurement of
    // the one thing everything else rests on: whether a Perm_Rw round trip
    // costs the page its execute bit. If permPreFinal has no Perm_X, the guest
    // cannot run at all after this point, and the log has to be able to say so.
    const Result permRwPreRc = svcSetMemoryPermission(
        page, kPageSize, static_cast<uint32_t>(Perm_Rw));
    r.permRwPreResult = static_cast<uint32_t>(permRwPreRc);
    if (R_SUCCEEDED(permRwPreRc)) {
        uint32_t probeRungs[kPermLadderSteps];
        uint32_t probeCount = 0;
        uint32_t probeWinner = 0;
        Result probeQ = 0;
        uint32_t probePerm = 0;
        const Result probeRc = applyPagePerm(page, probeRungs, &probeCount,
                                             &probeWinner, &probeQ, &probePerm);
        r.permPreRestoreRc = static_cast<uint32_t>(probeRc);
        r.permPreQueryRes = static_cast<uint32_t>(probeQ);
        r.permPreFinal = probePerm;
    }

    // The relocated trampoline needs a page that is both writable and
    // executable. Neither svcSetMemoryPermission nor
    // svcSetProcessMemoryPermission can hand back an RWX page (Perm_X is not a
    // requestable bit; see the ladder comment above), so use libnx's JIT pair:
    // write through the RW alias, branch to the RX alias. jitTransitionToExecutable
    // is libnx's own way of flipping the alias pair, and it is the only
    // supported route to executable-plus-writable on this system.
    const Result jitRc = jitCreate(&g_state.jit, kPageSize);
    r.jitCreateResult = static_cast<uint32_t>(jitRc);
    if (R_FAILED(jitRc)) {
        r.stage = kDeInfoJitCreateFailed;
        return publish(false);
    }

    auto* writable = static_cast<uint8_t*>(jitGetRwAddr(&g_state.jit));
    auto* executable = static_cast<uint8_t*>(jitGetRxAddr(&g_state.jit));
    if (executable == nullptr || (reinterpret_cast<uintptr_t>(executable) & 3u) != 0) {
        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        r.stage = kDeInfoTrampolineMisaligned;
        return publish(false);
    }
    const uintptr_t trampVa = reinterpret_cast<uintptr_t>(executable);
    r.trampolineVa = trampVa;
    r.branchDelta = static_cast<int64_t>(trampVa) -
                    static_cast<int64_t>(guestSiteRuntimeVa);

    std::memcpy(writable, kTrampolineCode, sizeof(kTrampolineCode));
    if (!patchLiterals(writable, cbzTarget, resume)) {
        r.stage = (literalFault(writable) == LiteralFault::kSlot)
                      ? kDeInfoRelocSentinelMismatch
                      : kDeInfoLiteralSlotMismatch;
        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        return publish(false);
    }

    // Build the entry stub and then read it back. The decode is the check: it
    // proves the 16 bytes we are about to write into guest .text branch to the
    // address we actually allocated, at any distance, rather than merely being
    // 16 bytes that look right.
    uint32_t stub[4] = {0, 0, 0, 0};
    if (!encodeEntryStub(trampVa, stub)) {
        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        r.stage = kDeInfoStubEncodeFailed;
        return publish(false);
    }
    uintptr_t decoded = 0;
    if (!decodeEntryStub(stub, guestSiteRuntimeVa, &decoded) ||
        decoded != trampVa) {
        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        r.stage = kDeInfoStubEncodeFailed;
        return publish(false);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        r.stubWords[i] = stub[i];
    }
    r.wordWritten = stub[0];

    armDCacheFlush(writable, sizeof(kTrampolineCode));
    armICacheInvalidate(executable, sizeof(kTrampolineCode));
    if (R_FAILED(jitTransitionToExecutable(&g_state.jit))) {
        jitClose(&g_state.jit);
        r.stage = kDeInfoJitCreateFailed;
        return publish(false);
    }

    // ---- the guest page, last, so nothing above can leave it half-patched ----
    //
    // `page` was computed and logged much earlier, before jitCreate, so that a
    // wrong page shows up in the log whether or not we get this far. The
    // request itself is the last thing that can fail before a guest byte moves.
    const Result permRwRc = svcSetMemoryPermission(
        page, kPageSize, static_cast<uint32_t>(Perm_Rw));
    r.permRwResult = static_cast<uint32_t>(permRwRc);
    if (R_FAILED(permRwRc)) {
        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        r.stage = kDeInfoPermRwFailed;
        return publish(false);
    }

    // One memcpy, sixteen bytes, one page. install() writes the whole window or
    // nothing; there is no path that writes three words and gives up.
    std::memcpy(reinterpret_cast<void*>(guestSiteRuntimeVa), stub,
                kDeInfoWindowBytes);
    armDCacheFlush(reinterpret_cast<void*>(guestSiteRuntimeVa), kDeInfoWindowBytes);
    armICacheInvalidate(reinterpret_cast<void*>(guestSiteRuntimeVa),
                        kDeInfoWindowBytes);

    // ---- put the page back, and PROVE it went back --------------------
    //
    // This is the step that produced stage=11 on 2026-10-06 04:31, and it is
    // the step that gets the whole ladder plus a measurement. The bytes are
    // already in guest .text here, so there are exactly two questions left and
    // both have to be answered with evidence:
    //   1. can the page be taken back off Perm_Rw at all?
    //   2. is it still executable afterwards?
    //
    // Question 2 is the one an emulator will lie about. Eden does not enforce
    // execute permission: in the 04:31 run the guest ran on through to
    // ordinal=686 after this function had already left the page Perm_Rw and
    // then non-executable, and that is NOT evidence that a console would do the
    // same. A console would fault on the first instruction fetch from a page
    // whose execute bit we had dropped. So the success criterion here is the
    // reading from svcQueryMemory and not the guest's silence.
    Result permQ = 0;
    (void)applyPagePerm(page, r.permLadderRc, &r.permLadderLen,
                        &r.permRestoreRung, &permQ, &r.permFinal);
    r.permQueryRes = static_cast<uint32_t>(permQ);
    r.permExecFinal = permHasExec(r.permFinal);
    r.permRestoreMode = r.permRestoreRung != 0u ? 1u : 0u;

    // Three distinct ways to be a failure, so the report says which.
    //
    //   19  every rung refused        the page is still Perm_Rw
    //   18  svcQueryMemory refused    the page is in an unknown state, and an
    //                                 unknown state is not an executable one
    //   17  a rung was accepted but the page has no Perm_X
    //                                 the guest can no longer run; this is the
    //                                 one that a real console would die on
    int32_t permFault = kDeInfoOk;
    if (r.permRestoreRung == 0u) {
        permFault = kDeInfoPermRungsExhausted;
    } else if (R_FAILED(static_cast<Result>(r.permQueryRes))) {
        permFault = kDeInfoPermQueryFailed;
    } else if (r.permExecFinal == 0u) {
        permFault = kDeInfoPermExecLost;
    }

    if (permFault != kDeInfoOk) {
        // ---- the one and only way out of a failed install -------------
        //
        // Invariant: when this block ends, guest .text holds its own four
        // original words and the page is in whatever state we could get it
        // into, MEASURED and logged. It is never legal to leave sixteen bytes
        // of stub behind on a page the guest might be executing from, and it is
        // equally illegal to leave the page writable or non-executable and
        // report nothing. Both of those are states, and states get logged.
        const Result rbRwRc = svcSetMemoryPermission(
            page, kPageSize, static_cast<uint32_t>(Perm_Rw));
        r.permRollbackRwResult = static_cast<uint32_t>(rbRwRc);
        if (R_SUCCEEDED(rbRwRc)) {
            std::memcpy(reinterpret_cast<void*>(guestSiteRuntimeVa),
                        kDeInfoEatenWordsData, kDeInfoWindowBytes);
            armDCacheFlush(reinterpret_cast<void*>(guestSiteRuntimeVa),
                           kDeInfoWindowBytes);
            armICacheInvalidate(reinterpret_cast<void*>(guestSiteRuntimeVa),
                                kDeInfoWindowBytes);
            r.permRollbackResult = 1u;
        } else {
            // The worst state this function can produce: the page is not
            // writable, so the original words cannot be put back and the stub
            // stays live. It gets a value of its own rather than being folded
            // into the boolean, because the operator's next move is completely
            // different and a log that says "rollback ok" here would be a lie.
            r.permRollbackResult = 2u;
        }

        // Then do the best that can be done about execute, and report what we
        // ended up with instead of assuming it. Note this uses the same ladder,
        // so on real hardware the Perm_R rung is the one that will land.
        uint32_t rbRungs[kPermLadderSteps];
        uint32_t rbCount = 0;
        uint32_t rbWinner = 0;
        Result rbQ = 0;
        uint32_t rbPerm = 0;
        (void)applyPagePerm(page, rbRungs, &rbCount, &rbWinner, &rbQ, &rbPerm);
        r.permRollbackQueryRes = static_cast<uint32_t>(rbQ);
        r.permRollbackFinal = rbPerm;
        r.permRollbackExec = permHasExec(rbPerm);

        jitTransitionToWritable(&g_state.jit);
        jitClose(&g_state.jit);
        // The perm fault is the diagnosis. The restore verification is still
        // run, because "the permission step failed" and "the window is not the
        // guest's own bytes" are independent and the second one is worse.
        r.stage = windowRestored(reinterpret_cast<const uint8_t*>(guestSiteRuntimeVa))
                      ? permFault
                      : kDeInfoRestoreVerifyFailed;
        if (r.permRollbackResult == 2u) {
            r.stage = kDeInfoRollbackPermFailed;
        }
        return publish(false);
    }

    g_state.installed = true;
    g_state.siteVa = guestSiteRuntimeVa;
    r.stage = kDeInfoOk;
    r.installed = 1;
    return publish(true);
}

bool rr3DeInfoUninstall(DeInfoInstallReport* report) {
    DeInfoInstallReport r{};
    if (!g_state.installed) {
        r.stage = kDeInfoOk;  // idempotent
        if (report != nullptr) {
            *report = r;
        }
        return true;
    }

    const uintptr_t site = g_state.siteVa;
    void* const page = reinterpret_cast<void*>(site & kPageMask);
    r.pageVa = static_cast<uint64_t>(site & kPageMask);
    r.siteVa = site;
    r.trampolineVa = reinterpret_cast<uintptr_t>(jitGetRxAddr(&g_state.jit));

    // The page as it is now, before we touch it. install() measured the same
    // thing into permBefore, so the pair brackets the whole lifetime of the
    // patch and says whether uninstall handed the guest back the page it had.
    Result preQuery = 0;
    r.permBefore = queryPagePerm(page, &preQuery);
    r.permQueryResPre = static_cast<uint32_t>(preQuery);

    if (R_FAILED(svcSetMemoryPermission(page, kPageSize,
                                       static_cast<uint32_t>(Perm_Rw)))) {
        // Cannot write, so cannot undo. Report the truth rather than claiming
        // success: the stub is still live and the JIT buffer must stay alive.
        r.permRollbackResult = 2u;
        r.stage = kDeInfoPermRwFailed;
        if (report != nullptr) {
            *report = r;
        }
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(site), kDeInfoEatenWordsData,
                kDeInfoWindowBytes);
    armDCacheFlush(reinterpret_cast<void*>(site), kDeInfoWindowBytes);
    armICacheInvalidate(reinterpret_cast<void*>(site), kDeInfoWindowBytes);

    // Same ladder and same measurement as install(). The byte-for-byte check
    // below stays exactly as strict as it was -- kDeInfoRestoreVerifyFailed
    // must keep firing on a write that did not take -- but a correct window on
    // a page the guest can no longer execute is not a successful uninstall, so
    // that is a separate stage now instead of a silent one.
    Result permQ = 0;
    (void)applyPagePerm(page, r.permLadderRc, &r.permLadderLen,
                        &r.permRestoreRung, &permQ, &r.permFinal);
    r.permQueryRes = static_cast<uint32_t>(permQ);
    r.permExecFinal = permHasExec(r.permFinal);
    r.permRestoreMode = r.permRestoreRung != 0u ? 1u : 0u;
    r.permRollbackResult = 1u;
    r.permRollbackQueryRes = r.permQueryRes;
    r.permRollbackFinal = r.permFinal;
    r.permRollbackExec = r.permExecFinal;

    // Read the window back and compare it to the guest's original words, one
    // byte at a time. "It should have been restored" is not a verification; a
    // page that came back Perm_Rw-sticky or a truncated write shows up here.
    const uint8_t* const window = reinterpret_cast<const uint8_t*>(site);
    const bool restored = windowRestored(window);
    for (uint32_t i = 0; i < kDeInfoEatenWords; ++i) {
        r.stubWords[i] = read32(window + i * 4);
    }

    r.stage = restored ? kDeInfoOk : kDeInfoRestoreVerifyFailed;
    if (restored && r.permRestoreRung == 0u) {
        r.stage = kDeInfoPermRungsExhausted;
    } else if (restored && R_FAILED(static_cast<Result>(r.permQueryRes))) {
        r.stage = kDeInfoPermQueryFailed;
    } else if (restored && r.permExecFinal == 0u) {
        r.stage = kDeInfoPermExecLost;
    }

    g_state.installed = false;
    // Safe now that no branch can reach the trampoline. There is no
    // concurrency guard on this by design: uninstall is a frame-loop action
    // taken at a point where the guest thread is known not to be in the loop.
    (void)jitTransitionToWritable(&g_state.jit);
    jitClose(&g_state.jit);
    g_state = InstallState{};

    if (report != nullptr) {
        *report = r;
    }
    return restored;
}

}  // namespace rr3