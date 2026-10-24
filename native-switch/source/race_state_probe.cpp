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
constexpr uintptr_t kGameModeVptrElfVa = 0x1e98168ULL;
constexpr uintptr_t kRuleSetRaceMetricsVptrElfVa = 0x1e4b378ULL;
constexpr uintptr_t kRaceMetricsVptrElfVa = 0x1e4b3f8ULL;
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
constexpr uintptr_t kGameModeMapOff = 0x128ULL;
constexpr uintptr_t kMapRootOff = 0x08ULL;
constexpr uintptr_t kMapNodeValueOff = 0x38ULL;
// The RuleSet_RaceMetrics object is 0xe0 bytes (the guest allocates it with
// operator_new(0xe0) at ELF 0xf16be8).
constexpr uintptr_t kRuleSetRaceMetricsBytes = 0xE0ULL;

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
int g_ownerCandidates = 0;
int g_ownerHits = 0;
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

void fillMetrics(RaceMetricsSample* out, uintptr_t address, uintptr_t vptr) {
    out->address = address;
    out->vptr = vptr;
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

void discover() {
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        return;
    }
    const uintptr_t g = cGlobalObject();
    if (g == 0) {
        return;
    }
    // STEP 1: the GameMode.
    //
    // The guest builds the GameMode IN PLACE inside another object. The
    // guest's own code (ELF 0x10af1d4, the owner object's method) says
    // where:
    //
    //   0x10af21c  mov  x19, x0        ; x19 = the owner object
    //   0x10af244  add  x23, x19, x8   ; x8 = 0x173f0
    //   0x10af298  ldr  x0, [x23, #0xa10]
    //   0x10af2a0  bl   0xf168b4       ; the GameMode constructor
    //
    // so the GameMode's vptr is at owner+0x17e00. The guest builds a second
    // GameMode at owner'+0x1d88 (ELF 0xb17218), so both offsets are tried.
    //
    // The owner is looked for two ways, cheapest first:
    //   (a) the owner IS the CGlobal, so the GameMode's vptr is a word of
    //       the CGlobal. Every word of the CGlobal is inside the guest
    //       image, so this scan costs no syscalls at all.
    //   (b) the owner is one of the objects the CGlobal points at, so each
    //       of the CGlobal's pointers is tested at the two fixed offsets.
    const uintptr_t gameModeVptr = bias + kGameModeVptrElfVa;
    uintptr_t gameMode = 0;
    int ownerCandidates = 0;
    int ownerHits = 0;
    g_cglobal = g;

    // The owner object is big -- the GameMode sits at owner+0x17e00 -- so the
    // CGlobal's own scan has to reach that far. 0x1a000 covers it.
    for (size_t off = 0; off + 8 <= 0x1a000ULL && gameMode == 0; off += 8) {
        uint64_t word = 0;
        if (!readWord(g + off, &word)) {
            continue;
        }
        if (word == gameModeVptr) {
            gameMode = g + off;
            break;
        }
    }
    if (gameMode == 0) {
        // (b) the owner is a sub-object of the CGlobal. The GameMode sits
        // at owner+0x17e00, so ONE read per candidate is enough -- and ONE
        // read is the whole point. The first version of this probe scanned
        // each candidate's first 4 KiB, and reading 4 KiB of a cold,
        // file-backed mapping under Eden is disk I/O on the guest's own
        // thread: that version stalled the guest before it drew a frame.
        for (size_t off = 0; off + 8 <= 0x3000ULL && gameMode == 0; off += 8) {
            uint64_t word = 0;
            if (!readWord(g + off, &word)) {
                continue;
            }
            if (!looksLikePointer(bias, word)) {
                continue;
            }
            const uintptr_t owner = static_cast<uintptr_t>(word);
            ++ownerCandidates;
            if (ownerCandidates > 48) {
                break;
            }
            uint64_t head = 0;
            if (!readWord(owner + 0x17e00ULL, &head)) {
                continue;
            }
            if (head == gameModeVptr) {
                gameMode = owner + 0x17e00ULL;
                ++ownerHits;
            }
        }
    }
    g_ownerCandidates = ownerCandidates;
    g_ownerHits = ownerHits;
    if (gameMode == 0) {
        // The GameMode is not reachable from the CGlobal by either route the
        // guest's own code shows. The metrics object is still reachable: it
        // is HEAP allocated, and the guest's heap region is one syscall
        // away.
        g_gameMode = 0;
        findMetricsInHeap(bias, g);
        return;
    }
    g_gameMode = gameMode;

    // STEP 2: the metrics map. The GameMode's own map at GameMode+0x128;
    // its root node is at map+8 and the node's value is at node+0x38.
    uint64_t rootWord = 0;
    if (!readWord(gameMode + kGameModeMapOff + kMapRootOff, &rootWord) ||
        rootWord == 0) {
        return;
    }
    const uintptr_t root = static_cast<uintptr_t>(rootWord);
    const uintptr_t ruleSetVptr = bias + kRuleSetRaceMetricsVptrElfVa;
    const uintptr_t metricsVptr = bias + kRaceMetricsVptrElfVa;

    // The map holds one entry per race, and the guest inserts one in the
    // GameMode's constructor, but the root of a tree is not necessarily the
    // node wanted, so the root and its two children are checked.
    uintptr_t candidates[3] = {root, 0, 0};
    uint64_t childWord = 0;
    if (readWord(root, &childWord)) {
        candidates[1] = static_cast<uintptr_t>(childWord);
    }
    childWord = 0;
    if (readWord(root + 8, &childWord)) {
        candidates[2] = static_cast<uintptr_t>(childWord);
    }

    uintptr_t ruleSet = 0;
    for (int i = 0; i < 3 && ruleSet == 0; ++i) {
        const uintptr_t node = candidates[i];
        if (node == 0) {
            continue;
        }
        uint64_t value = 0;
        if (!readWord(node + kMapNodeValueOff, &value) || value == 0) {
            continue;
        }
        uint64_t head = 0;
        if (!readWord(static_cast<uintptr_t>(value), &head)) {
            continue;
        }
        if (head == ruleSetVptr) {
            ruleSet = static_cast<uintptr_t>(value);
        }
    }
    if (ruleSet == 0) {
        g_ruleSetRaceMetrics = 0;
        findMetricsInHeap(bias, g);
        return;
    }
    g_ruleSetRaceMetrics = ruleSet;

    // STEP 3: the metrics object itself. The guest keeps a
    // std::vector<RuleSetModifier_RaceMetrics> inside the
    // RuleSet_RaceMetrics, and the vector's inline storage holds the object
    // BY VALUE (ELF 0xb35940 constructs it at RuleSet_RaceMetrics+0x40), so
    // the object may be embedded OR reached through a pointer. Both are
    // checked, and both are checked by the vptr, so neither is a guess.
    for (size_t off = 0; off + 8 <= kRuleSetRaceMetricsBytes; off += 8) {
        uint64_t word = 0;
        if (!readWord(ruleSet + off, &word)) {
            continue;
        }
        if (word == metricsVptr) {
            // Embedded: the object starts here.
            g_metrics[0] = ruleSet + off;
            g_metricsCount = 1;
            return;
        }
        if (!looksLikePointer(bias, word)) {
            continue;
        }
        uint64_t head = 0;
        if (!readWord(static_cast<uintptr_t>(word), &head)) {
            continue;
        }
        if (head == metricsVptr) {
            g_metrics[0] = static_cast<uintptr_t>(word);
            g_metricsCount = 1;
            return;
        }
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
    if (active < 0) {
        std::snprintf(
            detail, sizeof(detail),
            "f=%llu g=0x%llx gm=0x%llx gm_ok=%d gm_vptr=0x%llx metrics=%d "
            "active=-1 owncand=%d ownhit=%d heap=0x%llx+0x%llx "
            "drive=%d cfg=%d",
            frame, static_cast<unsigned long long>(g_cglobal),
            static_cast<unsigned long long>(sample.gameMode),
            sample.gameModeFound ? 1 : 0,
            static_cast<unsigned long long>(sample.gameModeVptr),
            sample.metricsCount, g_ownerCandidates, g_ownerHits,
            static_cast<unsigned long long>(g_heapBase),
            static_cast<unsigned long long>(g_heapSize),
            g_drive.enabled ? 1 : 0,
            g_driveConfig.present ? 1 : 0);
    } else {
        const RaceMetricsSample& m = sample.metrics[active];
        std::snprintf(
            detail, sizeof(detail),
            "f=%llu g=0x%llx gm=0x%llx gm_ok=%d gm_vptr=0x%llx rs=0x%llx metrics=%d "
            "active=%d owncand=%d ownhit=%d heap=0x%llx+0x%llx "
            "cls=27RuleSetModifier_RaceMetrics m=0x%llx vptr_ok=%d "
            "laps=%u lapF=%.3f lapd=%u lapdF=%.3f prev=%u total=%u "
            "totalF=%.3f speed=%u speedF=%.3f time=%u timeF=%.3f "
            "rate=%.6f prog=%.3f from_total=%d sawprog=%d "
            "drive=%d steer=%.3f mag=%.3f sign=%d best=%.6f flips=%d stalls=%d",
            frame, static_cast<unsigned long long>(g_cglobal),
            static_cast<unsigned long long>(sample.gameMode),
            sample.gameModeFound ? 1 : 0,
            static_cast<unsigned long long>(sample.gameModeVptr),
            static_cast<unsigned long long>(g_ruleSetRaceMetrics),
            sample.metricsCount, active, g_ownerCandidates, g_ownerHits,
            static_cast<unsigned long long>(g_heapBase),
            static_cast<unsigned long long>(g_heapSize),
            static_cast<unsigned long long>(m.address),
            (m.vptr == resolveMainImageLoadBias() + kRaceMetricsVptrElfVa) ? 1 : 0,
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
        fillMetrics(&m, g_metrics[0], bias + kRaceMetricsVptrElfVa);
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
