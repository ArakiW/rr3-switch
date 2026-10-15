#pragma once

// Container-teardown loop probe for libRealRacing3.so.
//
// Guest ELF VA 0xcd2aa0 sits inside an 8 KiB real cycle (scc.py: cycle
// [0xcd15ec..0xcd35ec], closed by `b` at 0xcd35ec) in the function
// [0xcd1014, 0xcd3648). The main thread wedges there and we have never been
// able to read the loop's two cursor words. This probe overwrites the 16 bytes
// at that site with a position-independent absolute jump into a hand-assembled
// trampoline that copies the two words out, steps a release counter, replays
// the four guest instructions it displaced, and jumps back to site+16.
//
// Nothing here is installed by the build. The frame loop calls
// rr3DeInfoInstall() / rr3DeInfoDrain() / rr3DeInfoUninstall() when it wants
// to; until then this is inert data plus dead code.

#include <stdint.h>

namespace rr3 {

// The window the guest holds today. install() refuses to patch unless all four
// words actually match this, so a re-based or differently-built guest fails
// closed instead of corrupting its own code.
inline constexpr uint64_t kDeInfoSiteElfVa = 0x000cd2aa0ull;
inline constexpr uint32_t kDeInfoOriginalWord = 0xf85b83b4u;  // ldur x20,[x29,#-0x48]

// An absolute jump on AArch64 has to be synthesised, and the smallest
// self-contained form that fits in 16 bytes puts the constant FIRST and reaches
// back to it:
//
//   0x00  ldr  x16, #8        0x58000050
//   0x04  br   x16            0xd61f0200
//   0x08  <8 bytes>          the constant: the trampoline's address
//
// The constant sits AFTER the branch that consumes it, which is what makes 16
// bytes enough: the forward-looking spelling (`ldr`, `b .+8`, `.dword`, `br`) is
// five words and twenty bytes and does not fit the window at all. No skip branch
// is needed because `br x16` leaves before +0x08 is ever fetched; the two
// literal words are read by the ldr, never executed.
//
// The only constrained distance is the ldr's own +8, which is fixed by the
// layout and trivially inside the +/-1 MiB LDR-literal window. The 64-bit
// constant has no range limit, so this works at any trampoline address -- which
// matters, because jitCreate() lands the trampoline 12.4 GB from the site
// (measured: site 0x2106a2baa0, trampoline 0x23e8cc1000). That distance is why
// the previous design, a 4-byte `B` reaching +/-128 MiB, failed closed on every
// run.
inline constexpr uint32_t kDeInfoWindowBytes = 0x10u;
inline constexpr uint32_t kDeInfoEatenWords = 4;

// Guest 0xcd2aa0 .. 0xcd2ab0, verbatim from the ELF:
//
//   +0  0xf85b83b4  ldur x20, [x29, #-0x48]
//   +4  0xb4000354  cbz   x20, 0xcd2b0c
//   +8  0xf85c03b5  ldur x21, [x29, #-0x40]
//   +12 0xeb1402bf  cmp   x21, x20
//
// install() checks all four, so a window that merely *starts* right is not
// enough. The trampoline replays +0, +8 and +12 with these exact encodings;
// +4 cannot be replayed verbatim because its displacement is PC-relative, so it
// is re-expressed as the absolute transfer it denotes (see kDeInfoCbzTargetElfVa).
inline constexpr uint32_t kDeInfoEatenWordsData[kDeInfoEatenWords] = {
    0xf85b83b4u, 0xb4000354u, 0xf85c03b5u, 0xeb1402bfu,
};

// Where the eaten `cbz` goes, and where the guest is handed back. Both are guest
// ELF VAs; install() rebases them onto the site it was handed.
inline constexpr uint64_t kDeInfoCbzTargetElfVa = 0x000cd2b0cull;
inline constexpr uint64_t kDeInfoResumeElfVa = 0x000cd2ab0ull;  // == site + 16

// 76 instructions + a 40-byte literal pool. Regenerate with
// tools/rr3-deinfo-probe/build-trampoline.py.
inline constexpr uint32_t kDeInfoTrampolineSize = 0x158u;

// One captured pass through the loop.
struct DeInfoSample {
    uint64_t b;    // [x29-0x48] -- the sentinel the walk compares against
    uint64_t e;    // [x29-0x40] -- the node currently being examined
    uint32_t seq;  // trampoline pass counter at the moment of capture
};

// Failure stages, for report->stage. 0..11 keep their historical values so old
// log lines stay readable; 12.. are the checks the 16-byte window added.
enum DeInfoStage : int32_t {
    kDeInfoOk = 0,
    kDeInfoAlreadyInstalled = 1,
    kDeInfoBadSiteAlignment = 2,
    kDeInfoSiteWordMismatch = 3,       // window word 0 did not match
    kDeInfoJitCreateFailed = 4,
    kDeInfoRelocRange = 5,             // unused now: nothing range-limited remains
    kDeInfoRelocSentinelMismatch = 6,  // a literal slot was not the placeholder
    kDeInfoBranchOutOfRange = 7,       // unused now: the entry jump is absolute
    kDeInfoTrampolineMisaligned = 8,
    kDeInfoPermRwFailed = 9,
    kDeInfoAlreadyPatched = 10,        // the window holds our stub, state was lost
    kDeInfoInstallRestored = 11,       // historical: "failed, words went back".
                                       // No longer produced -- the perm verdict
                                       // below names the reason instead -- but
                                       // the value stays so 04:31 and older
                                       // log lines still read correctly.
    kDeInfoWindowWordMismatch = 12,    // window word 1..3 did not match
    kDeInfoWindowStraddlesPage = 13,   // window is not inside one 4 KiB page
    kDeInfoLiteralSlotMismatch = 14,   // a `ldr xN, lit` moved under us
    kDeInfoStubEncodeFailed = 15,      // entry stub did not assemble as specified
    kDeInfoRestoreVerifyFailed = 16,   // uninstall wrote but the bytes disagree
    // 17..20 are the permission verdict, split up because "it did not install"
    // is not a diagnosis. The window is 16 bytes and the page is Perm_Rw at
    // this point in every one of these, so each of them means the guest has to
    // be rolled back before the function returns.
    kDeInfoPermExecLost = 17,          // a rung was accepted but perm lost Perm_X
    kDeInfoPermQueryFailed = 18,       // svcQueryMemory failed: cannot prove X
    kDeInfoPermRungsExhausted = 19,    // every rung of the ladder was refused
    kDeInfoRollbackPermFailed = 20,    // could not take Perm_Rw to put the words back
};

struct DeInfoInstallReport {
    int32_t stage;
    uint32_t installed;         // 1 once the stub is live
    uint64_t siteVa;            // runtime address actually patched
    // The page those bytes live on, i.e. what is handed to the kernel. Kept
    // separate from siteVa on purpose: a correct site with a wrong page is a
    // shape that hides completely when only the site is logged.
    uint64_t pageVa;
    uint64_t trampolineVa;      // runtime address the stub jumps to
    int64_t branchDelta;        // trampolineVa - siteVa, bytes (informational now)
    uint32_t wordWritten;       // first word of the 16-byte stub
    uint32_t jitCreateResult;   // libnx Result from jitCreate
    // 1 = some rung of the permission ladder was accepted, 0 = every rung was
    // refused. Which rung, and the raw Result of each one, are in
    // permRestoreRung / permLadderRc. This is a yes/no because the stage
    // field already says which of the two happened.
    uint32_t permRestoreMode;
    // libnx Result of the install's OWN svcSetMemoryPermission(Perm_Rw) on the
    // guest page. Recorded because stage 9 alone cannot distinguish "the page
    // refused" from "the syscall refused" -- see section 23.8 of the handoff.
    uint32_t permRwResult;
    // Result of the SAME Perm_Rw request made BEFORE jitCreate, so one run
    // tells us whether jitCreate is what breaks the later call. See the
    // PRE-jitCreate probe in rr3_deinfo_probe.cpp.
    uint32_t permRwPreResult;
    uint32_t alreadyInstalled;
    uint32_t windowVerified;    // 4 once all four eaten words matched
    uint64_t cbzTargetVa;       // runtime address the replayed cbz goes to
    uint64_t resumeVa;          // runtime address the guest is handed back to
    uint32_t stubWords[4];      // the exact 16 bytes written into guest .text

    // ---- the permission verdict -------------------------------------------
    //
    // Perm_Rx cannot be REQUESTED. svc.h:470 says "Perm_X is not allowed" and
    // switchbrew names the Result: 0xD801, "Valid permissions are ---, r-- and
    // rw-". So the page does not become Perm_Rx; it becomes whatever the
    // kernel's own mapping already says once read/write is dropped, and the
    // only honest way to report that is to read it back. svcQueryMemory's
    // MemoryInfo carries a `perm` field (svc.h:96) and the query is plain and
    // unprivileged, so the fields below are measurements, not inferences.

    // The page as it was BEFORE install() touched it, and that query's Result.
    uint32_t permQueryResPre;
    uint32_t permBefore;
    // The PRE-jitCreate probe's own restore, which is the first measurement of
    // whether a Perm_Rw round trip costs the execute bit.
    uint32_t permPreRestoreRc;
    uint32_t permPreQueryRes;
    uint32_t permPreFinal;
    // The post-write ladder: every rung's Result, how many were tried, and
    // which one was accepted (1-based; 0 = none).
    uint32_t permLadderRc[5];
    uint32_t permLadderLen;
    uint32_t permRestoreRung;
    // What the page actually IS after the ladder, which is the field that
    // answers "can the guest still execute this page".
    uint32_t permQueryRes;
    uint32_t permFinal;
    uint32_t permExecFinal;     // 1 if (permFinal & Perm_X)

    // The fail-closed path, reported rather than assumed: 0 = not taken,
    // 1 = the four original words are back, 2 = Perm_Rw itself was refused so
    // the words could NOT be put back. 2 is the one state in which guest
    // .text is left holding sixteen bytes of stub, and it is why it gets its
    // own value instead of a boolean.
    uint32_t permRollbackResult;
    uint32_t permRollbackRwResult;
    uint32_t permRollbackQueryRes;
    uint32_t permRollbackFinal;
    uint32_t permRollbackExec;   // 1 if (permRollbackFinal & Perm_X)
};

// Patches the 16 bytes at guestSiteRuntimeVa. Idempotent.
bool rr3DeInfoInstall(uintptr_t guestSiteRuntimeVa, DeInfoInstallReport* report);

// Restores the original four words. Idempotent; safe to call when never
// installed. Returns false and reports kDeInfoRestoreVerifyFailed if the bytes
// it wrote back do not read back equal.
bool rr3DeInfoUninstall(DeInfoInstallReport* report);

bool rr3DeInfoInstalled();

// True, and fills *out, only when the loop has been entered at least once
// since the previous call. Performs no I/O and takes no lock -- the caller
// logs. False means "nothing new", not "nothing happened".
bool rr3DeInfoDrain(DeInfoSample* out);

}  // namespace rr3