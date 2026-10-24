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
constexpr uintptr_t kOffLapDistance = 0x08ULL;
constexpr uintptr_t kOffTotalLaps = 0x20ULL;
constexpr uintptr_t kOffPreviousSpline = 0x38ULL;
constexpr uintptr_t kOffTotalSpline = 0x50ULL;
constexpr uintptr_t kOffTopSpeed = 0x68ULL;
constexpr uintptr_t kOffTotalTime = 0x88ULL;
// The vtable range in the guest image (.data.rel.ro), so that a word which
// is a vptr can be told apart from a word which is a heap pointer.
constexpr uintptr_t kVtableSectionLo = 0x1e1aae0ULL;
constexpr uintptr_t kVtableSectionHi = 0x1f6d280ULL;

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
uint32_t g_lastTotalLaps = 0xFFFFFFFFu;
unsigned long long g_brakeUntilFrame = 0;
unsigned long long g_lastFlipFrame = 0;

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
    const double delta = g_progressFromTotal ? deltaTotal : deltaLap;
    if (delta != 0.0) {
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
        return;
    }
    if (sample.activeIndex < 0) {
        g_drive.steer = 0.0f;
        return;
    }
    const RaceMetricsSample& m = sample.metrics[sample.activeIndex];
    const double rate = m.progressRate;
    g_drive.rate = static_cast<float>(rate);

    if (g_drive.magnitude <= 0.0f) {
        g_drive.magnitude = g_driveConfig.base;
        g_drive.sign = g_driveConfig.sign;
    }

    // A negative rate means the wrong way round: flip. The rate is a
    // 120-frame window, so the flip needs a HYSTERESIS: without one the
    // loop would flip on every window and oscillate.
    if (rate < -0.0001 && frame >= g_lastFlipFrame + 600ULL) {
        g_drive.sign = -g_drive.sign;
        g_lastFlipFrame = frame;
        ++g_drive.flips;
        g_drive.bestRate = 0.0f;
        g_smoothedRate = 0.0;
    }

    if (rate > g_drive.bestRate) {
        g_drive.bestRate = static_cast<float>(rate);
    }

    // A stall: no progress for a while. The recovery turns the wheel hard
    // the other way, which is what frees a car that is against a wall, and
    // asks for the brake: the guest's own message ("hold the brake to
    // reverse") says the brake is what backs the car off a wall.
    if (g_lastProgressFrame != 0 &&
        frame - g_lastProgressFrame > 240ULL) {
        ++g_drive.stalls;
        g_drive.sign = -g_drive.sign;
        g_drive.bestRate = 0.0f;
        g_lastProgressFrame = frame;
        g_brakeUntilFrame = frame + 45ULL;
    }
    g_drive.brake = (g_brakeUntilFrame != 0 && frame <= g_brakeUntilFrame);

    // THE MAGNITUDE SWEEP.
    //
    // A bigger steering angle gives a TIGHTER circle, and on a closed track
    // only ONE circle matches the track, so the magnitude that maximises
    // the progress rate is the one that is on the racing line. The sweep
    // walks the magnitude up and down and remembers the best rate it has
    // seen; a rate that collapses well below the best means the sweep has
    // walked past the peak, so it steps back to the best and reverses.
    if ((frame % 240ULL) == 0ULL) {
        if (rate > 0.0 && rate > g_drive.bestRate) {
            g_drive.bestRate = static_cast<float>(rate);
            g_drive.bestMagnitude = g_drive.magnitude;
        } else if (g_drive.bestRate > 0.0f &&
                   rate < g_drive.bestRate * 0.7) {
            g_drive.magnitude = g_drive.bestMagnitude;
            g_drive.sweepDirection = -g_drive.sweepDirection;
        }
        g_drive.magnitude += 0.06f * static_cast<float>(g_drive.sweepDirection);
        if (g_drive.magnitude > 0.95f) {
            g_drive.magnitude = 0.95f;
            g_drive.sweepDirection = -1;
        }
        if (g_drive.magnitude < 0.05f) {
            g_drive.magnitude = 0.05f;
            g_drive.sweepDirection = 1;
        }
    }
    g_drive.steer = static_cast<float>(g_drive.sign) * g_drive.magnitude;
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
            "laps=%u lapF=%.3f lapd=%u lapdF=%.3f prev=%u total=%u "
            "totalF=%.3f speed=%u speedF=%.3f time=%u timeF=%.3f "
            "rate=%.6f prog=%.3f from_total=%d sawprog=%d "
            "drive=%d steer=%.3f mag=%.3f sign=%d best=%.6f flips=%d stalls=%d",
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
            m.totalLaps, static_cast<double>(m.totalLapsF),
            m.lapDistance, static_cast<double>(m.lapDistanceF),
            m.previousSplineDistance, m.totalSplineDistance,
            static_cast<double>(m.totalSplineDistanceF),
            m.topSpeed, static_cast<double>(m.topSpeedF),
            m.totalTime, static_cast<double>(m.totalTimeF),
            m.progressRate, m.progress,
            m.progressFromTotal ? 1 : 0,
            g_sawProgress ? 1 : 0,
            g_drive.enabled ? 1 : 0, static_cast<double>(g_drive.steer),
            static_cast<double>(g_drive.magnitude), g_drive.sign,
            static_cast<double>(g_drive.bestRate), g_drive.flips, g_drive.stalls);
    }
    appendGuestBootLine(kGuestViewLogPath, "race-state", 100, 0, detail);
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
    // And yet the readings are noise. So the readings are not being trusted
    // and the RAW WORDS are dumped instead: the field that actually counts
    // the laps is the one that behaves like a lap counter over time, and
    // the only way to find it is to let the data say so.
    //
    // One line per dump, every 30 frames, 24 words of the modifier and 28
    // words of the RuleSet. "r=" is the modifier, "s=" is the RuleSet.
    if (g_metricsCount > 0 && g_ruleSetRaceMetrics != 0) {
        char raw[768];
        int written = std::snprintf(raw, sizeof(raw), "f=%llu r=", frame);
        for (size_t off = 0; off < 0xC0 && written > 0 &&
             written < static_cast<int>(sizeof(raw)) - 12; off += 4) {
            uint32_t word = 0;
            readDword(g_metrics[0] + off, &word);
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     "%08x", word);
        }
        if (written > 0 && written < static_cast<int>(sizeof(raw)) - 4) {
            written += std::snprintf(raw + written,
                                     sizeof(raw) - static_cast<size_t>(written),
                                     " s=");
            for (size_t off = 0; off < 0xE0 && written > 0 &&
                 written < static_cast<int>(sizeof(raw)) - 12; off += 4) {
                uint32_t word = 0;
                readDword(g_ruleSetRaceMetrics + off, &word);
                written += std::snprintf(raw + written,
                                         sizeof(raw) - static_cast<size_t>(written),
                                         "%08x", word);
            }
        }
        // THE VECTOR'S ELEMENT.
        //
        // The RuleSet_RaceMetrics keeps a std::vector of POINTERS to the
        // metrics modifiers (the guest's constructor pushes the address of
        // the inline one at +0x40), so the LIVE modifier is whatever the
        // vector's element points at. The inline copy at +0x40 is only the
        // one the constructor made: its named members hold high-entropy
        // values, while an UNREGISTERED member at +0x80 counts up
        // monotonically, which is what a template that nobody updates looks
        // like next to a live object. So the element is read here, and so is
        // the 0xE0 bytes at the address it points to.
        uint64_t vectorBegin = 0;
        if (readWord(g_ruleSetRaceMetrics + 0x08, &vectorBegin) && vectorBegin != 0) {
            uint64_t element = 0;
            if (readWord(static_cast<uintptr_t>(vectorBegin), &element)) {
                written += std::snprintf(raw + written,
                                         sizeof(raw) - static_cast<size_t>(written),
                                         " e=%llx", static_cast<unsigned long long>(element));
                if (element != 0) {
                    uint64_t head = 0;
                    readWord(static_cast<uintptr_t>(element), &head);
                    written += std::snprintf(raw + written,
                                             sizeof(raw) - static_cast<size_t>(written),
                                             " ev=%llx", static_cast<unsigned long long>(head));
                    for (size_t off = 8; off < 0xA0 && written > 0 &&
                         written < static_cast<int>(sizeof(raw)) - 12; off += 4) {
                        uint32_t word = 0;
                        readDword(static_cast<uintptr_t>(element) + off, &word);
                        written += std::snprintf(raw + written,
                                                 sizeof(raw) - static_cast<size_t>(written),
                                                 "%08x", word);
                    }
                }
            }
        }
        appendGuestBootLine(kGuestViewLogPath, "race-raw", 102, 0, raw);
    }

    updateDrive(frame, g_sample);

    if (frame >= g_lastLogFrame + 30ULL) {
        g_lastLogFrame = frame;
        logSample(frame, g_sample);
    }
}

const RaceStateSample& raceStateSample() {
    return g_sample;
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
