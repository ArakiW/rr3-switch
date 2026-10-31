//
// THE RACE-STATE PROBE.
//
// The guest's HUD telemetry is never drawn, so the lap counter cannot be read
// off the screen. This file reads it out of the guest's own memory, using
// the class and the member offsets that were read out of the guest BINARY.
//
// THE EVIDENCE FOR EVERY CONSTANT IN HERE
// ---------------------------------------
// The guest binary is stripped (no .symtab), so every name below was
// recovered from the image itself:
//
//  * The class NAMES come from the Itanium RTTI: every __class_type_info
//    object's __name slot (offset +8) points at a mangled name, and every
//    vtable points at that typeinfo. Walking .rela.dyn's R_AARCH64_RELATIVE
//    relocations turns either direction into a lookup (see
//    native-switch/tools/guest_analysis/rtti_vtable.py).
//
//      0x1e98168  "8GameMode"                      (vptr)
//      0x1e4b378  "19RuleSet_RaceMetrics"         (vptr)
//      0x1e4b3f8  "27RuleSetModifier_RaceMetrics" (vptr)
//
//  * The MEMBER OFFSETS come from the guest's own member-registration
//    function, which names each member as it registers it (see
//    native-switch/tools/guest_analysis/member_table.py). The function is
//    the RuleSetModifier_RaceMetrics constructor at ELF 0xb35a30:
//
//      0xb35ab8  add x0, x19, #0x20  ; "m_nTotalLaps"   -> +0x20
//      0xb35af4  add x20, x19, #0x38 ; "m_nPreviousSplineDistance" -> +0x38
//      0xb35b4c  add x20, x19, #0x50 ; "m_nTotalSplineDistanceTravelled" -> +0x50
//      0xb35bb0  add x0, x19, #0x68 ; "m_nTopGameSpeed" -> +0x68
//      0xb35bf4  add x0, x19, #0x88 ; "m_nTotalTime"    -> +0x88
//
//    and the member before them, at ELF 0xb35a5c, is "m_nLapDistance",
//    registered against the object's address AFTER the vtable store
//    ("str x9, [x0], #8" at 0xb35a68), i.e. +0x08.
//
//  * The CHAIN is the guest's own code:
//
//      0x10af2a0  ldr x0, [x23, #0xa10] ; bl 0xf168b4 (GameMode ctor)
//                  -> the GameMode is built IN PLACE at G+0xa10
//      0xf168b4  the GameMode constructor
//      0xf16bf8  bl 0xb358f4 (RuleSet_RaceMetrics ctor)
//      0xf16c08  bl 0xfea208 -> map.insert(key, metrics)
//                  -> the map lives at GameMode+0x128
//      0xb35950  str x22, [x9], #8   -> the metrics object goes into the
//                  vector at RuleSet_RaceMetrics+0x10, whose inline storage
//                  starts at +0x40
//
// WHY THE PROBE SEARCHES FOR THE OBJECT INSTEAD OF FOLLOWING THE CHAIN
// ---------------------------------------------------------------------
// Because a blind chain through the guest's own std::map is a chain through
// the guest's own node layout, and the GameMode is built at TWO places in
// the guest (G+0xa10 and G+0x1d88). So the probe walks the object graph and
// accepts an object when it can PROVE the object's identity: an object's
// first word is its vptr, and the vtable address was derived from the
// class's own RTTI. So "the object whose vptr is 0x1e4b3f8" is not a guess
// about a layout: it is a statement about a class, and the class has a name.
//
// THE POSITIVE CONTROL
// --------------------
// Every line this file writes carries the vptr it matched and the class
// name that vptr belongs to, so a reader can check the identity claim
// against the guest binary without running anything.
//
// THE CLOSED LOOP
// ---------------
// The steering here steers on the guest's OWN progress metric (the distance
// the guest has measured along the track's spline). A negative rate means
// the car is travelling against the spline's direction, which on a closed
// track means it is going round the wrong way, so the steering sign is
// flipped. A stall (no progress for a while) means the car is not moving
// and the recovery kicks in. The magnitude is servoed: a bigger steering
// angle gives a bigger radius, and the servo looks for the biggest radius
// that still keeps the progress rate up.
//
#include "rr3/race_state_probe.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/member_value_store.hpp"
#include "rr3/race_metrics_probe.hpp"
#include "rr3/recovery_guard.hpp"

#include <switch.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace {

// ---- the guest's layout, from the guest's own code ----------------------
//
// The GOT slot that holds the .bss CELL -> G. The same slot the input-door
// probe reads (rr3_InputDoors.cpp: kCGlobalGotSlotVa = 0x1f6d5d8).
constexpr uintptr_t kCGlobalGotSlotVa = 0x1f6d5d8ULL;
// The vtable ADDRESS POINTS (the value an object's first word holds).
//
// kGameModeVptrElfVa is the address point of the ABSTRACT BASE class
// GameMode. It is kept ONLY as a negative control: the guest instantiates
// 34 DERIVED GameMode classes (SoloMode, StandardRaceMode, TimeTrialMode,
// ...) and never a bare GameMode, so a live object's first word is never
// this value. The probe no longer searches for it; it follows the pointer
// the guest itself stores and reads the vptr only to say which mode it is.
constexpr uintptr_t kGameModeVptrElfVa = 0x1e98168ULL;
// These two ARE exact: neither class has any derived class, so the address
// point of the class is the address point of every live instance.
constexpr uintptr_t kRuleSetRaceMetricsVptrElfVa = 0x1e4b378ULL;
constexpr uintptr_t kRaceMetricsVptrElfVa = 0x1e4b3f8ULL;
// The GameMode is a POINTER MEMBER of the CGlobal.
//
// THE PROVEN OFFSET IS 0x10d88. Two independent readings of the guest agree:
//   the teardown, ELF 0x10af1d4 (x19 = the CGlobal, this):
//     mov  w8, #0x378 ; movk w8, #1, lsl #16   ; w8 = 0x10378
//     add  x23, x19, x8
//     ldr  x1, [x23, #0xa10]                  ; -> CGlobal + 0x10d88
//   the mode creation, ELF 0xb08960 (x19 = the CGlobal, this):
//     mov  w8, #0xbb0 ; movk w8, #1, lsl #16   ; w8 = 0x10bb0
//     add  x23, x19, x8
//     str  x20, [x23, #0x1d8]                 ; -> CGlobal + 0x10d88
//
// NOTE THE ARITHMETIC: a mov/movk pair writes the LOW half first and the
// high half second, so 0x378 + (1 << 16) is 0x10378, NOT 0x1378. A first
// reading of this code dropped the 0x10000 and produced 0x1d88, which is
// the offset this probe used to carry.
//
// The probe tries all three offsets the guest's own code mentions as
// pointer members of the CGlobal and takes the first one that really leads
// to a RuleSet_RaceMetrics, so a mis-read of the guest's arithmetic cannot
// turn into a silent "not found".
constexpr uintptr_t kCGlobalGameModeOff = 0x10D88ULL;
constexpr uintptr_t kGameModeCandidateOffsets[] = {
    0x10D88ULL,   // proven by two independent readings
    0x1D88ULL,    // the mis-read (the 0x10000 dropped)
    0x173F0ULL,   // the pointer member the teardown reads at ELF 0x10af224
};
constexpr int kGameModeCandidateCount =
    static_cast<int>(sizeof(kGameModeCandidateOffsets) / sizeof(uintptr_t));
// The guest's own map insert (ELF 0xfea280) gives the node layout, and it
// is libc++'s (the guest links libc++_shared.so):
//   node+0x00 left child, node+0x08 right child, node+0x10 parent,
//   node+0x18 colour, node+0x20 the key (std::string), node+0x38 the value
//   (a RuleSet*), and the node is 64 bytes ("mov w0, #0x40").
constexpr uintptr_t kMapNodeLeftOff = 0x00ULL;
constexpr uintptr_t kMapNodeRightOff = 0x08ULL;
constexpr uintptr_t kMapNodeKeyOff = 0x20ULL;
constexpr uintptr_t kMapNodeValueOff = 0x38ULL;
// The key the guest inserts the race metrics under: the global at 0x1f81250
// holds the C string "racemetrics" and the insert at 0xf16b7c loads it.
// The probe reports where the string is found inside the key rather than
// assuming libc++'s short-string layout, because the layout is the one
// thing here that is NOT proved by the guest's own code.
constexpr char kRaceMetricsMapKey[] = "racemetrics";
// The member offsets inside RuleSetModifier_RaceMetrics.
//
// WHAT IS ACTUALLY STORED AT THESE OFFSETS IS NOT THE VALUE.
//
// The guest registers each member with a NAME, and the registration call
// (ELF 0x996ffc) writes a 16-byte DESCRIPTOR over the member's slot:
//
//     0x997074  str  x24, [x19]      ; *(this+OFF) = a unique id
//     0x9970a0  bl   0x15c3640      ; hash of the member's name
//     0x9970a4  str  x0, [x19, #8]  ; *(this+OFF+8) = the name hash
//
// and the id is a freshly minted unique number, not a pointer:
//
//     0x991edc  ldr  x0, [x8, #0x690] ; a counter
//     0x991ee0  bl   0x10a24dc
//     0x991eec  ldr  x0, [x8, #0x698] ; another counter
//     0x991ef0  bl   0x10a24dc
//     0x991ef8  bfi  x0, x19, #0x20, #0x20 ; id = (A << 32) | B
//
// So the value of m_nTotalLaps is NOT at this+0x20: the slot holds the
// member's identity, and the VALUE lives in a central store keyed by that
// identity. The registry is a .bss singleton at ELF 0x1f97980 (its id map
// is at +0xc0).
//
// That is why the readings look like noise: they are unique ids and name
// hashes, which change on every registration and are spread uniformly over
// the whole 32-bit range. It is not a lap counter read at the wrong
// offset; it is a descriptor read where the value was expected.
//
// The probe therefore REPORTS these slots as descriptors, not as lap
// counts, and does not drive on them. Finding the value store is the open
// question.
constexpr uintptr_t kOffLapDistance = 0x08ULL;
constexpr uintptr_t kOffTotalLaps = 0x20ULL;
constexpr uintptr_t kOffPreviousSpline = 0x38ULL;
constexpr uintptr_t kOffTotalSpline = 0x50ULL;
constexpr uintptr_t kOffTopSpeed = 0x68ULL;
constexpr uintptr_t kOffTotalTime = 0x88ULL;
// The registry singleton that owns the ids, in the guest's .bss.
constexpr uintptr_t kMemberRegistryBssVa = 0x1F97980ULL;
// The vtable range in the guest image (.data.rel.ro), so that a word which
// is a vptr can be told apart from a word which is a heap pointer.
constexpr uintptr_t kVtableSectionLo = 0x1e1aae0ULL;
constexpr uintptr_t kVtableSectionHi = 0x1f6d280ULL;

// THE SCAN: how far around the CGlobal to look for the GameMode, and how
// many candidates to examine.
//
// WHY A SCAN AT ALL.  MEASURED 2026-10-28 (.build/gamemode-holder.md):
// the guest's OWN slot for the GameMode is CGlobal+0x10d88 -- the guest
// reads it itself at ELF 0xf152a4 (`ldr x19, [x0, x8]` with x8 = 0x10d88
// and x0 = the CGlobal taken from the GOT slot 0x1f6d5d8) and then makes
// a vtable call through it.  So the probe's first candidate is RIGHT.
//
// But that slot is 0 in every one of the 1195/1204 (f1m2) and 837/846
// (f1n) race-state log lines: the guest only stores the GameMode there
// between creating it (0xb088fc) and destroying it (0x10af2e4), and the
// creation is conditional (0xb0895c `cbnz` skips it wholesale).
//
// The lap counter does NOT read that slot.  It reaches a GameMode through a
// different chain, and the object it comes from is not a CGlobal member at
// all: the holder is the first argument of ELF 0xf2b508, and one of that
// function's two call sites passes a STACK object (0x1522c40
// `add x0, sp, #0x148`).  So "CGlobal + some offset" is the wrong question
// for the lap counter.
//
// THE SCAN IS THE QUESTION TURNED INTO A MEASUREMENT.  The guest's own
// shape for a GameMode is: a pointer V whose first word is a vtable address
// point and whose ruleset map lives at V+0x128.  So the probe walks the
// CGlobal's neighbourhood, takes every pointer it finds there, and keeps
// the one whose vptr is a vtable AND whose map really yields the
// RuleSet_RaceMetrics.  The check is the guest's own map walk, so a
// candidate that passes it is a GameMode by the guest's own definition.
//
// THE POSITIVE CONTROL IS THE SAME SCAN: it must also find the slot the
// guest reads, CGlobal+0x10d88, whenever that slot is non-zero.  A scan
// that cannot find a GameMode the guest itself is using would be a scan
// that proves nothing, so the control is reported next to the result.
constexpr uintptr_t kGameModeScanSpan = 0x20000ULL;
// How many vptr-passing candidates to try.  Every candidate costs a map
// walk, so the bound keeps the scan from turning into a stall.
constexpr int kGameModeScanMaxCandidates = 4096;

constexpr const char* kGuestViewLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";
constexpr const char* kDriveControlPath =
    "sdmc:/switch/rr3-native-rehost/rr3-drive.txt";

// ---- safe reads ----------------------------------------------------------
//
// A pointer the guest published can point at a page that is no longer there.
// A diagnostic that can crash the run it is measuring is worse than no
// diagnostic, so every read that is not inside the guest image goes through
// svcQueryMemory first.
//
// THE PAGE CACHE IS 64 ENTRIES, NOT 8. The object-graph walk touches many
// pages in a row, and this project has already measured that a probe
// spending ~1500 svcQueryMemory calls per sample dropped a run to 3120
// frames, so the syscall count -- not the read count -- is the budget.
constexpr uintptr_t kPlausibleLo = 0x10000ULL;
constexpr uintptr_t kPlausibleHi = 0x0000800000000000ULL;

bool pointerPlausible(uintptr_t address) {
    return address >= kPlausibleLo && address < kPlausibleHi;
}

bool memoryReadable(uintptr_t address) {
    constexpr uintptr_t kPageMask = ~static_cast<uintptr_t>(0xFFF);
    const uintptr_t page = address & kPageMask;
    static uintptr_t s_page[64] = {0};
    static bool s_readable[64] = {false};
    static bool s_valid[64] = {false};
    int hit = -1;
    int freeSlot = -1;
    for (int i = 0; i < 64; ++i) {
        if (s_valid[i] && page == s_page[i]) {
            hit = i;
            break;
        }
        if (freeSlot < 0 && !s_valid[i]) {
            freeSlot = i;
        }
    }
    if (hit >= 0) {
        return s_readable[hit];
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    bool readable = false;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        readable = false;
    } else {
        readable = (info.perm & Perm_R) != 0;
    }
    const int slot = (freeSlot >= 0) ? freeSlot : 0;
    s_page[slot] = page;
    s_readable[slot] = readable;
    s_valid[slot] = true;
    return readable;
}

bool readWord(uintptr_t address, uint64_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint64_t))) {
        if (!pointerPlausible(address) || !memoryReadable(address)) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
}

bool readDword(uintptr_t address, uint32_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint32_t))) {
        if (!pointerPlausible(address) || !memoryReadable(address)) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint32_t*>(address);
    return true;
}

bool readFloat(uintptr_t address, float* out) {
    uint32_t bits = 0;
    if (!readDword(address, &bits)) {
        return false;
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
}

// ---- the CGlobal ---------------------------------------------------------
//
// The GOT slot lives inside the guest image, so it is mapped for the whole
// run and can be read directly. Its value is the .bss CELL (a guest
// pointer), and the cell holds G.
uintptr_t cGlobalObject() {
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        return 0;
    }
    const uintptr_t slot = bias + kCGlobalGotSlotVa;
    if (!guestImageContains(slot, sizeof(uint64_t))) {
        return 0;
    }
    const uint64_t cell =
        *reinterpret_cast<const volatile uint64_t*>(slot);
    if (cell == 0 || !pointerPlausible(static_cast<uintptr_t>(cell))) {
        return 0;
    }
    uint64_t g = 0;
    if (!readWord(static_cast<uintptr_t>(cell), &g) || g == 0) {
        return 0;
    }
    return static_cast<uintptr_t>(g);
}

// ---- the map, as the guest's own map code lays it out -------------------
//
// The GameMode owns a std::map<std::string, RuleSet_RaceMetrics*> at
// GameMode+0x128. The guest's own insert function (ELF 0xfea280) gives the
// layout, and the value's offset is proved by the store itself:
//
//   0xfea2b4  add  x8, x20, #8      ; the map is at x20
//   0xfea2b8  cmp  x8, x0           ; the tree is empty when x0 == map+8
//   0xfea2c4  mov  x0, x20
//   0xfea368  mov  w0, #0x40        ; a node is 64 bytes
//   0xfea374  add  x0, x0, #0x20    ; the key (a 24-byte string) at node+0x20
//   0xfea384  str  xzr, [x22, #0x38] ; the value slot is node+0x38
//   0xfea3c4  str  x19, [x22, #0x38] ; ... and the value is stored there
//   0xfea418  ldr  x0, [x0, #0x38]    ; and read back from there
//
// So: the root node is at map+8 (the map object at GameMode+0x128, so the
// root node pointer is at GameMode+0x130), and the RuleSet_RaceMetrics
// pointer is at root+0x38.
//
// THE MAP HOLDS MORE THAN ONE ENTRY. The guest also inserts a ruleset under
// the key "grid" (StandardRaceMode's constructor, ELF 0x103dae8), so the
// ROOT NODE IS NOT NECESSARILY the race metrics one. The probe therefore
// WALKS THE WHOLE TREE and picks the node whose VALUE points at a
// RuleSet_RaceMetrics, instead of assuming the root is it.
//
// The node layout is libc++'s -- the guest links libc++_shared.so -- and the
// node is 64 bytes, so with the key (a 24-byte std::string) at node+0x20 and
// the value at node+0x38, the two child pointers are the first two words.
constexpr uintptr_t kGameModeMapOff = 0x128ULL;
constexpr uintptr_t kMapRootOff = 0x08ULL;
// The RuleSet_RaceMetrics object is 0xe0 bytes (the guest allocates it with
// operator_new(0xe0) at ELF 0xf16be8).
constexpr uintptr_t kRuleSetRaceMetricsBytes = 0xE0ULL;
// The metrics modifier is built IN PLACE inside the RuleSet_RaceMetrics at
// +0x40 (the guest's constructor, ELF 0xb35938, constructs it at this+0x40).
constexpr uintptr_t kRuleSetMetricsInlineOff = 0x40ULL;

// ---- the heap scan -------------------------------------------------------
//
// The probe's own state. It is declared HERE, before the heap scan, because
// the heap scan fills it in.
uintptr_t g_gameMode = 0;
uintptr_t g_ruleSetRaceMetrics = 0;
uintptr_t g_metrics[4] = {0, 0, 0, 0};
int g_metricsCount = 0;
uintptr_t g_heapBase = 0;
uintptr_t g_heapSize = 0;
// The live GameMode's vptr, and whether it is (implausibly) the abstract
// base class's address point. See kGameModeVptrElfVa.
uintptr_t g_gameModeVptr = 0;
bool g_gameModeIsBase = false;
// Which of the candidate offsets actually led to the GameMode, and what
// every candidate held. A reader can see which offsets were tried and what
// they contained, so "not found" is never a black box.
int g_gameModeCandidate = -1;
uintptr_t g_candidatePointers[4] = {0, 0, 0, 0};
uintptr_t g_candidateVptrs[4] = {0, 0, 0, 0};
// How the map walk went: how many nodes it visited, and where inside the
// key's 24 bytes the string "racemetrics" was found (or -1).
int g_mapNodesVisited = 0;
int g_mapKeyOffset = -1;
uintptr_t g_cglobal = 0;

// Forward declaration: the heap scan below uses the pointer filter, which is
// defined after it.
bool looksLikePointer(uintptr_t bias, uint64_t word);
void logGameModeSlots(unsigned long long frame);

//
// The metrics object is HEAP allocated (the guest's operator new), so it is
// not inside the guest image and not inside the CGlobal. The one way to
// find it without a pointer chain is to scan the region the guest's own
// allocations live in.
//
// THE REGION IS ASKED FOR, NOT GUESSED: svcQueryMemory on a known guest
// object returns the region that contains it, and the guest's allocations
// are all in that region. ONE syscall, then a linear scan of a region the
// kernel says is readable -- so the scan is a plain read of hot memory with
// no syscall per page.
uintptr_t scanRegionForVptr(uintptr_t regionBase, uintptr_t regionSize,
                            uintptr_t vptrValue) {
    if (regionBase == 0 || regionSize < 8) {
        return 0;
    }
    // A hard cap, so a huge region cannot turn the probe into a stall. The
    // guest's metrics object is allocated early in the race, so the start of
    // the region is where it is.
    constexpr uintptr_t kMaxScanBytes = 192ULL * 1024ULL * 1024ULL;
    uintptr_t limit = regionSize;
    if (limit > kMaxScanBytes) {
        limit = kMaxScanBytes;
    }
    const uintptr_t end = regionBase + limit;
    for (uintptr_t at = regionBase; at + 8 <= end; at += 8) {
        const uint64_t word =
            *reinterpret_cast<const volatile uint64_t*>(at);
        if (word == vptrValue) {
            return at;
        }
    }
    return 0;
}

// Finds the metrics object in the guest's heap. The vptr VALUE is the
// identity: an object of this class has that exact first word, and the
// vtable address came from the class's own RTTI.
bool findMetricsInHeap(uintptr_t bias, uintptr_t g) {
    // The anchors are the objects the guest's own state points at, because
    // the kernel reports the region that CONTAINS an address, and the
    // guest's allocations are spread over a few regions. Eight anchors is
    // eight syscalls and, at worst, eight scans of a region of hot memory.
    uintptr_t anchors[8] = {g, 0, 0, 0, 0, 0, 0, 0};
    int anchorCount = 1;
    uint64_t word = 0;
    if (readWord(g + 0x1818ULL, &word)) {
        anchors[anchorCount++] = static_cast<uintptr_t>(word);
    }
    word = 0;
    if (readWord(g + 0x17f8ULL, &word)) {
        anchors[anchorCount++] = static_cast<uintptr_t>(word);
    }
    // The CGlobal's own pointers: the objects it owns are allocated by the
    // same allocator as the guest's race objects.
    for (size_t off = 0; off + 8 <= 0x800ULL && anchorCount < 8; off += 8) {
        word = 0;
        if (!readWord(g + off, &word)) {
            continue;
        }
        if (!looksLikePointer(bias, word)) {
            continue;
        }
        anchors[anchorCount++] = static_cast<uintptr_t>(word);
    }

    uintptr_t seenBase[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int seenCount = 0;
    for (int i = 0; i < anchorCount; ++i) {
        const uintptr_t anchor = anchors[i];
        if (anchor == 0 || !pointerPlausible(anchor)) {
            continue;
        }
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(anchor)))) {
            continue;
        }
        if ((info.perm & Perm_R) == 0) {
            continue;
        }
        const uintptr_t base = static_cast<uintptr_t>(info.addr);
        const uintptr_t size = static_cast<uintptr_t>(info.size);
        bool already = false;
        for (int k = 0; k < seenCount; ++k) {
            if (seenBase[k] == base) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        if (seenCount < 8) {
            seenBase[seenCount++] = base;
        }
        g_heapBase = base;
        g_heapSize = size;
        // The metrics object itself.
        const uintptr_t hit = scanRegionForVptr(base, size,
                                                bias + kRaceMetricsVptrElfVa);
        if (hit != 0) {
            g_metrics[0] = hit;
            g_metricsCount = 1;
            return true;
        }
        // Or the GameMode itself: it is heap allocated too, and its own map
        // at +0x128 holds the metrics.
        const uintptr_t mode = scanRegionForVptr(base, size,
                                                  bias + kGameModeVptrElfVa);
        if (mode != 0) {
            g_gameMode = mode;
            uint64_t rootWord = 0;
            if (readWord(mode + kGameModeMapOff + kMapRootOff, &rootWord) &&
                rootWord != 0) {
                const uintptr_t node = static_cast<uintptr_t>(rootWord);
                uint64_t value = 0;
                if (readWord(node + kMapNodeValueOff, &value) && value != 0) {
                    uint64_t head = 0;
                    if (readWord(static_cast<uintptr_t>(value), &head) &&
                        head == bias + kRuleSetRaceMetricsVptrElfVa) {
                        const uintptr_t owner =
                            static_cast<uintptr_t>(value);
                        g_ruleSetRaceMetrics = owner;
                        for (size_t off = 0;
                             off + 8 <= kRuleSetRaceMetricsBytes; off += 8) {
                            uint64_t inner = 0;
                            if (!readWord(owner + off, &inner)) {
                                continue;
                            }
                            if (inner == bias + kRaceMetricsVptrElfVa) {
                                g_metrics[0] = owner + off;
                                g_metricsCount = 1;
                                return true;
                            }
                            if (!looksLikePointer(bias, inner)) {
                                continue;
                            }
                            uint64_t head2 = 0;
                            if (!readWord(static_cast<uintptr_t>(inner),
                                          &head2)) {
                                continue;
                            }
                            if (head2 == bias + kRaceMetricsVptrElfVa) {
                                g_metrics[0] = static_cast<uintptr_t>(inner);
                                g_metricsCount = 1;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        // Or the owner of it, which carries it inside.
        const uintptr_t owner = scanRegionForVptr(base, size,
                                                  bias + kRuleSetRaceMetricsVptrElfVa);
        if (owner != 0) {
            g_ruleSetRaceMetrics = owner;
            for (size_t off = 0; off + 8 <= kRuleSetRaceMetricsBytes; off += 8) {
                uint64_t inner = 0;
                if (!readWord(owner + off, &inner)) {
                    continue;
                }
                if (inner == bias + kRaceMetricsVptrElfVa) {
                    g_metrics[0] = owner + off;
                    g_metricsCount = 1;
                    return true;
                }
                if (!looksLikePointer(bias, inner)) {
                    continue;
                }
                uint64_t head = 0;
                if (!readWord(static_cast<uintptr_t>(inner), &head)) {
                    continue;
                }
                if (head == bias + kRaceMetricsVptrElfVa) {
                    g_metrics[0] = static_cast<uintptr_t>(inner);
                    g_metricsCount = 1;
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- the pointer filter --------------------------------------------------
//
// A POINTER CANDIDATE is a word that could be the address of a guest object.
//
// IT MUST NOT CALL svcQueryMemory. This test runs on every word of every
// visited object, and this project has already measured that a probe
// spending ~1500 svcQueryMemory calls dropped a run to 3120 frames. So the
// filter here is arithmetic only, and the readability check happens ONCE,
// where the candidate is actually read.
//
// The lower bound matters as much as the syscall: a guest struct is full of
// small integers, and an integer that happens to be 8-aligned looks like a
// pointer. Nothing a guest object points at lives below 16 MiB, so a word
// below that is an integer.
constexpr uintptr_t kMinimumObjectAddress = 0x01000000ULL;

bool looksLikePointer(uintptr_t bias, uint64_t word) {
    if ((word & 7) != 0) {
        return false;
    }
    if (word < kMinimumObjectAddress || word >= kPlausibleHi) {
        return false;
    }
    // A vtable address is not a pointer to an object.
    if (word >= bias + kVtableSectionLo && word < bias + kVtableSectionHi) {
        return false;
    }
    return true;
}

// ---- reading a member's VALUE ---------------------------------------------
//
// The slot holds a DESCRIPTOR, not a number: a 64-bit member id at +0x00 and
// the name's hash after it.  The number lives in the guest's global value
// store, and the guest's own accessor (ELF 0x991c8c) is what normally puts
// it there -- but that accessor also RE-KEYS the member, so a probe must not
// call it.  The pure read is include/rr3/member_value_store.hpp, and it
// decodes the value the guest's way: ~(cell1 ^ cell2), out of tables 1 and 2,
// with tables 3 and 4 carrying the guest's own integrity check.
//
// The id is a ROTATING token: the guest re-keys the member every time it
// reads one, so the id is read fresh here and never cached.  The guest's own
// accessor has the same race between the two reads, so a miss is retried
// once before it is believed.
struct ValueReader {
    bool operator()(uintptr_t address, uint64_t* out) const {
        return readWord(address, out);
    }
};

bool readMemberValue(uintptr_t slotAddress, int32_t* value, uint64_t* idOut) {
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0 || value == nullptr) {
        return false;
    }
    const uintptr_t registry = bias + member_value_store::kRegistryBssVa;
    ValueReader reader;
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint64_t id = 0;
        if (!readWord(slotAddress, &id) || id == 0) {
            return false;
        }
        member_value_store::Lookup lookup{};
        const char* why = member_value_store::lookup(reader, registry, id, &lookup);
        if (why == nullptr) {
            *value = lookup.value;
            if (idOut != nullptr) {
                *idOut = id;
            }
            return true;
        }
    }
    return false;
}

// ---- the numeric reading -------------------------------------------------
//
// The guest's member names say "m_n..." (an integer) but the guest stores
// some of them as floats. The probe does not assume: it reads the 32-bit
// word BOTH ways and picks the one that is a number in a sane range. A
// float's bit pattern read as an integer is astronomically large, and an
// integer read as a float is a denormal, so the two are told apart by the
// size of the integer reading.
double numericValue(uint32_t raw, float asFloat) {
    const int64_t asInt = static_cast<int64_t>(static_cast<int32_t>(raw));
    if (asInt >= -100000000LL && asInt <= 100000000LL) {
        return static_cast<double>(asInt);
    }
    if (asFloat > -1.0e9f && asFloat < 1.0e9f) {
        return static_cast<double>(asFloat);
    }
    return static_cast<double>(asInt);
}

// ---- the drive control file ---------------------------------------------
//
// The closed loop is OFF unless the control file says so. It lives on the
// card at sdmc:/switch/rr3-native-rehost/rr3-drive.txt and understands:
//
//   enable=1|0   the closed loop drives (default 1 when the file exists)
//   probe=1|0    read the race state but do not steer (default 0)
//   base=<f>     the initial steering magnitude, 0..1 (default 0.30)
//   sign=1|-1    the initial steering direction (default 1)
//
// Every key is optional.
struct DriveConfig {
    bool present{};
    bool enable{true};
    bool probeOnly{false};
    float base{0.30f};
    int sign{1};
    // The pure-pursuit controller: the angle (in radians) that means "full
    // lock", and the sign of the axis.  A POSITIVE axis 0 steers RIGHT
    // (MEASURED: lx=+0.6 at the launch ended up on the right runoff,
    // .build/rr3-pad-mirror.txt), and the world frame's handedness is the
    // one thing here that is not measured, so the sign is a knob and one
    // round settles it.
    float steerGain{0.50f};
    int steerSign{1};
};

DriveConfig g_driveConfig;
bool g_driveConfigRead = false;

void readDriveConfig() {
    g_driveConfig = DriveConfig{};
    g_driveConfigRead = true;
    std::FILE* file = std::fopen(kDriveControlPath, "rb");
    if (file == nullptr) {
        return;
    }
    g_driveConfig.present = true;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* eq = std::strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char* key = line;
        const char* value = eq + 1;
        while (*key == ' ' || *key == '\t') {
            ++key;
        }
        char* keyEnd = key + std::strlen(key);
        while (keyEnd > key && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t' ||
                                keyEnd[-1] == '\r' || keyEnd[-1] == '\n')) {
            --keyEnd;
        }
        *keyEnd = '\0';
        if (std::strcmp(key, "enable") == 0) {
            g_driveConfig.enable = (std::strtol(value, nullptr, 10) != 0);
        } else if (std::strcmp(key, "probe") == 0) {
            g_driveConfig.probeOnly = (std::strtol(value, nullptr, 10) != 0);
        } else if (std::strcmp(key, "base") == 0) {
            g_driveConfig.base = std::strtof(value, nullptr);
            if (g_driveConfig.base < 0.0f) {
                g_driveConfig.base = 0.0f;
            }
            if (g_driveConfig.base > 1.0f) {
                g_driveConfig.base = 1.0f;
            }
        } else if (std::strcmp(key, "sign") == 0) {
            g_driveConfig.sign = (std::strtol(value, nullptr, 10) < 0) ? -1 : 1;
        } else if (std::strcmp(key, "steergain") == 0) {
            g_driveConfig.steerGain = std::strtof(value, nullptr);
            if (g_driveConfig.steerGain < 0.01f) {
                g_driveConfig.steerGain = 0.01f;
            }
            if (g_driveConfig.steerGain > 3.0f) {
                g_driveConfig.steerGain = 3.0f;
            }
        } else if (std::strcmp(key, "steersign") == 0) {
            g_driveConfig.steerSign = (std::strtol(value, nullptr, 10) < 0) ? -1 : 1;
        }
    }
    std::fclose(file);
}

// ---- the probe state -----------------------------------------------------
RaceStateSample g_sample;
RaceDriveState g_drive;
unsigned long long g_lastSearchFrame = 0;
unsigned long long g_lastLogFrame = 0;
double g_lastTotalValue = 0.0;
double g_lastLapValue = 0.0;
bool g_haveProgressValue = false;
bool g_sawProgress = false;
bool g_progressFromTotal = true;
double g_smoothedRate = 0.0;
double g_windowValue = 0.0;
unsigned long long g_windowFrame = 0;
unsigned long long g_lastProgressFrame = 0;
// THE PROGRESS REFERENCE, FOR THE THRESHOLD TEST.
//
// The old test was "did the value change AT ALL", and a car grinding
// against a barrier jitters the spline distance by a unit or two, so the
// port always believed it was moving.  Measured 2026-10-28 (archive
// 20261028-213445-eden-q8): the car sat against a wall for 18,270 frames
// and the stall counter never moved off 0, so the recovery that frees a
// car from a wall was unreachable.
//
// The guest's distance unit is 1/256 metre -- its own HUD label
// "Spline Distance (metres)" at ELF 0xb35814 multiplies the member by
// 1/256 -- so 256 units is one metre, and one metre per 240 frames is half
// a kilometre per hour.  Anything faster than that is progress.
constexpr double kProgressEpsilon = 256.0;
double g_lastProgressValue = 0.0;
bool g_progressMetricIsTotal = true;
// THE STEERING FLOOR AND THE RECOVERY ANGLE.
//
// The floor used to be 0.05, i.e. 5 % of the steering axis: measured on the
// guest's own controller state as ax[0] = +-0.050, which is 1/12 of the
// +-0.6 the port's own human-scale experiment used.  A car that cannot turn
// runs straight into the first corner's barrier.  The recovery angle is the
// same 0.6, held the OTHER way, while the car reverses.
constexpr float kDriveMagnitudeFloor = 0.30f;
constexpr float kDriveRecoveryMagnitude = 0.60f;
// How long the recovery holds the brake: ~7.3 s at 30.35 ms/frame.  The old
// value, 45 frames, is a TAP, and a tap does not reverse anything.
constexpr unsigned long long kDriveRecoveryFrames = 240ULL;
uint32_t g_lastTotalLaps = 0xFFFFFFFFu;
unsigned long long g_brakeUntilFrame = 0;
unsigned long long g_lastFlipFrame = 0;

// THE GUEST'S OWN PER-CAR STATE.
//
// The port could read how FAR the car had come along the spline, and that is
// enough to notice a car that is not moving -- but not enough to steer it,
// because one scalar cannot say "the road curves and I am not following it"
// apart from "I am off the road".  The guest keeps everything a lateral
// offset needs, and it is all reachable from the object the port already
// resolves.
//
// The chain, each step proved by the guest's own code:
//
//   RuleSet_RaceMetrics + 0x20    the CAR                    (ELF 0xb35c94)
//   CAR + 0x288                    the CAR_STATE             (ELF 0xb35ce8)
//   CAR_STATE + 0x1a8              the SPLINE NODE INDEX     (int)
//   CAR_STATE + 0x1c8              the DISTANCE TRAVELLED    (int)
//   CAR + 0x20 / +0x24             the previous/current spline node
//   CAR + 0x30/+0x34/+0x38         the car's WORLD POSITION
//
// THE LAST TWO ARE ASSUMED, NOT MEASURED, AND MEASUREMENT SHOWS THEM
// WRONG.  The port took them from the guest's DEBUG REGISTRATION function
// (0xd052e0), which describes a 68-BYTE-PER-RECORD ARRAY, not the CAR
// object itself.  MEASURED 2026-10-29, q21: cnode=0, cpos=2,33,0 and
// ctgt=0,0,0, while CAR_STATE's pair (snode, sdist) is right.  The CAR's
// own bytes are now dumped (phase=car-raw) so one round shows where the
// position really is; until then the pursuit error is identically zero and
// the port says so rather than steering on noise.
//
// Three independent cross-checks exist for the CAR_STATE pair: the writer
// 0xc2d888 (`ldr x8,[x0,#0x288]` then `str w4,[x8,#0x1a8]`), the
// projection helper snapToClosestNode at 0xd05e44, and 0xb29264/0xb29284.
//
// NOTE WHAT IS NOT HERE: the racing-line offset itself.  `m_acrossAngle` is
// NOT it -- that belongs to CarBodyPart_Swinging (a swinging body part's
// rigid-body ROLL angle, in degrees), and the guest computes the offset in
// its projection rather than storing it.  The position and the node index
// are what the offset would be computed FROM.
//
// The reads are the port's own safe reads (plausibility gate, guest-image
// check, svcQueryMemory, read) and nothing here writes anything.
void fillCarState(RaceMetricsSample* out) {
    if (g_ruleSetRaceMetrics == 0) {
        return;
    }
    uint64_t car = 0;
    if (!readWord(g_ruleSetRaceMetrics + 0x20ULL, &car) || car == 0) {
        return;
    }
    out->carAddress = static_cast<uintptr_t>(car);
    // THE CAR'S OWN WORLD POSITION.
    //
    // MEASURED 2026-10-30 by reading the guest's own code, and the numbers
    // are the guest's instructions:
    //
    //   CAR+0xc8 / +0xcc / +0xd0   the car's WORLD POSITION, 3 x int32 in
    //                               the guest's 1/256 metre unit, with Y
    //                               NEGATED (the converter at 0xf05600
    //                               does `mov w8,#-0x3c800000` before
    //                               scaling Y).
    //
    //   The writer is 0xc2d888: `str x9,[x0,#0xc8]` for x,y and
    //   `str w8,[x0,#0xd0]` for z, and its argument comes from 0xf05600,
    //   which reads a float3 in METRES and converts it.
    //
    //   The guest USES it as the position: 0xc2de4c reads w9 from
    //   [x8,#0xc8] and ldp w10,w11 from [x0,#0xcc]; the camera's
    //   snapToClosestNode at 0xd05e4c reads [x8,#0xc8] and [x8,#0xcc]
    //   for its spline projection.
    //
    //   The node index is at CAR+0x1a8 (0x109e258) as well as at
    //   CarState+0x1a8 (written by 0xc2d8dc).
    //
    // WHAT WAS WRONG BEFORE, AND WHY IT LOOKED RIGHT.  The port used
    // CAR+0x20/+0x24 for the node indices and CAR+0x30.. for the
    // position and CAR+0x3c.. for the target.  Those offsets are the
    // FOLLOW CAMERA's debug members, not the car's: the guest's debug
    // registration at 0xd052e0 registers m_nPrevNode (+0x20),
    // m_nCurrentNode (+0x24), m_nCamNum (+0x28),
    // m_savedCameraIndex (+0x2c), m_vPosition (+0x30..+0x38) and
    // m_vLastKnownTarget (+0x3c..+0x44) on the CAMERA CONTROLLER, and
    // that controller copies its position and target out of a 68-byte
    // camera-preset table (0xd05184: the preset base at x19+0x158, the
    // index at x19+0x2c, the stride 0x44).  The port read a camera's
    // presets and called them the car's.
    //
    // The camera's own object hangs off the car: the car's per-car debug
    // object is at CAR+0x448 (0xc27fe8), the camera is at +0x08 of it
    // and the camera's owner pointer is at +0x6a0 of it (0xd04f50).
    //
    // The AIM TARGET is a candidate, not a proof: CAR+0xfc/+0x104/+0x108
    // is a second 3 x int32 written by the same routine (0xc2d8c4) and
    // the same unit, and the guest compares CAR+0x78/+0x7c with the car's
    // position in 2D (0xc2dea4), so both are worth reading.  Neither is
    // used for steering until one of them is SEEN to move with the car.
    readDword(car + 0xc8ULL, reinterpret_cast<uint32_t*>(&out->carX));
    readDword(car + 0xccULL, reinterpret_cast<uint32_t*>(&out->carY));
    readDword(car + 0xd0ULL, reinterpret_cast<uint32_t*>(&out->carZ));
    readDword(car + 0xfcULL, reinterpret_cast<uint32_t*>(&out->carTargetX));
    readDword(car + 0x104ULL, reinterpret_cast<uint32_t*>(&out->carTargetY));
    readDword(car + 0x108ULL, reinterpret_cast<uint32_t*>(&out->carTargetZ));
    readDword(car + 0x1a8ULL, &out->carNode);
    // THE PLAUSIBILITY GATE.
    //
    // The offsets above were measured on the guest's own code, but the
    // port has to be able to say "this read is not a position" if it ever
    // stops being one.  A world position in the guest's 1/256 metre unit
    // is a signed integer: on a circuit of a few kilometres it is in the
    // tens of thousands, and it is NEVER all zero while the car is
    // driving.  So the gate is:
    //
    //   |x| < 400000        (<= about 1.5 km from the origin)
    //   |y| < 400000
    //   |z| <  20000        (<= about 78 m of altitude)
    //
    // and the verdict is reported in the log, so a run that read the wrong
    // object says so instead of driving on noise.
    out->positionPlausible =
        (out->carX > -400000 && out->carX < 400000) &&
        (out->carY > -400000 && out->carY < 400000) &&
        (out->carZ > -20000 && out->carZ < 20000);
    // The car's own number, which the guest reads at 0xb35ca8
    // (`ldr w1,[x9,#0x49c]`).  It is a cheap identity check: the value has
    // to be the same one the guest would read, and it changes when the
    // player changes car.
    readDword(car + 0x49cULL, &out->carNumber);
    // THE CAR'S OWN LAYOUT, DUMPED.
    //
    // MEASURED 2026-10-29, q21: the node index, the world position and the
    // aim target all read as zero or noise (cnode=0, cpos=2,33,0,
    // ctgt=0,0,0), while CAR_STATE's node index and distance ARE right
    // (snode=290, sdist=13508).  So the offsets the port uses on the CAR
    // are not the fields it thinks they are.
    //
    // Rather than guess again, this prints the CAR object's own bytes, the
    // same way the metrics modifier is dumped: 32-bit words with "%08x",
    // which byte-swaps each group of four relative to memory, so a reader
    // must undo it.  One round of this settles where the position is.
    //
    // It is a DIAGNOSTIC, and it is bounded: 0x120 bytes at a fixed rate.
    {
        // The probe is pumped once per frame by the frame loop, so a local
        // counter is the frame number.  It is a LOCAL static on purpose:
        // this file has no global pump counter, and borrowing one from
        // another translation unit would couple two unrelated files.
        static unsigned long long s_dumpAt = 0;
        static unsigned long long s_calls = 0;
        const unsigned long long now = ++s_calls;
        if (now >= s_dumpAt) {
            s_dumpAt = now + 600ULL;
            char raw[2048];
            int written = std::snprintf(raw, sizeof(raw), "car=0x%llx w=",
                                        static_cast<unsigned long long>(car));
            for (int off = 0x00; off < 0x120; off += 4) {
                if (written <= 0 || written >= static_cast<int>(sizeof(raw)) - 12) {
                    break;
                }
                uint32_t word = 0;
                readDword(car + static_cast<uintptr_t>(off), &word);
                written += std::snprintf(raw + written,
                                         sizeof(raw) - static_cast<size_t>(written),
                                         "%08x", word);
            }
            appendGuestBootLine(kGuestViewLogPath, "car-raw", 103, 0, raw);
        }
    }
    // The previous node index is the camera controller's, and the camera
    // controller is a DIFFERENT object from the car, so there is no
    // "previous node" on the car to read.  It stays 0.
    out->carPrevNode = 0;
    uint64_t carState = 0;
    if (!readWord(car + 0x288ULL, &carState) || carState == 0) {
        return;
    }
    out->trackStateAddress = static_cast<uintptr_t>(carState);
    readDword(carState + 0x1a8ULL, &out->splineNode);
    readDword(carState + 0x1c8ULL, &out->splineDistance);
}

void fillMetrics(RaceMetricsSample* out, uintptr_t address) {
    out->address = address;
    // THE Vptr IS READ, NOT ASSUMED. The previous version was handed the
    // EXPECTED address point and recorded that, so the log's "vptr_ok=1"
    // was a tautology: it said "the constant equals itself". The probe's
    // own identity check has to be a MEASUREMENT, or it checks nothing.
    uint64_t head = 0;
    if (readWord(address, &head)) {
        out->vptr = static_cast<uintptr_t>(head);
    } else {
        out->vptr = 0;
    }
    readDword(address + kOffLapDistance, &out->lapDistance);
    readDword(address + kOffTotalLaps, &out->totalLaps);
    readDword(address + kOffPreviousSpline, &out->previousSplineDistance);
    readDword(address + kOffTotalSpline, &out->totalSplineDistance);
    readDword(address + kOffTopSpeed, &out->topSpeed);
    readDword(address + kOffTotalTime, &out->totalTime);
    readFloat(address + kOffLapDistance, &out->lapDistanceF);
    readFloat(address + kOffTotalLaps, &out->totalLapsF);
    readFloat(address + kOffPreviousSpline, &out->previousSplineDistanceF);
    readFloat(address + kOffTotalSpline, &out->totalSplineDistanceF);
    readFloat(address + kOffTopSpeed, &out->topSpeedF);
    readFloat(address + kOffTotalTime, &out->totalTimeF);

    // THE VALUES, read out of the guest's value store.  The words above are
    // the member DESCRIPTORS (rotating ids and name hashes), which is why
    // they read as noise: the numbers the engine uses are elsewhere, and
    // the probe has to ask the value store for them the way the guest does.
    //
    // MEASURED 2026-10-29, q21: the VALUES are right (mres=0x3f, and the
    // guest's own class name comes back as 27RuleSetModifier_RaceMetrics),
    // so the SLOTS are the guest's own.  What the slots do NOT hold is the
    // member NAME - see the identity check below.
    //
    // A member that resolves replaces its descriptor reading; one that does
    // not keeps it, and the mask says which happened.
    //
    // THE GUEST'S OWN NAMES, AND A RUNTIME IDENTITY CHECK.
    //
    // Every member the port reads is listed with the name THE GUEST uses
    // for it, the offset it lives at, and the ELF address that proves it
    // (the metrics modifier registers its members in its constructor at
    // ELF 0xb35a30, the registration calls at 0xb35a5c..0xb35ba0).  Nothing
    // the port logs may be an inference.
    //
    // And the check is a MEASUREMENT, not a tautology.  The descriptor in
    // the slot is (the rotating id, the NAME HASH, len ^ hash), so the
    // name's LENGTH can be recovered from the guest's own bytes --
    // len = hash ^ (len ^ hash) -- and compared with the length of the name
    // the port believes is there.  A member that is NOT the one the port
    // thinks it is cannot pass that test.
    //
    // THE LIMIT, STATED PLAINLY: two of the names are the same length
    // ("m_nTotalLaps" and "m_nTotalTime", 12 characters each), so for those
    // two the length does not discriminate; the guest's full name hash would
    // settle it and is not implemented.
    struct MemberSlot {
        uintptr_t offset;
        uint32_t* raw;
        float* asFloat;
        const char* name;
    };
    const MemberSlot slots[] = {
        {kOffLapDistance, &out->lapDistance, &out->lapDistanceF, "m_nLapDistance"},
        {kOffTotalLaps, &out->totalLaps, &out->totalLapsF, "m_nTotalLaps"},
        {kOffPreviousSpline, &out->previousSplineDistance,
         &out->previousSplineDistanceF, "m_nPreviousSplineDistance"},
        {kOffTotalSpline, &out->totalSplineDistance, &out->totalSplineDistanceF,
         "m_nTotalSplineDistanceTravelled"},
        {kOffTopSpeed, &out->topSpeed, &out->topSpeedF, "m_nTopGameSpeed"},
        {kOffTotalTime, &out->totalTime, &out->totalTimeF, "m_nTotalTime"},
    };
    out->membersResolved = 0;
    out->membersIdentified = 0;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        int32_t value = 0;
        if (!readMemberValue(address + slots[i].offset, &value, nullptr)) {
            continue;
        }
        *slots[i].raw = static_cast<uint32_t>(value);
        *slots[i].asFloat = static_cast<float>(value);
        out->membersResolved |= (1u << i);
        // THE IDENTITY CHECK IS OFF, AND THE REASON IS MEASURED.
        //
        // MEASURED 2026-10-29, archive 20261029-174557-eden-rr3-q21-isolated-
        // calendar-fix: this check reported ident=0x0 for EVERY member while
        // mres=0x3f said all six of them resolved.  A test that fails for
        // all six cannot be a name mismatch, and the port's own raw dump
        // says why.
        //
        // The dump (phase=race-raw prints the metrics object from -0x100 to
        // +0x140) shows the object's first 0x48 bytes are POINTERS, and the
        // bytes from +0x48 on are the hash/rotating-id words:
        //
        //     +0x00  78 33 c4 0a 21 00 00 00   0x210ac43378  a pointer
        //     +0x08  10 ef 13 31 21 00 00 00   0x2113ef1010  a pointer
        //     +0x10  18 ef 13 31 21 00 00 00   0x2113ef1018  a pointer
        //     ...
        //     +0x50  ad 05 0e 85 9c 14 2e 66   hash-like bytes
        //
        // So the words at slot+0x08 and slot+0x10 are POINTERS, not a name
        // hash and a len^hash.  Read as two 32-bit halves of adjacent
        // pointers they XOR to 0, which is exactly the len the log shows.
        //
        // WHERE THE NAME REALLY IS: the value store's CHAIN NODE
        // (member_value_store.hpp): node+0x00 next, node+0x08 the key,
        // node+0x10 the key again, node+0x18 a pointer to the value cell.
        // The member NAME is not in the metrics object at all -- the object
        // holds only ROTATING ids that the value store maps to values.  The
        // port's name table therefore describes slots that carry no names,
        // and the length test cannot pass.
        //
        // Until the guest's own registration (ELF 0xb35a30, the calls at
        // 0xb35a5c..0xb35ba0) has been read on a LIVE object to find where
        // the name and its len^hash are kept, this check stays off.
        // Reporting a mask the port cannot back would be a claim, not a
        // measurement; reporting "not measured" is honest.
        //
        // WHAT IS MEASURED AND REPORTED INSTEAD: the six member VALUES
        // (which resolve, mres=0x3f) and the guest's own class name
        // (27RuleSetModifier_RaceMetrics).  The values are what the port
        // acts on.
        uint32_t nameHash = 0;
        uint32_t lenXorHash = 0;
        if (false && readDword(address + slots[i].offset + 0x08ULL, &nameHash) &&
            readDword(address + slots[i].offset + 0x10ULL, &lenXorHash)) {
            const uint32_t recoveredLength = nameHash ^ lenXorHash;
            const uint32_t expectedLength =
                static_cast<uint32_t>(std::strlen(slots[i].name));
            if (recoveredLength == expectedLength) {
                out->membersIdentified |= (1u << i);
            }
        }
    }
    out->valid = true;
}

// Where inside the 24 bytes of a libc++ std::string does the text appear?
//
// The key's short-string layout is the one thing in this file that the
// guest's own code does NOT spell out, so the probe REPORTS the offset
// instead of assuming one. Returns -1 when the text is not in the inline
// bytes (a long string keeps its characters on the heap).
//
// This is a cross-check, not the decision: the decision is the value's
// vptr, which is exact.
int findKeyBytes(uintptr_t keyAddress, const char* text) {
    const size_t textLength = std::strlen(text);
    if (textLength == 0 || textLength > 24) {
        return -1;
    }
    uint8_t buffer[24];
    for (size_t word = 0; word < 3; ++word) {
        uint32_t bits = 0;
        if (!readDword(keyAddress + word * 4, &bits)) {
            return -1;
        }
        for (size_t byte = 0; byte < 4; ++byte) {
            buffer[word * 4 + byte] = static_cast<uint8_t>((bits >> (byte * 8)) & 0xFF);
        }
    }
    for (size_t offset = 0; offset + textLength <= sizeof(buffer); ++offset) {
        bool match = true;
        for (size_t i = 0; i < textLength; ++i) {
            if (buffer[offset + i] != static_cast<uint8_t>(text[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            return static_cast<int>(offset);
        }
    }
    return -1;
}

// Reads the GameMode's ruleset map and the metrics object behind it.
//
// Returns true when the chain produced a live RuleSet_RaceMetrics. The
// results are written into the out-parameters only on success, so a
// candidate that does not lead anywhere cannot leave half a state behind.
bool followGameMode(uintptr_t bias, uintptr_t gameMode, uintptr_t* outRuleSet,
                    uintptr_t* outMetrics) {
    // The map at GameMode+0x128 holds MORE THAN ONE ENTRY -- StandardRaceMode
    // also inserts a ruleset under the key "grid" (ELF 0x103dae8) -- so the
    // root node is not necessarily the race metrics one. The probe WALKS THE
    // WHOLE TREE and takes the node whose VALUE points at a
    // RuleSet_RaceMetrics.
    //
    // The walk uses an explicit stack, never recursion: the guest's tree is
    // tiny, but a corrupt pointer must not be able to blow the C stack.
    const uintptr_t map = gameMode + kGameModeMapOff;
    uint64_t rootWord = 0;
    if (!readWord(map + kMapRootOff, &rootWord) || rootWord == 0) {
        return false;
    }
    const uintptr_t root = static_cast<uintptr_t>(rootWord);
    const uintptr_t ruleSetVptr = bias + kRuleSetRaceMetricsVptrElfVa;
    const uintptr_t metricsVptr = bias + kRaceMetricsVptrElfVa;

    uintptr_t ruleSet = 0;
    int nodesVisited = 0;
    int keyOffset = -1;
    uintptr_t stack[64];
    int stackSize = 0;
    stack[stackSize++] = root;
    while (stackSize > 0 && ruleSet == 0) {
        const uintptr_t node = stack[--stackSize];
        if (node == 0 || node == map + kMapRootOff) {
            // The map's own header points at itself when the tree is empty;
            // it is not a node.
            continue;
        }
        if (++nodesVisited > 64) {
            break;
        }
        uint64_t value = 0;
        if (readWord(node + kMapNodeValueOff, &value) && value != 0) {
            uint64_t head = 0;
            if (readWord(static_cast<uintptr_t>(value), &head) && head == ruleSetVptr) {
                ruleSet = static_cast<uintptr_t>(value);
                keyOffset = findKeyBytes(node + kMapNodeKeyOff, kRaceMetricsMapKey);
            }
        }
        uint64_t child = 0;
        if (stackSize < 62 && readWord(node + kMapNodeLeftOff, &child) && child != 0 &&
            child != node) {
            stack[stackSize++] = static_cast<uintptr_t>(child);
        }
        child = 0;
        if (stackSize < 62 && readWord(node + kMapNodeRightOff, &child) && child != 0 &&
            child != node) {
            stack[stackSize++] = static_cast<uintptr_t>(child);
        }
    }
    if (ruleSet == 0) {
        return false;
    }

    // The metrics modifier is built IN PLACE inside the RuleSet_RaceMetrics
    // at +0x40 (the guest's constructor, ELF 0xb35938, constructs it at
    // this+0x40). It may also be reached through a pointer, and both are
    // checked BY THE VPTR, so neither is a guess.
    uint64_t inlineHead = 0;
    if (readWord(ruleSet + kRuleSetMetricsInlineOff, &inlineHead) &&
        inlineHead == metricsVptr) {
        *outRuleSet = ruleSet;
        *outMetrics = ruleSet + kRuleSetMetricsInlineOff;
        g_mapNodesVisited = nodesVisited;
        g_mapKeyOffset = keyOffset;
        return true;
    }
    for (size_t off = 0; off + 8 <= kRuleSetRaceMetricsBytes; off += 8) {
        uint64_t word = 0;
        if (!readWord(ruleSet + off, &word)) {
            continue;
        }
        if (word == metricsVptr) {
            *outRuleSet = ruleSet;
            *outMetrics = ruleSet + off;
            g_mapNodesVisited = nodesVisited;
            g_mapKeyOffset = keyOffset;
            return true;
        }
        if (!looksLikePointer(bias, word)) {
            continue;
        }
        uint64_t head = 0;
        if (!readWord(static_cast<uintptr_t>(word), &head)) {
            continue;
        }
        if (head == metricsVptr) {
            *outRuleSet = ruleSet;
            *outMetrics = static_cast<uintptr_t>(word);
            g_mapNodesVisited = nodesVisited;
            g_mapKeyOffset = keyOffset;
            return true;
        }
    }
    return false;
}

void discover() {
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        return;
    }
    const uintptr_t g = cGlobalObject();
    if (g == 0) {
        return;
    }
    g_cglobal = g;

    // ---------------------------------------------------------------------
    // STEP 1: the GameMode.
    //
    // THE GUEST STORES IT AS A POINTER MEMBER OF THE CGlobal. Two
    // independent readings of the guest agree on the offset:
    //
    //   the teardown, ELF 0x10af1d4 (x19 = the CGlobal, this):
    //     0x10af22c  mov  w8, #0x378
    //     0x10af234  movk w8, #1, lsl #16    ; w8 = 0x10378
    //     0x10af244  add  x23, x19, x8
    //     0x10af280  ldr  x1, [x23, #0xa10]  ; -> CGlobal + 0x10d88
    //
    //   the mode creation, ELF 0xb08960 (x19 = the CGlobal, this):
    //     0xb08960   mov  w8, #0xbb0
    //     0xb08964   movk w8, #1, lsl #16    ; w8 = 0x10bb0
    //     0xb08968   add  x23, x19, x8
    //     0xb0898c   str  x20, [x23, #0x1d8]; -> CGlobal + 0x10d88
    //
    // NOTE THE ARITHMETIC. A mov/movk pair writes the LOW half first and
    // the high half second, so 0x378 + (1 << 16) is 0x10378, NOT 0x1378.
    // A first reading of this code dropped the 0x10000 and produced
    // 0x1d88, which is the offset this probe used to carry.
    //
    // THE GameMode IS ALWAYS A DERIVED CLASS. The guest instantiates 34 of
    // them (SoloMode, StandardRaceMode, TimeTrialMode, ...) and never a bare
    // GameMode, so the abstract base's address point (0x1e98168) is NOT the
    // first word of any live GameMode. A probe that searches for that value
    // therefore finds NOTHING, structurally, no matter how long it looks.
    // That is exactly what the archived run did: 49 owner candidates,
    // 0 hits.
    //
    // So the probe does not search for a vptr. It follows the pointer the
    // guest itself stores, and reads the vptr only to say WHICH mode it is.
    //
    // THE CANDIDATES. The probe tries the offsets the guest's own code
    // mentions as pointer members of the CGlobal and takes the first one
    // that REALLY leads to a RuleSet_RaceMetrics. Every candidate's pointer
    // and vptr go into the log, so a mis-read of the guest's arithmetic
    // cannot turn into a silent "not found".
    // ---------------------------------------------------------------------
    for (int i = 0; i < kGameModeCandidateCount; ++i) {
        g_candidatePointers[i] = 0;
        g_candidateVptrs[i] = 0;
        uint64_t pointer = 0;
        if (!readWord(g + kGameModeCandidateOffsets[i], &pointer) || pointer == 0) {
            continue;
        }
        g_candidatePointers[i] = static_cast<uintptr_t>(pointer);
        const uintptr_t gameMode = static_cast<uintptr_t>(pointer);
        uint64_t vptr = 0;
        if (readWord(gameMode, &vptr)) {
            g_candidateVptrs[i] = static_cast<uintptr_t>(vptr);
        }
        if (g_gameMode != 0) {
            continue;
        }
        uintptr_t ruleSet = 0;
        uintptr_t metrics = 0;
        if (!followGameMode(bias, gameMode, &ruleSet, &metrics)) {
            continue;
        }
        g_gameMode = gameMode;
        g_gameModeCandidate = i;
        g_gameModeVptr = g_candidateVptrs[i];
        // NEGATIVE CONTROL, kept on purpose. A live GameMode whose vptr is
        // the abstract base's address point would mean the guest DOES
        // instantiate a bare GameMode, which would mean the whole class
        // hierarchy above is wrong. The log carries the fact so a reader
        // can check it.
        g_gameModeIsBase = (g_gameModeVptr == bias + kGameModeVptrElfVa);
        g_ruleSetRaceMetrics = ruleSet;
        g_metrics[0] = metrics;
        g_metricsCount = 1;
    }
    if (g_gameMode == 0) {
        g_ruleSetRaceMetrics = 0;
        g_metricsCount = 0;
        findMetricsInHeap(bias, g);
        return;
    }
}

// The rate is measured over a WINDOW, not per frame. A per-frame rate is
// too noisy to steer on, and the steering itself changes the car's radius,
// which takes seconds to settle -- so the instrument needs the car's
// average progress over the last couple of seconds.
constexpr unsigned long long kRateWindowFrames = 120ULL;

void updateProgress(unsigned long long frame, RaceMetricsSample* sample) {
    // The progress metric is the guest's own distance along the track's
    // spline. Two members carry it and the guest's names do not say which is
    // the live one, so BOTH are watched and the one that moves is used.
    const double total = numericValue(sample->totalSplineDistance,
                                      sample->totalSplineDistanceF);
    const double lap = numericValue(sample->lapDistance,
                                    sample->lapDistanceF);
    if (!g_haveProgressValue) {
        g_lastTotalValue = total;
        g_lastLapValue = lap;
        g_haveProgressValue = true;
        g_progressFromTotal = true;
        g_lastProgressFrame = frame;
        g_lastProgressValue = total;
        g_progressMetricIsTotal = true;
        g_windowValue = total;
        g_windowFrame = frame;
        g_smoothedRate = 0.0;
        sample->progress = total;
        sample->progressRate = 0.0;
        sample->progressFromTotal = true;
        return;
    }
    const double deltaTotal = total - g_lastTotalValue;
    const double deltaLap = lap - g_lastLapValue;
    g_lastTotalValue = total;
    g_lastLapValue = lap;
    // The live metric is the one that moved; the total one is preferred.
    if (deltaTotal != 0.0) {
        g_progressFromTotal = true;
    } else if (deltaLap != 0.0) {
        g_progressFromTotal = false;
    }
    const double value = g_progressFromTotal ? total : lap;
    // The live metric can switch, and the two members are not on the same
    // scale, so the threshold's reference has to start again when it does.
    if (g_progressFromTotal != g_progressMetricIsTotal) {
        g_progressMetricIsTotal = g_progressFromTotal;
        g_lastProgressValue = value;
    }
    // PROGRESS IS A DISTANCE, NOT A NUDGE.  See kProgressEpsilon.
    //
    // AND WHILE THE RECOVERY IS REVERSING, THE REFERENCE FOLLOWS THE CAR
    // DOWN.  The recovery deliberately drives the car backwards, so the
    // distance it had made goes down with it; if the reference stayed up
    // there, the car would have to regain every metre it lost before it
    // counted as progress, the stall would fire again every 240 frames,
    // and the car would reverse its way down the circuit for the rest of
    // the race.  Measured 2026-10-28: the distance fell from 125,357 to
    // 103,119 and the stall counter ran to 8.
    if (g_drive.brake) {
        g_lastProgressValue = value;
    } else if (value >= g_lastProgressValue + kProgressEpsilon) {
        g_lastProgressValue = value;
        g_lastProgressFrame = frame;
        g_sawProgress = true;
    }
    if (frame >= g_windowFrame + kRateWindowFrames) {
        const double frames = static_cast<double>(frame - g_windowFrame);
        g_smoothedRate = (value - g_windowValue) / frames;
        g_windowValue = value;
        g_windowFrame = frame;
    }
    sample->progress = value;
    sample->progressRate = g_smoothedRate;
    sample->progressFromTotal = g_progressFromTotal;
}

void updateDrive(unsigned long long frame, const RaceStateSample& sample) {
    g_drive.frames = frame;
    if (!g_driveConfigRead) {
        readDriveConfig();
    }
    g_drive.enabled = g_driveConfig.present && g_driveConfig.enable &&
                      !g_driveConfig.probeOnly && sample.metricsFound &&
                      g_sawProgress;
    g_drive.discovered = sample.metricsFound;
    if (!g_drive.enabled) {
        g_drive.steer = 0.0f;
        g_drive.brake = false;
        g_drive.throttle = true;
        return;
    }
    if (sample.activeIndex < 0) {
        g_drive.steer = 0.0f;
        g_drive.brake = false;
        g_drive.throttle = true;
        return;
    }
    const RaceMetricsSample& m = sample.metrics[sample.activeIndex];
    const double rate = m.progressRate;
    g_drive.rate = static_cast<float>(rate);

    if (g_drive.magnitude <= 0.0f) {
        g_drive.magnitude = g_driveConfig.base;
        g_drive.sign = g_driveConfig.sign;
        // THE SERVO'S ANCHOR.  Without it the "step back to the best
        // magnitude" branch in the sweep below writes ZERO into the
        // magnitude -- bestMagnitude is zero-initialised and the branch
        // that would have assigned it was dead code -- and the floor pinned
        // it there for the rest of the race.  Measured 2026-10-28: the
        // steering axis sat at 5 % of lock for 43 % of the race and the car
        // ran into the barrier 491 m into a 7,056 m lap.
        g_drive.bestMagnitude = g_drive.magnitude;
    }

    // A stall: no METRE of progress for a while (the threshold is in
    // updateProgress).  The recovery turns the wheel hard the other way,
    // which is what frees a car that is against a wall, and asks for the
    // brake: the guest's own message ("hold the brake to reverse") says the
    // brake is what backs the car off a wall.
    //
    // THE SIGN IS NOT TOUCHED HERE.  The sign is the direction AROUND THE
    // TRACK; a car stuck against a wall is not a car going the wrong way,
    // and flipping it sends the car backwards around the circuit.
    //
    // THE THROTTLE IS RELEASED.  The port held the throttle down in every
    // frame, so throttle+brake was just braking and the guest never
    // reversed at all.  The guest reverses only when the brake is held and
    // the throttle is let go.
    //
    // THE PROGRESS REFERENCE COMES DOWN WITH THE CAR.  The recovery backs
    // the car off the barrier, so the distance it had made goes DOWN with
    // it; updateProgress() keeps the reference in step with the car while
    // the brake is held, so the stall does not fire again every 240 frames
    // and reverse the car down the circuit for the rest of the race.
    if (g_lastProgressFrame != 0 &&
        frame - g_lastProgressFrame > 240ULL) {
        ++g_drive.stalls;
        g_drive.bestRate = 0.0f;
        g_lastProgressFrame = frame;
        g_brakeUntilFrame = frame + kDriveRecoveryFrames;
    }
    g_drive.brake = (g_brakeUntilFrame != 0 && frame <= g_brakeUntilFrame);
    g_drive.throttle = !g_drive.brake;

    // A negative rate means the wrong way round: flip. The rate is a
    // 120-frame window, so the flip needs a HYSTERESIS: without one the
    // loop would flip on every window and oscillate.
    //
    // A TINY NEGATIVE RATE IS NOT "THE WRONG WAY ROUND".  A car grinding
    // against a barrier jitters the telemetry by a unit or two, and the
    // threshold here was -0.0001, so the port read that jitter as "the car
    // is going backwards" and flipped the direction AROUND THE TRACK.
    // Measured 2026-10-28: 24 sign flips in one race, while the car sat
    // still -- the autopilot was steering a car that was not moving, in
    // both directions in turn.
    //
    // AND THE RECOVERY IS NOT "GOING THE WRONG WAY" EITHER.  The recovery
    // reverses the car ON PURPOSE, so the distance falls and the rate goes
    // negative -- that is the recovery working.  Measured 2026-10-28: the
    // recovery reversed the car 30 m back down the track, that negative rate
    // flipped the sign, and the controller then drove the wrong way round
    // the circuit for the rest of the race (the distance fell from 125,357
    // to 103,119 and the stall counter ran to 8).
    if (rate < -0.0001 && !g_drive.brake &&
        frame >= g_lastFlipFrame + 600ULL) {
        g_drive.sign = -g_drive.sign;
        g_lastFlipFrame = frame;
        ++g_drive.flips;
        g_drive.bestRate = 0.0f;
        g_smoothedRate = 0.0;
    }

    // THE MAGNITUDE SWEEP.
    //
    // A bigger steering angle gives a TIGHTER circle, and on a closed track
    // only ONE circle matches the track, so the magnitude that maximises
    // the progress rate is the one that is on the racing line. The sweep
    // walks the magnitude up and down and remembers the best rate it has
    // seen; a rate that collapses well below the best means the sweep has
    // walked past the peak, so it steps back to the best and reverses.
    //
    // THE "BEST" IS ONLY COMPARED AT THE TICK.  The running maximum used to
    // be folded in on EVERY frame (the `if (rate > g_drive.bestRate)` that
    // used to sit above the stall test), which made the tick's own
    // comparison -- "is this rate better than the best?" -- impossible to
    // ever be true: bestRate was already the maximum, so the branch that
    // RECORDS the magnitude was dead code and bestMagnitude stayed 0.
    if ((frame % 240ULL) == 0ULL) {
        if (rate > g_drive.bestRate) {
            g_drive.bestRate = static_cast<float>(rate);
            g_drive.bestMagnitude = g_drive.magnitude;
        } else if (g_drive.bestRate > 0.0f &&
                   rate < g_drive.bestRate * 0.7f) {
            g_drive.magnitude = g_drive.bestMagnitude;
            g_drive.sweepDirection = -g_drive.sweepDirection;
        }
        g_drive.magnitude += 0.06f * static_cast<float>(g_drive.sweepDirection);
        if (g_drive.magnitude > 0.95f) {
            g_drive.magnitude = 0.95f;
            g_drive.sweepDirection = -1;
        }
        // THE FLOOR IS A STEER, NOT A NUDGE -- BUT NOT AROUND THE TRACK
        // EITHER.  The floor used to be 0.05, i.e. 5 % of the steering axis,
        // which the guest received as ax[0] = +-0.050; a car that cannot
        // turn runs straight into the first corner's barrier.  It was then
        // raised to 0.30, and THAT was worse: a constant 0.30 is a constant
        // tight circle, so the car scrubbed along at 5 km/h instead of
        // driving at 49.  Measured 2026-10-28 (q10 with sign=+1 and q11
        // with sign=-1): the progress rate sat at 6.5 units/frame with a
        // 0.30 angle, against 129 units/frame with a 0.05 one.
        //
        // So the floor is the CONFIGURED base: the config file decides how
        // small the steering may get, and the servo explores upwards from
        // there when the progress rate improves.  0.30 is only the fallback
        // for a config that does not say.
        const float floorMagnitude =
            (g_driveConfig.base > 0.0f) ? g_driveConfig.base
                                         : kDriveMagnitudeFloor;
        if (g_drive.magnitude < floorMagnitude) {
            g_drive.magnitude = floorMagnitude;
            g_drive.sweepDirection = 1;
        }
    }
    // THE STEERING: PURE PURSUIT AT THE GUEST'S OWN TARGET POINT.
    //
    // One scalar -- the progress rate along the spline -- cannot say which
    // way the track goes, so the controller's best strategy was always "go
    // straight" and the car left the road at 491 m of a 7,056 m lap.  The
    // guest keeps, on the car object, the car's WORLD POSITION
    // (CAR+0x30/+0x34/+0x38) and the point it is AIMING AT
    // (CAR+0x3c/+0x40/+0x44), both 32-bit integers in 1/256 metre, and the
    // guest's own AI drives by exactly that pair.  So the port can drive by
    // it too:
    //
    //   heading = the car's direction of travel, from the difference of two
    //            position samples (this object carries no heading);
    //   desired = the direction from the car to the target;
    //   error   = the signed angle from the heading to the desired
    //            direction;
    //   axis    = clamp(error / steerGain, -1, +1).
    //
    // This is a pure-pursuit controller: it aims at the point the guest
    // itself is aiming at, which is the racing line.
    double steerError = 0.0;
    bool pursuit = false;
    {
        static double s_lastX = 0.0;
        static double s_lastY = 0.0;
        static bool s_haveLast = false;
        const double px = static_cast<double>(m.carX) / 256.0;
        const double py = static_cast<double>(m.carY) / 256.0;
        const double tx = static_cast<double>(m.carTargetX) / 256.0;
        const double ty = static_cast<double>(m.carTargetY) / 256.0;
        if (s_haveLast) {
            const double hx = px - s_lastX;
            const double hy = py - s_lastY;
            const double dx = tx - px;
            const double dy = ty - py;
            const double hLen = std::sqrt(hx * hx + hy * hy);
            const double dLen = std::sqrt(dx * dx + dy * dy);
            // 0.05 m: below that the car is standing still or the target is
            // on top of it, and the heading is noise.
            if (hLen > 0.05 && dLen > 0.05) {
                const double ax = hx / hLen;
                const double ay = hy / hLen;
                const double bx = dx / dLen;
                const double by = dy / dLen;
                const double cross = ax * by - ay * bx;
                const double dot = ax * bx + ay * by;
                steerError = std::atan2(cross, dot);
                pursuit = true;
            }
        }
        s_lastX = px;
        s_lastY = py;
        s_haveLast = true;
    }
    g_drive.steerError = static_cast<float>(steerError);
    g_drive.pursuit = pursuit;

    g_drive.steer = static_cast<float>(g_drive.sign) * g_drive.magnitude;
    // WHILE REVERSING, HOLD A DECISIVE ANGLE THE OTHER WAY.  The car has
    // to come off the barrier pointing back down the track, and the swept
    // magnitude the servo is still hunting for is not a recovery angle.
    if (g_drive.brake) {
        g_drive.steer = -static_cast<float>(g_drive.sign) * kDriveRecoveryMagnitude;
    } else if (pursuit) {
        g_drive.steer = static_cast<float>(steerError / g_driveConfig.steerGain);
    }
    if (g_drive.steer > 1.0f) {
        g_drive.steer = 1.0f;
    }
    if (g_drive.steer < -1.0f) {
        g_drive.steer = -1.0f;
    }
}

void logSample(unsigned long long frame, const RaceStateSample& sample) {
    char detail[768];
    const int active = sample.activeIndex;
    const uintptr_t bias = resolveMainImageLoadBias();
    // The GameMode's vptr, and the ELF ADDRESS POINT it corresponds to. The
    // runtime value is bias + address point, so the address point is what
    // names the class offline: the guest has 34 derived GameMode classes and
    // a reader can look the address point up in the guest's RTTI.
    const uintptr_t modeAddressPoint =
        (g_gameModeVptr > bias) ? (g_gameModeVptr - bias) : 0;
    // EVERY CANDIDATE, so "not found" is never a black box: the reader sees
    // what each of the guest's pointer members held.
    char candidates[192];
    int written = 0;
    candidates[0] = '\0';
    for (int i = 0; i < kGameModeCandidateCount && written < 180; ++i) {
        const int n = std::snprintf(
            candidates + written, sizeof(candidates) - static_cast<size_t>(written),
            "%s%llx/%llx", (i == 0) ? "" : ",",
            static_cast<unsigned long long>(g_candidatePointers[i]),
            static_cast<unsigned long long>(g_candidateVptrs[i]));
        if (n <= 0) {
            break;
        }
        written += n;
    }
    if (active < 0) {
        std::snprintf(
            detail, sizeof(detail),
            "f=%llu g=0x%llx gm=0x%llx gm_ok=%d gm_vptr=0x%llx gm_ap=0x%llx "
            "gm_base=%d gm_cand=%d cand=%s metrics=%d "
            "active=-1 nodes=%d keyoff=%d heap=0x%llx+0x%llx "
            "drive=%d cfg=%d",
            frame, static_cast<unsigned long long>(g_cglobal),
            static_cast<unsigned long long>(sample.gameMode),
            sample.gameModeFound ? 1 : 0,
            static_cast<unsigned long long>(sample.gameModeVptr),
            static_cast<unsigned long long>(modeAddressPoint),
            g_gameModeIsBase ? 1 : 0,
            g_gameModeCandidate, candidates,
            sample.metricsCount,
            g_mapNodesVisited, g_mapKeyOffset,
            static_cast<unsigned long long>(g_heapBase),
            static_cast<unsigned long long>(g_heapSize),
            g_drive.enabled ? 1 : 0,
            g_driveConfig.present ? 1 : 0);
    } else {
        const RaceMetricsSample& m = sample.metrics[active];
        std::snprintf(
            detail, sizeof(detail),
            "f=%llu g=0x%llx gm=0x%llx gm_ok=%d gm_vptr=0x%llx gm_ap=0x%llx "
            "gm_base=%d gm_cand=%d cand=%s rs=0x%llx metrics=%d "
            "active=%d nodes=%d keyoff=%d heap=0x%llx+0x%llx "
            "cls=27RuleSetModifier_RaceMetrics m=0x%llx vptr_ok=%d "
            "mres=0x%x "
            "laps=%u lapF=%.3f lapd=%u lapdF=%.3f prev=%u total=%u "
            "totalF=%.3f speed=%u speedF=%.3f time=%u timeF=%.3f "
            "rate=%.6f prog=%.3f from_total=%d sawprog=%d "
            "car=0x%llx cnum=%u cnode=%u cprev=%u cpos=%d,%d,%d ctgt=%d,%d,%d "
            "snode=%u sdist=%u tsok=%d posok=%d perr=%.4f pur=%d ident=0x%llx "
            "drive=%d steer=%.3f mag=%.3f sign=%d best=%.6f flips=%d stalls=%d "
            "thr=%d brk=%d",
            frame, static_cast<unsigned long long>(g_cglobal),
            static_cast<unsigned long long>(sample.gameMode),
            sample.gameModeFound ? 1 : 0,
            static_cast<unsigned long long>(sample.gameModeVptr),
            static_cast<unsigned long long>(modeAddressPoint),
            g_gameModeIsBase ? 1 : 0,
            g_gameModeCandidate, candidates,
            static_cast<unsigned long long>(g_ruleSetRaceMetrics),
            sample.metricsCount, active,
            g_mapNodesVisited, g_mapKeyOffset,
            static_cast<unsigned long long>(g_heapBase),
            static_cast<unsigned long long>(g_heapSize),
            static_cast<unsigned long long>(m.address),
            (m.vptr == bias + kRaceMetricsVptrElfVa) ? 1 : 0,
            m.membersResolved,
            m.totalLaps, static_cast<double>(m.totalLapsF),
            m.lapDistance, static_cast<double>(m.lapDistanceF),
            m.previousSplineDistance, m.totalSplineDistance,
            static_cast<double>(m.totalSplineDistanceF),
            m.topSpeed, static_cast<double>(m.topSpeedF),
            m.totalTime, static_cast<double>(m.totalTimeF),
            m.progressRate, m.progress,
            m.progressFromTotal ? 1 : 0,
            g_sawProgress ? 1 : 0,
            static_cast<unsigned long long>(m.carAddress), m.carNumber, m.carNode,
            m.carPrevNode,
            m.carX, m.carY, m.carZ,
            m.carTargetX, m.carTargetY, m.carTargetZ,
            m.splineNode, m.splineDistance,
            m.positionPlausible ? 1 : 0,
            m.trackStateAddress != 0 ? 1 : 0,
            static_cast<double>(g_drive.steerError), g_drive.pursuit ? 1 : 0,
            static_cast<unsigned long long>(m.membersIdentified),
            g_drive.enabled ? 1 : 0, static_cast<double>(g_drive.steer),
            static_cast<double>(g_drive.magnitude), g_drive.sign,
            static_cast<double>(g_drive.bestRate), g_drive.flips, g_drive.stalls,
            g_drive.throttle ? 1 : 0, g_drive.brake ? 1 : 0);
    }
    appendGuestBootLine(kGuestViewLogPath, "race-state", 100, 0, detail);
    logGameModeSlots(frame);
}

// THE FIVE SLOTS.  One line per sample, five cheap reads, and they answer
// the question the static analysis could not: is the port looking at the
// guest's CGlobal at all, and is the guest's own GameMode slot live?
//
// MEASURED 2026-10-28 (.build/gamemode-holder.md):
//
//   A = *(CGlobal+0x10d88)   the slot the GUEST itself reads the GameMode
//                             from (ELF 0xf152a4: `ldr x19, [x0, x8]` with
//                             x8 = 0x10d88).  It is only filled between
//                             creating the mode (0xb088fc) and destroying it
//                             (0x10af2e4), and the creation is conditional
//                             (0xb0895c `cbnz`).
//   B = *(CGlobal+0x173f0)   the slot the probe's third candidate read.  The
//                             guest uses it as a POINTER (it is fed to
//                             0xc15fd8), so a small integer here means the
//                             port is reading a different object than the
//                             guest does.
//   C = *(uint32*)(CGlobal+0x1c0)  the scene state; 1 triggers the teardown.
//   D = *(uint32*)(CGlobal+0x10c78) the offset the guest itself reads from
//                             the CGlobal at 0xf2bb8c.
//   E = *(CGlobal)           the CGlobal's own vptr.  If E is not a vtable
//                             address point, the port's `g` is not the
//                             guest's CGlobal and every offset above is
//                             meaningless.
void logGameModeSlots(unsigned long long frame) {
    const uintptr_t g = cGlobalObject();
    if (g == 0) {
        return;
    }
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t e = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    readWord(g + 0x10d88ULL, &a);
    readWord(g + 0x173f0ULL, &b);
    readWord(g, &e);
    readDword(g + 0x1c0ULL, &c);
    readDword(g + 0x10c78ULL, &d);
    const uintptr_t bias = resolveMainImageLoadBias();
    // Is E a vtable address point?  That is the check that says "this really
    // is the guest's CGlobal".
    const bool eIsVptr =
        bias != 0 && e >= bias + kVtableSectionLo && e < bias + kVtableSectionHi;
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "f=%llu g=0x%llx A=0x%llx B=0x%llx C=%u D=%u E=0x%llx "
                  "eIsVptr=%d",
                  frame, static_cast<unsigned long long>(g),
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b),
                  static_cast<unsigned>(c), static_cast<unsigned>(d),
                  static_cast<unsigned long long>(e), eIsVptr ? 1 : 0);
    appendGuestBootLine(kGuestViewLogPath, "gamemode-slots", 0, 0, detail);
}

}  // namespace

void pumpRaceStateProbe(unsigned long long frame) {
    if (!g_driveConfigRead) {
        readDriveConfig();
    }
    const uintptr_t bias = resolveMainImageLoadBias();
    g_sample = RaceStateSample{};
    if (bias == 0) {
        return;
    }

    // (Re)discover. The search is expensive -- it walks the guest's object
    // graph -- so it runs on a CADENCE, not every frame. While the objects
    // are still missing it looks every 120 frames; once they are found it
    // re-checks every 600 frames as a safety net.
    //
    // The cadence matters: the first version of this probe searched every
    // frame while the objects were missing, and the guest's own frame rate
    // collapsed from about 3 frames a second to about 0.5.
    const bool found = (g_gameMode != 0) && (g_metricsCount > 0);
    const unsigned long long period = found ? 600ULL : 300ULL;
    const bool needSearch = (frame >= g_lastSearchFrame + period);
    if (needSearch) {
        g_lastSearchFrame = frame;
        discover();
    }

    g_sample.gameMode = g_gameMode;
    g_sample.gameModeFound = g_gameMode != 0;
    if (g_gameMode != 0) {
        uint64_t vptr = 0;
        if (readWord(g_gameMode, &vptr)) {
            g_sample.gameModeVptr = static_cast<uintptr_t>(vptr);
        }
    }

    // Read the metrics object.
    g_sample.metricsCount = 0;
    if (g_metricsCount > 0) {
        RaceMetricsSample m{};
        fillMetrics(&m, g_metrics[0]);
        fillCarState(&m);
        updateProgress(frame, &m);
        g_sample.metrics[0] = m;
        g_sample.metricsCount = 1;
        g_sample.activeIndex = 0;
    }
    g_sample.metricsFound = g_sample.metricsCount > 0;

    // The lap counter. The guest's own member is reported, and the probe
    // notes when it CHANGES, because a field that never changes is not the
    // lap counter.
    if (g_sample.activeIndex >= 0) {
        const uint32_t laps = g_sample.metrics[g_sample.activeIndex].totalLaps;
        if (laps != g_lastTotalLaps) {
            g_lastTotalLaps = laps;
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "f=%llu total_laps=%u changed=1", frame, laps);
            appendGuestBootLine(kGuestViewLogPath, "race-lap", 101, 0, detail);
        }
    }

    // THE RAW DUMP.
    //
    // The member offsets are proved by the guest's own member registration
    // (m_nLapDistance +0x08, m_nTotalLaps +0x20, m_nPreviousSplineDistance
    // +0x38, m_nTotalSplineDistanceTravelled +0x50, m_nTopGameSpeed +0x68,
    // m_nTotalTime +0x88), and the OBJECT is proved too: the port found it
    // through the map, matched the key "racemetrics", and checked the
    // vptrs of both the RuleSet and the inline modifier.
    //
    // And yet the readings are noise: every named member holds a value that
    // changes EVERY FRAME and is uniform over the whole 32-bit range, which
    // is what a random number looks like and not what a lap counter looks
    // like. So the readings are not being trusted and the RAW WORDS are
    // dumped instead: the field that actually counts the laps is the one
    // that behaves like a lap counter over time, and the only way to find
    // it is to let the data say so.
    //
    // THREE WINDOWS, because where the live data is is not known:
    //   w=   the NEIGHBOURHOOD of the RuleSet (rs-0x100 .. rs+0x140). The
    //        guest allocates sequentially, so a second, live metrics object
    //        would very likely be next to the one the map points at.
    //   p=   the POINTERS the modifier holds (it has two, at +0xb0 and
    //        +0xb8) and the 0x90 bytes at each of their targets. The guest
    //        keeps its references there, so this is where a live copy of
    //        the metrics would be.
    //   e=   the vector's element, printed FIRST because the line has a
    //        budget and the interesting number must not be the one cut.
    if (g_metricsCount > 0 && g_ruleSetRaceMetrics != 0) {
        char raw[4096];
        int written = std::snprintf(raw, sizeof(raw), "f=%llu", frame);
        uint64_t vectorBegin = 0;
        if (readWord(g_ruleSetRaceMetrics + 0x08, &vectorBegin) && vectorBegin != 0) {
            uint64_t element = 0;
            readWord(static_cast<uintptr_t>(vectorBegin), &element);
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     " e=%llx", static_cast<unsigned long long>(element));
        }
        written += std::snprintf(raw + written,
                                 sizeof(raw) - static_cast<size_t>(written),
                                 " w=");
        for (int off = -0x100; off < 0x140; off += 4) {
            if (written <= 0 || written >= static_cast<int>(sizeof(raw)) - 12) {
                break;
            }
            uint32_t word = 0;
            readDword(g_ruleSetRaceMetrics + off, &word);
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     "%08x", word);
        }
        // The modifier's own pointer members, and what they point at.
        written += std::snprintf(raw + written,
                                 sizeof(raw) - static_cast<size_t>(written),
                                 " p=");
        int pointers = 0;
        for (size_t off = 0; off < 0xC0 && pointers < 3; off += 8) {
            if (written <= 0 || written >= static_cast<int>(sizeof(raw)) - 64) {
                break;
            }
            uint64_t pointer = 0;
            if (!readWord(g_metrics[0] + off, &pointer) || pointer == 0) {
                continue;
            }
            if (!looksLikePointer(bias, pointer)) {
                continue;
            }
            ++pointers;
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     "[%llx:", static_cast<unsigned long long>(pointer));
            for (size_t inner = 0; inner < 0x90; inner += 4) {
                if (written <= 0 || written >= static_cast<int>(sizeof(raw)) - 12) {
                    break;
                }
                uint32_t word = 0;
                readDword(static_cast<uintptr_t>(pointer) + inner, &word);
                written += std::snprintf(raw + written,
                                         sizeof(raw) - static_cast<size_t>(written),
                                         "%08x", word);
            }
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     "]");
        }
        appendGuestBootLine(kGuestViewLogPath, "race-raw", 102, 0, raw);
    }

    // THE VALUE STORE. The slots above are DESCRIPTORS, not numbers: the
    // member's id lives there and the number lives in the guest's global
    // member value store. The value-store probe reads it out with a pure
    // read (no guest call, so nothing in the guest is mutated) and logs it
    // as "race-metrics". It is pumped here, next to the rest of the race
    // state, so it sees the same object the rest of this file sees.
    pumpRaceMetricsProbe(frame);

    updateDrive(frame, g_sample);

    if (frame >= g_lastLogFrame + 30ULL) {
        g_lastLogFrame = frame;
        logSample(frame, g_sample);
    }
}

const RaceStateSample& raceStateSample() {
    return g_sample;
}

// A SAFE read of one 64-bit word of guest memory.
//
// This is the safe-read path this file has always used -- the plausibility
// gate, then the guest-image check, then svcQueryMemory, then the read --
// exposed so that another probe reads guest memory through exactly the same
// door instead of through a second, subtly different one. A diagnostic that
// can fault the run it is measuring is worse than no diagnostic.
bool raceStateReadWord(uintptr_t address, uint64_t* out) {
    return readWord(address, out);
}

// The live CGlobal, exposed for the semantic UI probe.  See the header for
// why the chain lives here and not in the caller.
uintptr_t raceStateResolveGlobal() {
    return cGlobalObject();
}

// The live RuleSet_RaceMetrics object, resolved the way this file resolves
// it: the CGlobal -> the GameMode pointer -> the GameMode's ruleset map ->
// the node whose value is a RuleSet_RaceMetrics.
//
// It is resolved ON DEMAND rather than served from the probe's own cache,
// because a new race allocates a NEW object and a cached pointer would still
// point at the previous one. The value-store probe (source/race_metrics_probe
// .cpp) uses this instead of re-deriving the chain: the chain is the guest's
// own bookkeeping, and one implementation of it is one thing to keep correct.
//
// THE SCAN.  Finds the GameMode by the guest's own shape instead of by a
// fixed offset, because the guest's lap counter does not read the CGlobal
// slot the probe used to look at (see the long note at kGameModeScanSpan).
//
// The rule is the guest's: a GameMode is a pointer whose first word is a
// vtable address point and whose ruleset map lives at +0x128, and the map
// must really yield the RuleSet_RaceMetrics.  followGameMode() is that
// check, unchanged.
//
// THE POSITIVE CONTROL runs first and is reported with the result: the scan
// must be able to find the slot the guest itself reads (CGlobal+0x10d88)
// whenever that slot is non-zero.  A scan that cannot find a GameMode the
// guest is using proves nothing, so the control is part of the answer.
struct GameModeScan {
    uintptr_t gameMode = 0;
    uintptr_t ruleSet = 0;
    uintptr_t metrics = 0;
    int candidatesTried = 0;
    int vptrCandidates = 0;
    // The control: did the scan find the slot the guest itself reads?
    bool controlApplicable = false;   // the guest's own slot was non-zero
    bool controlFound = false;        // ... and the scan found it
    uintptr_t offsetFromCGlobal = 0;  // where the winner lives
};

GameModeScan g_gameModeScan;

// Scans [g, g + kGameModeScanSpan) for a pointer that is a GameMode.
//
// It is expensive, so it runs at most once per kGameModeScanPeriod calls
// and its result is reused in between: a race allocates a NEW GameMode, but
// the CGlobal neighbourhood that holds it changes far more slowly than the
// frame rate.
constexpr unsigned long long kGameModeScanPeriod = 60ULL;
unsigned long long g_gameModeScanCall = 0;

GameModeScan scanForGameMode(uintptr_t bias, uintptr_t g) {
    GameModeScan result;

    // THE POSITIVE CONTROL: the guest's own slot, read the way the guest
    // reads it.  If it is non-zero, the scan has to find it.
    uint64_t ownSlot = 0;
    if (readWord(g + 0x10d88ULL, &ownSlot) && ownSlot != 0) {
        result.controlApplicable = true;
    }

    const uintptr_t vptrLo = bias + kVtableSectionLo;
    const uintptr_t vptrHi = bias + kVtableSectionHi;

    for (uintptr_t offset = 0; offset < kGameModeScanSpan; offset += 8) {
        // Only addresses INSIDE the guest image are read.  An address outside
        // it would cost a svcQueryMemory, and this file has already measured
        // that ~1500 of those per sample dropped a run to 3120 frames: the
        // syscall count, not the read count, is the budget.
        if (!guestImageContains(g + offset, sizeof(uint64_t))) {
            continue;
        }
        uint64_t candidate = 0;
        if (!readWord(g + offset, &candidate) || candidate == 0) {
            continue;
        }
        const uintptr_t pointer = static_cast<uintptr_t>(candidate);
        if (!pointerPlausible(pointer)) {
            continue;
        }
        uint64_t head = 0;
        if (!readWord(pointer, &head)) {
            continue;
        }
        const uintptr_t vptr = static_cast<uintptr_t>(head);
        if (vptr < vptrLo || vptr >= vptrHi) {
            continue;
        }
        ++result.vptrCandidates;
        if (result.vptrCandidates > kGameModeScanMaxCandidates) {
            break;
        }
        ++result.candidatesTried;
        uintptr_t ruleSet = 0;
        uintptr_t metrics = 0;
        if (!followGameMode(bias, pointer, &ruleSet, &metrics)) {
            continue;
        }
        result.gameMode = pointer;
        result.ruleSet = ruleSet;
        result.metrics = metrics;
        result.offsetFromCGlobal = offset;
        if (result.controlApplicable && pointer == static_cast<uintptr_t>(ownSlot)) {
            result.controlFound = true;
        }
        break;
    }
    return result;
}

// The live RuleSet_RaceMetrics object, resolved the way this file resolves
// it: the CGlobal -> the GameMode pointer -> the GameMode's ruleset map ->
// the node whose value is a RuleSet_RaceMetrics.
//
// It is resolved ON DEMAND rather than served from the probe's own cache,
// because a new race allocates a NEW object and a cached pointer would still
// point at the previous one. The value-store probe (source/race_metrics_probe
// .cpp) uses this instead of re-deriving the chain: the chain is the guest's
// own bookkeeping, and one implementation of it is one thing to keep correct.
uintptr_t raceStateResolveRuleSetRaceMetrics() {
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        return 0;
    }
    const uintptr_t g = cGlobalObject();
    if (g == 0) {
        return 0;
    }
    // The same candidate offsets discover() tries, and the same order.
    for (int i = 0; i < kGameModeCandidateCount; ++i) {
        uint64_t pointer = 0;
        if (!readWord(g + kGameModeCandidateOffsets[i], &pointer) || pointer == 0) {
            continue;
        }
        uintptr_t ruleSet = 0;
        uintptr_t metrics = 0;
        if (!followGameMode(bias, static_cast<uintptr_t>(pointer), &ruleSet,
                            &metrics)) {
            continue;
        }
        return ruleSet;
    }
    // THE FALLBACK: the fixed offsets did not lead anywhere, so the GameMode
    // is looked for by the guest's own shape instead.  This is what the
    // 844-of-846 failures needed: the guest's lap counter does not use the
    // CGlobal slot the probe was reading.
    if (++g_gameModeScanCall >= kGameModeScanPeriod) {
        g_gameModeScanCall = 0;
        g_gameModeScan = scanForGameMode(bias, g);
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "vptrCands=%d tried=%d found=0x%llx off=0x%llx "
                      "ctrlApplicable=%d ctrlFound=%d",
                      g_gameModeScan.vptrCandidates,
                      g_gameModeScan.candidatesTried,
                      static_cast<unsigned long long>(g_gameModeScan.gameMode),
                      static_cast<unsigned long long>(g_gameModeScan.offsetFromCGlobal),
                      g_gameModeScan.controlApplicable ? 1 : 0,
                      g_gameModeScan.controlFound ? 1 : 0);
        appendGuestBootLine(kGuestViewLogPath, "gamemode-scan", 0, 0, detail);
    }
    return g_gameModeScan.ruleSet;
}

bool raceStateProgress(unsigned long long frame, float* rate, double* progress,
                       bool* fromTotal) {
    (void)frame;
    if (!g_sample.metricsFound || g_sample.activeIndex < 0) {
        return false;
    }
    const RaceMetricsSample& m = g_sample.metrics[g_sample.activeIndex];
    if (rate != nullptr) {
        *rate = static_cast<float>(m.progressRate);
    }
    if (progress != nullptr) {
        *progress = m.progress;
    }
    if (fromTotal != nullptr) {
        *fromTotal = m.progressFromTotal;
    }
    return true;
}

bool raceStateDriveEnabled() {
    return g_drive.enabled;
}

float raceStateSteering() {
    return g_drive.steer;
}

const RaceDriveState& raceDriveState() {
    return g_drive;
}

}  // namespace rr3
