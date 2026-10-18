#include "rr3/jni_bridge.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rr3/elf_preflight.hpp"
#include "rr3/rr3_deinfo_probe.hpp"
#include "rr3/gfx_backend.hpp"
#include "rr3/guest_input.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/pthread_bridge.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/tls_probe.hpp"

// The deferred-callback pump (jni_compat/classes/cloudcell_defer.cpp). It is
// called on the thread that runs the guest's per-frame callbacks, bracketing
// each one, because on Android the replies these stubs owe the engine arrive
// from the UI thread through GLView.queueEvent - never inside the call that
// raised them. Draining around the view callbacks is the host equivalent of
// that queueEvent drain.
//
// Header comment aside, nothing in the tree called this until now.
#include "jni_compat/classes/cloudcell_defer.h"

namespace rr3 {

// Whether the host frame loop calls the guest's onViewRenderJNI every frame.
// Defined here because the frame-loop-driver log line prints it, and that call
// site sits ~500 lines below where the switch is otherwise used.
//
// HISTORY, because a negative result I believed for a day turned out to be a
// CONFOUNDED one, and that is the part worth keeping.
//
//   2026-10-04  set to 0: per-frame re-entry never returned, run never got past
//                CloudCell ordinal 686.
//   2026-10-11  set to 1, then BACK to 0 after gui4/gui5 reproduced the wedge
//                (frames 24000 -> 10, threads 6 -> 1, imports 28541 -> 385).
//                I wrote that up as "per-frame re-entry destroys the guest's
//                threads" and treated gui4 as the refutation.
//
//   2026-10-11  ...that conclusion is RETRACTED. gui4 and gui5 both ran
//                backend=mesa WITHOUT nofb=1 -- the configuration later shown to
//                wedge on its own, because nativeUiInit()'s framebufferCreate
//                on the default NWindow poisons it and Mesa's eglInitialize
//                never recovers. mesa2 added nofb=1 and the EGL ladder
//                completed. So both "refutations" were measuring the framebuffer
//                bug, not this switch. The switch was never tested on a working
//                backend at all.
//
//                gui4's own guest log is the tell. Its last words are
//                    Bad config. Corrupted JSON config: unexpected end of input
//                    START / RESUME / startMusic disabled during download
//                The guest never reached the GUI load stage around ordinal 900
//                that healthy runs reach. It died at STARTUP, long before any
//                rendering could be re-entered.
//
// So it is 1 again, now on the CORRECT base (backend=mesa + nofb=1). The guest
// draws only 6 four-vertex triangle strips per run with splashExit pinned at 0,
// and it has no Choreographer at all (a scan for Looper, Handler,
// MessageQueue, SurfaceHolder, GLSurfaceView, onDrawFrame and postDelayed
// returns ZERO hits), so the host driving the draw entry per frame is exactly
// what it is missing.
//
// Acceptance, fixed before running:
//   draws climbs past 6 AND lit > 0  -> the black screen was never a rendering
//                                      problem; the guest simply was never asked
//   wedge again (frames ~10, threads 1) -> the switch really is fatal, and the
//                                      original 2026-10-04 conclusion finally
//                                      holds on an unconfounded measurement
//
// Read `draws` only: calls/frame is the host's own glGetError and says nothing
// about the guest. And read `flr=` here before drawing any conclusion --
// render-call-pre/post are gated on straddleOpen alone, never on this switch,
// so both appear even when no guest call happened at all (gui3 recorded both,
// pre@4902 post@4918).
#ifndef RR3_FRAME_LOOP_RENDER
#define RR3_FRAME_LOOP_RENDER 1
#endif

// Declared here rather than in rr3/gfx_backend.hpp, for the same reason
// main.cpp declares gfxZeroPageRequest: this is an emulator-only opt-in, not
// part of the graphics contract. The conf file itself stays single-sourced in
// gfx/gfx_backend.cpp. External linkage, so they resolve to the one definition.
const char* gfxRenderThreadName();
bool gfxRenderThreadIsWorker();

namespace {

// Minimal cloudcell vtable stubs. Known call sites:
//   vtable+48   main-render path (void, must preserve this/x0)
//   vtable+136  main-render path (returns bool in x0, 0 = proceed)
//   vtable+272  CloudcellResume (void)
// Fill all slots with safe stubs so no NULL dereference occurs.
//
// EVERY PINNED SLOT GETS ITS OWN TRACING ENTRY. The generic stubs cannot be used
// for pins: slots 6, 8, 17, 18 and 34 were pinned to a shared non-tracing stub,
// so the guest calling slot 18 at 0x13c1fcc was never recorded and the cc-slot
// log silently omitted it. Pinning a scalar slot then made slot 59 "vanish" from
// the trace, which reads exactly like the fix working. A pin that swaps a tracing
// trampoline for a silent one makes the instrument stop reporting the very slot it
// changes -- probe silence is indistinguishable from symptom silence.
void cloudcellTraceSlot(int slot, void* self, uintptr_t caller);

#define RR3_CC_PIN_RECV(n)                                                     \
    extern "C" void* cloudcellPinRecv##n(void* self) {                         \
        cloudcellTraceSlot((n), self,                                          \
                           reinterpret_cast<uintptr_t>(                       \
                               __builtin_return_address(0)));                  \
        return self;                                                           \
    }
#define RR3_CC_PIN_ZERO(n)                                                     \
    extern "C" uint64_t cloudcellPinZero##n(void* self) {                     \
        cloudcellTraceSlot((n), self,                                          \
                           reinterpret_cast<uintptr_t>(                       \
                               __builtin_return_address(0)));                  \
        (void)self;                                                            \
        return 0;                                                              \
    }
#define RR3_CC_PIN_STR(n)                                                      \
    extern "C" uint64_t cloudcellPinStr##n(void* self) {                      \
        cloudcellTraceSlot((n), self,                                          \
                           reinterpret_cast<uintptr_t>(                       \
                               __builtin_return_address(0)));                  \
        (void)self;                                                            \
        return static_cast<uint64_t>(                                         \
            reinterpret_cast<uintptr_t>(kCcEmptyString));                      \
    }

RR3_CC_PIN_RECV(6)   RR3_CC_PIN_ZERO(17) RR3_CC_PIN_RECV(18)
RR3_CC_PIN_RECV(8)   RR3_CC_PIN_RECV(34)
RR3_CC_PIN_ZERO(59)

// Slot 27 (+0xd8) is the "am I authenticated?" decision point.
//
//   FUN_01e0307c:
//     plVar13 = slot43()(*(PTR_...6d5d0));       // factory
//     if ((plVar13->vtable[0xd8]() & 1) == 0) {
//         log("CC: Unable to send Sync, Social Media not initialised.");
//         return;                                 // clean give-up
//     }
//     ... ExclusiveMonitorPass + Yield/nanosleep wait loop ...
//
// The stub returned `self`, i.e. non-zero, so the engine believed it was
// authenticated and entered the wait loop that never ends. Reporting zero
// here is the same truthful answer as slot 46's clock and byte 1217's
// availability flag: this host has no Cloudcell backend, so say so and let the
// engine take its own give-up path.
extern "C" uint64_t cloudcellPinZero27(void* self) {
    cloudcellTraceSlot(27, self,
                       reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    (void)self;
    return 0;
}

// Slot 46 (+0x170) is not a scalar -- it is `double now_ms()`, and the two
// callers read it straight out of the FP return register with no fmov:
//
//   0x13c1fac  blr  x9
//   0x13c1fb0  fcvtzs w8, d0          ; d0, not x0
//   0x13c1fb8  str  w8, [x19,#0x58]   ; last-sync timestamp
//
//   FUN_014c2098: if (last + 600 < (int)now()) FUN_014c1f70();  -- a 600 ms throttle
//
// Returning a uint64 here put the value in x0 while the guest read d0, so the
// throttle compared against whatever happened to be in the FP registers. Pinning
// it to zero was worse: the guard became permanently false and the whole periodic
// sync stopped running. It has to be a real monotonic millisecond clock, which
// is this host truthfully reporting its own time -- not a fabricated service
// response.
extern "C" double cloudcellPinTime46(void* self) {
    cloudcellTraceSlot(46, self,
                       reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    (void)self;
    return static_cast<double>(svcGetSystemTick() / 1000000ULL);
}

// ---------------------------------------------------------------------------
// Per-slot Cloudcell vtable trampolines.
//
// The guest's Cloudcell init (libRealRacing3.so ~0xcd0c34, the function that
// logs "Cloudcell initializing with environment: %s") reaches the synthetic
// object through vtable slots 18 (+0x90) and 44 (+0x160). Before this, every
// unpinned slot shared one i%2 alternation, so we could not see WHICH slot the
// guest calls, how often, or from where. Each slot now has its own entry that
// records (slot, caller, this) under a global budget and then returns exactly
// what the old parity default returned -- observation only, no behaviour change.
//
// Bounded on purpose: a hot slot must not be able to drown the SD card, and the
// budget is a single global atomic (per-thread dedup is not a cap).
// ---------------------------------------------------------------------------
#ifndef RR3_CC_SLOT_TRACE_BUDGET
// 64 was sized before the run got past ordinal 686 and the guest started
// calling slots from its steady-state loop; startup alone burns ~60 of them,
// so every post-boot call was silently dropped. 512 keeps the same "bounded
// so a hot slot cannot flood the SD card" property while leaving the
// steady-state observable -- which is where every remaining question lives.
#define RR3_CC_SLOT_TRACE_BUDGET 512
#endif
constexpr unsigned kCloudcellSlotTraceBudget = RR3_CC_SLOT_TRACE_BUDGET;
std::atomic<unsigned> g_cloudcellSlotTraces{kCloudcellSlotTraceBudget};

void cloudcellTraceSlot(int slot, void* self, uintptr_t caller) {
    if (g_cloudcellSlotTraces.fetch_sub(1, std::memory_order_relaxed) == 0) {
        return;   // exhausted: unsigned wrap keeps it at 0, so stop writing
    }
    // TWO MEASUREMENTS THAT NEED NO ADDRESS ARITHMETIC.
    //
    // (1) The frame record under our own feet. This function has not built a
    // frame of its own, so x29 on entry is still the CALLER's -- the guest's.
    // [x29] and [x29+8] are therefore the guest's own saved frame pointer and
    // return address, read directly out of guest memory. slr is a guest code
    // address that can be trusted without subtracting a load bias, which is the
    // thing the caller field has been getting wrong.
    uintptr_t fp = 0;
    __asm__("mov %0, x29" : "=r"(fp));
    uintptr_t savedFp = 0;
    uintptr_t savedLr = 0;
    if (fp != 0 && (fp & 0xF) == 0) {
        savedFp = *reinterpret_cast<const volatile uintptr_t*>(fp);
        savedLr = *reinterpret_cast<const volatile uintptr_t*>(fp + 8);
    }
    // (2) The eight bytes AT the return address. The first PT_LOAD of
    // libRealRacing3.so is R-X, so this page is readable and these are the
    // actual instruction bytes the guest is about to execute. Comparing them
    // against the same offset in reverse/native/arm64/libRealRacing3.so
    // settles at runtime, with no inference, whether the loaded image is the
    // one on disk.
    uint32_t word0 = 0;
    uint32_t word1 = 0;
    if (caller != 0 && (caller & 3u) == 0u) {
        word0 = *reinterpret_cast<const volatile uint32_t*>(caller);
        word1 = *reinterpret_cast<const volatile uint32_t*>(caller + 4);
    }
    char line[256];
    __builtin_snprintf(line, sizeof(line),
                       "cc-slot: slot=%d self=%llx caller=%llx fp=%llx sfp=%llx "
                       "slr=%llx at=%08x%08x",
                       slot,
                       (unsigned long long)(uintptr_t)self,
                       (unsigned long long)caller,
                       (unsigned long long)fp,
                       (unsigned long long)savedFp,
                       (unsigned long long)savedLr,
                       word0, word1);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", line,
                        300 + static_cast<uint64_t>(slot),
                        static_cast<uint64_t>(slot));
}

// One trampoline per slot, and one table entry per slot, both generated, so
// the two can never drift apart.
// The return value here was originally "even slot -> 0, odd slot -> self".
// That parity rule is wrong: the Cloudcell init (libRealRacing3.so
// 0xcd1014..0xcd35xx) uses several slots as FACTORIES whose return value it
// immediately dereferences as a receiver -- 0xcd1710/0xcd1744 (slot 18) and
// 0xcd1ae4/0xcd1ae8 (slot 8) are both proven, and each time the slot was
// pinned to return `self` the guest walked one step further into init instead
// of faulting on a null read. Unpinned slots therefore default to `self`;
// the few with known-different semantics are overridden by kPins.
#define RR3_CC_SLOT(n)                                                       \
    extern "C" uint64_t cloudcellSlot##n(void* self) {                       \
        cloudcellTraceSlot((n), self,                                        \
                           reinterpret_cast<uintptr_t>(                     \
                               __builtin_return_address(0)));                \
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self));     \
    }
RR3_CC_SLOT(0)  RR3_CC_SLOT(1)  RR3_CC_SLOT(2)  RR3_CC_SLOT(3)
RR3_CC_SLOT(4)  RR3_CC_SLOT(5)  RR3_CC_SLOT(6)  RR3_CC_SLOT(7)
RR3_CC_SLOT(8)  RR3_CC_SLOT(9)  RR3_CC_SLOT(10) RR3_CC_SLOT(11)
RR3_CC_SLOT(12) RR3_CC_SLOT(13) RR3_CC_SLOT(14) RR3_CC_SLOT(15)
RR3_CC_SLOT(16) RR3_CC_SLOT(17) RR3_CC_SLOT(18) RR3_CC_SLOT(19)
RR3_CC_SLOT(20) RR3_CC_SLOT(21) RR3_CC_SLOT(22) RR3_CC_SLOT(23)
RR3_CC_SLOT(24) RR3_CC_SLOT(25) RR3_CC_SLOT(26) RR3_CC_SLOT(27)
RR3_CC_SLOT(28) RR3_CC_SLOT(29) RR3_CC_SLOT(30) RR3_CC_SLOT(31)
RR3_CC_SLOT(32) RR3_CC_SLOT(33) RR3_CC_SLOT(34) RR3_CC_SLOT(35)
RR3_CC_SLOT(36) RR3_CC_SLOT(37) RR3_CC_SLOT(38) RR3_CC_SLOT(39)
RR3_CC_SLOT(40) RR3_CC_SLOT(41) RR3_CC_SLOT(42) RR3_CC_SLOT(43)
RR3_CC_SLOT(44) RR3_CC_SLOT(45) RR3_CC_SLOT(46) RR3_CC_SLOT(47)
RR3_CC_SLOT(48) RR3_CC_SLOT(49) RR3_CC_SLOT(50) RR3_CC_SLOT(51)
RR3_CC_SLOT(52) RR3_CC_SLOT(53) RR3_CC_SLOT(54) RR3_CC_SLOT(55)
RR3_CC_SLOT(56) RR3_CC_SLOT(57) RR3_CC_SLOT(58) RR3_CC_SLOT(59)
RR3_CC_SLOT(60) RR3_CC_SLOT(61) RR3_CC_SLOT(62) RR3_CC_SLOT(63)
#undef RR3_CC_SLOT

#define RR3_CC_TABLE(n) reinterpret_cast<uint64_t>(&cloudcellSlot##n),
constexpr uint64_t kCloudcellSlotTable[64] = {
    RR3_CC_TABLE(0)  RR3_CC_TABLE(1)  RR3_CC_TABLE(2)  RR3_CC_TABLE(3)
    RR3_CC_TABLE(4)  RR3_CC_TABLE(5)  RR3_CC_TABLE(6)  RR3_CC_TABLE(7)
    RR3_CC_TABLE(8)  RR3_CC_TABLE(9)  RR3_CC_TABLE(10) RR3_CC_TABLE(11)
    RR3_CC_TABLE(12) RR3_CC_TABLE(13) RR3_CC_TABLE(14) RR3_CC_TABLE(15)
    RR3_CC_TABLE(16) RR3_CC_TABLE(17) RR3_CC_TABLE(18) RR3_CC_TABLE(19)
    RR3_CC_TABLE(20) RR3_CC_TABLE(21) RR3_CC_TABLE(22) RR3_CC_TABLE(23)
    RR3_CC_TABLE(24) RR3_CC_TABLE(25) RR3_CC_TABLE(26) RR3_CC_TABLE(27)
    RR3_CC_TABLE(28) RR3_CC_TABLE(29) RR3_CC_TABLE(30) RR3_CC_TABLE(31)
    RR3_CC_TABLE(32) RR3_CC_TABLE(33) RR3_CC_TABLE(34) RR3_CC_TABLE(35)
    RR3_CC_TABLE(36) RR3_CC_TABLE(37) RR3_CC_TABLE(38) RR3_CC_TABLE(39)
    RR3_CC_TABLE(40) RR3_CC_TABLE(41) RR3_CC_TABLE(42) RR3_CC_TABLE(43)
    RR3_CC_TABLE(44) RR3_CC_TABLE(45) RR3_CC_TABLE(46) RR3_CC_TABLE(47)
    RR3_CC_TABLE(48) RR3_CC_TABLE(49) RR3_CC_TABLE(50) RR3_CC_TABLE(51)
    RR3_CC_TABLE(52) RR3_CC_TABLE(53) RR3_CC_TABLE(54) RR3_CC_TABLE(55)
    RR3_CC_TABLE(56) RR3_CC_TABLE(57) RR3_CC_TABLE(58) RR3_CC_TABLE(59)
    RR3_CC_TABLE(60) RR3_CC_TABLE(61) RR3_CC_TABLE(62) RR3_CC_TABLE(63)
};
#undef RR3_CC_TABLE

// ---------------------------------------------------------------------------
// Second-level Cloudcell object: the data member at byte offset 0x10.
//
// The first-level cc-slot trace proves the guest reaches our synthetic object
// and that its first-level virtual calls work. What does NOT work is the
// member at +0x10, which s_ccBuf[2] left at zero. libRealRacing3.so
// 0xcd1d80 (inside CC_Helpers::Manager::Manager, 0xcd1014..0xcda8e4):
//
//   cd1d80  ldr  x8, [x28]         ; x28 = *(GOT 0x1f6d5d0) -> our object
//   cd1d84  ldr  x23, [x8, #0x10]  ; x23 = member +0x10  == s_ccBuf[2] == 0
//   cd1d88  ldr  x8, [x23]         ; reads ITS OWN vtable -> ldr from [0]
//
// So s_ccBuf[2] is a pointer to a SEPARATE C++ object, and the guest then
// makes virtual calls on that object. A full capstone sweep of the manager
// body for `mov x0, x23` + `ldr x8,[x8,#imm]` + `blr x8` finds exactly four
// dispatched slots on this second-level object:
//
//   slot  8 (+0x40)  0xcd2e40  -> x22, and 0xcd2e54 does `ldr x8, [x22]`
//                               => FACTORY, return is dereferenced as a
//                                  receiver. Returning `self` is
//                                  self-consistent here (element 0 is the
//                                  vtable), so it keeps the default.
//   slot 11 (+0x58)  0xcd1d90  -> x1, then basic_string ctor at 0xcd1da0
//   slot 15 (+0x78)  0xcd1db0  -> x1, then basic_string ctor at 0xcd1dbc
//   slot 16 (+0x80)  0xcd1e38  -> x1, then basic_string ctor at 0xcd1e48
//
// Slots 11, 15 and 16 are the same shape, so they all get the string
// treatment; 16 is not in the original two-slot brief but was measured, and
// leaving it on the `self` default would just move the fault one instruction
// later. A sweep of 0xcd1d00..0xcd2200 found no path literal being loaded
// there, so the intended CONTENT of those three strings is not derivable from
// the binary yet; they return "" -- NUL-terminated and safe, and the win is
// that the guest no longer dereferences null or runs strlen over an object
// address.
//
// 32 slots, not 16: the highest slot measured is 16, and the caller at
// 0xcd2e60 reads index 5 and 0xcd2e70 index 5 of the receiver's vtable, so
// headroom past the measured range is deliberate rather than incidental.
// ---------------------------------------------------------------------------
constexpr const char* kCcEmptyString = "";

// ---- Cloudcell async-request completion ---------------------------------
//
// Byte 0x4c1 is the guest's "Cloudcell is available" flag, and it gates TWO
// things with opposite polarity: the main-menu loader (0x12aefd8, needs 1) and
// the async-request guard at 0xab2c30 (needs 0, or the request never completes
// and `pending` at 0x1f970c0 sticks at 1 forever). Measured: byte 0 gives
// 46,630 clean frames and no menu; byte 1 wedges the first frame.
//
// So the request has to actually COMPLETE. Guest-side disassembly, closed
// chain (ELF vaddrs):
//   0xab2c30  slot3(obj) -> x1                 ; a GETTER: the endpoint object,
//                                               ; not a callback
//   0xcf1018  ldr x9,[x1] ; ldr x9,[x9,#0x10] ; blr x9
//             x1->vtable[2](x1, str, w2, std::function)
//             the std::function is on the stack: +0 vtable, +8 invoker
//             (0xcf10d4), +0x18 blob  -- 0x28 bytes total
//   later the callee calls it -> 0xcf11d8 (vtable[6]) -> 0xcf10d4
//             ldr x8,[x0] ; ldr x8,[x8,#0x18] ; blr x8
//   -> blob vtable slot 3 = 0xab36f8, whose tail is unconditional:
//        00ab37e4  adrp  x8, #0x1f97000
//        00ab37e8  strb  wzr, [x8, #0xc0]   ; pending = 0
//
// 0xab36f8 has ZERO direct callers (verified with the correct 6-bit B/BL
// opcode mask, 0x05/0x25, over 545,100 branch words) -- it is reachable only
// through that continuation, so invoking the continuation is the only way to
// clear `pending`.
//
// This is NOT faking the service. The real Cloudcell init at 0xbc2dec (0x3ae4
// bytes) contains no pthread_create and no std::thread: responses arrive on a
// JNI callback. Pumping the continuation from the frame loop is the same shape
// as what the real host does. And the offline path is inside the normal
// callback -- 0xab36f8 does `cmp w21,#1 ; cset w21,eq` and writes a success
// bit; a denial is a record with different values, not a separate branch.
// (There IS a `cmp w21,#2` assert in it, which is why the call is gated.)
struct CloudcellContinuation {
    void* vtable;
    void* invoker;
    void* manager;
    void* payload;
    void* blob;
};
static CloudcellContinuation g_ccPending{nullptr, nullptr, nullptr, nullptr, nullptr};
static bool g_ccRequestSeen = false;

// ---- where the continuation gets invoked (ccmode) -------------------------
//
// The delivery site that existed -- the frame loop, two calls below -- is
// unreachable for the run this exists to fix, and unreachable structurally
// rather than by accident:
//
//   The guest calls slot 3 (a getter) and then slot 2 on the endpoint, handing
//   over a std::function. cloudcellEndpointStubRequest below captures it and
//   returns 0 without ever calling it. The only code that ever calls it is
//   cloudcellPumpCompletion(), and its only call site sits AFTER
//   renderTarget(...) in frame 0's body. The guest blocks INSIDE that call, in
//   pure guest code (inbridge=0), after one last fopen. So the host's own call
//   ordering produced a circular wait: the thread that must deliver the answer
//   is the thread waiting for the answer. Two archived reproductions agree --
//   loop-top is logged once (it only logs for frames < 3), the 30-frame
//   heartbeat never arrives, guest-activity is therefore 0, and the
//   continuation has already been captured twice.
//
// "ccmode" moves the delivery point UP to the capture, which is a context that
// is provably running, and picks the thread it runs on:
//
//   now  invoke it inline, right here. Removes the circularity completely; the
//        cost is guest code running RE-ENTRANTLY inside Cloudcell init, which is
//        the invariant cloudcell_defer.h exists to protect.
//   bg   cloudcell_defer() it. cloudcell_pump() drains the whole queue from
//        whichever thread calls it, and the alert-pump thread -- a raw
//        threadCreate thread that exists for exactly this purpose -- calls
//        cloudcell_pump("alert-pump", false) every ~80 ms for the life of the
//        process (jni_bridge.cpp:2508). So the drain needs no new queue, no new
//        thread and no edit to that file; the callback runs on a thread that is
//        still ticking while the game thread is wedged.
//   off  (default, absent key) nothing here, and the frame-loop site is still
//        the only delivery site -- i.e. today's behaviour, unchanged.
//
// The record is shared state in every mode except off, because bg mode writes
// it on the game thread and reads it on the pump thread. libnx Mutex, NOT
// std::mutex, for the reason cloudcell_defer.cpp measured rather than chose:
// a std mutex on a guest-visible path took host CPU from 13.7 s to 80.7 s by
// dragging the guest's pthread shim into a host lock. It is not recursive, so
// the discipline is take -> copy -> drop, and it is NEVER held across the
// invoke below -- that call is a blr into guest ARM64 code and owes this bridge
// nothing, so a lock held across it would be held until the process died.
static Mutex g_ccPendingLock{};

// Take the pending record, or say why there is nothing to take.
//
// Three answers rather than a bool, because the frame-loop caller runs this
// every frame under "ccdone=1" and the caller of an inline "now" delivery can
// run it from inside the guest. "nothing" has to stay SILENT or it would write
// one log line per frame forever, which is the same reason cloudcell_pump takes
// always_log=false on its per-frame site.
//
// The record is left in place when it is unusable, exactly as it was before
// this lock existed: an unusable vtable is a finding to keep re-reporting, not
// something to consume.
enum class CcTake { kNothing, kUnusable, kTaken };
static CcTake takePendingContinuation(CloudcellContinuation* out,
                                      void** slot6Out) {
    CcTake result = CcTake::kNothing;
    mutexLock(&g_ccPendingLock);
    if (g_ccRequestSeen && g_ccPending.vtable != nullptr) {
        auto** vtable = reinterpret_cast<void**>(g_ccPending.vtable);
        result = (vtable != nullptr && vtable[6] != nullptr) ? CcTake::kTaken
                                                             : CcTake::kUnusable;
        if (result == CcTake::kTaken) {
            *out = g_ccPending;
            *slot6Out = vtable[6];
            g_ccPending = {};
            g_ccRequestSeen = false;
        }
        else {
            // Copied out but NOT consumed: the SKIPPED log line below needs the
            // real vtable to be worth anything, and the record has to survive
            // so the next caller sees the same unusable capture.
            *out = g_ccPending;
        }
    }
    mutexUnlock(&g_ccPendingLock);
    return result;
}

void cloudcellPumpCompletion(const char* mode);

// x1->vtable[2](x1, str, w2, w3, &record)  -- the guest's call at 0xcf107c.
//
// ABI, recovered from the binary and verified byte-for-byte with capstone
// against a control (0x12aefd8 reads back `ldrb w8,[x0,#0x4c1]` from bytes
// 08045339):
//
//   0xcf1050  ldp  w8, w3, [x0, #8]   ; w2 = [obj+8], w3 = [obj+0xc], BOTH 32-bit
//   0xcf105c  str  x21, [sp, #0x20]   ; x21 = sp, saved across the call
//   0xcf1068  mov  x4, sp             ; <-- THE CONTINUATION IS IN x4
//   0xcf106c  mov  x0, x1             ; self
//   0xcf1070  ldr  x9, [x9, #0x10]    ; slot 2
//   0xcf1074  mov  x1, x2             ; const char* into a caller's std::string
//   0xcf1078  mov  w2, w8
//   0xcf107c  blr  x9
//
// So this stub used to read the WRONG REGISTER. It took x3 as the
// continuation pointer, but x3 is a 32-bit scalar field loaded by `ldp w8,
// w3` -- encoding bits[31:30]=00, which cannot carry a 64-bit pointer at ANY
// of the 25 call sites of 0xcf1018. That is why every archived run logged
//
//   vtable=0x0 invoker=0x0 manager=0x0 payload=0x0 blob=0x0 w2=10795
//
// and why one of the two calls logged "fn was NULL": x3 was the small
// constant 0x1da0 (7584) at the confirmed site and 0 at another.
//
// Corroboration that the numbers are the right ones and not coincidence:
// call site 0xab2d1c builds its object with `stur q0,[x0,#8]` from
// .rodata:0x388280 = 2b2a0000 a01d0000 ..., so w2 = 0x2a2b = 10795 -- the
// logged w2 exactly -- and w3 = 0x1da0.
//
// The continuation record is the callee's incoming stack frame: x4 = sp, and
// the five 64-bit words are vptr, invoker, manager, payload, &record, which is
// what CloudcellContinuation above already declares.
//
// Delivery stays on vtable slot 6. After the call returns the guest does
//   0xcf1084 ldr x0,[sp,#0x20] ; 0xcf1088 cmp x21,x0 ; 0xcf108c b.eq #0xcf10a0
// which is a frame-integrity DISPATCH, not a self-comparison: [sp+0x20] held
// sp before the call, so equality means the callee left it alone and the
// guest takes vptr[4] (0xcf11d0 = a bare `ret`, because the response has not
// arrived yet), while inequality means it was clobbered and the guest takes
// vptr[5] (0xcf11d4 = operator delete). Neither delivers anything. vptr[6] is
// 0xcf11d8, a real prologue, and it is the only entry with code -- which is
// why the host stores the record and calls it later.
extern "C" uint64_t cloudcellEndpointStubRequest(void* self, const void* str,
                                                 uint32_t w2, uint32_t w3,
                                                 const CloudcellContinuation* cont) {
    (void)self;
    (void)str;
    (void)w3;
    if (cont != nullptr && cont->vtable != nullptr) {
        // Copied under the lock, not read out of the global for the log below:
        // with "bg" set, a drain thread can take and clear this record between
        // the store above and the snprintf, and an unlocked read of it here
        // would print the zeros the other thread just left behind -- i.e. a log
        // line that contradicts itself.
        CloudcellContinuation captured{};
        {
            mutexLock(&g_ccPendingLock);
            g_ccPending = *cont;
            g_ccRequestSeen = true;
            captured = g_ccPending;
            mutexUnlock(&g_ccPendingLock);
        }
        char diag[256];
        __builtin_snprintf(diag, sizeof(diag),
            "cc-endpoint-request: captured vtable=%p invoker=%p manager=%p"
            " payload=%p blob=%p w2=%u w3=%u",
            captured.vtable, captured.invoker, captured.manager,
            captured.payload, captured.blob, w2, w3);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "cc-endpoint-request", 249, 0, diag);

        // Deliver from HERE, the only context on the capture path that is not
        // downstream of the call the guest is wedged in.
        const char* mode = gfxCloudcellCompleteMode();
        if (std::strcmp(mode, "now") == 0) {
            // Inline. Re-entrant by construction -- the guest is still inside
            // the Cloudcell call that produced this request -- which is the
            // hazard cloudcell_defer.h exists to prevent, so this mode is a
            // bounded A/B and not the safe one.
            cloudcellPumpCompletion("now");
        }
        else if (std::strcmp(mode, "bg") == 0) {
            // Deferred. Nothing is passed in: the record lives in
            // g_ccPending, which the drain reads under g_ccPendingLock.
            cloudcell_defer([]() { cloudcellPumpCompletion("bg"); });
        }
    } else {
        char diag[160];
        __builtin_snprintf(diag, sizeof(diag),
            "cc-endpoint-request: no continuation, cont=%p vtable=%p w2=%u w3=%u",
            (const void*)cont,
            (const void*)(cont != nullptr ? cont->vtable : nullptr), w2, w3);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "cc-endpoint-request", 249, 0, diag);
    }
    return 0;
}

// slot 3 must return an OBJECT, not a scalar: the caller does `ldr x9,[x1]`
// and treats the result as a vtable. Returning `self` (the default stub
// behaviour) makes it dereference the Cloudcell object's own first word.
//
// EVERY entry of that vtable must be populated, or the guest branches to zero.
// This object used to have exactly one --
//
//   009f2a60  ldr  x0, [x21]        ; Cloudcell object
//   009f2a64  ldr  x8, [x0]
//   009f2a68  ldr  x8, [x8, #0x18]  ; slot 3 -> here
//   009f2a6c  blr  x8               ; returns the endpoint
//   009f2a70  ldr  x8, [x0]
//   009f2a74  ldr  x8, [x8, #0x20]  ; endpoint vtable index 4  == 0
//   009f2a78  blr  x8               ; *** branch to 0
//   009f2a7c  add  x0, sp, #8       ; never reached
//
// and that single instruction was the whole "hang": the thread jumped to
// address 0, which is inside the guest space, so it made no imports, held no
// host lock, and consumed no CPU -- exactly the signature the run showed
// (inbridge=0, delta=0, idle_ms=280239). The census agrees independently:
// the wedged thread's x30 is 0x2105e3fa7c in 66 of 66 guest-classified
// samples, which is mainBias+0x9f2a7c -- the return address of that blr.
//
// Every index below 8 gets a real function that logs which index was asked
// for. That is deliberately not an implementation: we do not yet know what
// the guest wants from indices other than 2, and a legible fault that names
// itself is worth more than a plausible-looking value that corrupts state
// quietly. Returning 0 here turns "hangs forever with no output" into either
// progress or a crash report that points straight at the next index.
#define RR3_CC_ENDPOINT_SLOT(n)                                              \
    extern "C" uint64_t cloudcellEndpointSlot##n(void* self) {               \
        char diag[128];                                                      \
        __builtin_snprintf(diag, sizeof(diag),                               \
                            "cc-endpoint-slot: index=%d self=%p", (n), self);\
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",  \
                            "cc-endpoint-slot", 249, 0, diag);               \
        return 0;                                                            \
    }

RR3_CC_ENDPOINT_SLOT(0)
RR3_CC_ENDPOINT_SLOT(1)
RR3_CC_ENDPOINT_SLOT(3)
RR3_CC_ENDPOINT_SLOT(4)
RR3_CC_ENDPOINT_SLOT(5)
RR3_CC_ENDPOINT_SLOT(6)
RR3_CC_ENDPOINT_SLOT(7)
#undef RR3_CC_ENDPOINT_SLOT

// Every UNPINNED first-level Cloudcell slot needs a real function too, for the
// same reason the endpoint vtable needed one. s_ccVtable is 64 entries and only
// ten of them ever had a target -- eight semantic pins plus slot 3 -- so any
// guest call to one of the other 54 was `blr` to address 0.
//
// That is not hypothetical: ccdeliver4 wedged on exactly
//   0xb44808  ldr x8,[x19] ; 0xb44814 ldr x8,[x8,#0x60] ; 0xb44818 blr x8
// i.e. first-level index 12. The second-level table (s_cc2Vtable) is filled
// from kCloudcell2SlotTable and does not have this problem, which is why the
// cc-slot histogram showed slot 12 "being called" four times -- those were the
// second-level calls, and the first-level one is the one that jumped to zero.
//
// As with the endpoint slots this logs, but it MUST return `self` and not 0.
// An unpinned first-level slot is often a GETTER, and the caller dereferences
// the RETURN VALUE as an object rather than calling it. That is exactly this:
//
//   0xb3bc30  ldr  x8, [x8, #0x28]  ; slot 5
//   0xb3bc34  ldr  x9, [x8]         ; the RESULT, used as a vtable
//   0xb3bc3c  ldr  x9, [x9, #0x80]
//   0xb3bc40  blr  x9               ; <- wedged here, lr=0xb3bc44
//
// Returning 0 there makes the guest dereference address 0, read 0x80 bytes
// past it and branch to whatever is found -- which is how the fill below
// turned a working "returns self" into a new wedge in ccdeliver5. `self` is
// the right answer because s_ccBuf[0] is s_ccVtable, so [self] is already a
// valid vtable and index 16 of it is now a real function.
#define RR3_CC_UNPINNED(n)                                                   \
    extern "C" uint64_t cloudcellUnpinned##n(void* self) {                   \
        char d[128];                                                         \
        __builtin_snprintf(d, sizeof(d),                                     \
                           "cc-slot-unpinned: slot=%d self=%p", (n), self);  \
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",  \
                            "cc-slot-unpinned", 249, 0, d);                   \
        return reinterpret_cast<uint64_t>(self);                            \
    }
RR3_CC_UNPINNED(0) RR3_CC_UNPINNED(1) RR3_CC_UNPINNED(2) RR3_CC_UNPINNED(3)
RR3_CC_UNPINNED(4) RR3_CC_UNPINNED(5) RR3_CC_UNPINNED(6) RR3_CC_UNPINNED(7)
RR3_CC_UNPINNED(8) RR3_CC_UNPINNED(9) RR3_CC_UNPINNED(10) RR3_CC_UNPINNED(11)
RR3_CC_UNPINNED(12) RR3_CC_UNPINNED(13) RR3_CC_UNPINNED(14) RR3_CC_UNPINNED(15)
RR3_CC_UNPINNED(16) RR3_CC_UNPINNED(17) RR3_CC_UNPINNED(18) RR3_CC_UNPINNED(19)
RR3_CC_UNPINNED(20) RR3_CC_UNPINNED(21) RR3_CC_UNPINNED(22) RR3_CC_UNPINNED(23)
RR3_CC_UNPINNED(24) RR3_CC_UNPINNED(25) RR3_CC_UNPINNED(26) RR3_CC_UNPINNED(27)
RR3_CC_UNPINNED(28) RR3_CC_UNPINNED(29) RR3_CC_UNPINNED(30) RR3_CC_UNPINNED(31)
RR3_CC_UNPINNED(32) RR3_CC_UNPINNED(33) RR3_CC_UNPINNED(34) RR3_CC_UNPINNED(35)
RR3_CC_UNPINNED(36) RR3_CC_UNPINNED(37) RR3_CC_UNPINNED(38) RR3_CC_UNPINNED(39)
RR3_CC_UNPINNED(40) RR3_CC_UNPINNED(41) RR3_CC_UNPINNED(42) RR3_CC_UNPINNED(43)
RR3_CC_UNPINNED(44) RR3_CC_UNPINNED(45) RR3_CC_UNPINNED(46) RR3_CC_UNPINNED(47)
RR3_CC_UNPINNED(48) RR3_CC_UNPINNED(49) RR3_CC_UNPINNED(50) RR3_CC_UNPINNED(51)
RR3_CC_UNPINNED(52) RR3_CC_UNPINNED(53) RR3_CC_UNPINNED(54) RR3_CC_UNPINNED(55)
RR3_CC_UNPINNED(56) RR3_CC_UNPINNED(57) RR3_CC_UNPINNED(58) RR3_CC_UNPINNED(59)
RR3_CC_UNPINNED(60) RR3_CC_UNPINNED(61) RR3_CC_UNPINNED(62) RR3_CC_UNPINNED(63)
#undef RR3_CC_UNPINNED

extern "C" void* cloudcellSlot3Endpoint(void* self) {
    (void)self;
    static uint64_t s_endpointVtable[8] = {};
    static uint64_t s_endpointObject[4] = {};
    if (s_endpointVtable[2] == 0) {
        s_endpointVtable[0] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot0);
        s_endpointVtable[1] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot1);
        // 2 is the request capture, installed by cloudcellEndpointStubRequest
        // below's own registration; keep it last so the check above still
        // distinguishes "first call" from "already built".
        s_endpointVtable[2] = reinterpret_cast<uint64_t>(&cloudcellEndpointStubRequest);
        s_endpointVtable[3] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot3);
        s_endpointVtable[4] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot4);
        s_endpointVtable[5] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot5);
        s_endpointVtable[6] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot6);
        s_endpointVtable[7] = reinterpret_cast<uint64_t>(&cloudcellEndpointSlot7);
    }
    s_endpointObject[0] = reinterpret_cast<uint64_t>(s_endpointVtable);
    return s_endpointObject;
}

// Call the saved continuation once. 0x14-byte record, matching the guest's own
// reads (0xab36f8 decodes three ints from it). Left all-zero deliberately: the
// offline branch is a record with non-success values, not a special path.
//
// This is a FAILURE record and stays one. There is no Cloudcell server behind
// this stub, so no authentication is invented, no response body is invented and
// no success bit is invented: the guest gets a denial it can decode, clears
// `pending`, and takes its own offline branch. That is the honest outcome, and
// the alternative -- forging a success -- is the one thing this port must not do.
//
// `mode` is the delivery path that got here ("now", "bg" or "frame") and is
// logged, because "the continuation never ran" and "it ran on the wrong thread"
// are different failures and only the thread field separates them.
void cloudcellPumpCompletion(const char* mode) {
    CloudcellContinuation saved{};
    void* slot6 = nullptr;
    const CcTake take = takePendingContinuation(&saved, &slot6);
    if (take == CcTake::kNothing) {
        // Silent by design: the frame-loop caller under "ccdone=1" reaches this
        // once per frame with an empty record. See takePendingContinuation.
        return;
    }
    if (take == CcTake::kUnusable) {
        // ONE-SHOT, added 2026-10-16 after review. The frame-loop caller runs
        // this once per frame under "ccdone=1", and the gate-closed baseline
        // reaches ~46,000 frames, so an unconditional line here is ~46,000
        // 160-byte appends to the SD card that bury the cc-deliver line they
        // exist to explain. Same reason kNothing is silent; the difference is
        // that this case is a finding worth stating once rather than a rate to
        // report. The record is deliberately NOT consumed, so a later capture
        // that is usable still delivers, and this stays spent.
        static bool g_unusableReported = false;
        if (!g_unusableReported) {
            g_unusableReported = true;
            char diag[160];
            __builtin_snprintf(diag, sizeof(diag),
                "cc-complete: SKIPPED, mode=%s thread=0x%x pending_vtable=%p"
                " slot6=%p (one-shot; record kept, not consumed)", mode != nullptr ? mode : "(null)",
                (unsigned)threadGetCurHandle(), saved.vtable, nullptr);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "cc-complete", 249, 0, diag);
        }
        return;
    }
    // The one line per delivery actually attempted, and the one that makes the
    // next run self-diagnosing: it is written BEFORE the invoke, so it survives
    // a continuation that never returns (the alert-pump worker's own comment on
    // entered/drained_total records the same shape of hazard). Fields:
    //   mode     -- where this call was made from, per ccmode
    //   thread   -- libnx handle of the CALLING thread. In bg mode this is the
    //               alert-pump thread, which is how a run distinguishes "the
    //               queue was drained by the background tick" from "the frame
    //               loop reached its own site after all"; in now mode it is the
    //               game thread, re-entered.
    //   valid    -- 1 only when a captured continuation existed AND its vtable
    //               slot 6 (0xcf11d8 -> 0xcf10d4 -> 0xab36f8) was non-null. It
    //               cannot be 0 here; the unusable case returned above and logs
    //               on its own, so valid=1 here is the assertion that the
    //               delivered record was a real capture.
    char diag[192];
    __builtin_snprintf(diag, sizeof(diag),
        "cc-deliver: mode=%s thread=0x%x valid=1 pending_vtable=%p slot6=%p"
        " record=0x14-zero", mode != nullptr ? mode : "(null)",
        (unsigned)threadGetCurHandle(), saved.vtable, slot6);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "cc-deliver", 268, 0, diag);
    using PFNInvoke = void (*)(void*, void*);
    auto invoke = reinterpret_cast<PFNInvoke>(slot6);
    uint8_t record[0x14] = {};
    invoke(&saved, record);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "cc-complete-returned", 249, 0);
}


extern "C" uint64_t cloudcellVtableStubString(void* self) {
    (void)self;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(kCcEmptyString));
}

void cloudcellTraceSlot2(int slot, void* self, uintptr_t caller) {
    // Same global budget as first level, on purpose: one cap for the whole
    // Cloudcell wedge, so no level can starve the other and a hot slot still
    // cannot flood the SD card. The recorded run spent 19 of 64 on first-level
    // slots, so ~45 lines of headroom remain for this level.
    if (g_cloudcellSlotTraces.fetch_sub(1, std::memory_order_relaxed) == 0) {
        return;   // exhausted: unsigned wrap keeps it at 0, so stop writing
    }
    char line[128];
    __builtin_snprintf(line, sizeof(line),
                       "cc2-slot: slot=%d self=%llx caller=%llx", slot,
                       (unsigned long long)(uintptr_t)self,
                       (unsigned long long)caller);
    // Ordinals 400+ so the two levels stay separable in the log as well as by
    // the cc2-slot prefix.
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", line,
                        400 + static_cast<uint64_t>(slot),
                        static_cast<uint64_t>(slot));
}

#define RR3_CC2_SLOT(n)                                                      \
    extern "C" uint64_t cloudcellSlot2_##n(void* self) {                     \
        cloudcellTraceSlot2((n), self,                                       \
                            reinterpret_cast<uintptr_t>(                     \
                                __builtin_return_address(0)));                \
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self));     \
    }
RR3_CC2_SLOT(0)  RR3_CC2_SLOT(1)  RR3_CC2_SLOT(2)  RR3_CC2_SLOT(3)
RR3_CC2_SLOT(4)  RR3_CC2_SLOT(5)  RR3_CC2_SLOT(6)  RR3_CC2_SLOT(7)
RR3_CC2_SLOT(8)  RR3_CC2_SLOT(9)  RR3_CC2_SLOT(10) RR3_CC2_SLOT(11)
RR3_CC2_SLOT(12) RR3_CC2_SLOT(13) RR3_CC2_SLOT(14) RR3_CC2_SLOT(15)
RR3_CC2_SLOT(16) RR3_CC2_SLOT(17) RR3_CC2_SLOT(18) RR3_CC2_SLOT(19)
RR3_CC2_SLOT(20) RR3_CC2_SLOT(21) RR3_CC2_SLOT(22) RR3_CC2_SLOT(23)
RR3_CC2_SLOT(24) RR3_CC2_SLOT(25) RR3_CC2_SLOT(26) RR3_CC2_SLOT(27)
RR3_CC2_SLOT(28) RR3_CC2_SLOT(29) RR3_CC2_SLOT(30) RR3_CC2_SLOT(31)
#undef RR3_CC2_SLOT

#define RR3_CC2_TABLE(n) reinterpret_cast<uint64_t>(&cloudcellSlot2_##n),
constexpr uint64_t kCloudcell2SlotTable[32] = {
    RR3_CC2_TABLE(0)  RR3_CC2_TABLE(1)  RR3_CC2_TABLE(2)  RR3_CC2_TABLE(3)
    RR3_CC2_TABLE(4)  RR3_CC2_TABLE(5)  RR3_CC2_TABLE(6)  RR3_CC2_TABLE(7)
    RR3_CC2_TABLE(8)  RR3_CC2_TABLE(9)  RR3_CC2_TABLE(10) RR3_CC2_TABLE(11)
    RR3_CC2_TABLE(12) RR3_CC2_TABLE(13) RR3_CC2_TABLE(14) RR3_CC2_TABLE(15)
    RR3_CC2_TABLE(16) RR3_CC2_TABLE(17) RR3_CC2_TABLE(18) RR3_CC2_TABLE(19)
    RR3_CC2_TABLE(20) RR3_CC2_TABLE(21) RR3_CC2_TABLE(22) RR3_CC2_TABLE(23)
    RR3_CC2_TABLE(24) RR3_CC2_TABLE(25) RR3_CC2_TABLE(26) RR3_CC2_TABLE(27)
    RR3_CC2_TABLE(28) RR3_CC2_TABLE(29) RR3_CC2_TABLE(30) RR3_CC2_TABLE(31)
};
#undef RR3_CC2_TABLE

// Global renderTarget address for menu loader stub.
static uintptr_t g_renderTarget = 0;

// ---------------------------------------------------------------------------
// EGL autopsy.
//
// "the log stops at view-egl-get-display" is a stop-sign, not a diagnosis.
// The breadcrumb now brackets the call on both sides (probe-begin,
// get-display-ret, init-begin) so the hang is pinned to a single EGL entry
// point instead of a whole function.
//
// A note on a rejected instrument: an in-guest watchdog THREAD was tried here
// to separate "spinning" from "deadlocked" in-process. It cannot work on Eden
// -- a thread created through the guest pthread bridge never gets scheduled
// out of svcSleepThread there, so it silently produces no heartbeats and looks
// exactly like the hang it was built to detect. The spin/freeze discriminator
// lives on the HOST instead: Test-OnEden.ps1 samples eden.exe CPU every 10 s
// and archives the series, where a spin climbs steadily and a deadlock
// collapses to ~0.
//
// eglQueryString(EGL_NO_DISPLAY, ...) is legal per spec and touches no device,
// so it separates "Mesa's own init hangs" from "opening a display hangs".
// ---------------------------------------------------------------------------
static const char* kEglWatchdogPath = "sdmc:/switch/rr3-native-rehost.egl-probe.log";

void appendWatchdogLine(const char* text) {
    std::FILE* f = std::fopen(kEglWatchdogPath, "ab");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "%s\n", text);
    std::fflush(f);
    std::fclose(f);
}

// Menu-loader vtable stub. Called at 0x12af010 as:
//   obj->vtable[21](obj, 2, 0x60)
// Calls FUN_01175720 (XML loader) to load menu layout.
// FUN_01175720: ELF 0x1175720, delta from onViewRenderJNI = -0x13DEF4.
extern "C" uint64_t menuLoaderStub(void* self, int mode, int res) {
    (void)mode;
    (void)res;
    // Reached only if the host's stub owns slot 21. With menugate=1 the guest
    // normally keeps its OWN vtable (see menu-loader-slot21), so this firing or
    // not is itself the measurement that separates the two implementations.
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "menu-loader-stub-enter", 249, 0,
                        "menuLoaderStub entered: obj->vtable[21](obj,2,0x60)");
    // Allocate 592-byte object for XML loader.
    static uint64_t s_menuObj[74] = {};  // 592 bytes
    // FUN_01175720(object, filename, flags)
    using XmlLoaderFn = void* (*)(void*, const char*, int);
    // Delta from onViewRenderJNI (ELF 0x12B3614) to FUN_01175720 (ELF 0x1175720).
    constexpr uint64_t kXmlLoaderDelta = 0x0013DEF4ULL;
    const uintptr_t xmlLoader = g_renderTarget - kXmlLoaderDelta;
    const char* menuFile = "ESportsMainMenu.xml";
    reinterpret_cast<XmlLoaderFn>(xmlLoader)(s_menuObj, menuFile, 0);
    return reinterpret_cast<uint64_t>(self);
}

constexpr const char* kCreatedExport =
    "Java_com_firemint_realracing_MainActivity_onViewCreatedJNI";
constexpr const char* kChangedExport =
    "Java_com_firemint_realracing_MainActivity_onViewChangedJNI";
// Defined in multi_dso_linker.cpp. Walks the global segment table and reports each
// mapping's owner/slave pair -- the writable alias of guest .text.
extern "C" void reportSegmentOwnerSlave();
extern "C" void rr3RootPcSamplerStart(void);
extern "C" void* guestCodeWritableAddress(uintptr_t guestAddress);

constexpr const char* kRenderExport =
    "Java_com_firemint_realracing_MainActivity_onViewRenderJNI";
constexpr const char* kFocusExport =
    "Java_com_firemint_realracing_MainActivity_onWindowFocusChangedJNI";

void recordEglFailure(GuestViewProbe& probe) {
    probe.eglError = static_cast<uint32_t>(eglGetError());
}

// A probe that stops logging and returns a failure code is indistinguishable
// from a hang in the breadcrumb log -- that is how "Eden hangs at
// eglGetDisplay" survived as a belief when Mesa was in fact returning
// EGL_NO_DISPLAY in under a millisecond. Every terminal path logs.
void logEglFailure(const char* stage, EGLDisplay display, GuestViewProbe& probe) {
    recordEglFailure(probe);
    char diag[128];
    __builtin_snprintf(diag, sizeof(diag),
        "egl-fail: stage=%s display=%p eglError=0x%x", stage,
        reinterpret_cast<void*>(display), (unsigned)probe.eglError);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        diag, 249, 3);
}

void releaseEgl(EGLDisplay display, EGLSurface surface, EGLContext context) {
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
        eglTerminate(display);
    }
}

// ---------------------------------------------------------------------------
// Host PC sampler: the program counter of the frozen main thread.
//
// Why this instrument exists: Eden's HW.Memory fault lines carry no PC and no
// thread id (by design), the census already proves the main thread is running
// guest code rather than parked in the pthread bridge, and it never issues a
// futex -- so the single missing fact is where the PC actually is.
//
// HOW the read works, and the one trap in it: Horizon exposes no
// "read a running thread's PC". svcGetThreadContext3() is documented by libnx
// as dumping a thread that has been PAUSED by svcSetThreadActivity(). So every
// sample here is stop -> dump -> restart, not a free-running read. If the pause
// is a silent no-op on this host, the dump returns whatever the kernel last
// saved and the histogram is fiction. That is why the control below is
// mandatory: while the main thread has published kSamplerPhaseInGuest it is
// provably inside onViewRenderJNI, so those samples MUST land inside a guest
// image. A control failure invalidates every negative the sampler produces.
//
// Why a HOST thread: the census reporter is a host ::pthread_create and it
// keeps sweeping on Eden while the main thread burns a core, whereas a thread
// created through the guest pthread bridge never leaves svcSleepThread there.
// This is a raw libnx thread, which is further from the guest bridge than that.
// ---------------------------------------------------------------------------

constexpr const char* kSamplerLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// Phase tags published by the main thread, so every sample can be attributed to
// a known region of the frame loop instead of being a naked address.
enum SamplerPhase : uint32_t {
    kSamplerPhasePreLoop = 0,
    kSamplerPhaseFrameLoopHost = 1,
    kSamplerPhaseInGuest = 2,   // main thread is inside onViewRenderJNI
    kSamplerPhasePostGuest = 3, // onViewRenderJNI has returned
    kSamplerPhaseExited = 4,
};
static std::atomic<uint32_t> g_samplerPhase{kSamplerPhasePreLoop};

struct SamplerImageRange {
    uint64_t base;
    uint64_t bytes;
    char soname[64];
};
static SamplerImageRange g_samplerImages[kGuestStartupMaximumImages];
static uint32_t g_samplerImageCount = 0;
static int g_samplerMainImage = -1;   // index of libRealRacing3.so
static uint64_t g_samplerMainBias = 0; // load bias, so PC -> ELF VA is exact
static bool g_samplerImagesReady = false;

struct SamplerHistogramEntry {
    uint64_t pc;
    uint64_t count;
};

constexpr size_t kSamplerHistogramSlots = 32;
constexpr size_t kSamplerHistogramTop = 8;
constexpr uint64_t kSamplerRawSampleBudget = 16;  // first-N raw dump
constexpr uint64_t kSamplerFlushInterval = 40;    // ~3 s at 75 ms
constexpr int64_t kSamplerIntervalNs = 75000000;  // 75 ms

// 0x3B, not the 0x2C this thread used to be created at. libnx's own header
// (switch/kernel/thread.h:37) says 0x3B "is a special priority on cores 0..2
// that enables preemptive multithreading"; 0x2C is only "the usual priority of
// the main thread" and is NOT time-sliced, so a busy 0x2C thread keeps its core
// and blocks same-or-lower-priority threads even on idle cores. The sampler is
// long-lived and ticks 13 times a second. Grep kDiagnosticThreadPriority.
//
// NOT applied to the victimWorker self-test thread above, which stays at 0x2C
// on purpose: it exists to demonstrate the starvation this priority causes, so
// raising it would destroy the thing it measures.
constexpr int kDiagnosticThreadPriority = 0x3B;

struct SamplerState {
    uint64_t samples;
    uint64_t guestTagged;            // samples taken while in onViewRenderJNI
    uint64_t guestTaggedResident;    // ... of those, inside a guest image
    uint64_t postGuestTagged;
    uint64_t postGuestTaggedResident;
    uint64_t distinctPcs;
    uint64_t notAarch64;
    uint64_t pauseFailures;
    uint64_t contextFailures;
    uint64_t resumeFailures;
    uint32_t pauseResult;
    uint32_t contextResult;
    SamplerHistogramEntry slots[kSamplerHistogramSlots];
};
static SamplerState g_sampler;
static std::atomic<bool> g_samplerRunning{false};
static Thread g_samplerThread;
static bool g_samplerStarted = false;

int samplerResidentImage(uint64_t address) {
    for (uint32_t index = 0; index < g_samplerImageCount; ++index) {
        const SamplerImageRange& image = g_samplerImages[index];
        if (address >= image.base &&
            address < image.base + image.bytes) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

const char* samplerPhaseName(uint32_t phase) {
    switch (phase) {
        case kSamplerPhasePreLoop: return "pre-loop";
        case kSamplerPhaseFrameLoopHost: return "loop-host";
        case kSamplerPhaseInGuest: return "in-guest";
        case kSamplerPhasePostGuest: return "post-guest";
        case kSamplerPhaseExited: return "exited";
        default: return "unknown";
    }
}

void samplerRecord(uint64_t pc) {
    for (size_t slot = 0; slot < kSamplerHistogramSlots; ++slot) {
        if (g_sampler.slots[slot].count == 0) {
            g_sampler.slots[slot].pc = pc;
            g_sampler.slots[slot].count = 1;
            ++g_sampler.distinctPcs;
            return;
        }
        if (g_sampler.slots[slot].pc == pc) {
            ++g_sampler.slots[slot].count;
            return;
        }
    }
    // Histogram full. Fold the overflow into the last slot so the counts still
    // sum to the sample total, and let "32 distinct PCs" be visible in the
    // flush line rather than silently dropping samples.
    ++g_sampler.slots[kSamplerHistogramSlots - 1].count;
}

// Emits the counters plus a top-N PC histogram with each PC attributed to a
// guest image and an exact ELF virtual address. Bounded by construction: one
// line every kSamplerFlushInterval samples, never one line per sample.
void samplerFlush(const char* trigger, bool includeHistogram) {
    char head[320];
    __builtin_snprintf(head, sizeof(head),
        "trigger=%s samples=%llu phase=%s control_in_guest=%llu/%llu"
        " control_post_guest_resident=%llu/%llu distinct=%llu notA64=%llu"
        " pauseFail=%llu ctxFail=%llu resumeFail=%llu pauseRes=0x%x ctxRes=0x%x"
        " images=%u mainBias=0x%llx",
        trigger,
        (unsigned long long)g_sampler.samples,
        samplerPhaseName(g_samplerPhase.load(std::memory_order_relaxed)),
        (unsigned long long)g_sampler.guestTaggedResident,
        (unsigned long long)g_sampler.guestTagged,
        (unsigned long long)g_sampler.postGuestTaggedResident,
        (unsigned long long)g_sampler.postGuestTagged,
        (unsigned long long)g_sampler.distinctPcs,
        (unsigned long long)g_sampler.notAarch64,
        (unsigned long long)g_sampler.pauseFailures,
        (unsigned long long)g_sampler.contextFailures,
        (unsigned long long)g_sampler.resumeFailures,
        (unsigned)g_sampler.pauseResult, (unsigned)g_sampler.contextResult,
        (unsigned)g_samplerImageCount,
        (unsigned long long)g_samplerMainBias);

    char tail[560];
    size_t tailUsed = 0;
    tail[0] = '\0';
    if (includeHistogram) {
        SamplerHistogramEntry sorted[kSamplerHistogramSlots];
        std::memcpy(sorted, g_sampler.slots, sizeof(sorted));
        for (size_t a = 0; a < kSamplerHistogramSlots; ++a) {
            size_t best = a;
            for (size_t b = a + 1; b < kSamplerHistogramSlots; ++b) {
                if (sorted[b].count > sorted[best].count) {
                    best = b;
                }
            }
            const SamplerHistogramEntry swap = sorted[a];
            sorted[a] = sorted[best];
            sorted[best] = swap;
        }
        for (size_t rank = 0; rank < kSamplerHistogramTop; ++rank) {
            if (sorted[rank].count == 0) {
                break;
            }
            const uint64_t pc = sorted[rank].pc;
            const int image = samplerResidentImage(pc);
            char entry[160];
            // The ELF VA is pc - loadBias, not pc - rangeStart: the two differ
            // if executableStart() is page-rounded away from the load bias, and
            // only the load-bias form is the address Ghidra and the known
            // anchors speak in.
            const bool haveElfVa = image >= 0 && g_samplerMainBias != 0 &&
                                   static_cast<int>(image) == g_samplerMainImage;
            __builtin_snprintf(entry, sizeof(entry),
                " #%zu pc=0x%llx img=%d soname=%s elfva=0x%llx n=%llu",
                rank, (unsigned long long)pc, image,
                image >= 0 ? g_samplerImages[image].soname : "host",
                (unsigned long long)(haveElfVa ? pc - g_samplerMainBias : 0ULL),
                (unsigned long long)sorted[rank].count);
            const size_t entryLength = std::strlen(entry);
            if (tailUsed + entryLength + 1 >= sizeof(tail)) {
                break;
            }
            std::memcpy(tail + tailUsed, entry, entryLength + 1);
            tailUsed += entryLength;
        }
    }

    char diag[900];
    __builtin_snprintf(diag, sizeof(diag), "%s%s", head, tail);
    appendGuestBootLine(kSamplerLogPath, "sampler-flush", 320, 0, diag);
}

// ---------------------------------------------------------------------------
// Guest-independent positive control for svcGetThreadContext3.
//
// A first run on Eden returned Result 0 from both svcSetThreadActivity and
// svcGetThreadContext3 and a bit-identical ThreadContext on every one of 1480
// samples spanning 110 s -- including a window where the main thread provably
// executed different code -- with pc pinned to the constant 0x4. So the
// question that decides whether ANY cross-thread context read can work here is
// not "is the guest main thread special" but "does this syscall produce a live
// PC for anyone at all".
//
// This control answers that without the guest in the picture: a host thread we
// create ourselves, parked in svcSleepThread, dumped three ways. If a sane PC
// comes back the mechanism works and the main thread is the anomaly; if pc is
// 0x4 again, the syscall is not implemented and the whole class of approach is
// dead on this emulator.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_victimRuns{true};
static volatile uint64_t g_victimCounter = 0;
static Thread g_victimThread;
static bool g_victimStarted = false;

// noinline so the spin has a real, dumpable call site of its own.
__attribute__((noinline)) uint64_t victimSpinStep(uint64_t value) {
    g_victimCounter += value;
    for (int i = 0; i < 4000; ++i) {
        g_victimCounter += static_cast<uint64_t>(i);
    }
    return g_victimCounter;
}

// A RUNNING thread, not a sleeping one. The first self-test used a thread
// parked in svcSleepThread, which only proves that dumping a *blocked* thread
// works. It leaves open whether pausing actually suspends a thread that is
// mid-instruction-stream -- and that is the only case the frozen main thread
// is in. A monotonic counter sampled either side of each dump proves the
// thread really made progress between dumps.
//
// It yields every iteration, and that is not politeness: a variant that spun
// with no svcSleepThread at all produced ZERO sampler output and ZERO
// self-test lines across a 120 s run -- the spinning guest thread starved the
// main thread and the sampler thread on Eden, so not even the control could
// report. Yielding keeps the counter meaningful without starving anyone. The
// cost is that the "pause a thread that is genuinely mid-instruction" case
// remains untested on this emulator.
void victimWorker(void* opaque) {
    (void)opaque;
    uint64_t value = 1;
    while (g_victimRuns.load(std::memory_order_relaxed)) {
        value = victimSpinStep(value);
        svcSleepThread(20000000);
    }
}

void runSamplerSelfTest() {
    if (g_victimStarted) {
        return;
    }
    g_victimStarted = true;
    if (R_FAILED(threadCreate(&g_victimThread, victimWorker, nullptr, nullptr,
                              0x4000, 0x2C, -2))) {
        appendGuestBootLine(kSamplerLogPath, "sampler-selftest-thread-fail",
                            328, 0, "threadCreate failed for the control thread");
        return;
    }
    threadStart(&g_victimThread);
    // Let it reach svcSleepThread so the dump is of a blocked thread.
    svcSleepThread(200000000);
    const Handle victim = g_victimThread.handle;

    // A: dump with NO pause. Real Horizon must refuse a running thread.
    {
        ThreadContext context;
        std::memset(&context, 0, sizeof(context));
        const Result result = svcGetThreadContext3(&context, victim);
        char diag[224];
        __builtin_snprintf(diag, sizeof(diag),
            "probe=no-pause res=0x%x pc=0x%llx lr=0x%llx sp=0x%llx x0=0x%llx"
            " psr=0x%x a64=%d verdict=%s",
            (unsigned)result, (unsigned long long)context.pc.x,
            (unsigned long long)context.lr, (unsigned long long)context.sp,
            (unsigned long long)context.cpu_gprs[0].x, (unsigned)context.psr,
            threadContextIsAArch64(&context) ? 1 : 0,
            R_FAILED(result) ? "kernel-correctly-refused"
                             : "ACCEPTED-UNPAUSED-THREAD");
        appendGuestBootLine(kSamplerLogPath, "sampler-selftest-a", 328, 0, diag);
    }
    // B: pause -> dump -> resume, four times, 100 ms apart, against a thread
    // that is demonstrably running (see the counter in each line). A mechanism
    // that suspends a running thread must return a plausible NRO PC, and that
    // PC must move between rounds.
    for (int round = 0; round < 4; ++round) {
        const uint64_t before = g_victimCounter;
        const Result pauseResult =
            svcSetThreadActivity(victim, ThreadActivity_Paused);
        ThreadContext context;
        std::memset(&context, 0, sizeof(context));
        const Result contextResult = svcGetThreadContext3(&context, victim);
        const Result resumeResult =
            svcSetThreadActivity(victim, ThreadActivity_Runnable);
        const uint64_t after = g_victimCounter;
        char diag[288];
        __builtin_snprintf(diag, sizeof(diag),
            "round=%d pauseRes=0x%x ctxRes=0x%x resumeRes=0x%x pc=0x%llx"
            " lr=0x%llx sp=0x%llx x0=0x%llx psr=0x%x"
            " counterBefore=%llu counterAfter=%llu advanced=%llu",
            round, (unsigned)pauseResult, (unsigned)contextResult,
            (unsigned)resumeResult, (unsigned long long)context.pc.x,
            (unsigned long long)context.lr, (unsigned long long)context.sp,
            (unsigned long long)context.cpu_gprs[0].x, (unsigned)context.psr,
            (unsigned long long)before, (unsigned long long)after,
            (unsigned long long)(after - before));
        appendGuestBootLine(kSamplerLogPath, "sampler-selftest-b", 328,
                            static_cast<unsigned long long>(round), diag);
        svcSleepThread(100000000);
    }
    g_victimRuns.store(false, std::memory_order_relaxed);
    threadWaitForExit(&g_victimThread);
    threadClose(&g_victimThread);
}

// The 75 ms tick's wait, swapped from svcSleepThread to the pair the detached
// reaper already blocks on (pthread_bridge.cpp:3989). Same primitive as
// victimWorker's control measured against it, so there is nothing new to
// trust. In 20261004-030501-pump-before this thread logged "sampler-start" and
// then produced zero sampler-record and zero sampler-flush lines, while
// sampler-selftest-b's four rounds on the main thread completed -- the tick
// never woke.
//
// libnx Mutex + CondVar, NOT std::mutex. Host-only: created by threadCreate at
// line 833 and never entered from a guest call path, so the "no std::mutex on a
// guest-visible path" rule is not engaged.
Mutex g_samplerWaitLock{};
CondVar g_samplerWait{};

// One-shot, second iteration only. The sampler's own sampler-flush output is
// every 40 ticks (~3 s), which is far too late to tell a working wait from a
// dead one on a 150 s run: this line is the fast, bounded answer.
std::atomic<bool> g_samplerWakeReported{false};

void samplerWorker(void* opaque) {
    const Handle target = static_cast<Handle>(reinterpret_cast<uintptr_t>(opaque));
    uint64_t sinceFlush = 0;
    // cntpct_el0 ticks, scaled to ns below; see the same pattern in
    // jni_bridge.cpp's alertPumpWorker.
    const uint64_t firstTick = armGetSystemTick();
    unsigned long long ticks = 0;
    while (g_samplerRunning.load(std::memory_order_relaxed)) {
        ++ticks;
        // A/B, 2026-10-04. DEFAULT IS SLEEP, NOT CONDVAR-WAIT.
        //
        // NOT a fix for the missing records, and the reason matters. A first
        // version of this comment claimed condvarWaitTimeout never expires on
        // Eden. That was wrong: 20261004-075251-alertpumpfix already logged
        // "sampler-wake iters=2 interval_ns=150907708" with the condvar in
        // place. What is true is weaker and messier -- the wait is
        // UNRELIABLE. The alert pump, on byte-identical code, went wait-result
        // 0 -> 1 between two consecutive runs. A tick that fires sometimes is
        // worse for diagnosis than one that never fires, because the run that
        // happens to work reads as a fix.
        //
        // The switch stays, and not as a fix: g_samplerWait has no signaler
        // anywhere, so the Mutex + CondVar guard no invariant and the tick is a
        // sleep wearing a lock. svcSleepThread says exactly that. jni_bridge.cpp's
        // alertPumpWorker now uses the same primitive, so one grep compares them.
        //
        // THE ACTUAL BLOCKER is measured by sampler-selftest-b, and it is not a
        // wait at all. That self-test pauses a victim thread, reads its context
        // and resumes it:
        //
        //   pauseRes=0x0 ctxRes=0x0 resumeRes=0x0 pc=0x80cf8a18 lr=0x807c8d44
        //   counterBefore=8181954512 counterAfter=8181954512 advanced=0
        //
        // The pause works -- advanced=0 -- and every syscall returns success.
        // But pc=0x80cf8a18 is identical across every round and is a host-NRO
        // address, i.e. the READER's own program counter. svcGetThreadContext3
        // does not deliver another thread's ARM64 context under Eden. That is
        // why every census-pc line reads pc_img=host-nro / pc_elfva=n/a, and it
        // is not fixable from this side. Real hardware is the only path left.
#ifndef RR3_SAMPLER_CONDVAR
#define RR3_SAMPLER_CONDVAR 0
#endif
        if (RR3_SAMPLER_CONDVAR) {
            mutexLock(&g_samplerWaitLock);
            // 0xEA01 on timeout is expected and ignored: pure tick, no wakeup to
            // lose.
            (void)condvarWaitTimeout(&g_samplerWait, &g_samplerWaitLock,
                                     static_cast<s64>(kSamplerIntervalNs));
            mutexUnlock(&g_samplerWaitLock);
        } else {
            svcSleepThread(static_cast<s64>(kSamplerIntervalNs));
        }
        if (ticks == 2 && !g_samplerWakeReported.exchange(true)) {
            const uint64_t freq = armGetSystemTickFreq();
            const uint64_t intervalNs =
                (armGetSystemTick() - firstTick) * 1000000000ULL /
                (freq != 0 ? freq : 1);
            char diag[160];
            __builtin_snprintf(diag, sizeof(diag),
                "iters=%llu interval_ns=%llu target_ns=%lld"
                " prim=condvarWaitTimeout",
                ticks, (unsigned long long)intervalNs,
                (long long)kSamplerIntervalNs);
            appendGuestBootLine(kSamplerLogPath, "sampler-wake", 329,
                                static_cast<unsigned long long>(ticks), diag);
        }
        if (!g_samplerRunning.load(std::memory_order_relaxed)) {
            break;
        }
        const Result pauseResult =
            svcSetThreadActivity(target, ThreadActivity_Paused);
        if (R_FAILED(pauseResult)) {
            if (g_sampler.pauseFailures == 0) {
                g_sampler.pauseResult = static_cast<uint32_t>(pauseResult);
                char diag[224];
                __builtin_snprintf(diag, sizeof(diag),
                    "svcSetThreadActivity(Paused) failed res=0x%x handle=0x%x"
                    " -- PLATFORM FACT: another thread's registers are not"
                    " readable through this path here, sampler cannot work",
                    (unsigned)pauseResult, (unsigned)target);
                appendGuestBootLine(kSamplerLogPath, "sampler-cap-fail-pause",
                                    321, 0, diag);
            }
            ++g_sampler.pauseFailures;
            continue;
        }
        ThreadContext context;
        std::memset(&context, 0, sizeof(context));
        const Result contextResult = svcGetThreadContext3(&context, target);
        const Result resumeResult =
            svcSetThreadActivity(target, ThreadActivity_Runnable);
        if (R_FAILED(resumeResult)) {
            // The target is paused and we could not restart it. Say so loudly;
            // it is the one failure that turns "stuck" into "wedged".
            if (g_sampler.resumeFailures == 0) {
                char diag[192];
                __builtin_snprintf(diag, sizeof(diag),
                    "svcSetThreadActivity(Runnable) failed res=0x%x"
                    " -- target left PAUSED, sampler stopping",
                    (unsigned)resumeResult);
                appendGuestBootLine(kSamplerLogPath, "sampler-resume-fail",
                                    322, 0, diag);
            }
            ++g_sampler.resumeFailures;
            g_samplerRunning.store(false, std::memory_order_relaxed);
            break;
        }
        if (R_FAILED(contextResult)) {
            if (g_sampler.contextFailures == 0) {
                g_sampler.contextResult = static_cast<uint32_t>(contextResult);
                char diag[160];
                __builtin_snprintf(diag, sizeof(diag),
                    "svcGetThreadContext3 failed res=0x%x handle=0x%x",
                    (unsigned)contextResult, (unsigned)target);
                appendGuestBootLine(kSamplerLogPath, "sampler-cap-fail-ctx",
                                    323, 0, diag);
            }
            ++g_sampler.contextFailures;
            continue;
        }

        ++g_sampler.samples;
        const uint32_t phase = g_samplerPhase.load(std::memory_order_relaxed);
        const uint64_t pc = context.pc.x;
        const int image = samplerResidentImage(pc);
        if (phase == kSamplerPhaseInGuest) {
            ++g_sampler.guestTagged;
            if (image >= 0) {
                ++g_sampler.guestTaggedResident;
            }
        }
        else if (phase == kSamplerPhasePostGuest) {
            ++g_sampler.postGuestTagged;
            if (image >= 0) {
                ++g_sampler.postGuestTaggedResident;
            }
        }
        if (!threadContextIsAArch64(&context)) {
            ++g_sampler.notAarch64;
        }

        // First N raw samples, unconditionally. A histogram cannot distinguish
        // "the target is stuck here" from "the sampler is stuck", so the raw
        // registers are on disk before any aggregate is trusted.
        if (g_sampler.samples <= kSamplerRawSampleBudget) {
            char diag[384];
            __builtin_snprintf(diag, sizeof(diag),
                "i=%llu phase=%s pc=0x%llx img=%d elfva=0x%llx lr=0x%llx"
                " sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx"
                " psr=0x%x a64=%d",
                (unsigned long long)g_sampler.samples, samplerPhaseName(phase),
                (unsigned long long)pc, image,
                (unsigned long long)(image >= 0 && g_samplerMainBias != 0 &&
                                     static_cast<int>(image) == g_samplerMainImage
                                         ? pc - g_samplerMainBias
                                         : 0ULL),
                (unsigned long long)context.lr, (unsigned long long)context.sp,
                (unsigned long long)context.cpu_gprs[0].x,
                (unsigned long long)context.cpu_gprs[1].x,
                (unsigned long long)context.cpu_gprs[2].x,
                (unsigned long long)context.cpu_gprs[3].x,
                (unsigned)context.psr,
                threadContextIsAArch64(&context) ? 1 : 0);
            appendGuestBootLine(kSamplerLogPath, "sample-raw", 324, 0, diag);
        }

        samplerRecord(pc);
        if (++sinceFlush >= kSamplerFlushInterval) {
            sinceFlush = 0;
            samplerFlush("periodic", true);
        }
    }
    samplerFlush("stop", true);
}

void startGuestPcSampler() {
    if (g_samplerStarted) {
        return;
    }
    g_samplerStarted = true;

    // "Inside guest code" needs a definition, and the startup plan is the only
    // in-process source of the images' runtime [base, base+bytes). The rebase:
    // lines in the same log give newBias per image; the sampler-start line
    // below is what proves the two agree.
    const GuestStartupPlan plan = captureGuestStartupPlan();
    g_samplerImageCount = 0;
    g_samplerMainImage = -1;
    g_samplerMainBias = resolveMainImageLoadBias();
    if (plan.available) {
        const uint32_t count =
            plan.imageCount < kGuestStartupMaximumImages
                ? static_cast<uint32_t>(plan.imageCount)
                : static_cast<uint32_t>(kGuestStartupMaximumImages);
        for (uint32_t order = 0; order < count; ++order) {
            const GuestStartupImage& image = plan.images[order];
            if (image.base == 0 || image.imageBytes == 0 ||
                g_samplerImageCount >= kGuestStartupMaximumImages) {
                continue;
            }
            SamplerImageRange& range = g_samplerImages[g_samplerImageCount];
            range.base = image.base;
            range.bytes = image.imageBytes;
            std::snprintf(range.soname, sizeof(range.soname), "%s", image.soname);
            if (std::strstr(range.soname, "RealRacing3") != nullptr) {
                g_samplerMainImage = static_cast<int>(g_samplerImageCount);
            }
            ++g_samplerImageCount;
        }
        g_samplerImagesReady = g_samplerImageCount != 0;
    }

    char diag[192];
    __builtin_snprintf(diag, sizeof(diag),
        "planAvailable=%d images=%u ready=%d mainImage=%d mainBias=0x%llx"
        " mainHandle=0x%x intervalNs=%lld",
        plan.available ? 1 : 0, (unsigned)g_samplerImageCount,
        g_samplerImagesReady ? 1 : 0, g_samplerMainImage,
        (unsigned long long)g_samplerMainBias,
        (unsigned)threadGetCurHandle(), (long long)kSamplerIntervalNs);
    appendGuestBootLine(kSamplerLogPath, "sampler-start", 325, 0, diag);
    for (uint32_t index = 0; index < g_samplerImageCount; ++index) {
        const SamplerImageRange& range = g_samplerImages[index];
        char line[224];
        __builtin_snprintf(line, sizeof(line),
            "idx=%u soname=%s base=0x%llx bytes=0x%llx end=0x%llx",
            (unsigned)index, range.soname, (unsigned long long)range.base,
            (unsigned long long)range.bytes,
            (unsigned long long)(range.base + range.bytes));
        appendGuestBootLine(kSamplerLogPath, "sampler-image", 325,
                            static_cast<unsigned long long>(index), line);
    }

    g_samplerRunning.store(true, std::memory_order_relaxed);
    // Note: svcSetThreadActivity/svcGetThreadContext3 are called directly on
    // the main thread's Handle rather than through libnx's threadPause/
    // threadResume, which take a Thread* that only exists for threads this
    // file created. The Handle is the kernel object the syscalls need.
    const Result createResult = threadCreate(
        &g_samplerThread, samplerWorker,
        reinterpret_cast<void*>(static_cast<uintptr_t>(threadGetCurHandle())),
        nullptr, 0x8000, kDiagnosticThreadPriority, -2 /* default core */);
    if (R_FAILED(createResult)) {
        g_samplerRunning.store(false, std::memory_order_relaxed);
        char line[96];
        __builtin_snprintf(line, sizeof(line), "threadCreate failed res=0x%x",
                           (unsigned)createResult);
        appendGuestBootLine(kSamplerLogPath, "sampler-thread-create-fail",
                            326, 0, line);
        return;
    }
    threadStart(&g_samplerThread);
}

void stopGuestPcSampler() {
    if (!g_samplerStarted) {
        return;
    }
    g_samplerRunning.store(false, std::memory_order_relaxed);
    threadWaitForExit(&g_samplerThread);
    threadClose(&g_samplerThread);
    g_samplerStarted = false;

    // The verdict, stated as a verdict. Thresholds are deliberately modest:
    // >=4 in-guest samples, >=2 distinct PCs (a stale/frozen dump has 1), and
    // >=50% of in-guest samples resident in a guest image. Anything less and
    // the histogram above is not evidence.
    const uint64_t guestPct = g_sampler.guestTagged != 0
        ? g_sampler.guestTaggedResident * 100ULL / g_sampler.guestTagged
        : 0ULL;
    const bool controlPass =
        g_samplerImagesReady && g_sampler.samples > 0 &&
        g_sampler.guestTagged >= 4 && g_sampler.distinctPcs >= 2 &&
        guestPct >= 50 && g_sampler.pauseFailures == 0 &&
        g_sampler.contextFailures == 0;
    char diag[288];
    __builtin_snprintf(diag, sizeof(diag),
        "control=%s imagesReady=%d samples=%llu inGuestResident=%llu/%llu"
        " (%llu%%) postGuestResident=%llu/%llu distinctPcs=%llu"
        " pauseFail=%llu ctxFail=%llu resumeFail=%llu notA64=%llu",
        controlPass ? "PASS" : "FAIL", g_samplerImagesReady ? 1 : 0,
        (unsigned long long)g_sampler.samples,
        (unsigned long long)g_sampler.guestTaggedResident,
        (unsigned long long)g_sampler.guestTagged, (unsigned long long)guestPct,
        (unsigned long long)g_sampler.postGuestTaggedResident,
        (unsigned long long)g_sampler.postGuestTagged,
        (unsigned long long)g_sampler.distinctPcs,
        (unsigned long long)g_sampler.pauseFailures,
        (unsigned long long)g_sampler.contextFailures,
        (unsigned long long)g_sampler.resumeFailures,
        (unsigned long long)g_sampler.notAarch64);
    appendGuestBootLine(kSamplerLogPath, "sampler-control", 327, 0, diag);
}

// The frame loop's exit was, until now, invisible in the log. The swap-failure
// path called recordEglFailure(), which only stores probe.eglError and writes
// nothing, and then `break`ed -- so "the loop ended" and "the loop was never
// entered again" looked identical. That is the same silent-failure class that
// cost a day in multi_dso_linker.cpp, so every exit path now names itself
// before it leaves the loop.
const char* g_frameLoopExitReason = "flag-cleared";

void logFrameLoopExit(const char* reason, const char* site, uint64_t frames,
                      const GuestViewProbe& probe, uintptr_t renderTarget) {
    const GuestViewGlStats stats = guestViewGlStats();
    char diag[288];
    __builtin_snprintf(diag, sizeof(diag),
        "reason=%s site=%s frames=%llu swapped=%d eglError=0x%x"
        " renderTarget=0x%llx liveThreads=%u clears=%u drawArrays=%u"
        " drawElements=%u texImg=%u",
        reason != nullptr ? reason : "unknown",
        site != nullptr ? site : "unknown",
        (unsigned long long)frames, probe.swapped ? 1 : 0, (unsigned)probe.eglError,
        (unsigned long long)renderTarget, (unsigned)guestPthreadLiveCount(),
        (unsigned)stats.clears, (unsigned)stats.drawArrays,
        (unsigned)stats.drawElements, (unsigned)stats.texImage2D);
    appendGuestBootLine(kSamplerLogPath, "frame-loop-exit", 330, 0, diag);
}

// Ghidra DAT_02097030 is the ndActivity singleton pointer, stored at a fixed
// offset from the onViewRenderJNI export (both live in libRealRacing3.so).
// Ghidra addresses for this image are 0x00100000 above the ELF VAs used at
// runtime, so the delta below is (0x02097030 - 0x013B3614).
constexpr uint64_t kActivitySingletonDeltaFromRenderExport = 0x00CE3A1CULL;

struct GuestActivitySnapshot {
    bool valid{};
    uintptr_t activity{};
    uintptr_t engine{};
    uintptr_t container{};
    uintptr_t renderer{};
    uint8_t glObjectsCreated;   // +0x4a4
    uint8_t landscapeApplied;   // +0x4a5
    uint8_t isLandscape;        // +0x4a6
    uint8_t paused;             // +0x4d0
    uint8_t flag4d1;
    uint8_t engineReady;        // engine+0xa
    uint8_t containerActive;    // container+0xa 閿?gates FUN_013b43a0/013b4448
    uint8_t containerReady;     // container+0x20 閿?gates FUN_013b4940
    uint8_t splashExit;         // renderer+0x1f5a9 閿?splash閳姦enu trigger
    uint8_t splashFlagAb;       // renderer+0x1f5ab
    uint8_t splashFlagAc;       // renderer+0x1f5ac
    uint8_t renderer120;        // renderer+0x120
    uint8_t renderer38c;        // renderer+0x38c 閿?early-return when !=0
    uint64_t containerQ0;       // *container 閿?vtable or first field
    uint64_t containerQ1;
    uint64_t containerQ2;
    int32_t width;              // +0x4a8
    int32_t height;             // +0x4ac
    int32_t width2;             // +0x4b0  -- render-head.py: cmp w1,w8 on 0x4a8/0x4b0
    int32_t height2;            // +0x4b4  -- render-head.py: cmp w8,w9 on 0x4ac/0x4b4
    int32_t frameCounter;       // +0x4d4  -- ++'d only when BOTH pairs differ AND +0x4d1
    int32_t frameState;         // +0x4d8
    int32_t splashFrames;       // +0x4dc
};

// Ghidra DAT_020b2688 is the engine pointer used by the render loop
// (vtable+0x38 time, +0xa landscape-ready flag). Same image as the activity
// singleton; delta from onViewRenderJNI = 0x020b2688 - 0x013b3614.
constexpr uint64_t kEngineSingletonDeltaFromRenderExport = 0x00CFF074ULL;

// onViewRenderJNI (ELVVA 0x12b3614) loads its argument like this:
//
//   0x12b3614: adrp x8, #0x1f6e000
//   0x12b3618: ldr  x8, [x8, #0x540]     <- slot at ELVVA 0x1f6e540
//   0x12b361c: ldr  x0, [x8]             <- x0 = that object's first word
//   0x12b3620: cbz  x0, #0x12b3628
//   0x12b3624: b    #0x12aea7c           <- tail call, x0 unchanged
//
// So the object the render function receives is *(*(0x1f6e540)), which is NOT
// the slot kEngineSingletonDeltaFromRenderExport names: that one resolves to
// 0x1fb2688, 0x3a148 higher. Both are in the same image segment, so neither
// address is obviously the wrong one, and "they are the the same object" has
// been assumed rather than checked -- the fields printed off the other slot do
// look sane (width 1280, height 720, aspect 1.0f), which is exactly what lets
// an unchecked assumption survive. These deltas are computed, never hand-added:
// 0x1f6e540 - 0x12b3614 = 0x00cbaf2c. Two hand-computed versions of that one
// subtraction were both wrong before it was finally computed.
constexpr uint64_t kRenderArgSingletonDeltaFromRenderExport = 0x00CBAF2CULL;

GuestActivitySnapshot readGuestActivitySnapshot(uintptr_t renderExport) {
    GuestActivitySnapshot snap{};
    if (renderExport == 0) {
        return snap;
    }
    const uintptr_t singletonSlot =
        renderExport + kActivitySingletonDeltaFromRenderExport;
    const uintptr_t activity =
        *reinterpret_cast<const uintptr_t*>(singletonSlot);
    if (activity == 0) {
        return snap;
    }
    snap.valid = true;
    snap.activity = activity;
    const volatile uint8_t* bytes =
        reinterpret_cast<const volatile uint8_t*>(activity);
    snap.glObjectsCreated = bytes[0x4a4];
    snap.landscapeApplied = bytes[0x4a5];
    snap.isLandscape = bytes[0x4a6];
    snap.paused = bytes[0x4d0];
    snap.flag4d1 = bytes[0x4d1];
    const volatile int32_t* words =
        reinterpret_cast<const volatile int32_t*>(activity);
    snap.width = words[0x4a8 / 4];
    snap.height = words[0x4ac / 4];
    snap.width2 = words[0x4b0 / 4];
    snap.height2 = words[0x4b4 / 4];
    snap.frameCounter = words[0x4d4 / 4];
    snap.frameState = words[0x4d8 / 4];
    snap.splashFrames = words[0x4dc / 4];
    const uintptr_t engineSlot =
        renderExport + kEngineSingletonDeltaFromRenderExport;
    const uintptr_t engine = *reinterpret_cast<const uintptr_t*>(engineSlot);
    if (engine != 0) {
        snap.engine = engine;
        snap.engineReady = *reinterpret_cast<const volatile uint8_t*>(engine + 0xa);
    }
    // activity+0x58 閿?container (small object with +0xa/+0x20 flags).
    // First qword of container is the large renderer (FUN_013aea7c does
    // lVar12 = **(long**)(activity+0x58)).
    const uintptr_t container =
        *reinterpret_cast<const uintptr_t*>(activity + 0x58);
    if (container != 0) {
        snap.container = container;
        snap.containerActive =
            *reinterpret_cast<const volatile uint8_t*>(container + 0xa);
        snap.containerReady =
            *reinterpret_cast<const volatile uint8_t*>(container + 0x20);
        const volatile uint64_t* qwords =
            reinterpret_cast<const volatile uint64_t*>(container);
        snap.containerQ0 = qwords[0];
        snap.containerQ1 = qwords[1];
        snap.containerQ2 = qwords[2];
        snap.renderer = snap.containerQ0;
        if (snap.renderer != 0) {
            snap.splashExit =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5a9);
            snap.splashFlagAb =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5ab);
            snap.splashFlagAc =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5ac);
            snap.renderer120 =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x120);
            snap.renderer38c =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x38c);
        }
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Render-call straddle: the state onViewRenderJNI is ABOUT TO CONSUME.
//
// WHY NOT A REGISTER DUMP. Reading the registers of the thread that sits in the
// guest is what the project already tries and it is structurally dead on this
// platform: svcGetThreadContext3 answers with the host NRO's pc for a guest
// thread (5/5 samples pc_img=host-nro), and the census frame walk of a wedged
// thread returns an all-host chain (fpwalk-raw role=stall, [sp+8]=0x810c9a94
// img=host, census-reg-body images=0). So this records the INPUT side of the
// call instead: the arguments as the host passes them, and the guest-owned
// words the render entry dereferences before it issues its first draw. Both are
// readable from this side of the call, with no thread context and no syscall.
//
// WHAT THE GUEST READS FIRST, from the disassembly of this same binary
// (ELF 0x12B3614 == Ghidra 0x13B3614, the kRenderExport):
//
//   0x12b3614  adrp x8,#0x1f6e000 / ldr x8,[x8,#0x540] / ldr x0,[x8]
//   0x12b3620  cbz x0 -> 0x12b3628 (ret)      singleton null -> nothing runs
//   0x12b3624  b 0x12aea7c                    tail call
//
// Two consequences that are easy to get wrong and that this line now records:
// the host's x0 (the JNIEnv) and x1 (the activity jobject) are NEVER read by
// the guest at this entry -- it takes the activity from a global and passes it
// on as x0 -- and the host's w2=2 / w3=0 survive the tail call untouched into
// w23/w22 (0x12aeabc / 0x12aeab4), where they are compared against
// activity+0x4b8 / +0x4bc (0x12aecb4 / 0x12aecc0). So 2 and 0 are a MODE pair
// the guest caches, not a width and a height.
//
// FUN_013aea7c (ELF 0x12aea7c), for frameState == 0 -- the value every archived
// run reports as fstate=0 -- dispatches to 0x12aec2c and walks this chain:
//
//   0x12aec2c  ldrb w8,[x19,#0x4d0]      paused gate (+0x4d2 is the second)
//   0x12aec4c  ldr x8,[x19,#0x58]        container
//   0x12aec50  ldr x0,[x8]               renderer = *container   <-- deref 1
//   0x12aec5c  ldrsw x9,[x19,#0x4a0]     SIGNED 32-bit load used AS AN INDEX
//   0x12aec68  add x9, x19, x9, lsl #2
//   0x12aec6c  str w8,[x9,#0xe0]         store at activity + 4*idx + 0xe0
//   0x12aee24  bl 0x12b4824(container, w=+0x4a8, h=+0x4ac, s0=[+0x4c0])
//                                       the one draw gfx-stats counted
//   0x12aed08  bl 0x12b46f0(x19+0x58)    container+0xa gate, then tail-calls
//                                       0x13bcb5c(renderer)   <-- deref 2
//   0x12aeed8  bl 0x12b4940(container)   container+0x20 gate    <-- deref 3
//
// The same ldrsw/add/str triple runs five times on this path (+0xe0, +0x180,
// +0x2c0, +0x270, +0x310 at 0x12aec6c / 0x12aee10 / 0x12aee74 / 0x12aee98 /
// 0x12aeea8), so activity+0x4a0 is multiplied by a value the snapshot has never
// read: one negative or large word there is five wild stores inside the guest,
// and it is the cheapest single explanation for a wedge that arrives with an
// unmapped-access storm. Whether *container is non-null at all is the second
// thing the snapshot cannot see. Both are read here; everything else on the
// line is a field the run already reports, carried at the same instant so the
// pre line and the post line diff without a second lookup.
//
// Bounded and one-shot by its caller: a relaxed latch fires the pair on the
// first call and never again, so the budget is exactly two lines for the whole
// run. boot_ms is armGetSystemTick()*1000/freq, the same convention as
// censusMillis in pthread_bridge.cpp and the reaper's boot_ms, so the two lines
// are comparable across archives.
//
// The latch, same idiom as g_samplerWakeReported above: one pair for the whole
// run, on the first call, so the budget is two lines and not two per
// iteration. Written only by the render thread and only before the call, so it
// needs no more than a relaxed exchange.
static std::atomic<bool> g_renderStraddleEmitted{false};

int formatRenderCallStraddle(char* out, size_t capacity, const char* side,
                             uintptr_t renderExport, const void* jniEnvironment,
                             const void* activityObject, int arg2,
                             uint32_t arg3) {
    const GuestActivitySnapshot snap = readGuestActivitySnapshot(renderExport);
    const uint64_t freq = armGetSystemTickFreq();
    const uint64_t bootMs =
        armGetSystemTick() * 1000ULL / (freq != 0 ? freq : 1);
    if (!snap.valid) {
        // A null singleton is itself the answer: the guest's own entry would
        // have returned immediately, so say that rather than print zeroes that
        // read as data.
        __builtin_snprintf(out, capacity,
                           "side=%s boot_ms=%llu th=0x%08x render=0x%llx"
                           " env=0x%llx jact=0x%llx a2=%d a3=%u"
                           " valid=0 reason=activity-singleton-null",
                           side, (unsigned long long)bootMs,
                           static_cast<unsigned>(threadGetCurHandle()),
                           (unsigned long long)renderExport,
                           (unsigned long long)(uintptr_t)jniEnvironment,
                           (unsigned long long)(uintptr_t)activityObject,
                           arg2, static_cast<unsigned>(arg3));
        return 0;
    }
    const volatile int32_t* words =
        reinterpret_cast<const volatile int32_t*>(snap.activity);
    const volatile uint8_t* bytes =
        reinterpret_cast<const volatile uint8_t*>(snap.activity);
    const int32_t index4a0 = words[0x4a0 / 4];   // the signed array index
    const int32_t cached4b0 = words[0x4b0 / 4];  // width/height compare chain
    const int32_t cached4b4 = words[0x4b4 / 4];
    const int32_t mode4b8 = words[0x4b8 / 4];    // where the host's 2 is cached
    const int32_t mode4bc = words[0x4bc / 4];    // where the host's 0 is cached
    const uint32_t aspect4c0 =
        static_cast<uint32_t>(words[0x4c0 / 4]); // the s0 float of the draw
    const uint8_t flag4d2 = bytes[0x4d2];         // second paused-branch gate
    const int32_t retry4d4 = words[0x4d4 / 4];   // counter compared against 3
    const int32_t base0088 = words[0x88 / 4];    // time subtrahend at 0x12aec58

    // The object onViewRenderJNI actually hands to FUN_012aea7c, resolved
    // independently of snap.activity so the two can be compared rather than
    // assumed equal. This is the guest's x19, and 0x4fc is the word the render
    // loop's exit test waits to become 4 (cmp #4 / b.ne at 0x12af188), so these
    // are the numbers the wedge is actually waiting on. Read only, and only
    // after the holder and the pointer itself are both non-null: this runs on
    // the guest-visible render path, where a bad dereference is exactly the
    // failure being hunted. SENTINEL_ marks "not read", never 0, so a real
    // stored -1 is distinguishable from a slot we never got to.
    constexpr int32_t kTierUnread = static_cast<int32_t>(0x5eed0000);
    uintptr_t renderArg = 0;
    if (renderExport != 0) {
        const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(
            renderExport + kRenderArgSingletonDeltaFromRenderExport);
        if (holder != 0) {
            renderArg = *reinterpret_cast<const uintptr_t*>(holder);
        }
    }
    int32_t tier4fc = kTierUnread;
    int32_t parsed500 = kTierUnread;
    // Bytes on the same object, each one an actual branch in the code rather
    // than a field we happened to find useful. 0x4a4 selects between the two
    // arms of 0x12ad504 (one sets w1 = -1 at 0x12ad604), 0x4a6 short-circuits
    // the whole frame at 0x12aeaa8, 0x50c is the flag 0x12afaa0 clears and
    // re-tests, 0x50d gates the usleep at 0x12af0a0. Printed as 0/1/other so
    // "never read" and "read zero" cannot be confused again.
    uint32_t b4a4 = kTierUnread, b4a6 = kTierUnread;
    uint32_t b50c = kTierUnread, b50d = kTierUnread;
    uint32_t w4c8 = kTierUnread;

    // Reachability first, then the one address that matters. Walking the CFG
    // from the fall-through of the frameState==0 arm (0x12aec2c) reaches 287 of
    // 567 instructions and makes exactly ONE PLT call: __stack_chk_fail, which
    // three archived runs show is never reached (bionicStackChkFail already
    // logs). So this path cannot be blocking in any host-side import. Of its 27
    // internal calls, 0x10b2f5c appears TEN times -- it is the vtable thunk
    // `ldr x1,[x8,#0x38]; br x1`, and its receiver is loaded at 0x12aec4c from
    // [rend+0x58]. A thunk is 24 bytes and has no loop of its own, so the
    // stall, if it is in guest code at all, is in whatever that slot points at.
    //
    // Which is why we read the slot instead of guessing. slot38 is the actual
    // target of ten separate call sites on the live path; its value, minus the
    // image bias, is the ELF address to disassemble. Read-only, two hops of
    // null checks, no allocation, no lock. If it reads back kTierUnread the
    // object graph is not there and the assumption is wrong -- which is
    // exactly what the sentinel is for.
    uint64_t slot38 = kTierUnread;
    if (renderArg != 0) {
        const uintptr_t engine =
            *reinterpret_cast<const uintptr_t*>(renderArg + 0x58);
        if (engine != 0) {
            const uintptr_t obj =
                *reinterpret_cast<const uintptr_t*>(engine);
            if (obj != 0) {
                const uintptr_t vtable =
                    *reinterpret_cast<const uintptr_t*>(obj);
                if (vtable != 0) {
                    slot38 = *reinterpret_cast<const uintptr_t*>(vtable + 0x38);
                }
            }
        }
    }
    if (renderArg != 0) {
        const volatile int32_t* tier =
            reinterpret_cast<const volatile int32_t*>(renderArg);
        const volatile uint8_t* rb =
            reinterpret_cast<const volatile uint8_t*>(renderArg);
        tier4fc = tier[0x4fc / 4];
        parsed500 = tier[0x500 / 4];
        b4a4 = rb[0x4a4];
        b4a6 = rb[0x4a6];
        b50c = rb[0x50c];
        b50d = rb[0x50d];
        w4c8 = static_cast<uint32_t>(tier[0x4c8 / 4]);
    }
    __builtin_snprintf(
        out, capacity,
        "side=%s boot_ms=%llu th=0x%08x render=0x%llx env=0x%llx jact=0x%llx"
        " a2=%d a3=%u valid=1 act=0x%llx c58=0x%llx rend=0x%llx"
        " idx4a0=%d w=%d h=%d asp4c0=0x%08x b0=%d b4=%d b8=%d bc=%d"
        " d2=%u d4=%d t88=%d fs=%d sf=%d p4d0=%u d1=%u ca=%u c20=%u"
        " r120=%u r38c=%u sa9=%u sab=%u sac=%u"
        " ra=0x%llx t4fc=%d p500=%d b4a4=%u b4a6=%u b50c=%u b50d=%u w4c8=%u"
        " vt38=0x%llx",
        side, (unsigned long long)bootMs,
        static_cast<unsigned>(threadGetCurHandle()),
        (unsigned long long)renderExport,
        (unsigned long long)(uintptr_t)jniEnvironment,
        (unsigned long long)(uintptr_t)activityObject,
        arg2, static_cast<unsigned>(arg3),
        (unsigned long long)snap.activity,
        (unsigned long long)(*(reinterpret_cast<const volatile uintptr_t*>(
            snap.activity + 0x58))),
        (unsigned long long)snap.renderer,
        index4a0, snap.width, snap.height, aspect4c0,
        cached4b0, cached4b4, mode4b8, mode4bc,
        static_cast<unsigned>(flag4d2), retry4d4, base0088,
        snap.frameState, snap.splashFrames,
        static_cast<unsigned>(snap.paused),
        static_cast<unsigned>(snap.flag4d1),
        static_cast<unsigned>(snap.containerActive),
        static_cast<unsigned>(snap.containerReady),
        static_cast<unsigned>(snap.renderer120),
        static_cast<unsigned>(snap.renderer38c),
        static_cast<unsigned>(snap.splashExit),
        static_cast<unsigned>(snap.splashFlagAb),
        static_cast<unsigned>(snap.splashFlagAc),
        (unsigned long long)renderArg, tier4fc, parsed500,
        b4a4, b4a6, b50c, b50d, w4c8,
        static_cast<unsigned long long>(slot38));
    return 0;
}

void writeGuestViewState(const char* progressPath, uintptr_t renderExport,
                         unsigned long long ordinal) {
    if (progressPath == nullptr) {
        return;
    }
    const GuestActivitySnapshot snap = readGuestActivitySnapshot(renderExport);
    const GuestViewGlStats stats = guestViewGlStats();
    char details[384]{};
    // Append-only sidecar: writeGuestBootProgress truncates, and the final
    // handoff-process-exit line wiped every guest-activity dump (round 11).
    constexpr const char* kViewLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";
    if (!snap.valid) {
        appendGuestBootLine(kViewLog, "guest-activity-null", ordinal, 0);
        return;
    }
    const int written = __builtin_snprintf(
        details, sizeof(details),
        "a4=%u a5=%u a6=%u d0=%u d1=%u d8=%d dc=%d eng=%u engp=%llx w=%d h=%d "
        "ca=%u cr=%u se=%u c38c=%u cq0=%llx cq1=%llx cq2=%llx "
        "gl=%u/%u/%u/%u/%u/%u/%u vp=%u sc=%u fbo=%u/%u st=%x buf=%u texb=%u vap=%u "
        "t0=%u b0=%u lasttex=%u lastbuf=%u gentex=%u genbuf=%u lastgen=%u/%u hinj=%u lastvp=%u,%u,%u,%u",
        snap.glObjectsCreated, snap.landscapeApplied, snap.isLandscape,
        snap.paused, snap.flag4d1, snap.frameState, snap.splashFrames,
        snap.engineReady, static_cast<unsigned long long>(snap.engine),
        snap.width, snap.height,
        snap.containerActive, snap.containerReady, snap.splashExit, snap.renderer38c,
        static_cast<unsigned long long>(snap.containerQ0),
        static_cast<unsigned long long>(snap.containerQ1),
        static_cast<unsigned long long>(snap.containerQ2),
        stats.clears, stats.drawArrays,
        stats.drawElements, stats.texImage2D, stats.compressedTexImage2D,
        stats.bindFramebuffer, stats.useProgram, stats.viewports, stats.scissors,
        stats.fboChecks, stats.fboIncomplete, stats.lastFboStatus,
        stats.bindBuffer, stats.bindTexture, stats.vertexAttribPointer,
        stats.bindTextureZero, stats.bindBufferZero,
        stats.lastBoundTexture, stats.lastBoundBuffer,
        stats.genTextures, stats.genBuffers, stats.lastGenTexture,
        stats.lastGenBuffer, stats.hostTexInject,
        stats.lastViewport[0], stats.lastViewport[1], stats.lastViewport[2],
        stats.lastViewport[3]);
    if (written > 0) {
        appendGuestBootLine(kViewLog, "guest-activity", ordinal,
                            snap.activity, details);
    }
}

// ---------------------------------------------------------------------------
// Frame-loop driver: which thread calls the guest's onViewRenderJNI.
//
// The guest boots, renders exactly one frame (the bootstrap render below, on
// the main thread, which returns), and then the FIRST render call made from
// inside the frame loop never returns. Same function, same arguments,
// different call site -- so the open question is whether the block belongs to
// the MAIN THREAD or to the CALL.
//
// "render-thread=worker" in sdmc:/switch/rr3-gfx.conf answers it: the loop
// moves to a guest worker thread created through guestPthreadCreate (so the
// guest sees the right Android TLS, because pthread_bridge's thread start
// installs a GuestTlsScope, and the census attributes the thread), and the
// main thread does nothing but wait. The main thread MUST NOT also call
// render, or the two threads race the same guest state and the run proves
// nothing.
//
// Default is the byte-for-byte historical path: the main thread runs the loop
// itself. Nothing here is on that path but one branch and one conf read.
// ---------------------------------------------------------------------------

struct FrameLoopContext {
    GuestViewProbe* probe;
    void* jniEnvironment;
    void* activityObject;
    uintptr_t renderTarget;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    const char* progressPath;
    PadState pad;
    uint64_t frames;
    uint32_t lastGlError;
    bool worker;
    // Set by the driver, whatever thread it runs on. Release/acquire is the
    // whole handshake: the main thread sleeps on it and then reads frames.
    std::atomic<bool> done;
};

// Defined at the end of this file, after runGuestViewProbe, because the body
// is the frame loop verbatim and it must stay textually below the bootstrap
// render it is compared against.
void* frameLoopDriver(void* opaque);

} // namespace

GuestViewProbe runGuestViewProbe(NWindow* window, const char* progressPath) {
    GuestViewProbe probe{};
    probe.attempted = true;
    probe.surfaceMode = 1; // default NWindow surface
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-probe-enter", 240, 0);
    if (window == nullptr) {
        probe.failureCode = 5;
        probe.eglError = EGL_BAD_NATIVE_WINDOW;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-null-window", 241, 0);
        return probe;
    }

    // Resolve every callback before installing guest TLS. This keeps a missing
    // export a normal diagnostic result rather than a fault in the guest scope.
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-created", 242, 0);
    const uintptr_t createdTarget = resolveMainImageExport(kCreatedExport);
    if (createdTarget == 0) {
        probe.failureCode = 1;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-created-fail", 243, 0);
        return probe;
    }
    probe.createdResolved = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-changed", 244, 0);
    const uintptr_t changedTarget = resolveMainImageExport(kChangedExport);
    if (changedTarget == 0) {
        probe.failureCode = 3;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-changed-fail", 245, 0);
        return probe;
    }
    probe.changedResolved = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-render", 246, 0);
    const uintptr_t renderTarget = resolveMainImageExport(kRenderExport);
    if (renderTarget == 0) {
        probe.failureCode = 4;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-render-fail", 247, 0);
        return probe;
    }
    probe.renderResolved = true;
    g_renderTarget = renderTarget;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-resolve-ok", 248, 0);

    // ---- EGL autopsy: see the comment above -------------------------------
    {
        std::FILE* f = std::fopen(kEglWatchdogPath, "wb");
        if (f != nullptr) {
            std::fprintf(f, "egl-probe begin\n");
            std::fclose(f);
        }
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "view-egl-probe-begin", 249, 0);

    {
        // No-display queries touch no device: if these hang, Mesa's own
        // initialisation is the wall, not DRM or the EGL platform.
        const char* vendor = eglQueryString(EGL_NO_DISPLAY, EGL_VENDOR);
        const char* version = eglQueryString(EGL_NO_DISPLAY, EGL_VERSION);
        const char* apis = eglQueryString(EGL_NO_DISPLAY, EGL_CLIENT_APIS);
        const char* exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        char diag[256];
        __builtin_snprintf(diag, sizeof(diag),
            "egl-nodisplay: vendor=%s version=%s apis=%s ext_len=%d",
            vendor != nullptr ? vendor : "(null)",
            version != nullptr ? version : "(null)",
            apis != nullptr ? apis : "(null)",
            exts != nullptr ? (int)std::strlen(exts) : -1);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            diag, 249, 1);
        if (exts != nullptr) {
            // The extension list is what decides whether any Mesa platform
            // could ever open a display here, so keep it verbatim on disk.
            std::FILE* f = std::fopen(kEglWatchdogPath, "ab");
            if (f != nullptr) {
                std::fprintf(f, "egl-extensions: %s\n", exts);
                std::fclose(f);
            }
        }
    }
    {
        // eglGetProcAddress yields a function pointer type; route through
        // uintptr_t so it can be printed rather than converted to void*.
        const auto platformExt =
            reinterpret_cast<uintptr_t>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        const auto platformCore =
            reinterpret_cast<uintptr_t>(eglGetProcAddress("eglGetPlatformDisplay"));
        char diag[128];
        __builtin_snprintf(diag, sizeof(diag),
            "egl-proc: platformDisplayEXT=0x%llx platformDisplay=0x%llx",
            (unsigned long long)platformExt, (unsigned long long)platformCore);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            diag, 249, 2);
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "view-egl-probe-end", 249, 0);

    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-get-display", 249, 0);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    // TRIED AND REFUTED 2026-10-11, archive 20261011-1*mesa1, tag mesa1:
    // building the display from the enumerated device instead --
    //     eglQueryDevicesEXT -> eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, dev)
    // -- works exactly as far as it can be observed working and changes
    // nothing afterwards:
    //     device-platform: max=1 found=1 first=0x80c41b10
    //     dev[0]=0x80c41b10 display=0x2103eb1ac0     <- valid, non-null
    //     view-egl-get-display-ret
    //     view-egl-init-begin                        <- and then nothing, 412 s
    // Identical to the default-platform path. So the display handle is NOT the
    // bug: eglInitialize blocks on ANY display in this process. That kills the
    // "wrong platform enum" hypothesis outright, and leaves the real question
    // open -- something about this process's GPU frontend state before
    // runGuestViewProbe ever asks for a display.
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-get-display-ret", 249, 0);

    if (display == EGL_NO_DISPLAY) {
        probe.failureCode = 5;
        logEglFailure("getDisplay", display, probe);
        return probe;
    }
    probe.displayCreated = true;

    EGLint major = 0;
    EGLint minor = 0;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-init-begin", 249, 0);
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        probe.failureCode = 5;
        logEglFailure("initialize", display, probe);
        eglTerminate(display);
        return probe;
    }
    probe.initialized = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-init-ok", 250, 0);

    constexpr EGLint configAttributes[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config{};
    EGLint configCount = 0;
    if (eglChooseConfig(display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
        configCount == 0) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        eglTerminate(display);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-config-fail", 251, 0);
        return probe;
    }
    probe.configChosen = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-config-ok", 252, 0);

    EGLSurface surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        eglTerminate(display);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-surface-fail", 253, 0);
        return probe;
    }
    probe.surfaceCreated = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-surface-ok", 254, 0);

    constexpr EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        probe.failureCode = 5;
        logEglFailure("createContext", display, probe);
        releaseEgl(display, surface, EGL_NO_CONTEXT);
        return probe;
    }
    probe.contextCreated = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-context-ok", 255, 0);

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        releaseEgl(display, surface, context);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-current-fail", 256, 0);
        return probe;
    }
    probe.madeCurrent = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-egl-current-ok", 257, 0);

    // First exercise the exact host GLES call that the guest's first View
    // initialization uses. This is deliberately outside GuestTlsScope so a
    // bad NWindow/EGL handoff is separated from a Mesa/TLS interaction.
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-outside-enter", 0, 0);
    }
    probe.hostGlOutsideTlsCalled = true;
    const GLubyte* outsideVersion = ::glGetString(GL_VERSION);
    probe.hostGlOutsideTlsReturned = true;
    probe.hostGlOutsideTlsNonNull = outsideVersion != nullptr;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-outside-return",
                               outsideVersion != nullptr ? 1 : 0, 0);
    }

    // The callbacks use the Activity object and JNI table created by the
    // preceding guest-entry probe. EGL is deliberately established first so
    // every guest GL dereference sees the same live context and NWindow.
    const JniSlotHistogram beforeCreated = captureJniHistogram();
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-tls-scope-enter", 258, 0);
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        probe.failureCode = 2;
        releaseEgl(display, surface, context);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-tls-scope-fail", 259, 0);
        return probe;
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-tls-scope-ok", 260, 0);

    // Repeat the host query while TPIDR_EL0 has the Android-compatible layout.
    // If this is the last breadcrumb, Mesa/its driver path閳ユ攺ot guest code閳ユ攰s
    // the first component that depends on the guest TLS register.
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-inside-enter", 0, 0);
    }
    probe.hostGlInsideTlsCalled = true;
    const GLubyte* insideVersion = ::glGetString(GL_VERSION);
    probe.hostGlInsideTlsReturned = true;
    probe.hostGlInsideTlsNonNull = insideVersion != nullptr;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-inside-return",
                               insideVersion != nullptr ? 1 : 0, 0);
    }

    // Bind the trace only around guest callbacks. The resolver target remains
    // transparent everywhere else, including the independent renderer NRO.
    enableGuestViewGlTrace(progressPath);
    // LIFECYCLE ORDER. onCreateJNI must run BEFORE onViewCreatedJNI.
    //
    // The guest's exported onViewCreatedJNI (0x12b35e4) is four instructions:
    //
    //   0x12b35e4  adrp x8, #0x1f6e000
    //   0x12b35e8  ldr  x8, [x8, #0x540]      ; the MainActivity singleton cell
    //   0x12b35ec  ldr  x0, [x8]
    //   0x12b35f0  cbz  x0, #0x12b35f8        ; NULL -> silent return
    //   0x12b35f4  b    #0x12ad504            ; otherwise build the term-1 object
    //
    // and func 0x12ad504 is a ONE-SHOT: `ldrb w8,[x19,#0x4a4] ; cbz` builds the
    // object on first entry, and every later entry dereferences it with no null
    // check. That object's vtable[21] is 0x14851e8 -- the menu loader the
    // per-frame gate at 0x12af010 calls:
    //
    //   0x12aefb8  ldr  x23, [x23, #0xf50]   ; GOT 0x1f6df50, the term-1 cell
    //   0x12aefc4  cbz  x8, #0x12af014        ; TERM 1 of the four-way AND
    //   ... cc->[0x4c1], the vtable[17] tag ...
    //   0x12af010  blr  x8                   ; vtable[21] = menu loader
    //
    // The host called onViewCreatedJNI from runGuestViewProbe (main.cpp:1176)
    // while onCreateJNI only runs later, from runGuestEntryProbe (main.cpp:
    // 1276). So the singleton was null at that moment, the cbz fired, and term
    // 1 stayed false for the whole run -- 136/136 polls reading 0x0. Everything
    // downstream of the gate was never entered.
    //
    // This is a host ordering bug, not a guest limitation, and it is not an
    // anti-cheat gate: no credential is involved, only a null check on an
    // object this same process is responsible for creating first.
    cloudcell_pump("before-created");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-call-created", 261, 0);
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewCreatedJNI", 0,
            static_cast<unsigned long long>(createdTarget));
    }
    void* jniEnvironment = guestJniEnvironment();
    void* activityObject = guestActivityObject();
    // LIFECYCLE ORDER. onCreateJNI must run BEFORE onViewCreatedJNI.
    //
    // The guest's exported onViewCreatedJNI (0x12b35e4) is four instructions:
    //
    //   0x12b35e4  adrp x8, #0x1f6e000
    //   0x12b35e8  ldr  x8, [x8, #0x540]      ; the MainActivity singleton cell
    //   0x12b35ec  ldr  x0, [x8]
    //   0x12b35f0  cbz  x0, #0x12b35f8        ; NULL -> silent return
    //   0x12b35f4  b    #0x12ad504            ; otherwise build the term-1 object
    //
    // and func 0x12ad504 is a ONE-SHOT: `ldrb w8,[x19,#0x4a4] ; cbz` builds the
    // object on first entry, and every later entry dereferences it with no null
    // check. That object's vtable[21] is 0x14851e8 -- the menu loader the
    // per-frame gate at 0x12af010 calls:
    //
    //   0x12aefb8  ldr  x23, [x23, #0xf50]   ; GOT 0x1f6df50, the term-1 cell
    //   0x12aefc4  cbz  x8, #0x12af014        ; TERM 1 of the four-way AND
    //   ... cc->[0x4c1], the vtable[17] tag ...
    //   0x12af010  blr  x8                   ; vtable[21] = menu loader
    //
    // The host called onViewCreatedJNI from runGuestViewProbe (main.cpp:1176)
    // while onCreateJNI only ran later, from runGuestEntryProbe (main.cpp:
    // 1276). So the singleton was null at that moment, the cbz fired, and term
    // 1 stayed false for the whole run -- 136/136 polls reading 0x0. Everything
    // downstream of the gate was never entered.
    //
    // This is a host ordering bug, not a guest limitation, and it is not an
    // anti-cheat gate: no credential is involved, only a null check on an
    // object this same process is responsible for creating first.
    //
    // Both call sites are direct branches, confirmed by scanning every bl/b in
    // .text: 0x12ad504 is reached only from 0x12b35f4, and the per-frame
    // function 0x12aea7c only from 0x12b3624. An earlier reverse pass reported
    // both as having zero direct callers; that was a scanner defect (capstone
    // yields the already-scaled displacement, so comparing it against an
    // unscaled imm12 silently matches nothing).
    const uintptr_t createTarget =
        resolveMainImageExport(
            "Java_com_firemint_realracing_MainActivity_onCreateJNI");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "onCreateJNI-pre", 110,
                        static_cast<unsigned long long>(createTarget));
    if (createTarget != 0) {
        // Twice, deliberately. onCreateJNI is a get-or-create:
        //
        //   0x12b3150  cbz  x8, #0x12b3160     ; NULL -> allocate + construct
        //   0x12b3154  ...                       ; NON-NULL -> return, skip
        //   0x12b3160  mov  w0, #0x540
        //   0x12b3178  b    #0x12ac2dc           ; tail-call MainActivity ctor
        //
        // so a second call is a cheap no-op when the cell is already set, and
        // a retry when it is not. Static analysis of the constructor
        // (0x12ac2dc) found exactly four stores to the cell at 0x1f97030 and
        // only one of them produces: 0x12ac330, which sits AFTER
        // `bl #0x12b62c8` at 0x12ac310 -- a call that dereferences x1 through a
        // JNIEnv vtable. A first call that does not reach that store leaves the
        // cell null, and onViewCreatedJNI then takes its silent
        // `cbz #0x12b35f8` and the term-1 object is never built.
        //
        // The archived logs show the guest itself calling onCreateJNI a second
        // time (CREATE at log line 195, right after view-call-created at 193),
        // and later logging "onWindowFocusChanged: hasFocus true", which is only
        // reachable past the same cell's null check at 0x12b3514. So the cell
        // does get populated -- just not in time for the first onViewCreatedJNI.
        //
        // Calling it twice here closes that window without changing guest
        // semantics: it is the same export, the same arguments, and the same
        // idempotent contract.
        for (int attempt = 0; attempt < 2; ++attempt) {
            reinterpret_cast<void (*)(void*, void*)>(createTarget)(
                jniEnvironment, activityObject);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                attempt == 0 ? "onCreateJNI-ret-a"
                                             : "onCreateJNI-ret-b",
                                110, 0);
        }
    }
    probe.createdCalled = true;
    reinterpret_cast<void (*)(void*, void*)>(createdTarget)(
        jniEnvironment, activityObject);
    probe.createdReturned = true;
    cloudcell_pump("after-created");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-created-ret", 262, 0);
    const JniSlotHistogram afterCreated = captureJniHistogram();
    probe.createdJni = diffJniHistograms(afterCreated, beforeCreated);

    cloudcell_pump("before-changed");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-call-changed", 263, 0);
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewChangedJNI", 0,
            static_cast<unsigned long long>(changedTarget));
    }
    probe.changedCalled = true;
    reinterpret_cast<void (*)(void*, void*, int, int, int, int)>(changedTarget)(
        jniEnvironment, activityObject, 1280, 720, 0, 0);
    probe.changedReturned = true;
    cloudcell_pump("after-changed");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-changed-ret", 264, 0);
    // Sample the guest's path-resolver gate HERE, before the render call, and
    // not only in the frame loop. The failures being chased
    // ("LoadReadOnlyMappedFile failed getting full path") happen during this
    // window; a sample taken later measures a state the guest has already left.
    // The frame-loop sample read the first byte as 1, which would have said the
    // gate is satisfied -- and that reading is worthless if the byte was 0
    // during the failures.
    noteContentRootGate("after-changed", renderTarget);
    const JniSlotHistogram afterChanged = captureJniHistogram();
    probe.changedJni = diffJniHistograms(afterChanged, afterCreated);

    // First-landscape Changed is supposed to run FUN_013b4328 which sets
    // engine+0xa (render early-outs / empty update while it is 0). If the
    // guest skipped that path, force the flag so the frame loop can draw.
    // Also force the three gates the resource FSM needs to leave splash:
    // container+0xa (menu draw), container+0x20 (ready), renderer+0x1f5a9
    // (splash閳姦enu). Guest never sets 0x1f5a9; without it the menu layout
    // switch never runs and no sprite atlases are opened.
    {
        const GuestActivitySnapshot afterChangedSnap =
            readGuestActivitySnapshot(renderTarget);
        if (afterChangedSnap.valid) {
            const uintptr_t engine =
                *reinterpret_cast<const uintptr_t*>(afterChangedSnap.activity + 0x58);
            if (engine != 0) {
                volatile uint8_t* ready =
                    reinterpret_cast<volatile uint8_t*>(engine + 0xa);
                if (*ready == 0) {
                    *ready = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-engine-ready", 78, engine);
                }
            }
            if (afterChangedSnap.container != 0) {
                volatile uint8_t* active =
                    reinterpret_cast<volatile uint8_t*>(afterChangedSnap.container + 0xa);
                if (*active == 0) {
                    *active = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-container-active", 80,
                                        afterChangedSnap.container);
                }
                volatile uint8_t* containerReady =
                    reinterpret_cast<volatile uint8_t*>(afterChangedSnap.container + 0x20);
                if (*containerReady == 0) {
                    *containerReady = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-container-ready", 81,
                                        afterChangedSnap.container);
                }
            }
            // Do NOT force frameState 閿?let the guest run its natural
            // warmup(1/2/3)閳墱L-reset(4)閳姱plash(5)閳姦enu sequence. The
            // connect() stub already prevents HTTP blocking.
            // Force renderer splash閳姦enu gate bytes. The main-render path
            // at 0x12aed14 reads renderer+0x1f5a9; if 0 it skips menu layout.
            // When non-zero it also checks +0x1f5ab, +0x1f5ac, +0x120.
            // renderer = *container (containerQ0). Only write when the
            // pointer is clearly on the heap (>4 GiB) 閿?writing
            // container+0x1f5a9 is OOB and crashed (round 32).
            //
            // A/B SWITCH, added 2026-10-04. DEFAULT IS OFF (leave the guest's
            // own state machine alone).
            //
            // Reason: the same call, with the same arguments, RETURNS on the
            // first JNI view render (view-call-render 265 / view-render-ret
            // 266) and does NOT return on the second, from the frame loop
            // (render-call-pre 285, no post). So the failure is state that
            // changed after the first frame -- and these four writes are the
            // only thing this tree does to the renderer between those two
            // calls. Forcing a guest's "splash has exited" bits by hand skips
            // whatever it does on the real transition, and 0x12aed14 is inside
            // the very function being wedged.
            //
            // AND NOW THE REAL REASON, read off the bytes (render-head.py,
            // 0x12aed14..0x12aede0):
            //
            //   0x12aed28  ldrb w10, [x9]        ; x9 = renderer + 0x1f5a9
            //   0x12aed2c  cbz  w10, 0x12aed7c   ; 0 -> take the other arm
            //   ...                                  (does 0x12ab4d8/0x12b4588)
            //   0x12aeddc  strb wzr, [x8, x9]    ; renderer[0x1f5a9] = 0
            //
            // The guest CLEARS that byte itself as soon as it has consumed it.
            // It is a one-shot "splash has exited, do the transition" flag, not
            // a level. The per-frame re-assert further down this file (which is
            // not guarded by this switch) put it back to 1 every 16 ms, so the
            // guest re-entered the menu transition 60 times a second forever.
            // That is the wedge, and it is ours.
#ifndef RR3_FORCE_SPLASH_FLAGS
#define RR3_FORCE_SPLASH_FLAGS 0
#endif
            if (RR3_FORCE_SPLASH_FLAGS &&
                afterChangedSnap.renderer != 0 &&
                afterChangedSnap.renderer > 0x100000000ULL) {
                volatile uint8_t* r = reinterpret_cast<volatile uint8_t*>(
                    afterChangedSnap.renderer);
                const uint8_t before = r[0x1f5a9];
                if (before == 0) {
                    r[0x1f5a9] = 1;
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "force-splash-exit", 91,
                        static_cast<unsigned long long>(r[0x1f5a9]));
                }
                if (r[0x1f5ab] == 0) {
                    r[0x1f5ab] = 1;
                }
                if (r[0x1f5ac] == 0) {
                    r[0x1f5ac] = 1;
                }
                if (r[0x120] == 0) {
                    r[0x120] = 1;
                }
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "force-splash-flags", 92,
                    (static_cast<unsigned long long>(r[0x1f5a9]) << 24) |
                    (static_cast<unsigned long long>(r[0x1f5ab]) << 16) |
                    (static_cast<unsigned long long>(r[0x1f5ac]) << 8) |
                    static_cast<unsigned long long>(r[0x120]));
            }
        }
    }

    cloudcell_pump("before-render");
    // Thread identity added 2026-10-04. The 265/266/285 numbers on these
    // breadcrumbs are SEQUENTIAL LOG ORDINALS, not thread handles -- the
    // frame-loop straddle is the only line that carried a real handle
    // (th=0x...), so the thread the first, returning render ran on could not be
    // compared with the thread the wedging one runs on. That comparison is the
    // whole test for SaveManager.cpp:699's "You can not load a save game on a
    // non main thread" (xref 0x13a4468), so both call sites now record it.
    {
        char tdiag[96];
        const int tn = __builtin_snprintf(
            tdiag, sizeof(tdiag), "th=0x%llx site=jni-view-render",
            static_cast<unsigned long long>(threadGetCurHandle()));
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "view-call-render", 265,
                            static_cast<unsigned long long>(threadGetCurHandle()),
                            tdiag);
    }
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewRenderJNI", 0,
            static_cast<unsigned long long>(renderTarget));
    }
    probe.renderCalled = true;
    // The guest render entry receives a render mode and flags, not dimensions;
    // mode 2 with no flags selects the normal foreground surface path.
    reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
        jniEnvironment, activityObject, 2, 0);
    probe.renderReturned = true;
    cloudcell_pump("after-render");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "view-render-ret", 266, 0);

    // ---- onWindowFocusChangedJNI: the one lifecycle native never delivered ---
    //
    // Decompiled (Ghidra 0x13B34F8 = elfva 0x12B34F8, size 0xD4):
    //   if (*PTR_DAT_0206e540 == 0) return;                 // activity singleton
    //   log("onWindowFocusChanged: hasFocus %s\n");
    //   if (*PTR_DAT_0206d5d8 == 0) activity[0x4d1] = hasFocus;   // <== SETS 0x4d1
    //   ... otherwise pause/resume music via FUN_015054bc/dc ...
    //   if (activity[0x4d0] && !activity[0x4d2]) return;
    //   FUN_013ad2e0(activity);                            // startMusic
    //
    // activity+0x4d1 is the hasFocus byte, and nothing else in the image writes
    // it. Calling this is still correct and still necessary -- the guest's
    // render body reads it -- but the rationale that used to sit here was
    // wrong, and it is worth recording why so nobody restores it.
    //
    // The old claim: "with +0x4d1 stuck at 0 the guest permanently takes the
    // no-surface-change arm and the frame FSM at activity+0x4d8 never advances."
    // The render entry, disassembled from raw words (0x12aea7c):
    //
    //   012aeaac  ldr  w1, [x0, #0x4a8]      ; w
    //   012aeab0  ldr  w8, [x0, #0x4b0]      ; w2
    //   012aeac0  cmp  w1, w8
    //   012aeac4  b.ne #0x12aead8            ; w != w2  -> change arm
    //   012aeac8  ldr  w8, [x19, #0x4ac]     ; h
    //   012aeacc  ldr  w9, [x19, #0x4b4]     ; h2
    //   012aead0  cmp  w8, w9
    //   012aead4  b.eq #0x12aeb20            ; h == h2  -> no-change arm
    //   012aead8  ldrb w8, [x19, #0x4d1]     ; hasFocus
    //   012aeadc  cbz  w8, #0x12aeb20        ; no focus -> no-change arm
    //   012aeaE0  ldr  w8, [x19, #0x4d4]     ; frameCounter
    //   012aeaE4  add  w9, w8, #1
    //   012aeaE8  cmp  w8, #3
    //   012aeaEC  str  w9, [x19, #0x4d4]     ; ++
    //   ...
    //   012aeb20  add  x20, x19, #0x4d4      <== BOTH arms land here
    //   012aeb28  ldr  w8, [x19, #0x4d8]     ; frameState, handled either way
    //
    // Both arms converge at 0x12aeb20, so the gate does not block the render
    // body: 0x4d8 is read and written on the no-change path too. +0x4d4 is a
    // resize debounce counter, not a per-frame tick. Two more facts kill the
    // old reading outright:
    //   * the condition is OR, not AND -- the old comment said "BOTH pairs
    //     differ", but `b.ne` on w and `b.eq` on h make it (w != w2) || (h != h2)
    //   * the surface-changed handler writes BOTH pairs. At 0x12ae72c/0x12ae730:
    //       str w21, [x20, #0x4b0]   ; w2  = new width
    //       str w19, [x20, #0x4b4]   ; h2  = new height
    //     so after any onViewChangedJNI the two pairs are equal by construction
    //     and the change arm is a debounce for resizes, exactly as a real
    //     device would behave.
    //
    // The call stays. It is part of the View lifecycle, the guest reads +0x4d1,
    // and the log line below proves the byte moved. What it is NOT is the
    // reason the guest stops at the splash -- that is still unexplained.
    {
        const uintptr_t focusTarget = resolveMainImageExport(kFocusExport);
        if (focusTarget != 0) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "view-call-focus", 267, 0);
            reinterpret_cast<void (*)(void*, void*, char)>(focusTarget)(
                jniEnvironment, activityObject, 1);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "view-focus-ret", 267, 0);
            const GuestActivitySnapshot focusSnap =
                readGuestActivitySnapshot(renderTarget);
            if (focusSnap.valid) {
                char diag[208];
                __builtin_snprintf(diag, sizeof(diag),
                    "focus-applied: activity=%llx flag4d1=%u paused=%u frameState=%d splash=%d"
                    " a4=%u a5=%u a6=%u surf=%dx%d",
                    static_cast<unsigned long long>(focusSnap.activity),
                    focusSnap.flag4d1, focusSnap.paused,
                    focusSnap.frameState, focusSnap.splashFrames,
                    focusSnap.glObjectsCreated, focusSnap.landscapeApplied,
                    focusSnap.isLandscape, focusSnap.width, focusSnap.height);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    diag, 267, 1);
            } else {
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "focus-applied-null-activity", 267, 1);
            }
        } else {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "view-resolve-focus-fail", 267, 0);
        }
    }
    writeGuestBootProgress(progressPath, "onViewRenderJNI-return", 46, renderTarget);
    const JniSlotHistogram afterRender = captureJniHistogram();
    probe.renderJni = diffJniHistograms(afterRender, afterChanged);
    probe.lastTraceStage = guestViewTraceLastStage();
    probe.postGlMalloc2800Called = guestViewTraceReached(9);
    probe.postGlMalloc2800Returned = guestViewTraceReached(10);
    probe.postGlMalloc3e38Called = guestViewTraceReached(11);
    probe.postGlMalloc3e38Returned = guestViewTraceReached(12);
    probe.postGlMalloc240FirstCalled = guestViewTraceReached(23);
    probe.postGlMalloc240FirstReturned = guestViewTraceReached(24);
    probe.postGlMalloc130Called = guestViewTraceReached(25);
    probe.postGlMalloc130Returned = guestViewTraceReached(26);
    probe.postGlMalloc40FirstCalled = guestViewTraceReached(27);
    probe.postGlMalloc40FirstReturned = guestViewTraceReached(28);
    probe.postGlMalloc40SecondCalled = guestViewTraceReached(29);
    probe.postGlMalloc40SecondReturned = guestViewTraceReached(30);
    disableGuestViewGlTrace();

    writeGuestBootProgress(progressPath, "view-gl-finish-enter", 47, 0);
    glFinish();
    writeGuestBootProgress(progressPath, "view-gl-finish-return", 48, 0);
    probe.glError = static_cast<uint32_t>(glGetError());
    writeGuestBootProgress(progressPath, "view-swap-enter", 49, probe.glError);
    probe.swapped = eglSwapBuffers(display, surface) == EGL_TRUE;
    writeGuestBootProgress(progressPath, "view-swap-return", 50, probe.swapped);
    if (!probe.swapped) {
        recordEglFailure(probe);
        writeGuestBootProgress(progressPath, "view-egl-release-enter", 51, 0);
        releaseEgl(display, surface, context);
        if (progressPath != nullptr) {
            writeGuestBootProgress(
                progressPath, "guest-view-swap-failed", 3,
                static_cast<unsigned long long>(renderTarget));
        }
        return probe;
    }

    // P3 persistent frame loop: keep the EGL context/surface alive and drive
    // the guest Render callback each frame instead of releasing after one swap.
    // The guest worker threads created during Changed keep using this context,
    // so it must stay current until the operator exits the game state.
    probe.frameLoopEntered = true;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "frame-loop-enter", 70, 0);
    }

    // Host clear once so a guest that draws nothing shows solid blue instead
    // of Mesa's undefined checkerboard. If the screen stays checkerboard the
    // guest itself is presenting garbage.
    glClearColor(0.05f, 0.12f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    setGuestInputProgressPath(progressPath);
    prepareGuestControllerInput();
    connectGuestController(jniEnvironment, activityObject, 1);

    // Create fake cloudcell + menu-loader objects BEFORE the first render
    // call. The guest loads these pointers at function entry; if they are
    // NULL on frame 0, the guest crashes dereferencing them.
    {
        // Cloudcell: GOT at ELF 0x1f6d5d0; delta from onViewRenderJNI = +0xCB9FBC.
        constexpr uint64_t kCloudcellGotDelta = 0x00CB9FBCULL;
        const uintptr_t ccGot = renderTarget + kCloudcellGotDelta;
        const uintptr_t ccGlobal = *reinterpret_cast<const uintptr_t*>(ccGot);
        // Diagnostic: dump GOT entry value.
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "cc-got-diag", 110,
            (static_cast<unsigned long long>(ccGot & 0xFFFFFFFFULL) << 32) |
            (ccGlobal & 0xFFFFFFFFULL));
        if (ccGlobal != 0 && ccGlobal > 0x100000000ULL) {
            uintptr_t cloudcell = *reinterpret_cast<const uintptr_t*>(ccGlobal);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "cc-ptr-diag", 111,
                (static_cast<unsigned long long>(ccGlobal & 0xFFFFFFFFULL) << 32) |
                (cloudcell & 0xFFFFFFFFULL));
            if (gfxCloudcellSynthetic() &&
                (cloudcell == 0 || cloudcell < 0x100000000ULL)) {
                static uint64_t s_ccBuf[256] = {};
                static uint64_t s_ccVtable[64] = {};
                // The guest's Cloudcell init function (libRealRacing3.so
                // ~0xcd0c34, the one that logs "Cloudcell initializing with
                // environment: %s") reaches this object through vtable slots
                // 18 (+0x90) and 44 (+0x160) -- and the x29 frame walk that
                // named it also showed the receiver is our synthetic object.
                // Until now every unpinned slot silently shared the i%2
                // alternation, so we could not see which slot the guest
                // actually calls, or how often, or from where.
                // Every unpinned slot now routes through a per-slot trampoline
                // that logs (slot, caller) under a global budget and then
                // returns exactly what the old parity default returned, so
                // this is observation-only.
                for (int i = 0; i < 64; ++i) {
                    s_ccVtable[i] = kCloudcellSlotTable[i];
                }
                struct VtablePin {
                    int index;
                    uint64_t target;
                    const char* why;
                };
                // The i%2 alternation got every recorded call site backwards.
                //   +48  (i=6)  void, must preserve x0      -> void stub (returns self)
                //   +136 (i=17) returns bool, 0 == "proceed" -> bool stub (returns 0)
                //   +272 (i=34) CloudcellResume, void       -> void stub (returns self)
                //
                //   +144 (i=18) FACTORY: returns a receiver, and the caller
                //     immediately uses the return value as a pointer:
                //       ldr  x0, [x28]            ; our Cloudcell
                //       ldr  x8, [x8, #0x90]      ; this slot
                //       blr  x8
                //       mov  x24, x0              ; x24 = OUR RETURN VALUE
                //       ...
                //       ldr  x8, [x24]            ; reads x24's vtable
                //       mov  x0, x24              ; passes x24 as `this`
                //       ldr  x8, [x8, #0x160]     ; slot 44 on that object
                //     The parity default returned 0 for even slots, so x24 was
                //     null and `ldr x8, [x24]` faulted at address 0 -- that is
                //     where the main thread stopped, for ~110 s, inside
                //     onViewRenderJNI. Returning `self` is self-consistent:
                //     s_ccBuf[0] is s_ccVtable, so [self] IS a vtable.
                static const VtablePin kPins[] = {
                    {6,  reinterpret_cast<uint64_t>(&cloudcellPinRecv6),
                     "void, preserve x0"},
                    {17, reinterpret_cast<uint64_t>(&cloudcellPinZero17),
                     "returns bool, 0 = proceed"},
                    {18, reinterpret_cast<uint64_t>(&cloudcellPinRecv18),
                     "factory: returns a receiver, caller derefs it at 0xcd1744"},
                    {8,  reinterpret_cast<uint64_t>(&cloudcellPinRecv8),
                     "factory: returns a receiver, caller derefs it at 0xcd1ae8"},
                    {34, reinterpret_cast<uint64_t>(&cloudcellPinRecv34),
                     "CloudcellResume (void)"},
                    // ---- SCALAR SLOTS: the return value is read as an int, not
                    // as a receiver. Returning `self` here hands the guest an
                    // object address where it reads a small number.
                    // NOTE 0x170/8 is slot 46, NOT 47 -- 0x170 == 368 == 46*8.
                    // It is `double now_ms()`, not a scalar: both callers read it
                    // out of d0 with no fmov. See cloudcellPinTime46 above.
                    {46, reinterpret_cast<uint64_t>(&cloudcellPinTime46),
                     "CLOCK: double now_ms() read from d0; FUN_014c2098 uses it as a "
                     "600 ms throttle against *(int*)(param_1+0x58). Returning an "
                     "integer here leaves the caller reading a stale d0."},
                    {59, reinterpret_cast<uint64_t>(&cloudcellPinZero59),
                     "SCALAR: FUN_014c1f70 does *(vtable+0x1d8)() then "
                     "FUN_01cbdd28 packs the result into a blob as a 4-byte COUNT, "
                     "which FUN_014c20f4 then loops over. Returning `self` makes "
                     "it loop ~2.1e9 times."},
                    // Slot 27 is the authenticated? bit. Returning `self` here
                    // made FUN_01e0307c skip its clean "not available" exit and
                    // enter an ExclusiveMonitor/Yield/nanosleep wait loop that
                    // never ends. See cloudcellPinZero27 above.
                    {27, reinterpret_cast<uint64_t>(&cloudcellPinZero27),
                     "AUTH FLAG: FUN_01e0307c does (*(vtable+0xd8)() & 1); "
                     "0 -> log 'Unable to send Sync' and give up. Non-zero -> "
                     "wait loop that never ends."},
                };
                for (const VtablePin& pin : kPins) {
                    s_ccVtable[pin.index] = pin.target;
                }
                // Slot 3 is a GETTER: 0xab2c30 calls it and then 0xcf1018 does
                // `ldr x9,[x1] ; ldr x9,[x9,#0x10]`, i.e. it treats the result
                // as an object with a vtable. The default stub returns `self`,
                // so the guest would dereference the Cloudcell object's own
                // first word. This returns a real endpoint object instead, and
                // its slot 2 captures the continuation the caller hands over.
                s_ccVtable[3] = reinterpret_cast<uint64_t>(&cloudcellSlot3Endpoint);
                // Whatever is STILL zero after the pins is an entry the guest
                // must not be able to `blr` to zero. See the note on
                // RR3_CC_UNPINNED above for the measurement that forced this.
                static const uint64_t kCloudcellSlotFallback[64] = {
#define RR3_CSF(n) reinterpret_cast<uint64_t>(&cloudcellUnpinned##n)
                    RR3_CSF(0),  RR3_CSF(1),  RR3_CSF(2),  RR3_CSF(3),
                    RR3_CSF(4),  RR3_CSF(5),  RR3_CSF(6),  RR3_CSF(7),
                    RR3_CSF(8),  RR3_CSF(9),  RR3_CSF(10), RR3_CSF(11),
                    RR3_CSF(12), RR3_CSF(13), RR3_CSF(14), RR3_CSF(15),
                    RR3_CSF(16), RR3_CSF(17), RR3_CSF(18), RR3_CSF(19),
                    RR3_CSF(20), RR3_CSF(21), RR3_CSF(22), RR3_CSF(23),
                    RR3_CSF(24), RR3_CSF(25), RR3_CSF(26), RR3_CSF(27),
                    RR3_CSF(28), RR3_CSF(29), RR3_CSF(30), RR3_CSF(31),
                    RR3_CSF(32), RR3_CSF(33), RR3_CSF(34), RR3_CSF(35),
                    RR3_CSF(36), RR3_CSF(37), RR3_CSF(38), RR3_CSF(39),
                    RR3_CSF(40), RR3_CSF(41), RR3_CSF(42), RR3_CSF(43),
                    RR3_CSF(44), RR3_CSF(45), RR3_CSF(46), RR3_CSF(47),
                    RR3_CSF(48), RR3_CSF(49), RR3_CSF(50), RR3_CSF(51),
                    RR3_CSF(52), RR3_CSF(53), RR3_CSF(54), RR3_CSF(55),
                    RR3_CSF(56), RR3_CSF(57), RR3_CSF(58), RR3_CSF(59),
                    RR3_CSF(60), RR3_CSF(61), RR3_CSF(62), RR3_CSF(63),
#undef RR3_CSF
                };
                for (int i = 0; i < 64; ++i) {
                    if (s_ccVtable[i] == 0) {
                        s_ccVtable[i] = kCloudcellSlotFallback[i];
                    }
                }
                // Second-level object for the member at byte offset 0x10
                // (s_ccBuf[2]). See the block comment above
                // kCloudcell2SlotTable for the measured call sites.
                static uint64_t s_cc2Buf[16] = {};   // 128 bytes; only [0] is written
                static uint64_t s_cc2Vtable[32] = {};
                for (int i = 0; i < 32; ++i) {
                    s_cc2Vtable[i] = kCloudcell2SlotTable[i];
                }
                // Slots 11, 15 and 16 all feed std::string's const char*
                // ctor, so they return a real NUL-terminated string instead
                // of `self` -- an object address is not a string and strlen
                // would walk arbitrary memory. Content is not yet known, so
                // it is ""; slot 8 stays on the `self` default because the
                // caller dereferences that return as a receiver.
                static const VtablePin kPins2[] = {
                    {11, reinterpret_cast<uint64_t>(&cloudcellVtableStubString),
                     "returns const char* -> basic_string ctor at 0xcd1da0"},
                    {15, reinterpret_cast<uint64_t>(&cloudcellVtableStubString),
                     "returns const char* -> basic_string ctor at 0xcd1dbc"},
                    {16, reinterpret_cast<uint64_t>(&cloudcellVtableStubString),
                     "returns const char* -> basic_string ctor at 0xcd1e48"},
                };
                for (const VtablePin& pin : kPins2) {
                    s_cc2Vtable[pin.index] = pin.target;
                }
                s_cc2Buf[0] = reinterpret_cast<uint64_t>(s_cc2Vtable);
                s_ccBuf[2] = reinterpret_cast<uint64_t>(s_cc2Buf);
                s_ccBuf[0] = reinterpret_cast<uint64_t>(s_ccVtable);
                // Byte 1217 == 0x4c1 is the guest's "Cloudcell is available"
                // flag on this object. FUN_00bb2c30 gates every async request on
                // it:
                //
                //   if (*PTR_...ebc0 == 0 && our_object && our_object->flag_0x4c1
                //       && (pending & 1) == 0) { ... issue request; pending = 1; }
                //
                // We used to set it to 1, which told the guest Cloudcell was
                // live. It then issued a request through our stub, we never call
                // its callback, `pending` stays 1 for the rest of the run, and
                // every later request is skipped by the same guard -- the guest
                // waits forever. Setting it to 0 is the truthful answer: this
                // host has no Cloudcell backend, so the engine takes its offline
                // branch instead of queueing a request nobody will ever answer.
                // Nothing is faked either way -- no callback, no response, no
                // telemetry; the flag simply reports what is actually here.
                //
                // BUT THIS SAME BYTE IS ALSO THE MAIN-MENU GATE, found
                // 2026-10-12 by guest-side disassembly (this project had been
                // chasing a black screen for days and never looked here):
                //
                //   0x12aefd8  ldrb w8, [x0, #0x4c1]
                //   0x12aefdc  cbz  w8, #0x12af014      ; flag==0 -> SKIP
                //   ...
                //   0x12af010  blr  x8                   ; obj->vtable[21](obj,2,0x60)
                //                                        ; loads ESportsMainMenu.xml
                //
                // The guest NEVER writes this byte itself: over all 4,459,271
                // decoded .text instructions there is exactly one store to
                // [Xn,#0x4c1], at 0x10f0990, and it targets sp. On a device
                // Cloudcell sets it during init. Here it is host-owned.
                //
                // ONE BYTE, TWO CONSUMERS, OPPOSITE POLARITY:
                //   0xab2c30  async-request gate  needs 0  (1 -> a request is
                //              issued through our stub, nobody ever calls its
                //              callback, pending sticks at 1, and every later
                //              request is skipped -- that was the ordinal-686
                //              wall, and why this was originally set to 0)
                //   0x12aefd8 main-menu gate      needs non-zero
                //
                // So a flat flip to 1 re-opens the 686 wedge. The separable
                // fix is to make the request stub actually COMPLETE, so pending
                // clears; that is the next piece of work, not this one.
                //
                // "menugate=1" in rr3-gfx.conf flips it anyway, as a bounded A/B,
                // because with the menu never loading every other observation
                // about the guest is second-hand. It fakes nothing: the menu
                // loader is the guest's own vtable[21] on an object that
                // already exists (ml-ptr-full 0x210dbf8920); only the gate
                // byte changes.
                //
                // Acceptance, fixed before running:
                //   menu-load reached  -> the gate was the blocker, and the
                //                         0xab2c30 hang must then be fixed by
                //                         completing the request, not by the flag
                //   guest wedges at ~686 again -> confirms the two are coupled
                //                         exactly as documented above
                const uint8_t menuGate = gfxMenuGateOpen() ? 1 : 0;
                reinterpret_cast<volatile uint8_t*>(s_ccBuf)[1217] = menuGate;
                cloudcell = reinterpret_cast<uint64_t>(s_ccBuf);
                *reinterpret_cast<volatile uintptr_t*>(ccGlobal) = cloudcell;
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "create-cloudcell", 101, cloudcell);
                {
                    char line[192];
                    __builtin_snprintf(
                        line, sizeof(line),
                        "cc2-install: obj=%llx vtable=%llx member0x10=%llx slots=32",
                        (unsigned long long)(uintptr_t)s_cc2Buf,
                        (unsigned long long)(uintptr_t)s_cc2Vtable,
                        (unsigned long long)s_ccBuf[2]);
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        line, 400, reinterpret_cast<uint64_t>(s_cc2Buf));
                }
            }
        }
        // Menu loader: GOT entry at ELF 0x1f6df50; delta = +0xCBA93C.
        // TWO levels of indirection:
        //   x23 = *(0x1f6df50)  = pointer to global variable
        //   x0  = *x23          = object pointer
        // The GOT entry is in RELRO; the global variable it points to is writable.
        constexpr uint64_t kMenuLoaderDelta = 0x00CBA93CULL;
        const uintptr_t gotEntry = renderTarget + kMenuLoaderDelta;
        const uintptr_t globalVarAddr =
            *reinterpret_cast<const uintptr_t*>(gotEntry);
        // Diagnostic: dump GOT entry value.
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "ml-got-diag", 112,
            (static_cast<unsigned long long>(gotEntry & 0xFFFFFFFFULL) << 32) |
            (globalVarAddr & 0xFFFFFFFFULL));
        if (globalVarAddr != 0 && globalVarAddr > 0x100000000ULL) {
            uintptr_t menuLoader =
                *reinterpret_cast<const uintptr_t*>(globalVarAddr);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "ml-ptr-full", 115, menuLoader);
            if (menuLoader == 0 || menuLoader < 0x100000000ULL) {
                // Create new object.
                static uint64_t s_mlBuf[64] = {};
                static uint64_t s_mlVtable[32] = {};
                for (int i = 0; i < 32; ++i) {
                    s_mlVtable[i] = reinterpret_cast<uint64_t>(&menuLoaderStub);
                }
                s_mlBuf[0] = reinterpret_cast<uint64_t>(s_mlVtable);
                menuLoader = reinterpret_cast<uint64_t>(s_mlBuf);
                *reinterpret_cast<volatile uintptr_t*>(globalVarAddr) = menuLoader;
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "create-menu-loader", 102, menuLoader);
            } else {
                // Object exists: ensure vtable is valid.
                volatile uint64_t* vtableSlot =
                    reinterpret_cast<volatile uint64_t*>(menuLoader);
                const uint64_t currentVtable = *vtableSlot;
                // WHICH implementation is in slot 21 decides everything once
                // the menu gate opens. Measured 2026-10-12: ml-ptr-full is
                // non-zero, so this branch runs, the guest's OWN vtable is
                // kept, and menuLoaderStub is never reached (create-menu-loader
                // and fix-menu-vtable both stay at 0 lines). With menugate=1
                // the guest therefore calls its real vtable[21] at 0x12af010
                // -- and that is what wedges on frame 0.
                //
                // Logging it is the only way to tell "the guest's own loader
                // hangs" from "ours was called and hung": both produce zero
                // menu-loader lines today.
                {
                    char diag[160];
                    if (currentVtable != 0 && currentVtable > 0x100000000ULL) {
                        auto** slots =
                            reinterpret_cast<void**>(currentVtable);
                        __builtin_snprintf(diag, sizeof(diag),
                            "menu-loader slot21=%p (guest own=%d, ours=%p)",
                            slots[21],
                            slots[21] == reinterpret_cast<void*>(&menuLoaderStub) ? 0 : 1,
                            reinterpret_cast<void*>(&menuLoaderStub));
                    } else {
                        __builtin_snprintf(diag, sizeof(diag),
                            "menu-loader vtable unusable (0x%llx)",
                            (unsigned long long)currentVtable);
                    }
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "menu-loader-slot21", 117, menuLoader, diag);
                }
                // "mlstub=1": take slot 21 even though the guest's own vtable is
                // legal. Measured 2026-10-12 (menu2): slot21 = 0x21064d21e8,
                // guest own = 1, ours = 0x805f7c40 -- and with the gate open the
                // guest's own implementation hangs on frame 0. Until now the
                // "only fix it when invalid" rule meant this stub was never
                // reachable in ANY run, which is why menu-loader logged zero
                // lines for a week and that zero meant "never called", not
                // "never triggered".
                if (gfxForceMenuLoaderStub() && currentVtable != 0 &&
                    currentVtable > 0x100000000ULL) {
                    auto** slots = reinterpret_cast<void**>(currentVtable);
                    const void* previous = slots[21];
                    slots[21] = reinterpret_cast<void*>(&menuLoaderStub);
                    char forced[144];
                    __builtin_snprintf(forced, sizeof(forced),
                        "menu-loader slot21 FORCED ours=%p (was %p)", slots[21],
                        previous);
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "force-menu-stub", 118, menuLoader, forced);
                }
                if (currentVtable == 0 || currentVtable < 0x100000000ULL) {
                    static uint64_t s_mlVtableFix[32] = {};
                    for (int i = 0; i < 32; ++i) {
                        s_mlVtableFix[i] = reinterpret_cast<uint64_t>(&menuLoaderStub);
                    }
                    *vtableSlot = reinterpret_cast<uint64_t>(s_mlVtableFix);
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "fix-menu-vtable", 116,
                        (static_cast<unsigned long long>(menuLoader & 0xFFFFFFFFULL) << 32) |
                        (currentVtable & 0xFFFFFFFFULL));
                }
            }
        }
        // NOP patch removed: svcSetMemoryPermission fails on guest .text
        // (different memory block). The menu-load check at 0xdacf40 is
        // left intact; the fake cloudcell/menu-loader objects should be
        // sufficient for the guest to proceed.
    }

    // The frame loop itself lives in frameLoopDriver() at the end of this file,
    // so the same body can run on the main thread (the default, and the
    // historical behaviour) or on a guest worker thread when
    // sdmc:/switch/rr3-gfx.conf says "render-thread=worker".
    FrameLoopContext loop{};
    loop.probe = &probe;
    loop.jniEnvironment = jniEnvironment;
    loop.activityObject = activityObject;
    loop.renderTarget = renderTarget;
    loop.display = display;
    loop.surface = surface;
    loop.context = context;
    loop.progressPath = progressPath;
    loop.frames = 0;
    loop.lastGlError = 0;
    loop.done.store(false, std::memory_order_release);
    loop.worker = gfxRenderThreadIsWorker();
    {
        char diag[192];
        // flr= is the switch value, because WITHOUT it this log cannot answer
        // the question it exists to answer. render-call-pre and render-call-post
        // are gated on straddleOpen alone -- not on RR3_FRAME_LOOP_RENDER -- so
        // both appear whether or not the guest render was ever invoked. An
        // archive with flr=0 and a pre/post pair says nothing; with flr=1 the
        // same pair means the call happened and returned. Verified 2026-10-11:
        // gui3 recorded pre@4902 and post@4918 with no guest call at all.
        __builtin_snprintf(diag, sizeof(diag),
                           "renderThread=%s requested=%d backend=%s"
                           " render=0x%llx padPumpOn=driver flr=%d",
                           gfxRenderThreadName(), loop.worker ? 1 : 0,
                           gfxBackendName(),
                           (unsigned long long)renderTarget,
                           RR3_FRAME_LOOP_RENDER);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "frame-loop-driver", 284,
                            loop.worker ? 1 : 0, diag);
    }
    if (loop.worker) {
        // Guest thread, not a bare host pthread: pthread_bridge's start routine
        // installs the GuestTlsScope and attaches the census entry, which is
        // what makes "the render thread" attributable in census-ord.
        GuestThread renderThread{};
        const int createResult = guestPthreadCreate(&renderThread, nullptr,
                                                    frameLoopDriver, &loop);
        char diag[160];
        __builtin_snprintf(diag, sizeof(diag),
                           "rc=%d handle=0x%llx creator=bridge self=0x%llx"
                           " mainThreadWill=wait",
                           createResult, (unsigned long long)renderThread,
                           (unsigned long long)guestPthreadSelf());
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "frame-loop-worker-create", 284,
                            static_cast<unsigned long long>(renderThread), diag);
        if (createResult == 0) {
            // The main thread parks here. It must not touch padUpdate, the
            // Cloudcell queue or onViewRenderJNI, or the experiment is void.
            while (!loop.done.load(std::memory_order_acquire)) {
                svcSleepThread(16666666LL); // ~60Hz
            }
        }
        else {
            // A worker that never started is not a result, it is a failed
            // experiment: fall back to the historical path and say so.
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "frame-loop-worker-fallback", 284,
                                static_cast<unsigned long long>(createResult),
                                "guestPthreadCreate failed; running on main");
            frameLoopDriver(&loop);
        }
    }
    else {
        frameLoopDriver(&loop);
    }
    const uint64_t frames = loop.frames;
    // Covers the PLUS exit and any other route out of the while condition; the
    // two break sites inside the driver have already logged with their own site
    // names, so a duplicate line here is the signal that the loop left via the
    // condition rather than a break.
    logFrameLoopExit(g_frameLoopExitReason, "loop-condition", frames, probe,
                     renderTarget);
    g_samplerPhase.store(kSamplerPhaseExited, std::memory_order_relaxed);
    probe.framesRendered = frames;
    writeGuestViewState(progressPath, renderTarget, 79);
    {
        const GuestActivitySnapshot snap = readGuestActivitySnapshot(renderTarget);
        const GuestViewGlStats stats = guestViewGlStats();
        probe.activityObject = snap.activity;
        probe.activityGlCreated = snap.glObjectsCreated;
        probe.activityLandscape = snap.isLandscape;
        probe.activityPaused = snap.paused;
        probe.activityFrameState = static_cast<uint32_t>(snap.frameState);
        probe.activitySplashFrames = static_cast<uint32_t>(snap.splashFrames);
        probe.activityWidth = snap.width;
        probe.activityHeight = snap.height;
        probe.glClears = stats.clears;
        probe.glDrawArrays = stats.drawArrays;
        probe.glDrawElements = stats.drawElements;
        probe.glTexImage = stats.texImage2D;
        probe.glCompressedTex = stats.compressedTexImage2D;
        probe.glBindFbo = stats.bindFramebuffer;
        probe.glUseProgram = stats.useProgram;
    }
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "frame-loop-exit", 71, frames);
    }

    // Guest workers still execute guest code after PLUS. Destroying the EGL
    // context they use races and was the round-9 leave-app error path.
    const unsigned int liveThreads = guestPthreadLiveCount();
    if (liveThreads != 0) {
        if (progressPath != nullptr) {
            writeGuestBootProgress(progressPath, "view-egl-keep-workers", 75,
                                   liveThreads);
        }
        if (progressPath != nullptr) {
            writeGuestBootProgress(
                progressPath, probe.swapped ? "guest-view-complete"
                                            : "guest-view-swap-failed",
                3, static_cast<unsigned long long>(renderTarget));
        }
        return probe;
    }

    writeGuestBootProgress(progressPath, "view-egl-release-enter", 51, 0);
    releaseEgl(display, surface, context);
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, probe.swapped ? "guest-view-complete" : "guest-view-swap-failed",
            3, static_cast<unsigned long long>(renderTarget));
    }
    return probe;
}

namespace {

// The frame loop, verbatim, on whichever thread the dispatch above chose.
//
// Two things are thread-dependent and are handled here rather than at the call
// site, so the body itself stays identical in both modes:
//
//   1. The PC sampler. startGuestPcSampler() targets threadGetCurHandle(), i.e.
//      whatever thread calls it, so starting it HERE keeps it pointed at the
//      thread that is actually inside onViewRenderJNI. Started on the main
//      thread while a worker renders, it would sample a parked main thread and
//      report the sentinel, which would read as a sampler failure.
//
//   2. The EGL current context. The null backend keeps s_currentContext in
//      process globals, so eglGetCurrentContext() already answers non-null from
//      any thread, but a real backend is per-thread, so the worker re-asserts
//      the current context for itself before the first guest call.
void* frameLoopDriver(void* opaque) {
    FrameLoopContext* context = static_cast<FrameLoopContext*>(opaque);
    GuestViewProbe& probe = *context->probe;
    void* jniEnvironment = context->jniEnvironment;
    void* activityObject = context->activityObject;
    const uintptr_t renderTarget = context->renderTarget;
    EGLDisplay display = context->display;
    EGLSurface surface = context->surface;
    const char* progressPath = context->progressPath;
    uint64_t& frames = context->frames;
    uint32_t& lastGlError = context->lastGlError;
    PadState& framePad = context->pad;
    padInitializeDefault(&framePad);

    {
        char diag[160];
        const uint32_t threadHandle =
            static_cast<uint32_t>(threadGetCurHandle());
        const EGLBoolean current = eglMakeCurrent(
            display, surface, surface, context->context);
        __builtin_snprintf(diag, sizeof(diag),
                           "thread=0x%08x worker=%d makeCurrent=%d"
                           " currentCtx=%llx padInit=%d",
                           threadHandle, context->worker ? 1 : 0,
                           current ? 1 : 0,
                           (unsigned long long)eglGetCurrentContext(),
                           1);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "frame-loop-driver-enter", 284,
                            threadHandle, diag);
    }

    // Diagnostic only: samples the rendering thread's PC from a host thread so
    // the frozen frame-2 call can be attributed to a guest address. Stopped on
    // every exit path below, immediately after the loop.
    //
    // WIDENED 2026-10-04, and the reason matters more than the change. The
    // archived samples all read `phase=loop-host`, never `in-guest`:
    //
    //   phase=sample-raw i=1..7 phase=loop-host pc=0x8128a698 img=-1 elfva=0x0
    //
    // kSamplerPhaseInGuest is published immediately before onViewRenderJNI and
    // kSamplerPhasePostGuest immediately after it returns, so `loop-host` is
    // only reachable while this thread is UPSTREAM of the render call --
    // inside padUpdate, pumpGuestControllerInput, or cloudcell_pump.
    //
    // DO NOT read that as "the render call is never reached". It is a sampling
    // artifact: the sampler is a host thread competing with a main thread
    // burning a whole core, and it only got 8 samples where 75 ms over 75 s
    // should have produced ~1000. kDiagnosticThreadPriority is already the one
    // preemptive value libnx documents (0x3B) and raising it is not available,
    // because that is a Horizon-hardware semantic that Eden does not honour.
    // Every sample therefore predates the call; the sampler is then starved and
    // never observes the region that matters. Absence of samples here is
    // absence of CPU, not absence of execution.
    //
    // What the same run does prove, from the straddle latch rather than the
    // sampler: render-call-pre is emitted under `!g_renderStraddleEmitted
    // .exchange(true)`, i.e. on the FIRST call only, and render-call-post is the
    // very next statement after that call. Pre present + post absent therefore
    // means the first -- and so far only -- onViewRenderJNI never returned.
    // frames is incremented after the call, so the frame-loop counter is 0.
    // `gfx-stats frames=1` is the graphics layer's own tally, NOT this loop's
    // counter; reading it as "frame 1 completed" is what sent this file down a
    // two-hour wrong path in the first place.
    startGuestPcSampler();
    g_samplerPhase.store(kSamplerPhaseFrameLoopHost, std::memory_order_relaxed);
    runSamplerSelfTest();
    while (probe.frameLoopActive) {
        // Placed BEFORE padUpdate, and that placement is the measurement.
        // Archive 20261004-060230-wedge3 (guard widened to frames < 3) produced
        // loop-top / after-pad-pump / after-cloudcell-pump exactly once each --
        // i.e. only for frame 1. Frame 2 satisfies frames < 3 just as frame 1
        // does, so if frame 2 emits nothing from any of the three steps, it
        // never reached the first of them, and the only statement between the
        // top of the loop body and that point is padUpdate. The emulator
        // services input over UDP (its log opens "UDP input server on
        // 127.0.0.1:26760" at startup), so a blocking padUpdate is a live
        // candidate for the whole wedge and this breadcrumb is what separates
        // it from the other two steps.
        if (frames < 3) {
            char pd[48];
            __builtin_snprintf(pd, sizeof(pd),
                              "wedge: step=before-pad-update f=%llu",
                              static_cast<unsigned long long>(frames));
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log", pd, 279, 0);
        }
        // Does the Cloudcell cell ever become non-zero, and when?
        //
        // cc-ptr-diag reads it ONCE, early, before the guest has had a chance to
        // publish, so a null there says nothing about the rest of the run. The
        // guest's constructor (0x1cbe028) writes the cell at 0x1cbe094, and
        // reverse-engineering says the only route to it is a virtual call on a
        // CloudcellService instance the guest does construct -- so the open
        // question is purely WHEN that call happens, if ever. Polling is the
        // cheapest way to answer it: one 4-secondly line, and the first
        // transition to non-zero names the frame in which it happened.
        //
        // Only meaningful when the host is not substituting its own object
        // (ccsynth=0, the default); with ccsynth=1 the cell holds s_ccBuf and
        // the line just confirms that.
        if ((frames % 240) == 0) {
            const uintptr_t pollGot = renderTarget + 0x00CB9FBCULL;
            const uintptr_t pollGlobal =
                *reinterpret_cast<const uintptr_t*>(pollGot);
            const uintptr_t pollCell =
                pollGlobal != 0
                    ? *reinterpret_cast<const uintptr_t*>(pollGlobal)
                    : 0;
            // CORRECTION 2026-10-17. This poll used to read GOT 0x1f6d5f50 and
            // reported 0 for 136 consecutive polls, which read as "term 1 is
            // false". It was meaningless: 0x1f6d5f50 is ~494 MB ABOVE the guest
            // image's upper bound 0x1fc7a99, so the poll was reading unmapped
            // memory. The real slot is 0x1f6df50, which is adrp #0x1f6d000 +
            // #0xf50 in the gate at 0x12aefb8, i.e. 0x980 above Cloudcell's
            // 0x1f6d5d0, not 0x180.
            //
            // 0x00CBA93C is not new arithmetic: it is kMenuLoaderDelta, which
            // this file has carried since 2026-10-12 with the comment "Menu
            // loader: GOT entry at ELF 0x1f6df50". The address was transcribed
            // wrong once and the wrong value was never range-checked.
            //
            // 0x00CBA93C is not new arithmetic: it is kMenuLoaderDelta, which
            // this file has carried since 2026-10-12 with the comment "Menu
            // loader: GOT entry at ELF 0x1f6df50". The address was transcribed
            // wrong once and the wrong value was never range-checked. (That
            // constant is declared inside the Cloudcell-install block below,
            // out of scope here, so it is spelled out again.)
            // Deltas, computed rather than done by hand. renderTarget is
            // base + 0x12B3614, so a guest vaddr's delta is vaddr - 0x12B3614:
            //
            //   0x00CB9FBC -> 0x1F6D5D0   Cloudcell cell  (0x12aecc8)
            //   0x00CBA93C -> 0x1F6DF50   term-1 cell     (0x12aefb8)
            //   0x00CBAF2C -> 0x1F6E540   onViewCreatedJNI singleton (0x12b35e8)
            //
            // 0x00CBA93C is also kMenuLoaderDelta, which this file has carried
            // since 2026-10-12 with the comment "Menu loader: GOT entry at ELF
            // 0x1f6df50" -- that comment was right and the poll's transcription
            // of it was not.
            //
            // NO RANGE CHECK AT RUNTIME, ON ANYTHING.
            //
            // A previous revision computed base = renderTarget - 0x12B3614 and
            // tested it against [0x100000, 0x1FC7A99]. That is the same mistake
            // one level down: `base` is itself a RUNTIME address
            // (0x2105xxxxxx), so the test could never pass, baseOk was
            // permanently false, and every GOT read behind it was reported as
            // 0x0 BY CONSTRUCTION. That is the true source of the last several
            // rounds of "singleton = 0x0" and "t1 = 0x0" -- not the guest.
            //
            // Nothing in this program is an ELF vaddr at run time, so there is
            // nothing here to check against ELF bounds. renderTarget + delta is
            // simply an address; if it is readable it is readable. After a
            // dereference the only defensible claim is non-zero.
            auto deref = [](uintptr_t p) -> uintptr_t {
                return p != 0 ? *reinterpret_cast<const uintptr_t*>(p) : 0;
            };
            const uintptr_t t1Global = deref(renderTarget + 0x00CBA93CULL);
            const uintptr_t t1Obj = deref(t1Global);
            const uintptr_t singletonCell =
                deref(renderTarget + 0x00CBAF2CULL);
            const uintptr_t singleton = deref(singletonCell);
            const uintptr_t sub280 =
                pollCell != 0
                    ? *reinterpret_cast<const uintptr_t*>(pollCell + 0x280)
                    : 0;
            // THE GATE'S OWN PRECONDITION.
            //
            // onViewCreatedJNI does:
            //   0x12b35e4  adrp x8, #0x1f6e000
            //   0x12b35e8  ldr  x8, [x8, #0x540]      ; GOT 0x1f6e540
            //   0x12b35ec  ldr  x0, [x8]
            //   0x12b35f0  cbz  x0, #0x12b35f8        ; NULL -> silent return
            //   0x12b35f4  b    #0x12ad504            ; else build term-1
            //
            // Calling onCreateJNI first did NOT make this non-null (cclife1:
            // onCreateJNI-pre/ret both logged, t1 still 0x0 across the run), so
            // either that export is not the producer or it needs something the
            // host has still not run. The host runs JNI_OnLoad (main.cpp:1239)
            // and initializeGuestMainConstructors (main.cpp:1269) AFTER
            // runGuestViewProbe (main.cpp:1176), so both are candidates for
            // whatever has to precede it. Read it rather than keep guessing.
            //
            // CORRECTED, and the correction is arithmetic-only. renderTarget is
            // base + 0x12B3614, so the delta for any guest vaddr is
            // vaddr - 0x12B3614. Computed rather than done by hand, because
            // doing it by hand is how this poll ended up reading 0x1F6E590
            // instead of 0x1F6E540 -- a 0x50 slip that stayed inside the image
            // and so was not caught by any range check:
            //
            //   0x00CB9FBC -> 0x1F6D5D0   Cloudcell cell  (0x12aecc8)  ok
            //   0x00CBA93C -> 0x1F6DF50   term-1 cell     (0x12aefb8)  ok
            //   0x00CBAF7C -> 0x1F6E590   WRONG by 0x50
            //   0x00CBAF2C -> 0x1F6E540   the onViewCreatedJNI singleton
            //
            // This is the one number that separates the two readings still
            // open for term 1: "the singleton was null so the builder never
            // ran" and "the singleton was fine so the builder ran but the
            // object did not appear". Both produce t1 = 0x0.
            //
            // LOG EVERY LEVEL OF BOTH CHAINS. A two-step dereference printed
            // only its final result, so "the cell is 0x0" could not say WHICH
            // level was zero -- the GOT slot, the cell, or the object. That is
            // exactly where a silent failure hides: the poll looked
            // trustworthy and still could not answer the question it was
            // written for. Three numbers per chain, no extra dereference of
            // anything not already read.
            const uintptr_t gotValue =
                deref(renderTarget + 0x00CBAF2CULL);
            const uintptr_t t1GotValue = deref(renderTarget + 0x00CBA93CULL);
            // THE NUMBER THAT CLOSES THE LOOP.
            //
            // S is the mtGLWrapper singleton, reached through GOT 0x1f6f970
            // (addend 0x1f97c88). S->0xc is the OpenGL ES MAJOR VERSION, written
            // once by 0x128a17c from glGetString(GL_VERSION) + sscanf("%d.%d").
            // The assert guard at 0x14851e8 silently returns when S->0xc < 3 and
            // asserts otherwise.
            //
            // ccdeliver13 proves the early context now answers
            // "OpenGL ES 3.2", so if S->0xc were 3 the run would die inside
            // bionicAssert2 ([[noreturn]], std::abort()). It did not die and it
            // logged no assert -- so S->0xc is evidently still below 3. Read it.
            // S->0x6c comes along because it is the guard's second term and
            // nothing in .text writes it.
            //
            // Same shape as the chains above, no guard of its own, and the delta
            // is computed rather than transcribed:
            //   0x1f6f970 - 0x1f6d5d0 = 0x23a0, so 0x00CB9FBC + 0x23a0
            const uintptr_t sObj =
                deref(deref(renderTarget + 0x00CBC35CULL));
            const unsigned glMajor =
                sObj != 0 ? *reinterpret_cast<const unsigned char*>(sObj + 0x0c) : 0;
            const unsigned glMinor =
                sObj != 0 ? *reinterpret_cast<const unsigned char*>(sObj + 0x10) : 0;
            const unsigned glFlag6c =
                sObj != 0 ? *reinterpret_cast<const unsigned char*>(sObj + 0x6c) : 0;
            char cc[440];
            __builtin_snprintf(
                cc, sizeof(cc),
                "cc-cell-poll: f=%llu ss_got=0x%x ss_cell=0x%x ss_obj=%p"
                " t1_got=0x%x t1_cell=0x%x t1_obj=%p cc1=%s sub280=%p tag=%d"
                " synth=%d GLVER=%u.%u S6c=%u Sobj=%p",
                static_cast<unsigned long long>(frames),
                (unsigned)gotValue,
                (unsigned)singletonCell,
                reinterpret_cast<void*>(singleton),
                (unsigned)t1GotValue,
                (unsigned)t1Global,
                reinterpret_cast<void*>(t1Obj),
                (pollCell != 0 &&
                 *reinterpret_cast<const uint8_t*>(pollCell + 0x4C1) != 0)
                    ? "1" : "0",
                reinterpret_cast<void*>(sub280),
                static_cast<int>(sub280 & 1ULL),
                gfxCloudcellSynthetic() ? 1 : 0,
                glMajor, glMinor, glFlag6c,
                reinterpret_cast<void*>(sObj));
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                cc, 74, 0);
            // The menu gate at 0x12aefc8 is a four-way AND whose terms are, in
            // order, [x23]!=0, [*ccGlobal]!=0, cc->[0x4c1]!=0, and
            // (vtable[17]() & 1)==0. The fourth is NOT a bool-returning method:
            // vtable slot 17 is 0x1cbfad4, which is `ldr x0,[x0,#0x280] ; ret`
            // -- an accessor returning the sub-object at Cloudcell+0x280, and
            // `& 1` is a LOW-BIT TAG test on that pointer. So the fourth term
            // asks whether that sub-object is present, and the host can read
            // the word directly instead of calling guest code across a thread
            // boundary -- which is also the only way to look at it that does
            // not do the very thing the gate exists to police.
            //
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                cc, 74, 0);
        }
        padUpdate(&framePad);
        // --- wedge breadcrumbs -------------------------------------------
        // The progress marker truncates, and frame-loop-heartbeat (below,
        // ordinal 74) fires on iteration 0. If that heartbeat is missing the
        // loop died somewhere in iteration 0, so log the step we are ABOUT to
        // enter: the last line present names the wedge point. Bounded to the
        // first two iterations and the flags are already in the snapshot, so
        // this adds 8 lines for the whole run and no locking on the hot path.
        if (frames < 3) {
            const GuestActivitySnapshot bs =
                readGuestActivitySnapshot(renderTarget);
            char bdiag[320];
            __builtin_snprintf(
                bdiag, sizeof(bdiag),
                "wedge: step=%s f=%llu a4=%u a5=%u a6=%u surf=%dx%d fstate=%d"
                " contA=%u contR=%u splash=%u"
                " GATE a8=%d ac=%d b0=%d b4=%d d1=%u d4=%d",
                "loop-top", static_cast<unsigned long long>(frames),
                bs.glObjectsCreated, bs.landscapeApplied, bs.isLandscape,
                bs.width, bs.height, bs.frameState, bs.containerActive,
                bs.containerReady, bs.splashExit,
                // The gate is the five words render-head.py prints as hasFocus /
                // frameCounter. bl 0x12ae290 is reached only when
                // (a8!=b0 || ac!=b4) && d1!=0; otherwise the guest clears d4.
                bs.width, bs.height, bs.width2, bs.height2,
                bs.flag4d1, bs.frameCounter);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log", bdiag, 280, frames);
        }
        // A/B, added 2026-10-04. RESULT: NULL. Reverted to 1.
        //
        // Acceptance criteria were written down BEFORE the run, and none of the
        // three moved (archive 20261004-133108-pumpoff, 150 s, versus
        // 20261004-094723-nettoffline):
        //   render-call-post   0 -> 0
        //   mmap ok / fail     126 / 0  ->  126 / 0
        //   glShaderSource     92      ->  92
        //   error dialog       0       ->  0
        //   unmapped reads  510094    ->  510234   (noise)
        // The guest log grew from 354 KB to 520 KB during that run, which looks
        // like progress and is not: the extra lines are census-reg-class (170),
        // census-deref (99) and census-ord (160) samples taken while the same
        // fault loop runs. Log volume is not a progress metric here.
        //
        // A third reading, from the logs rather than another run, closes the
        // window completely. Between the call that returns (view-call-render
        // 265 -> view-render-ret 266) and the one that does not (render-call-pre
        // 285), the frame loop runs exactly three host actions:
        //   1. this pump            -- ruled out by the A/B above
        //   2. cloudcell_pump("frame") -- NEVER FIRED. Its log line is emitted
        //      only when the queue is non-empty, and the archived run has no
        //      site=frame line at all, only the JNI view path's before/after-
        //      created, -changed and -render sites. It was a no-op.
        //   3. padUpdate            -- completed: the wedge probes go
        //      before-pad-update (279) -> loop-top (280), so the call returned.
        // All three are no-ops, so the hang is in the guest's own onViewRenderJNI
        // when it is re-entered from this thread. That is the frame-loop draw
        // call, which is what RR3_FRAME_LOOP_RENDER controls.
#ifndef RR3_FRAME_LOOP_PUMP
#define RR3_FRAME_LOOP_PUMP 1
#endif
        if (RR3_FRAME_LOOP_PUMP) {
            pumpGuestControllerInput(jniEnvironment, activityObject,
                                     reinterpret_cast<GuestPadHandle*>(&framePad), 1);
        }
        if (frames < 3) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "wedge: step=after-pad-pump f=0", 281, 0);
        }

        // Where cloudcell_defer.h says the drain belongs: immediately before
        // onViewRenderJNI, outside every Cloudcell lock, early enough that the
        // same frame's CC_HttpRequestManager_Class::Update() (0x93ae04) sees
        // the completion. Logs only when the queue was non-empty.
        cloudcell_pump("frame", false);
        // Pump the captured Cloudcell continuation, if any. Same placement and
        // same shape as cloudcell_pump above, and for the same reason: on a real
        // device the response arrives on a JNI callback (the real Cloudcell
        // init at 0xbc2dec starts no thread of its own), so draining it from the
        // frame loop is what the host does, not a shortcut.
        //
        // NOT where this can first happen, though: the guest blocks inside the
        // renderTarget(...) call lower down in this same frame body, so this site
        // is unreachable for the run "ccmode" exists to fix. It stays, labelled
        // "frame" in the log, because it is the delivery site that runs when the
        // frame loop is alive -- and because with "ccmode" set, this is now a
        // SECOND chance rather than the only one.
        if (gfxCloudcellCompleteRequests()) {
            cloudcellPumpCompletion("frame");
        }
        if (frames < 3) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "wedge: step=after-cloudcell-pump f=0", 282, 0);
        }

        // Publishes the control window for the PC sampler: between the store
        // below and the store after the call, this thread is provably executing
        // onViewRenderJNI, so its sampled PC must be guest-resident.
        g_samplerPhase.store(kSamplerPhaseInGuest, std::memory_order_relaxed);

        // ---------------------------------------------------------------------
        // EXPERIMENT RAN, ANSWERED, AND THE ANALYSIS IT WAS TESTING IS WRONG.
        // Kept at 0 so nobody re-runs it by accident. Full record, because the
        // negative result is the valuable part.
        //
        // Archive 20261004-055738-forcetier, backend=null, 90 s:
        //     render-call-pre ... t4fc=4 p500=420 b4a4=1 b4a6=1 b50c=1 b50d=0
        //     frames=1  calls=415  draws=1/0
        //     no render-call-post
        //     cpu 14.42 / 29.81 / 45.09 / 60.17   (still +1.5 s per second)
        //
        // The write landed: the guest read back 4, not 5. And the render call
        // still never returned and frames never left 1. So the loop-exit story
        // built on `cmp w9, #4 / b.ne` at 0x12af190 is NOT what holds the frame
        // loop, and [rend+0x4fc] being 5 instead of 4 is a real but non-blocking
        // difference. Five inputs were refuted before this one (GL model with
        // p500 as an internal control, 22 android.os.Build fields, the
        // AndroidHigh.plist open, b50c proving the probe completed, and an
        // in-place patch of core.romfs's onlyWIFI XML) and a sixth was thrown out
        // as invalid rather than counted, because the path mapping never let it
        // reach the guest.
        //
        // The address cited in the earlier probe comment was itself off by one
        // instruction: 0x12af188 is `cmp w8, #0x12` (the [rend+0x4a0] cursor
        // compare); the id compare is 0x12af190. Same branch either way, but the
        // citation was wrong and is now correct.
        //
        // What this leaves: the 8 back edges into 0x12aebac were never all
        // accounted for. Only the 0x12af190 arm was tested, and it is not the
        // gate. The remaining candidates are the other arms of that same loop,
        // or a wait inside the loop body that no [rend+offset] probe can see.
        // ---------------------------------------------------------------------
        constexpr int32_t kForceTier = 0;
        if (kForceTier != 0) {
            const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(
                renderTarget + kRenderArgSingletonDeltaFromRenderExport);
            if (holder != 0) {
                const uintptr_t obj =
                    *reinterpret_cast<const uintptr_t*>(holder);
                if (obj != 0) {
                    *reinterpret_cast<volatile int32_t*>(obj + 0x4fc) = kForceTier;
                }
            }
        }
        // --- render-call straddle (instrumentation only) ------------------
        // The `after-render-jni` breadcrumb below has never appeared, so this
        // pair is the one that has to be unambiguous: the pre line proves the
        // call was reached and records what the guest was handed, the post line
        // is the very next statement after the call and can only exist if the
        // call RETURNED. Pre present + post absent is therefore the wedge, with
        // nothing in between that can fail silently -- appendGuestBootLine
        // fflushes every line (recovery_guard.cpp) and never fsyncs, and the two
        // wedge lines above came out of this same thread and the same cached
        // handle. Exactly two lines for the whole run, latched on the FIRST
        // call rather than on `frames < 2`: the wedge has been the first call in
        // every archived run, and a frames-based gate would have emitted four
        // lines the first time a call ever did return.
        bool straddleOpen =
            !g_renderStraddleEmitted.exchange(true, std::memory_order_relaxed);
        if (straddleOpen) {
            // 768, not the 512 the other lines here use: this one carries 27
            // fields and a truncated tail would drop exactly the fields a
            // reader came for. The two buffers are in sibling scopes, so only
            // one is ever live.
            char diag[768];
            formatRenderCallStraddle(diag, sizeof(diag), "pre", renderTarget,
                                     jniEnvironment, activityObject, 2, 0);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "render-call-pre", 285,
                static_cast<unsigned long long>(threadGetCurHandle()), diag);
        }
        // A/B SWITCH, added 2026-10-04. DEFAULT IS OFF: do NOT re-enter the
        // guest render entry from the frame loop.
        //
        // The guest entry, called with the identical argument list
        // (env, activity, 2, 0), RETURNS on the first call -- the JNI View
        // render at ordinal 265, with `view-render-ret` at 266 -- and does not
        // return on the second, the one made from here at ordinal 285. The
        // whole View lifecycle around it is delivered and every callback
        // returns: created, changed, focus-ret, changed-ret, render-ret.
        //
        // So the guest is perfectly able to render and come back. What the
        // frame loop adds is a second entry into a guest that is already
        // mid-frame, and that is the one that never returns. The guest drives
        // its own rendering through the View callbacks; re-entering the draw
        // entry every tick is this probe's invention, not the game's contract.
        //
        // With it off, the loop still pumps the controller, still drains the
        // Cloudcell queue and still sleeps at 60 Hz -- it just stops poking the
        // draw entry. If frames climbs on its own, the guest was always able
        // to run and the wedge was ours.
        // A/B, added 2026-10-04. RESULT: P1 MET, P2 FAILED, and the change is
        // KEPT because P1 is decisive and the thing it removes is ours.
        //   P1  unmapped accesses: 510,234 reads / 436,526 writes
        //       -> 15 reads / 0 writes.                  PASS  (criterion <1000)
        //   P2  gfx-proc above 32: stayed at exactly 32.  FAIL
        //   P3  log still growing: yes, but the growth is census samples
        //       (census-ord 178 vs 5), not progress.        inconclusive
        // Plus, not in the original criteria:
        //   Eden log lines  948,823 -> 2,465      (388x smaller)
        //   CPU at t=90s       ~135 s -> 54.1 s  (-60%)
        //   render-call-post      0 -> 1         (the call now returns)
        //   guest-activity probe  0 -> 272, all 272 snapshots IDENTICAL
        //       (same eng, same engine pointer, same 1280x720, same cq1/cq2)
        // Archive 20261004-133537-framelooprender-off.
        //
        // So: the wedge was ours, it is gone, and the guest is now alive and
        // polling instead of faulting. What it is NOT doing is producing new
        // rendering -- gfx-proc is pinned at 32 and the activity snapshot never
        // changes.
        //
        // RETRACTED, later the same evening. "On Android the guest renders
        // because its View invalidates itself and the Choreographer posts a
        // frame callback" is a familiar architecture story, and it was never
        // measured against THIS guest. A scan of the image for Looper,
        // Handler, Choreographer, MessageQueue, SurfaceHolder, GLSurfaceView,
        // onDrawFrame and postDelayed returns ZERO hits for every one. There is
        // no Choreographer here to supply, so "add a View-driven draw loop" was
        // never a well-formed task -- it named a mechanism the guest does not
        // mention. What is actually true is only the first sentence: the guest
        // is alive and not producing new rendering, and the reason is unknown.
        // The same unmeasured-architecture move produced the content-root
        // reading, the frame-counter reading and the empty-stub reading earlier
        // the same day; three of four, all measured, all wrong.
        //
        // A/B 2026-10-04 afternoon. RESULT: NULL, and it is the useful kind --
        // it eliminates a whole hypothesis. Archive 20261004-134140-renderthread-worker.
        //
        //   P4  renderThread=worker requested=1   PASS (the experiment was real)
        //   P1  unmapped < 1,000: 944,979         FAIL
        //   P3  render-call-post == 1: post=0     FAIL
        //   P2  gfx-proc above 32: stayed 32      MEASUREMENT INVALID, see below
        //
        // So thread placement is NOT the cause. The call faults just as hard from
        // a guest worker thread with a real Android TLS (944,979) as it does from
        // the host main thread (510,234). That kills "the main thread is the
        // problem" and leaves "the call itself is bad from the second call on".
        //
        // P2 was my error and it is worth recording. `gfx-proc` was assumed to
        // count drawing. It does not: its 32 entries are ALL EGL/GL
        // initialisation queries (eglGetPlatformDisplayEXT,
        // glDebugMessageControlKHR, ...) with no draw and no swap among them, so
        // it could never have moved. The real drawing counters live in the
        // guest-activity snapshot as `gl=clears/drawArrays/drawElements/...`,
        // and there they read 1/3/0/635/191/2 -- the guest DID issue three
        // drawArrays and then stopped. A criterion built on the wrong counter
        // is worse than no criterion, because it reads as a null result.
        //
        // Current state, with this call off: 15 unmapped accesses, CPU -60%,
        // render-call-post=1, frame loop iterating, no deadlock (held_now=0,
        // parked_in_mutex_lock=0), guest alive and healthy -- and frozen on the
        // EA splash with splashExit=0 and three draw calls issued. It has
        // rendered its first frame and is waiting for whatever advances it.
        //
        // CORRECTION 2026-10-04, after archive 20261004-150736-framecap1 refuted
        // a frame-cap idea. The premise was wrong and it is worth stating
        // plainly: the frame loop's FIRST call is the one that never returns,
        // not a second one. `render-call-pre` is emitted under a latch so it
        // only ever appears once, which made this easy to misread as "the first
        // worked".
        //
        // What the evidence actually says:
        //   * the JNI view path's call at ordinal 265 RETURNS and is the one
        //     that issues the three drawArrays -- the EA splash. With this
        //     switch OFF the guest still shows clears=1 drawArrays=3, because
        //     that drawing happens on the JNI call, not here.
        //   * the frame loop's first call at ordinal 285 never returns, on the
        //     main thread or on a worker thread with a real Android TLS.
        // So a cap offers no useful configuration: 0 means "never call" (clean)
        // and 1 means "call the one that hangs".
        //
        // ---- 2026-10-11: re-tested and REFUTED, back to 0 (gui4) ----
        // The lines above are the 2026-10-04 record and stay as-is. The switch
        // was set to 1 on the theory that the 686 wall was what made re-entry
        // fatal. gui4 (archive 20261011-121916-gui4) reproduced the wedge at
        // once -- frames 24000 -> 10, threads 6 -> 1, guestLines 9036 -> 1511,
        // reaper asset PRESENT -> NULL-FIRST-WORD, main thread idle_ms=410806 --
        // so CloudCell initialising is not what unblocks re-entry.
        //
        // The trap worth recording: a ONE-OFF re-entry at frame 30 returns
        // (gui3 L4972 invoking, L5615 RETURNED) while per-frame re-entry from
        // ~frame 10 destroys the guest's threads. The frame-30 result does not
        // generalise to the loop, and reading it as if it did cost a run.
        //
        // Discriminator when this is ever re-opened, read draws ONLY -- calls/
        // frame is the host's own glGetError and says nothing about the guest:
        //   draws climbs       -> the guest renders once it is asked
        //   renders=6/0 again  -> still never asked
        //
        // CAVEAT on the straddle, found by review 2026-10-11: "pre present +
        // post absent" is NOT a unique signature of the wedge. Both lines are
        // gated on straddleOpen alone, never on this switch, so with the switch
        // at 0 they appear with no guest call at all (gui3 recorded both,
        // pre@4902 post@4918). The frame-30 call is not straddle-covered either:
        // g_renderStraddleEmitted is a one-shot latch that closes on frame 0.
        // Always confirm flr=0/1 in the frame-loop-driver line first, and treat a
        // missing post as ambiguous rather than conclusive.
        // The raise site of the fatal dialog, located 2026-10-04 by
        // tools/guest-analysis/xref-str.py, and it is a NULL, not a save:
        //
        //   0xcd1474  adrp x28, #0x1f6d000
        //   0xcd1478  ldr  x28, [x28, #0x5d0]     ; x28 = *(0x1f6d5d0)
        //   ...
        //   0xcd16fc  ldr  x0, [x28]
        //   0xcd1700  cbz  x0, #0xcd35c0          ; NULL -> raise
        //   0xcd35c0  adrp x0, #0x3a8000
        //   0xcd35c4  add  x0, x0, #0x462          ; GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE
        //   0xcd35c8  bl  #0x13c3fc4               ; localize()
        //   0xcd35d0  ldr  x8, [x8, #0x5d8]        ; the alert object
        //
        // FDE [0xcd1014, 0xcd3648), 2445 insns. 0x1f6d5d8 is the neighbouring
        // global the render entry's fsm==4 path uses (0x12aeb5c), so this whole
        // family is the data/asset manager, which the localization function's
        // own key list confirms -- its neighbours are GAMETEXT_NO_WIFI,
        // GAMETEXT_FREE_STORAGE_SPACE, GAMETEXT_ASSET_DOWNLOAD.
        //
        // "Never constructed" and "constructed, first word null" are different
        // bugs with different fixes, and nothing logged so far tells them
        // apart, so both are read here. The bias is derived from renderTarget's
        // own known ELF VA rather than hard-coded, so it cannot drift.
        // Sampled at three times on purpose. The content-root global is the
        // thing the guest's own resolver gates on, and the obvious question --
        // "is it empty because nothing ever sets it, or because it has not been
        // set YET" -- is answered by reading it again later instead of by
        // writing to guest heap memory and finding out the hard way. If it
        // fills in on its own, there is nothing to fix here.
        const bool sampleContentRoot =
            frames < 2 || frames == 10 || frames == 60;
        if (sampleContentRoot) {
            static constexpr uint64_t kAssetSingletonElfVa = 0x1F6D5D0ULL;
            static constexpr uint64_t kRenderExportElfVa = 0x12B3614ULL;
            // The guest's content-root global, read by its own path resolver at
            // 0x12bf4dc as `*(0x1f78d30)`. See the long note at the read below.
            static constexpr uint64_t kContentRootElfVa = 0x1F78D30ULL;
            // The engine singleton and the only two fields the guest consults
            // before deciding it is mid-download. Disassembled at 0x12ad2e0
            // (FDE [0x12ad2e0, 0x12ad3b0)), the function that logs
            // "startMusic disabled during download" (string at 0x3902ff):
            //
            //   adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5d8]   engine = *(0x1f6d5d8)
            //   ldr  x8,[x8] ; cbz x8, log                   null        -> downloading
            //   ldr  x8,[x8,#0x10518] ; cbz x8, log         null        -> downloading
            //   ldp  x8,x9,[x8,#0xd8] ; cmp x9,x8 ; b.eq skip
            //                                                  unequal    -> downloading
            //
            // The last pair is a container's begin/end, so "unequal" reads as
            // "not empty". All three are read here so the next run names WHICH
            // one is true instead of leaving three candidates standing.
            static constexpr uint64_t kEngineSingletonElfVa = 0x1F6D5D8ULL;
            static constexpr uint64_t kEngineDownloadVecOff = 0x10518ULL;
            static constexpr uint64_t kContainerPairOff = 0xD8ULL;
            // 512, was 384: the DL neighbourhood alone adds ~130 chars, and
            // snprintf truncates the tail silently -- which is exactly where the
            // branch verdict now lives.
            char adiag[512];
            const uintptr_t renderRuntime = renderTarget;
            if (renderRuntime != 0) {
                const uint64_t bias =
                    static_cast<uint64_t>(renderRuntime) - kRenderExportElfVa;
                const uintptr_t slot = static_cast<uintptr_t>(bias + kAssetSingletonElfVa);
                const uintptr_t obj =
                    *reinterpret_cast<const volatile uintptr_t*>(slot);
                const uintptr_t first = (obj != 0)
                    ? *reinterpret_cast<const volatile uintptr_t*>(obj) : 0;
                // +0x8d is the byte the documented freeze turns on. The chain, as
                // recorded in jni_compat/classes/rr3_Platform.cpp's header:
                //
                //   AssetDownloadService::OnUpdate (0x3276b8)
                //     first instruction:  if (am[0x8d]) return;      (0x3276e0)
                //     ShowNoWifiMessage (0x323534) writes am[0x8d] = 1
                //     and the only writers back to 0 are CancelAllDownloads
                //     (0x31ffe0), ContinueDownloadingVia3G (0x31ff1c) and
                //     OnResume -- i.e. the dialog's own buttons.
                //
                // Read here because the guest logs "startMusic disabled during
                // download" while no NoWifi dialog is ever raised, so "is the
                // freeze latched" is not answerable from the alert log. The
                // neighbouring 8 bytes of u64 fields are read too: 0x88/0x90 are
                // the most likely homes for a queued-download size, and one line
                // is cheaper than a second probe.
                unsigned freeze = 0xFFFFFFFFu;
                unsigned long long q88 = 0, q90 = 0, q98 = 0;
                if (obj != 0) {
                    const auto* base =
                        reinterpret_cast<const volatile unsigned char*>(obj);
                    freeze = base[0x8d];
                    q88 = *reinterpret_cast<const volatile unsigned long long*>(base + 0x88);
                    q90 = *reinterpret_cast<const volatile unsigned long long*>(base + 0x90);
                    q98 = *reinterpret_cast<const volatile unsigned long long*>(base + 0x98);
                }
                // The three values that make the guest log
                // "startMusic disabled during download" (string 0x3902ff, tail
                // call at 0x12ad324; FDE [0x12ad2e0, 0x12ad3b0)).
                //
                // VERBATIM, re-disassembled 2026-10-05 after the earlier
                // transcription turned out to be wrong:
                //
                //   12ad2ec  adrp x8, #0x1f6d000
                //   12ad2f0  ldr  x8, [x8, #0x5d8]        x8 = *(0x1f6d5d8)
                //   12ad2f4  ldr  x8, [x8]                x8 = *x8      <-- EXTRA LEVEL
                //   12ad2f8  cbz  x8, #0x12ad318          null  -> downloading  [br 1]
                //   12ad2fc  mov  w9, #0x518
                //   12ad300  movk w9, #1, lsl #16         w9 = 0x10518
                //   12ad304  ldr  x8, [x8, x9]            x8 = x8[0x10518]
                //   12ad308  cbz  x8, #0x12ad318          null  -> downloading  [br 2]
                //   12ad30c  ldp  x8, x9, [x8, #0xd8]     container begin/end
                //   12ad310  cmp  x9, x8
                //   12ad314  b.eq #0x12ad32c              equal  -> OK path
                //                                                  unequal-> downloading [br 3]
                //
                // TWO THINGS THE OLD TRANSCRIPTION GOT WRONG, both of which
                // invalidated a day's conclusion:
                //
                //  1. It wrote `ldr x8,[x8,#0x10518]`. That encoding DOES NOT
                //     EXIST. An unsigned-offset LDR carries imm12 scaled by 8,
                //     so its reach is 0..0x7FE8; 0x10518 is past it, and as a
                //     32-bit scaled-by-4 form (0x4146) it is past the 0xFFF
                //     field too. The offset has to be built in a register, which
                //     is what movz/movk + `ldr [x8, x9]` does.
                //  2. Because it hid that register form, it also hid the
                //     `ldr x8,[x8]` at 0x12ad2f4. The field therefore lives in
                //     *(*(0x1f6d5d8)), NOT in *(0x1f6d5d8).
                //
                // This probe had been reading dlEngine+0x10518 -- one level too
                // high -- and reported "field is NULL, so branch 2". The value
                // the guest actually branches on, *dlEngine, was never read by
                // anything, so the branch it takes was simply unknown.
                // BRANCH VERDICT. Reported as a single token so the next run
                // names the branch instead of leaving three candidates standing.
                //   0 = guest reaches the OK path at 0x12ad32c (not downloading)
                //   1 = *dlEngine == 0            (cbz at 0x12ad2f8)
                //   2 = x8[0x10518] == 0         (cbz at 0x12ad308)
                //   3 = container begin != end   (cmp at 0x12ad310)
                //   ? = a plausibility guard tripped; dlGuard says which, so a
                //       skipped read can never masquerade as a verdict.
                unsigned long long dlEngine = 0, dlFirst = 0, dlVecPtr = 0,
                                    dlBegin = 0, dlEnd = 0;
                unsigned long long dlNb[5] = {0, 0, 0, 0, 0};
                int dlGuard = 0;
                const uint64_t dlEngineElf = bias + kEngineSingletonElfVa;
                dlEngine = *reinterpret_cast<const volatile unsigned long long*>(
                    static_cast<uintptr_t>(dlEngineElf));
                // A guest pointer worth dereferencing. Deliberately wide: the
                // guard exists to avoid faulting on the null page, not to judge
                // whether the value is "valid". A rejection is reported as
                // DLBR=? with dlGuard set, so it can never be mistaken for a
                // branch verdict.
                static constexpr uint64_t kPlausibleLo = 0x10000ULL;
                static constexpr uint64_t kPlausibleHi = 0x0000800000000000ULL;
                const bool dlEngineOk = dlEngine >= kPlausibleLo &&
                                        dlEngine < kPlausibleHi;
                if (dlEngineOk) {
                    dlFirst = *reinterpret_cast<const volatile unsigned long long*>(
                        static_cast<uintptr_t>(dlEngine));
                } else {
                    dlGuard = 1;
                }
                const bool dlFirstOk =
                    dlEngineOk && dlFirst >= kPlausibleLo && dlFirst < kPlausibleHi;
                if (dlFirstOk) {
                    dlVecPtr = *reinterpret_cast<const volatile unsigned long long*>(
                        static_cast<uintptr_t>(dlFirst + kEngineDownloadVecOff));
                    // Neighbourhood, -0x18..+0x10 around the field at 8-byte
                    // steps (w == 3 is the field itself). Present so "this one
                    // field is null" can be told apart from "this whole region
                    // of the object was never written" -- two bugs with
                    // different fixes, and the single field read cannot tell
                    // them apart.
                    for (int w = 0; w < 5; ++w) {
                        dlNb[w] = *reinterpret_cast<const volatile unsigned long long*>(
                            static_cast<uintptr_t>(
                                dlFirst + kEngineDownloadVecOff - 0x18
                                + static_cast<unsigned>(w) * 8));
                    }
                    if (dlVecPtr != 0) {
                        dlBegin = *reinterpret_cast<const volatile unsigned long long*>(
                            static_cast<uintptr_t>(dlVecPtr + kContainerPairOff));
                        dlEnd = *reinterpret_cast<const volatile unsigned long long*>(
                            static_cast<uintptr_t>(dlVecPtr + kContainerPairOff + 8));
                    }
                } else if (dlEngineOk) {
                    dlGuard = 2;
                }
                const char* dlBr =
                    (dlGuard != 0)   ? "?"
                    : (dlFirst == 0)    ? "1"
                    : (dlVecPtr == 0)   ? "2"
                    : (dlBegin != dlEnd) ? "3"
                                        : "0";

                // GATE D is read in its own every-frame block below, after this
                // one closes. It cannot live here: this block samples only 4
                // frames, and a change-triggered log that is only consulted on 4
                // frames is just a 4-sample log wearing a disguise.
                // The guest's CONTENT ROOT global, read for the same reason and
                // because its disassembly is unambiguous (ELF 0x12bf4dc):
                //
                //   ldrb w8,[x0,#1] ; cmp w8,#':' ; b.ne 0x12bf548
                //   ... adrp x8,#0x1f78000 ; ldr x8,[x8,#0xd30] ; ldrb w8,[x8]
                //   cbnz w8, 0x12bf5fc          -> name used as-is
                //   ... ldrb w8,[x20] ; cmp w8,#'/' ; b.ne 0x12bf6d8
                //   0x12bf6d8: mov w0, wzr      -> RETURNS 0, i.e. failure
                //
                // So a name with no volume prefix, when this global is empty and
                // the name is not absolute, cannot resolve. That is exactly the
                // shape of every failure the guest reports:
                //   LoadReadOnlyMappedFile failed getting full path:
                //       gui/processed/GlobalStyles.gui
                // while files/.depot/gui/processed/GlobalStyles.gui IS in
                // core.romfs and isAssetRelative() already knows the "gui/"
                // prefix. Measured, not inferred.
                unsigned long long rootPtr = 0;
                char rootText[64] = "<null>";
                // Raw head of the object the pointer designates. Dumped because
                // the next question is "is it safe to write, and how much", and
                // the only honest way to answer that is to look at the object's
                // own header rather than to assume a std::string layout:
                //   * if the 8 bytes at +0 are a heap pointer, the object is
                //     heap-allocated and its capacity is stated at +16;
                //   * if the bytes at +0 are the short string itself, the
                //     object is inline-SSO and the inline capacity is at least
                //     22 bytes, so a short write is still inside it.
                // Either way this is a read; nothing here writes guest memory.
                unsigned long long head[4] = {0, 0, 0, 0};
                {
                    const auto slotAddr =
                        static_cast<uintptr_t>(bias + kContentRootElfVa);
                    rootPtr = *reinterpret_cast<const volatile unsigned long long*>(slotAddr);
                    if (rootPtr != 0) {
                        const auto objAddr = static_cast<uintptr_t>(rootPtr);
                        for (int w = 0; w < 4; ++w) {
                            head[w] = *reinterpret_cast<const volatile unsigned long long*>(
                                objAddr + static_cast<unsigned>(w) * 8);
                        }
                        const char* p = reinterpret_cast<const char*>(objAddr);
                        unsigned i = 0;
                        for (; i + 1 < sizeof(rootText) && p[i] != '\0'; ++i) {
                            rootText[i] = p[i];
                        }
                        rootText[i] = '\0';
                    }
                }
                __builtin_snprintf(adiag, sizeof(adiag),
                    "asset-singleton: frame=%u elfva=0x%llx bias=0x%llx slot=0x%llx"
                    " singleton=%llx first_word=%llx verdict=%s"
                    " am8d=%u(%s) q88=%llu q90=%llu q98=%llu"
                    " contentroot=0x%llx '%s' head=[%016llx %016llx %016llx %016llx]"
                    " DL eng=0x%llx@0x%llx first=0x%llx ptr=0x%llx"
                    " begin=%llu end=%llu empty=%d br=%s g=%d"
                    " nb=[%016llx %016llx %016llx %016llx %016llx]",
                    static_cast<unsigned>(frames),
                    static_cast<unsigned long long>(kAssetSingletonElfVa),
                    static_cast<unsigned long long>(bias),
                    static_cast<unsigned long long>(slot),
                    static_cast<unsigned long long>(obj),
                    static_cast<unsigned long long>(first),
                    obj == 0 ? "NULL-SINGLETON"
                             : (first == 0 ? "NULL-FIRST-WORD" : "PRESENT"),
                    freeze,
                    freeze == 0 ? "not-latched" : "LATCHED",
                    q88, q90, q98,
                    rootPtr, rootText,
                    head[0], head[1], head[2], head[3],
                    dlEngine, static_cast<unsigned long long>(dlEngineElf),
                    dlFirst, dlVecPtr,
                    dlBegin, dlEnd,
                    (dlBegin == dlEnd) ? 1 : 0, dlBr, dlGuard,
                    dlNb[0], dlNb[1], dlNb[2], dlNb[3], dlNb[4]);
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    adiag, 0, 0);
            }
        }

        // GATE D -- the single condition standing between the boot path and the
        // constructor that writes engine+0x10518. Read EVERY frame, not on the
        // 4-frame schedule above.
        //
        // In 0x14048c8 (FDE [0x14048c8,0x1404ac0)) the only path to
        //     0x1404994  bl 0x14045e0
        // runs through 0x1404ac0, whose result is 0 -- and only 0 -- when bit 2
        // of [*(0x1fb5328)+0x70] is set. 0x1fb5328 is the shared state of an
        // async worker. The engine's own string names the enclosing routine:
        // "system_InitialiseServices".
        //
        // Already measured, so the question is narrow rather than open. The
        // thread census independently reports start_elfva 0x1406f94 as a live
        // guest thread whose lr_elfva is 0x1404a3c -- inside 0x14048c8 -- so the
        // worker was spawned FROM this function, gates A/B/C did not return
        // early, and the future exists. Whether the ready bit ever lands is the
        // open half.
        //
        // CHANGE-TRIGGERED, and the first sample always logs so a baseline
        // exists even if nothing ever changes. A fixed schedule would report
        // "never" for a value that merely arrived after the last sample, which
        // is the same error as reading a bounded scan and concluding it had no
        // bound.
        // One-shot: report every mapped segment's owner/slave pair.
        //
        // The question this answers is "can guest .text be written at all",
        // and the answer is decided by a table walk rather than by any single
        // mapping path. Guest code is mapped as an owner/slave PAIR --
        // `ownerAddress` is a Perm_Rw alias the image bytes were copied into,
        // `targetAddress` is the Perm_Rx mapping the guest executes -- so
        // svcSetMemoryPermission cannot reach it. That is what
        // guest_view_probe.cpp:2165 recorded as "fails on guest .text" and
        // abandoned a NOP patch over; it asked the wrong API.
        // Supplementary ROOT-only PC sampler (root_pc_sampler.hpp, added
        // 2026-10-06). The census samples every thread and costs one
        // pause+context round trip per thread per pass, which measured out at
        // 0.079 Hz for the root thread -- far too slow to land inside a
        // function the guest only visits on its way to the wedge. This one
        // pauses ONE thread at kRequestedHz, and is inert until called.
        //
        // The census's own `if (pcGuest)` gate cannot be reused verbatim: in
        // the 420 s run all 33 root samples were host-nro, so a fast sampler
        // behind that gate would be silent for the whole out-of-range window
        // and would be indistinguishable from a sampler that never started.
        // That is why the worker added an explicit NOT-GUEST-IMAGE line.
        if (frames == 0) {
            rr3RootPcSamplerStart();
        }

        if (frames == 0) {
            reportSegmentOwnerSlave();

            // ---- install the container-teardown probe FIRST ----
            //
            // The order is load-bearing, and the previous order (permission
            // probe first, then install) was wrong. Both compute the SAME page:
            // the probe logged page=0x210762b000 and its svcSetMemoryPermission
            // returned rc=0, yet the install's own svcSetMemoryPermission on
            // that very page failed with stage=9 moments later. The only thing
            // between them is the probe's restore to Perm_Rx -- so the install
            // met the page on its SECOND Rx->Rw transition.
            //
            // The install is self-sufficient: it takes Perm_Rw itself
            // (rr3_deinfo_probe.cpp) and restores Perm_Rx itself. The old
            // "install needs Perm_Rw, so it must run after" comment was an
            // illusion created by the probe sitting in between. Running the
            // install first removes the second transition; the permission probe
            // below stays afterwards, purely as a diagnostic.
            {
                const uint64_t kProbeOff = 0xcd2aa0ULL;
                const uintptr_t guestCode =
                    static_cast<uintptr_t>(g_renderTarget) - 0x12B3614ULL + kProbeOff;
                ::rr3::DeInfoInstallReport rep{};
                const bool ok = ::rr3::rr3DeInfoInstall(guestCode, &rep);
                // 768, not 384. The permission verdict is the only thing this
                // line exists to carry now, and it does not fit: permBefore,
                // the pre-probe round trip, all five ladder Results, the
                // measured perm afterwards and the rollback's own perm. Adding
                // the widest plausible value for every field comes to ~618
                // bytes, which fits 640 with 21 to spare -- too little margin
                // to trust on a hand count, and a snprintf that silently
                // truncates leaves a half-written field at the end looking
                // like a real reading, which is the whole class of bug this log
                // line guards against. One bounded line, one write, frames==0
                // only, and the bound is still a bound.
                char inst[768];
                __builtin_snprintf(inst, sizeof(inst),
                                   "ok=%d stage=%d installed=%u site=0x%llx "
                                   "page=0x%llx "
                                   "tramp=0x%llx byteDist=%lld wrote=0x%08x "
                                   "jitCreateRes=%u permRwRes=0x%08x "
                                   "permRwPreRes=0x%08x "
                                   "permRestoreMode=%u alreadyInstalled=%u "
                                   "windowVerified=%u cbzTarget=0x%llx "
                                   "resume=0x%llx "
                                   "stub=[0x%08x 0x%08x 0x%08x 0x%08x] "
                                   "qPre=0x%08x permBefore=0x%x "
                                   "preRc=0x%08x preQ=0x%08x preFinal=0x%x "
                                   "ladderN=%u ladder=[0x%08x 0x%08x 0x%08x "
                                   "0x%08x 0x%08x] rung=%u "
                                   "permQ=0x%08x permFinal=0x%x exec=%u "
                                   "rb=%u rbRw=0x%08x rbQ=0x%08x rbFinal=0x%x "
                                   "rbExec=%u",
                                   ok ? 1 : 0, rep.stage, rep.installed,
                                   (unsigned long long)rep.siteVa,
                                   (unsigned long long)rep.pageVa,
                                   (unsigned long long)rep.trampolineVa,
                                   (long long)rep.branchDelta, rep.wordWritten,
                                   rep.jitCreateResult, rep.permRwResult,
                                   rep.permRwPreResult, rep.permRestoreMode,
                                   rep.alreadyInstalled,
                                   rep.windowVerified,
                                   (unsigned long long)rep.cbzTargetVa,
                                   (unsigned long long)rep.resumeVa,
                                   rep.stubWords[0], rep.stubWords[1],
                                   rep.stubWords[2], rep.stubWords[3],
                                   rep.permQueryResPre, rep.permBefore,
                                   rep.permPreRestoreRc, rep.permPreQueryRes,
                                   rep.permPreFinal,
                                   rep.permLadderLen,
                                   rep.permLadderRc[0], rep.permLadderRc[1],
                                   rep.permLadderRc[2], rep.permLadderRc[3],
                                   rep.permLadderRc[4], rep.permRestoreRung,
                                   rep.permQueryRes, rep.permFinal,
                                   rep.permExecFinal,
                                   rep.permRollbackResult,
                                   rep.permRollbackRwResult,
                                   rep.permRollbackQueryRes,
                                   rep.permRollbackFinal,
                                   rep.permRollbackExec);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "deinfo-install", ok ? 0u : 1u, 0, inst);
            }

            // The one measurement that decides whether patching is possible.
            //
            // There is NO owner/slave pair for this guest: the table walked
            // above comes back with segments=0, because the image goes through
            // mapNormalSegmentedRange (multi_dso_linker.cpp:979), which uses
            // plain `svcMapMemory` at L998. So the code-memory-handle route is
            // out, and what remains is the obvious one: a plain mapping should
            // be reachable by `svcSetMemoryPermission`.
            //
            // guest_view_probe.cpp:2165 records that it "fails on guest .text
            // (different memory block)" and dropped a NOP patch over it, but
            // never recorded WHICH call failed or with what result. This
            // measures it. Permissions only -- no guest byte is written -- so a
            // failure costs nothing and a success is a fact, not a story.
            const uint64_t kPageSz = 4096ULL;
            const uint64_t kProbeOff = 0xcd2aa0ULL;
            const uintptr_t guestCode =
                static_cast<uintptr_t>(g_renderTarget) - 0x12B3614ULL + kProbeOff;
            void* const page =
                reinterpret_cast<void*>(guestCode & ~(kPageSz - 1));
            char why[256];
            __builtin_snprintf(why, sizeof(why),
                               "guest_code=0x%llx page=0x%llx  Perm_R->Perm_Rw "
                               "on a plain svcMapMemory region",
                               (unsigned long long)guestCode,
                               (unsigned long long)(uintptr_t)page);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "guesttext-perm-start", 0, 0, why);
            const Result rc = svcSetMemoryPermission(page, kPageSz, Perm_Rw);
            __builtin_snprintf(why, sizeof(why),
                               "rc=%d module=%d desc=%d  => guest .text is %s",
                               (int)rc, (int)R_MODULE(rc), (int)R_DESCRIPTION(rc),
                               R_SUCCEEDED(rc) ? "WRITABLE via svcSetMemoryPermission"
                                               : "NOT writable that way");
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "guesttext-perm-result",
                                R_FAILED(rc) ? 1u : 0u,
                                (unsigned long long)rc, why);
            // RESTORE TO Perm_Rx, NOT Perm_R.
            //
            // libnx svc.h: Perm_R = BIT(0) is read-only, Perm_Rx = Perm_R|Perm_X,
            // and svcSetMemoryPermission "can be used to move back and forth
            // between Perm_None, Perm_R and Perm_Rw" -- i.e. it will not
            // return X. Restoring Perm_R here would strip EXECUTE from the
            // guest's .text page. The earlier version of this probe restored
            // Perm_R and claimed it "leaves no trace"; that claim was wrong.
            (void)svcSetMemoryPermission(page, kPageSz, Perm_Rx);

            // Does this page accept a SECOND Rx->Rw transition?
            //
            // The install failed with stage=9 on exactly this page moments ago,
            // and the only difference between the two calls was that the
            // install arrived second. Rather than infer "Eden refuses a repeat
            // remap" from a stage number, spend one syscall and one log line to
            // measure it. Restored to Perm_Rx again immediately below, so the
            // guest's page is left executable either way.
            const Result rc2 = svcSetMemoryPermission(page, kPageSz, Perm_Rw);
            __builtin_snprintf(why, sizeof(why),
                               "second_transition_rc=%d module=%d desc=%d => %s",
                               (int)rc2, (int)R_MODULE(rc2), (int)R_DESCRIPTION(rc2),
                               R_SUCCEEDED(rc2)
                                   ? "a repeat Rx->Rw on the same page IS allowed"
                                   : "a repeat Rx->Rw on the same page is REFUSED");
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "guesttext-perm-second",
                                R_FAILED(rc2) ? 1u : 0u,
                                (unsigned long long)rc2, why);
            (void)svcSetMemoryPermission(page, kPageSz, Perm_Rx);

            // The install used to live here, AFTER this restore. That order is
            // what produced stage=9: this restore hands the page back as
            // Perm_Rx, and the install then asked for Perm_Rw a second time on
            // the same page and was refused. It now runs before the probe, at
            // the top of this frames==0 block. The line above stays, so the
            // diagnostic still leaves the page exactly as it found it.
        }

        // Drain the probe. B is the sentinel the walk compares against, E is
        // the cursor. The loop terminates iff E >= B and (E-B) % 48 == 0, and
        // 48 is NOT a power of two, so `& 47` would be wrong.
        if (::rr3::rr3DeInfoInstalled()) {
            ::rr3::DeInfoSample sample{};
            if (::rr3::rr3DeInfoDrain(&sample)) {
                const bool ordered = sample.e >= sample.b;
                const uint64_t span = ordered ? (sample.e - sample.b) : 0;
                const bool strideOk = ordered && (span % 48ULL == 0ULL);
                char out[288];
                __builtin_snprintf(out, sizeof(out),
                                   "seq=%u B=0x%llx E=0x%llx span=0x%llx(%llu) "
                                   "E_ge_B=%d mod48=%d verdict=%s",
                                   sample.seq,
                                   (unsigned long long)sample.b,
                                   (unsigned long long)sample.e,
                                   (unsigned long long)span,
                                   (unsigned long long)span,
                                   ordered ? 1 : 0, strideOk ? 1 : 0,
                                   !ordered  ? "E_LT_B_NEVER_TERMINATES"
                                   : strideOk ? "TERMINATES"
                                              : "SPAN_NOT_MULTIPLE_OF_48");
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "deinfo-bounds", sample.seq, 0, out);
            }
        }

        if (renderTarget != 0 && frames < 600) {
            static constexpr uint64_t kRenderExportElfVa2 = 0x12B3614ULL;
            static constexpr uint64_t kAsyncFutureElfVa = 0x1FB5328ULL;
            static constexpr uint64_t kAsyncOnceElfVa   = 0x1FB5330ULL;
            static constexpr uint64_t kAsyncStateOff    = 0x70ULL;
            static constexpr uint64_t kGateLo = 0x10000ULL;
            static constexpr uint64_t kGateHi = 0x0000800000000000ULL;
            const uint64_t gbias =
                static_cast<uint64_t>(renderTarget) - kRenderExportElfVa2;
            const unsigned gOnce = *reinterpret_cast<const volatile unsigned char*>(
                static_cast<uintptr_t>(gbias + kAsyncOnceElfVa));
            const unsigned long long gFut =
                *reinterpret_cast<const volatile unsigned long long*>(
                    static_cast<uintptr_t>(gbias + kAsyncFutureElfVa));
            unsigned long long gSt70 = 0xFFFFFFFFFFFFFFFFULL;   // "not read"
            if (gFut >= kGateLo && gFut < kGateHi) {
                gSt70 = *reinterpret_cast<const volatile unsigned long long*>(
                    static_cast<uintptr_t>(gFut + kAsyncStateOff));
            }
            const unsigned gBit2 = static_cast<unsigned>((gSt70 >> 2) & 1u);

            // GATES A and B of the same function, and the one that decides
            // whether 0x1405868 continues past 0x1405898 at all:
            //   0x14048e8 ldrb w8,[x0,#0x83] ; cbz w8,#0x14048f8   (A)
            //   0x14048f8 ldrb w8,[x0,#0x82] ; cbz w8,#0x1404940   (B)
            //   0x14058c8 ldrb w8,[x19,#0x85]                      (caller)
            // x0 into 0x14048c8 is the same object 0x1405868 was called with
            // (0x140588c `mov x19,x0`, x0 untouched before the bl at 0x1405894).
            // That object is read as the one at *(0x1f6d5d0) -- the data/asset
            // manager this probe already samples at +0x8d for the
            // AssetDownloadService freeze, and whose neighbours in the
            // localisation key list are NO_WIFI / FREE_STORAGE / ASSET_DOWNLOAD.
            // 0x82/0x83/0x85/0x8d being one flag cluster is suggestive, NOT
            // established, so the whole run of bytes is dumped and the reader
            // can see the cluster rather than trust the guess.
            unsigned ab82 = 0xFFu, ab83 = 0xFFu, ab85 = 0xFFu, ab8d = 0xFFu;
            unsigned long long abObj = 0;
            {
                static constexpr uint64_t kAssetMgrElfVa = 0x1F6D5D0ULL;
                abObj = *reinterpret_cast<const volatile unsigned long long*>(
                    static_cast<uintptr_t>(gbias + kAssetMgrElfVa));
                if (abObj >= kGateLo && abObj < kGateHi) {
                    const volatile unsigned char* b =
                        reinterpret_cast<const volatile unsigned char*>(
                            static_cast<uintptr_t>(abObj));
                    ab82 = b[0x82]; ab83 = b[0x83]; ab85 = b[0x85]; ab8d = b[0x8d];
                }
            }

            // ALL FOUR GATES ARE OPEN and the field is still null, so the
            // constructor must have run -- on an object that is not the one the
            // guest reads. Two candidates, and guessing between them is exactly
            // what produced the last wrong answer, so BOTH are read:
            //
            //   mgr   = *(0x1f6d5d0)   the data/asset manager. 0x1405868's x0
            //                               is very likely this, and it is the
            //                               object whose 0x82/0x83 gates were
            //                               just read above.
            //   eng   = *(*(0x1f6d5d8))  the engine. 0x12ad2ec-0x12ad304 reads
            //                               eng[0x10518] and it is NULL.
            //
            // Whichever one has a populated 0x10518 is the object the
            // constructor was given. If BOTH are null then the constructor did
            // not run and the gates are being satisfied by a different caller
            // than the traced one -- also a distinguishable answer, and neither
            // outcome is assumed here.
            unsigned long long mgr10518 = 0, eng10518 = 0, engNb[3] = {0, 0, 0};
            unsigned long long engPtr = 0;
            {
                const unsigned long long eng = *reinterpret_cast<
                    const volatile unsigned long long*>(
                        static_cast<uintptr_t>(gbias + 0x1F6D5D8ULL));
                engPtr = (eng >= kGateLo && eng < kGateHi)
                    ? *reinterpret_cast<const volatile unsigned long long*>(
                          static_cast<uintptr_t>(eng)) : 0;
                if (engPtr >= kGateLo && engPtr < kGateHi) {
                    for (int w = 0; w < 3; ++w) {
                        engNb[w] = *reinterpret_cast<const volatile unsigned long long*>(
                            static_cast<uintptr_t>(engPtr + 0x10508
                                + static_cast<unsigned>(w) * 8));
                    }
                    eng10518 = engNb[1];
                }
                if (abObj >= kGateLo && abObj < kGateHi) {
                    mgr10518 = *reinterpret_cast<const volatile unsigned long long*>(
                        static_cast<uintptr_t>(abObj + 0x10518ULL));
                }
            }
            static unsigned long long pOnce = 0xFFFFFFFFFFFFFFFFULL,
                                    pFut  = 0xFFFFFFFFFFFFFFFFULL,
                                    pSt70 = 0xFFFFFFFFFFFFFFFFULL;
            static unsigned pA82 = 0xFFu, pA83 = 0xFFu, pA85 = 0xFFu, pA8d = 0xFFu;
            static unsigned long long pMgr = 0xFFFFFFFFFFFFFFFFULL,
                                    pEng  = 0xFFFFFFFFFFFFFFFFULL,
                                    pEng0 = 0xFFFFFFFFFFFFFFFFULL;
            static unsigned long long pAObj = 0xFFFFFFFFFFFFFFFFULL;
            // THIRD candidate, and the one the static trace actually names.
            // 0x12b4428 `ldr x0,[x19]` / 0x12b4438 `ldr x0,[x19]` /
            // 0x12b4440 `bl 0x1405868`  =>  `this` is *(x19), and x19 in
            // [0x12b43a0,0x12b4448) is the object onViewRenderJNI hands to
            // 0x12aea7c -- which this file already resolves as
            // readGuestActivitySnapshot(renderTarget).renderer, and whose +0x4fc
            // is the word the render loop's exit test waits to become 4.
            // One more dereference than the other two candidates, so it is
            // exactly the kind of step that gets dropped.
            unsigned long long renPtr = 0, ren10518 = 0, renNb[3] = {0, 0, 0};
            unsigned r82 = 0xFFu, r83 = 0xFFu, r85 = 0xFFu, r88 = 0xFFu;
            unsigned r80w = 0xDEADBEEFu, r80w2 = 0xDEADBEEFu;
            unsigned q80 = 0xFFu, q81 = 0xFFu, q84 = 0xFFu;
            unsigned r86 = 0xFFu, r87 = 0xFFu;
            unsigned long long r80lo = 0xDEADBEEFDEADBEEFULL;
            // The SAME 8 bytes on the object the render entry itself writes:
            // 0x12aecb8 `str x0,[x19,#0x80]` in [0x12aea7c,0x12af358) stores the
            // return of bl 0x10b2f5c, a virtual time getter on the engine
            // singleton, and 0x12aec9c reads it back as a clamped frame delta.
            // If +0x80 is a monotonic clock, its high bytes are 0x00/0x7f here.
            // Reading both candidates is the discriminator: one of them is a
            // clock, the other is whatever the latch object really is.
            unsigned long long rn80 = 0xDEADBEEFDEADBEEFULL;
            {
                const GuestActivitySnapshot ns2 = readGuestActivitySnapshot(renderTarget);
                if (ns2.valid && ns2.renderer != 0 &&
                    static_cast<uint64_t>(ns2.renderer) >= kGateLo &&
                    static_cast<uint64_t>(ns2.renderer) < kGateHi) {
                    rn80 = *reinterpret_cast<const volatile unsigned long long*>(
                        static_cast<uintptr_t>(ns2.renderer + 0x80));
                }
            }
            unsigned e82 = 0xFFu, e83 = 0xFFu, e85 = 0xFFu, e88 = 0xFFu;
            {
                const GuestActivitySnapshot rs = readGuestActivitySnapshot(renderTarget);
                if (rs.valid && rs.renderer != 0) {
                    renPtr = *reinterpret_cast<const volatile unsigned long long*>(
                        static_cast<uintptr_t>(rs.renderer));
                    if (renPtr >= kGateLo && renPtr < kGateHi) {
                        for (int w = 0; w < 3; ++w) {
                            renNb[w] = *reinterpret_cast<
                                const volatile unsigned long long*>(
                                    static_cast<uintptr_t>(
                                        renPtr + 0x10508
                                        + static_cast<unsigned>(w) * 8));
                        }
                        ren10518 = renNb[1];
                        // Gates A and B, read on the object the static trace
                        // actually names, after reading them on `mgr` proved
                        // to be reading a different object entirely.
                        const volatile unsigned char* b =
                            reinterpret_cast<const volatile unsigned char*>(
                                static_cast<uintptr_t>(renPtr));
                        r82 = b[0x82]; r83 = b[0x83]; r85 = b[0x85]; r88 = b[0x88];
                        // The four bytes 0x80..0x83 as ONE word. Little-endian,
                        // with byte[0x82]=0x22 and byte[0x83]=0x08 the word must
                        // be 0x0822???? -- which is what a single 32-bit store
                        // at +0x80 would produce, and what two separate STRBs
                        // from the latch code (which store #1) cannot. Reading
                        // the word settles whether 0x80 is a coherent field or
                        // four unrelated bytes, for one load.
                        r80w = *reinterpret_cast<const volatile unsigned int*>(
                            static_cast<uintptr_t>(renPtr + 0x80));
                        // The previous run reported b[0x82] = 130 while the
                        // word read one instruction earlier implies b[0x82] =
                        // 34. Two reads of one address a few instructions apart
                        // cannot both be right, and the guest has five threads
                        // running against this object. So the whole run is read
                        // again here, word and bytes, in the opposite order:
                        // word-then-bytes above, bytes-then-word here. If the
                        // numbers differ between the two blocks it is a torn
                        // read under concurrency; if they agree, one of the two
                        // read paths has an addressing bug and the word is not
                        // a flag field at all. Either way it is decided by
                        // measurement rather than by picking the nicer number.
                        r80w2 = *reinterpret_cast<const volatile unsigned int*>(
                            static_cast<uintptr_t>(renPtr + 0x80));
                        const unsigned q80v = b[0x80], q81v = b[0x81], q84v = b[0x84];
                        q80 = q80v; q81 = q81v; q84 = q84v;
                        r86 = b[0x86]; r87 = b[0x87];
                        r80lo = *reinterpret_cast<const volatile unsigned long long*>(
                            static_cast<uintptr_t>(renPtr + 0x80));
                    }
                }
                if (engPtr >= kGateLo && engPtr < kGateHi) {
                    const volatile unsigned char* b =
                        reinterpret_cast<const volatile unsigned char*>(
                            static_cast<uintptr_t>(engPtr));
                    e82 = b[0x82]; e83 = b[0x83]; e85 = b[0x85]; e88 = b[0x88];
                }
            }
            // GATES E and F -- two levels ABOVE gate A, and until now never
            // read at all. In [0x12b43a0,0x12b4448), before the call:
            //   12b4414 ldr  x8,[x8,#0xe98] ; ldr w8,[x8]
            //   12b441c cbz  w8,#0x12b43c0          (E) *(0x1f78e98) must be != 0
            //   12b4420 ldrb w8,[x19,#0xa]
            //   12b4424 cbz  w8,#0x12b43c0          (F) renderer[0xa] must be != 0
            //   12b4440 bl   0x1405868
            // Either one being zero skips the call entirely, so the ctor, gates
            // A-D, and the whole of system_InitialiseServices never happen --
            // silently, with no log, because these are ordinary flag tests.
            //
            // x19 here is the render object (0x12b4428 `ldr x0,[x19]` is what
            // becomes `this`), and 0x1405868's `this` is `*(x19)` = ren. So E
            // and F gate the same object the constructor was given.
            unsigned gE = 0xFFFFFFFFu, gF = 0xFFu, gRenWord = 0xFFFFFFFFu;
            {
                const GuestActivitySnapshot es = readGuestActivitySnapshot(renderTarget);
                const unsigned long long gateGlobal = *reinterpret_cast<
                    const volatile unsigned long long*>(
                        static_cast<uintptr_t>(gbias + 0x1F78E98ULL));
                gE = static_cast<unsigned>(gateGlobal & 0xFFFFFFFFu);
                if (es.valid && es.renderer != 0 &&
                    static_cast<uint64_t>(es.renderer) >= kGateLo &&
                    static_cast<uint64_t>(es.renderer) < kGateHi) {
                    const volatile unsigned char* rb =
                        reinterpret_cast<const volatile unsigned char*>(
                            static_cast<uintptr_t>(es.renderer));
                    gF = rb[0x0a];
                    gRenWord = static_cast<unsigned>(*reinterpret_cast<
                        const volatile unsigned int*>(
                            static_cast<uintptr_t>(es.renderer + 8)));
                }
            }
            static unsigned pE = 0xFFFFFFFFu, pF = 0xFFu, pRW = 0xFFFFFFFFu;
            const bool gateUpChanged = (gE != pE) || (gF != pF) || (gRenWord != pRW);
            if (gateUpChanged) {
                pE = gE; pF = gF; pRW = gRenWord;
                char upLine[256];
                __builtin_snprintf(upLine, sizeof(upLine),
                    "gate-ef: frame=%u E[0x1f78e98]=%08x(renderer) pass=%d"
                    " F[renderer+0xa]=%02x pass=%d renderer+8=%08x"
                    " => 0x1405868 %s",
                    static_cast<unsigned>(frames), gE, gE ? 1 : 0,
                    gF, gF ? 1 : 0, gRenWord,
                    (gE && gF) ? "CALLED" : "SKIPPED");
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    upLine, 0, 0);
            }
            static unsigned pR82 = 0xFFu, pR83 = 0xFFu, pR85 = 0xFFu, pR88 = 0xFFu;
            static unsigned pR80 = 0xDEADBEEFu, pR80b = 0xDEADBEEFu, pR86 = 0xFFu, pR87 = 0xFFu;
            static unsigned long long pR80c = 0xDEADBEEFDEADBEEFULL,
                                    pRn80 = 0xDEADBEEFDEADBEEFULL;
            static unsigned pE82 = 0xFFu, pE83 = 0xFFu, pE85 = 0xFFu, pE88 = 0xFFu;
            static unsigned long long pRen = 0xFFFFFFFFFFFFFFFFULL,
                                    pRen0 = 0xFFFFFFFFFFFFFFFFULL;
            const bool abChanged = (gOnce != pOnce) || (gFut != pFut) ||
                                   (gSt70 != pSt70) || (ab82 != pA82) ||
                                   (ab83 != pA83) || (ab85 != pA85) ||
                                   (ab8d != pA8d) || (abObj != pAObj) ||
                                   (mgr10518 != pMgr) || (eng10518 != pEng) ||
                                   (engNb[0] != pEng0) || (ren10518 != pRen) ||
                                   (renNb[0] != pRen0) || (r82 != pR82) ||
                                   (r83 != pR83) || (r85 != pR85) || (r88 != pR88) ||
                                   (r80w != pR80) || (r80w2 != pR80b) ||
                                   (r80lo != pR80c) || (rn80 != pRn80) ||
                                   (r86 != pR86) || (r87 != pR87) ||
                                   (e82 != pE82) || (e83 != pE83) ||
                                   (e85 != pE85) || (e88 != pE88);
            if (abChanged) {
                pOnce = gOnce; pFut = gFut; pSt70 = gSt70;
                pA82 = ab82; pA83 = ab83; pA85 = ab85; pA8d = ab8d; pAObj = abObj;
                pMgr = mgr10518; pEng = eng10518; pEng0 = engNb[0];
                pRen = ren10518; pRen0 = renNb[0];
                pR82 = r82; pR83 = r83; pR85 = r85; pR88 = r88; pR80 = r80w;
                pR80b = r80w2; pR80c = r80lo; pRn80 = rn80; pR86 = r86; pR87 = r87;
                pE82 = e82; pE83 = e83; pE85 = e85; pE88 = e88;
                char gateLine[768];
                __builtin_snprintf(gateLine, sizeof(gateLine),
                    "gate-d: frame=%u pollopen=%d"
                    " ren=0x%llx ren+80u64=0x%016llx w=0x%08x/0x%08x"
                    " bytes[80=%u 81=%u 82=%u 83=%u 84=%u 85=%u 86=%u 87=%u 88=%u]"
                    " rend+80u64=0x%016llx"
                    " renNB=[0x%llx 0x%llx 0x%llx]"
                    " eng=0x%llx engAB[82=%u 83=%u 85=%u 88=%u]",
                    static_cast<unsigned>(frames), gBit2 ? 1 : 0,
                    renPtr, r80lo, r80w, r80w2,
                    q80, q81, r82, r83, q84, r85, r86, r87, r88,
                    rn80,
                    renNb[0], renNb[1], renNb[2],
                    engPtr, e82, e83, e85, e88);
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    gateLine, 0, 0);
            }
        }

        // ---------------------------------------------------------------------
        // SECOND onViewRenderJNI CALL -- the experiment the whole chain points
        // at, and the first one that is ours rather than the guest's.
        //
        // Measured, all of it:
        //   * [0x12b43a0] opens with `ldrb w8,[x0,#0xa]` -- that is gate F -- and
        //     the probe read renderer[0xa] = 0x4b, so the object this probe
        //     calls `renderer` is the one the guest actually enters.
        //   * 0x12b4428 / 0x12b4438 `ldr x0,[x19]` then 0x12b4440
        //     `bl 0x1405868`, so the constructor's `this` is *(renderer) = ren.
        //   * 0x140395c, entered from 0x12b4434 one instruction before that call,
        //     executes `0x14039d0 strb wzr,[x19,#0x83]` -- it CLEARS the byte
        //     gate A tests, and it reloads the engine singleton right after.
        //   * All of 0x12b4434 / 0x12b4440 lives inside onViewRenderJNI's call
        //     tree, and RR3_FRAME_LOOP_RENDER = 0 means we enter that tree
        //     exactly once, ever.
        //
        // So the latch at +0x83 can only be cleared by a call we never make
        // again, and the constructor can never run. Whether a second call lets
        // the latch move is one bounded experiment, and it is the difference
        // between "the engine never initialises this subsystem" and "our port
        // gives it one frame and walks away".
        //
        // DELIBERATELY NOT GUARDED BY RR3_FRAME_LOOP_RENDER. That switch exists
        // because the per-frame draw re-entry never returned; this is a single
        // extra call at a known frame, which is a different claim and must be
        // able to be tested independently of it. The change-triggered gate-d
        // line above is the instrument: it logs +0x10518 and the latch bytes
        // on any change, so a run that hangs inside the call still leaves the
        // last state on record.
        //
        // Acceptance, fixed before running:
        //   ren+0x10518 becomes non-zero  -> root cause confirmed and repaired
        //   or ren+0x82 / ren+0x83 move    -> the latch is live and reachable
        //   or neither, and the log stops  -> the call hangs; the guest's own
        //                                       log is then the only witness
        // PIXEL READBACK -- answers "did the guest actually draw", which no
        // counter on this backend can.
        //
        // On Mesa the guest's GL imports are bound by the dynamic linker
        // straight into libGLESv2, so draws=0/0 / compile=0 / tex=0 say
        // nothing about what the guest did -- they only say our wrappers were
        // bypassed. Two attempts to fix that (mesa4, mesa5) both wedged, the
        // second because -Wl,--wrap=X aliases __real_X to X, so defining X
        // ourselves recurses forever. Structural, not fixable by another
        // wrapper layer.
        //
        // THIS readback sidesteps the question. It runs in OUR code, not the
        // guest's, so it reaches the real driver regardless of how the guest
        // resolved anything, and it samples the back buffer before the loop
        // swaps.
        //
        // SAMPLED TWICE, and the second sample is the one that matters. The
        // first attempt read only at frame 30, which is roughly half a second
        // after boot -- long before the guest finishes loading its GUI layout
        // (guest ordinal ~900, and the files only became loadable in gui9,
        // archive 20261011-1541*gui9). It faithfully reported black, which
        // said nothing: nothing had been drawn YET.
        //
        // AND it only sampled a 32x32 patch at the screen centre. That is a
        // second design error of the same kind: a splash that letterboxes, or
        // puts its logo anywhere but dead centre, reads as black no matter how
        // much it drew. So the LATE sample scans the WHOLE back buffer and
        // reports coverage plus five probe points. "Non-zero anywhere" is the
        // question; "non-zero at one arbitrary spot" was never the right proxy.
        //
        // Acceptance, fixed before running:
        //   lit > 0       -> content IS on screen somewhere; a 32x32 centre
        //                     patch was simply the wrong window
        //   lit == 0      -> the guest really has nothing drawn, and THAT is
        //                     the bug -- not the plumbing, which gui9/gui13
        //                     proved end to end (128 mmaps, 78 opens, shaders
        //                     resolving, guestOpen on the .z strip returning 0)
        // On backend=null glReadPixels is a counted no-op, so the buffer stays
        // zero; only read these lines when gfxBackendName() is "mesa".
        const auto readbackAndLog = [](const char* when, unsigned frame) {
            // 1280x720 RGBA is 3.5 MiB: too big for the stack, and this runs on
            // the frame-loop thread.
            static unsigned char* px = nullptr;
            if (px == nullptr) {
                px = static_cast<unsigned char*>(
                    std::malloc(static_cast<size_t>(1280) * 720 * 4));
                if (px == nullptr) {
                    return;
                }
            }
            std::memset(px, 0, static_cast<size_t>(1280) * 720 * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, 1280, 720, GL_RGBA, GL_UNSIGNED_BYTE, px);

            unsigned long long lit = 0;      // pixels with any non-zero RGB
            unsigned int unique[4] = {0, 0, 0, 0};
            for (int i = 0; i < 1280 * 720; ++i) {
                const unsigned char* p = px + i * 4;
                if (p[0] | p[1] | p[2]) { ++lit; }
                ++unique[p[0] >> 6];
            }
            // Five probe points: four corners and the centre, so a log line is
            // enough to tell "blank" from "content somewhere unexpected".
            struct { int x, y; } probe[5] = {
                {8, 8}, {1271, 8}, {8, 711}, {1271, 711}, {640, 360}
            };
            char pts[5 * 9] = {};
            for (int k = 0; k < 5; ++k) {
                const unsigned char* p =
                    px + (static_cast<size_t>(probe[k].y) * 1280 + probe[k].x) * 4;
                std::snprintf(pts + k * 9, 9, "%02x%02x%02x", p[0], p[1], p[2]);
            }
            char diag[288];
            std::snprintf(diag, sizeof(diag),
                "%s frame=%u full 1280x720 lit=%llu of %d px0=%02x%02x%02x%02x"
                " quads[r_bins=%u,%u,%u,%u] pts=[%s %s %s %s %s] glerr=0x%x",
                when, frame, lit, 1280 * 720, px[0], px[1], px[2], px[3],
                unique[0], unique[1], unique[2], unique[3],
                pts, pts + 9, pts + 18, pts + 27, pts + 36, glGetError());
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "view-pixel-readback", 249, 0, diag);
        };

        if (!gfxNullActive()) {
            static unsigned s_lateReadbackDone = 0;
            if (s_lateReadbackDone == 0 && frames >= 3000) {
                s_lateReadbackDone = 1;
                readbackAndLog("LATE", frames);
            }
        }

        if (renderTarget != 0 && frames == 30) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "second-render: frame=30 invoking onViewRenderJNI a second time",
                0, 0);
            reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
                jniEnvironment, activityObject, 2, 0);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "second-render: frame=30 RETURNED", 0, 0);
            if (!gfxNullActive()) {
                readbackAndLog("EARLY", frames);
            }
        }

        if (RR3_FRAME_LOOP_RENDER) {
        reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
            jniEnvironment, activityObject, 2, 0);
        }
        if (frames < 3) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "wedge: step=render-cap-removed f=0", 284, 0);
        }
        g_samplerPhase.store(kSamplerPhasePostGuest, std::memory_order_relaxed);
        if (straddleOpen) {
            char diag[768];
            formatRenderCallStraddle(diag, sizeof(diag), "post", renderTarget,
                                     jniEnvironment, activityObject, 2, 0);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "render-call-post", 285,
                static_cast<unsigned long long>(threadGetCurHandle()), diag);
        }
        if (frames < 3) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "wedge: step=after-render-jni f=0", 283, 0);
        }

        // Re-assert splash-exit flags each frame. Do NOT force frameState 閿?
        // let the guest run its natural warmup閳姱plash閳姦enu sequence.
        //
        // GUARDED BY THE SAME SWITCH, 2026-10-04. This block used to be
        // unconditional while RR3_FORCE_SPLASH_FLAGS only covered the one-shot
        // write before the first render -- so turning the switch "off" changed
        // nothing about the per-frame behaviour, which is why the A/B that
        // introduced it read as a null result.
        //
        // It is also wrong on its own terms. 0x12aeddc shows the guest clears
        // renderer[0x1f5a9] itself once it has consumed it; re-arming a
        // one-shot every 16 ms re-enters the menu transition 60x a second and
        // never lets the guest leave it.
        if (RR3_FORCE_SPLASH_FLAGS) {
            const GuestActivitySnapshot snapNow =
                readGuestActivitySnapshot(renderTarget);
            if (snapNow.valid) {
                if (snapNow.renderer != 0 &&
                    snapNow.renderer > 0x100000000ULL) {
                    volatile uint8_t* r = reinterpret_cast<volatile uint8_t*>(
                        snapNow.renderer);
                    bool changed = false;
                    if (r[0x1f5a9] == 0) { r[0x1f5a9] = 1; changed = true; }
                    if (r[0x1f5ab] == 0) { r[0x1f5ab] = 1; changed = true; }
                    if (r[0x1f5ac] == 0) { r[0x1f5ac] = 1; changed = true; }
                    if (r[0x120] == 0) { r[0x120] = 1; changed = true; }
                    if (changed && frames < 5) {
                        appendGuestBootLine(
                            "sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "reassert-splash-flags", frames,
                            (static_cast<unsigned long long>(r[0x1f5a9]) << 24) |
                            (static_cast<unsigned long long>(r[0x1f5ab]) << 16) |
                            (static_cast<unsigned long long>(r[0x1f5ac]) << 8) |
                            static_cast<unsigned long long>(r[0x120]));
                    }
                }
            }
        }

        // Fire CloudcellResume at several frames after hideSplash so it
        // lands after cloudcell Constructor completes (which happens async).
        // ELF 0x1cf2ad8; delta from onViewRenderJNI (ELF 0x12B3614) = +0xA3F4C4.
        if (frames == 10 || frames == 20 || frames == 30 ||
            frames == 50 || frames == 80) {
            constexpr uint64_t kCloudcellResumeDelta = 0x00A3F4C4ULL;
            using CloudcellResumeFn = void (*)(void*, void*);
            const uintptr_t cloudcellResume = renderTarget + kCloudcellResumeDelta;
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "call-cloudcell-resume", 93,
                (static_cast<unsigned long long>(frames) << 32) |
                (cloudcellResume & 0xFFFFFFFFULL));
            reinterpret_cast<CloudcellResumeFn>(cloudcellResume)(
                jniEnvironment, activityObject);
        }

        const uint32_t glError = static_cast<uint32_t>(glGetError());
        if (glError != 0 && glError != lastGlError) {
            lastGlError = glError;
            if (progressPath != nullptr) {
                writeGuestBootProgress(progressPath, "frame-loop-gl-error",
                                       73, glError);
            }
        }
        if (frames % 30 == 0) {
            if (progressPath != nullptr) {
                writeGuestBootProgress(progressPath, "frame-loop-heartbeat",
                                       74, frames);
            }
            writeGuestViewState(progressPath, renderTarget,
                                static_cast<unsigned long long>(77 + frames / 30));
        }

        // ---- PRESENT THE GUEST'S FBO ---------------------------------------
        //
        // eglSwapBuffers presents the DEFAULT framebuffer. The guest does not
        // draw into it: gui16 logged
        //     bindFramebuffer target=0x8ca9 fbo=0 status=0x0
        //     bindFramebuffer target=0x8d40 fbo=1 status=0x8cd5   (0x8CD5 =
        //                                                         FRAMEBUFFER_COMPLETE)
        //     bindFramebuffer target=0x8d40 fbo=2 status=0x8cd5
        //     bindFramebuffer target=0x8d40 fbo=3 status=0x8cd5
        //     ... guest-activity: fbo=26/0   (26 binds, 0 completeness checks)
        // so it renders offscreen and leaves an FBO bound. Nothing ever blits
        // that to fb0, which is exactly what the full-buffer readback shows:
        //     EARLY frame 30   lit=0 of 921600
        //     LATE  frame 3000 lit=0 of 921600
        // with viewport=0,0,1280,720 and scissor=0,0,1280,720 both confirmed
        // full-screen, so nothing is being clipped. Everything else about the
        // frame is healthy -- 128 mmaps, 78 opens, 95 programs, 191 compressed
        // textures, 6 drawArrays. The pixels exist; they are in a framebuffer
        // nobody presents.
        //
        // Acceptance, fixed before running:
        //   lit > 0   -> this was it, and the game is on screen
        //   lit == 0  -> the guest blits through a path we do not observe, or
        //                renders nothing at all, and the FBO is not the cause
        //
        // Guarded on the backend: with backend=null glBlitFramebuffer is a
        // counted no-op and lastBoundFramebuffer stays 0, so this is inert on
        // the historical path and cannot change that behaviour.
        //
        // ES3 entry point, resolved by hand. The port compiles against
        // <GLES2/gl2.h>, which has no glBlitFramebuffer / GL_*_FRAMEBUFFER, and
        // pulling in <GLES3/gl3.h> alongside it is a header-conflict fight for
        // two enum values. eglGetProcAddress already proved it hands back real
        // pointers on this Mesa (gui4 resolved eglGetPlatformDisplay through
        // it), so ask for it the same way and skip the blit if it is absent.
        if (!gfxNullActive() && guestViewGlStats().lastBoundFramebuffer != 0) {
            using PFNBlit = void (*)(GLint, GLint, GLint, GLint,
                                     GLint, GLint, GLint, GLint,
                                     GLbitfield, GLenum);
            static constexpr GLenum kReadFramebuffer = 0x8CA8;
            static constexpr GLenum kDrawFramebuffer = 0x8CA9;
            static PFNBlit blit = nullptr;
            static bool blitResolved = false;
            if (!blitResolved) {
                blitResolved = true;
                blit = reinterpret_cast<PFNBlit>(
                    eglGetProcAddress("glBlitFramebuffer"));
                if (blit == nullptr) {
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "fbo-present: glBlitFramebuffer unavailable, skipping",
                        249, 0);
                }
            }
            if (blit != nullptr) {
                const GLuint presentFbo = guestViewGlStats().lastBoundFramebuffer;
                glBindFramebuffer(kReadFramebuffer, presentFbo);
                glBindFramebuffer(kDrawFramebuffer, 0);
                blit(0, 0, 1280, 720, 0, 0, 1280, 720,
                     GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }

        if (eglSwapBuffers(display, surface) != EGL_TRUE) {
            recordEglFailure(probe);
            g_frameLoopExitReason = "swap-failed";
            logFrameLoopExit(g_frameLoopExitReason, "primary-swap", frames,
                             probe, renderTarget);
            break;
        }
        else if (guestViewGlStats().drawElements == 0 &&
                 guestViewGlStats().drawArrays == 0) {
            // Guest never issues any draws. Paint the host quad so the
            // device screen is a positive GL test (magenta/black) instead of
            // Mesa's cyan undefined-texture checkerboard.
            hostDrawMagentaQuad();
            if (eglSwapBuffers(display, surface) != EGL_TRUE) {
                recordEglFailure(probe);
                g_frameLoopExitReason = "host-quad-swap-failed";
                logFrameLoopExit(g_frameLoopExitReason, "host-quad-swap",
                                 frames, probe, renderTarget);
                break;
            }
        }
        ++frames;
        if ((padGetButtonsDown(&framePad) & HidNpadButton_Plus) != 0) {
            probe.frameLoopActive = false;
            g_frameLoopExitReason = "plus-button";
        }
        svcSleepThread(16666666LL); // ~60Hz
        if (frames >= 360000ULL) { // defensive cap ~100 minutes
            g_frameLoopExitReason = "frame-cap";
            logFrameLoopExit(g_frameLoopExitReason, "frame-cap", frames,
                             probe, renderTarget);
            break;
        }
    }
    // The two break sites above have already logged with their own site names.
    // The "loop-condition" line that closes the loop is written by the caller,
    // after it has read frames back, so there is exactly one of it either way.
    stopGuestPcSampler();
    context->done.store(true, std::memory_order_release);
    return nullptr;
}

} // namespace

// Bring a GL context up BEFORE the guest's early initialisation gets a chance
// to ask what GL it is running on.
//
// Why this exists. The guest samples glGetString(GL_VERSION) exactly once, in
// 0x128a17c: glGetString -> strstr("OpenGL ES") -> sscanf("%d.%d") into
// this->0xc (major) and this->0x10 (minor), on an object of class
// 11ndSingletonI11mtGLWrapperE. That sample happens during the guest's early
// onCreateJNI, which runs inside initializeGuestDependencies at
// main.cpp:1110. The host did not build or make current an EGL context until
// runGuestViewProbe at main.cpp:1176 -- 66 lines later. With no current
// context Mesa returns NULL, sscanf never runs, and the major version stays 0
// for the rest of the process. Every later check of that field -- including
// the GL assert guard at 0x14851e8, whose silent-return branch is
// "major < 3" -- is therefore permanently on the wrong side.
//
// Why it is a no-op for the view probe. The context here is SURFACELESS:
// EGL_NO_SURFACE for draw and read. The guest's own EGL extension list
// includes EGL_KHR_surfaceless_context, and nothing about the frame loop needs
// a window here -- it exists only so that glGetString answers. No surface is
// taken on the NWindow, so runGuestViewProbe's own ladder is untouched and
// cannot end up with two surfaces on one window. This function unbinds and
// destroys what it made, leaving the process exactly as it found it.
bool ensureEarlyGuestGlContext() {
    if (!gfxEarlyGlContext()) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "disabled by conf");
        return false;
    }
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        // Something already made one current. Do not stack a second context on
        // top of it; the version sample that follows will be answered anyway.
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "already current, skipped");
        return true;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "failed: no display");
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "failed: initialize");
        return false;
    }
    constexpr EGLint configAttributes[] = {
        EGL_RED_SIZE, 8,   EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig config{};
    EGLint configCount = 0;
    if (eglChooseConfig(display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
        configCount == 0) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "failed: chooseConfig");
        eglTerminate(display);
        return false;
    }
    constexpr EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "failed: createContext");
        eglTerminate(display);
        return false;
    }
    // Surfaceless on purpose -- see the header comment.
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-ctx-early", 110, 0, "failed: makeCurrent");
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    // Answer the question the guest is about to ask, while a context exists,
    // so the failure is visible here rather than as a mystery 0 later.
    const char* version = reinterpret_cast<const char*>(
        glGetString(GL_VERSION));
    char diag[192];
    __builtin_snprintf(diag, sizeof(diag), "surfaceless ok, glGetString=%s",
                       version != nullptr ? version : "(null)");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "gl-ctx-early", 110, 0, diag);

    // Leave the process as we found it: the view probe builds its own
    // windowed context for the frame loop, and it must not inherit this one.
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return true;
}

} // namespace rr3
