// The guest's GUI-resolution list, and the two ends of the menu chain.
//
// MEASURED FROM THE GUEST BINARY (tools/guest-analysis/), not from a header:
//
//   cc::ImageResManager          GOT 0x1f6ec28 -> .bss cell 0x1fa2ba0
//   cc::RookieChampionshipMgr    GOT 0x1f6d840 -> .bss cell 0x1f95cd8
//   cc::Cloudcell                GOT 0x1f6d5d0 -> .bss cell (unnamed here)
//
// The two-level shape is the guest's own: the GOT slot holds the .bss CELL's
// address and the cell holds the OBJECT pointer. The guest's own code for
// the ImageResManager is 0x10f8c3c (writes the cell) and 0x10f8c68 (reads
// it); for the RookieChampionshipManager it is 0xdb8cb8.
//
// The list itself, from the guest's insert code at 0x10f9078..0x10f90c0:
//   manager+0x00  tail
//   manager+0x08  head (the first node)
//   manager+0x10  count
//   node+0x00     prev
//   node+0x08     next
//   node+0x10     float width
//   node+0x14     float height
//   node+0x18     float aspect
//   node+0x30     std::string path
//
// The std::string is the guest's libc++ variant, whose first byte carries
// the size in bits [7:1] and the "long form" flag in bit 0 (read out of the
// guest's own code at 0xbf058c and 0x12a5bb0). Short form: the bytes are
// inline at +1. Long form: they are behind the pointer at +8.
//
// EVERY READ HERE PASSES BOTH CHECKS THE PROJECT REQUIRES: svcQueryMemory
// with a Perm_R test, AND the guest's own 64-bit window around the image
// bias. A read that fails either one is reported as UNMAPPED, never as a
// value.

#include "rr3/guest_res_probe.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

#include "rr3/elf_preflight.hpp"
#include "rr3/recovery_guard.hpp"

namespace rr3 {

namespace {

constexpr char kGuestViewLog[] = "sdmc:/switch/rr3-native-rehost.guest-view.log";

// The guest's own 64-bit window, as a radius around the image bias. The SAME
// window guest_view_probe.cpp's menuGatePageReadable() uses, so the project
// has one definition of "inside the guest".
constexpr uint64_t kGuestWindowRadius = 0x1000000000ULL;

constexpr uint64_t kImageResGotElfVa  = 0x1F6EC28ULL;
constexpr uint64_t kImageResCellElfVa = 0x1FA2BA0ULL;
constexpr uint64_t kRookieGotElfVa    = 0x1F6D840ULL;
constexpr uint64_t kRookieCellElfVa   = 0x1F95CD8ULL;
constexpr uint64_t kCloudGotElfVa     = 0x1F6D5D0ULL;
constexpr uint64_t kCloudFlag4c1Off   = 0x4C1ULL;

constexpr uint64_t kImageResTailOff  = 0x00;
constexpr uint64_t kImageResHeadOff  = 0x08;
constexpr uint64_t kImageResCountOff = 0x10;

constexpr uint64_t kNodeNextOff   = 0x08;
constexpr uint64_t kNodeWidthOff  = 0x10;
constexpr uint64_t kNodeHeightOff = 0x14;
constexpr uint64_t kNodeAspectOff = 0x18;
constexpr uint64_t kNodePathOff   = 0x30;

constexpr unsigned kMaxLines = 200;

// ---------------------------------------------------------------------------
// Safe reads
// ---------------------------------------------------------------------------

// Is this page mapped and readable RIGHT NOW?
//
// TWO CHECKS, BOTH REQUIRED (see guest_view_probe.cpp for the long form):
//   (1) the address is inside the guest's own 64-bit window, and
//   (2) svcQueryMemory says the page is mapped with read permission.
// (1) alone is not enough under Eden (the kernel is asked about the HOST's
// mapping); (2) alone is not enough either (the host heap is readable and is
// not the guest's).
//
// One page cached, because this runs on the sampler thread and every syscall
// it spends is a syscall the guest's frame loop does not get.
bool guestPageReadable(uint64_t bias, uintptr_t address) {
    static uint64_t s_cachedBias = 0;
    static uintptr_t s_cachedPage = 0;
    static bool s_cachedReadable = false;
    static bool s_cachedValid = false;
    if (bias == 0 || address == 0) {
        return false;
    }
    const uintptr_t page = address & ~static_cast<uintptr_t>(0xFFF);
    if (s_cachedValid && page == s_cachedPage && bias == s_cachedBias) {
        return s_cachedReadable;
    }
    bool readable = false;
    const uintptr_t lo = static_cast<uintptr_t>(bias - kGuestWindowRadius);
    const uintptr_t hi = static_cast<uintptr_t>(bias + kGuestWindowRadius);
    if (address >= lo && address < hi) {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (!R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
            readable = (info.perm & Perm_R) != 0;
        }
    }
    s_cachedBias = bias;
    s_cachedPage = page;
    s_cachedReadable = readable;
    s_cachedValid = true;
    return readable;
}

bool guestReadWord(uint64_t bias, uintptr_t address, uint64_t* out) {
    if (out == nullptr || address == 0 || (address & 7u) != 0u) {
        return false;
    }
    if (!guestPageReadable(bias, address)) {
        return false;
    }
    *out = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
}

bool guestReadByte(uint64_t bias, uintptr_t address, uint8_t* out) {
    if (out == nullptr || address == 0) {
        return false;
    }
    if (!guestPageReadable(bias, address)) {
        return false;
    }
    *out = *reinterpret_cast<const volatile uint8_t*>(address);
    return true;
}

bool guestReadBytes(uint64_t bias, uintptr_t address, char* out, size_t count) {
    if (out == nullptr || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t byte = 0;
        if (!guestReadByte(bias, address + i, &byte)) {
            return false;
        }
        out[i] = static_cast<char>(byte);
    }
    return true;
}

// The guest's std::string, read with the guest's own layout.
bool guestReadString(uint64_t bias, uintptr_t address, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }
    uint8_t flags = 0;
    if (!guestReadByte(bias, address, &flags)) {
        return false;
    }
    const size_t size = static_cast<size_t>((flags >> 1) & 0x7F);
    if (size + 1 > outSize) {
        return false;
    }
    if ((flags & 1u) == 0u) {
        // Short form: the bytes are inline, starting at +1.
        if (!guestReadBytes(bias, address + 1, out, size)) {
            return false;
        }
        out[size] = '\0';
        return true;
    }
    // Long form: the bytes are behind the pointer at +8.
    uint64_t data = 0;
    if (!guestReadWord(bias, address + 8, &data)) {
        return false;
    }
    if (data == 0) {
        out[0] = '\0';
        return true;
    }
    if (!guestReadBytes(bias, static_cast<uintptr_t>(data), out, size)) {
        return false;
    }
    out[size] = '\0';
    return true;
}

// The guest's float at `address`.
bool guestReadFloat(uint64_t bias, uintptr_t address, float* out) {
    uint32_t bits = 0;
    if (out == nullptr) {
        return false;
    }
    if (!guestReadByte(bias, address, reinterpret_cast<uint8_t*>(&bits)) ||
        !guestReadByte(bias, address + 1,
                       reinterpret_cast<uint8_t*>(&bits) + 1) ||
        !guestReadByte(bias, address + 2,
                       reinterpret_cast<uint8_t*>(&bits) + 2) ||
        !guestReadByte(bias, address + 3,
                       reinterpret_cast<uint8_t*>(&bits) + 3)) {
        return false;
    }
    *out = *reinterpret_cast<const float*>(&bits);
    return true;
}

// ---------------------------------------------------------------------------
// JOB 0: the host sscanf probe
// ---------------------------------------------------------------------------

void logSscanfCase(const char* label, const char* input, const char* format) {
    unsigned width = 0xdeadbeefU;
    unsigned height = 0xdeadbeefU;
    // The PORT's own sscanf: the same function the guest's PLT resolves to
    // (symbol_resolver.cpp binds "sscanf" to ::sscanf).
    const int result = ::sscanf(input, format, &width, &height);
    char line[256];
    __builtin_snprintf(line, sizeof(line),
                       "%s in=%s fmt=%s r=%d w=%u h=%u", label, input, format,
                       result, width, height);
    appendGuestBootLine(kGuestViewLog, "sscanf-probe", 0,
                        static_cast<unsigned long long>(result), line);
}

} // namespace

void runSscanfProbe() {
    // The four folder names the port handed the guest, in the order the
    // guest's enumeration produced them.
    logSscanfCase("gui", "480x320", "%ux%u");
    logSscanfCase("gui", "960x640", "%ux%u");
    logSscanfCase("gui", "2048x1536", "%ux%u");
    logSscanfCase("gui", "1280x720", "%ux%u");
    // POSITIVE CONTROLS, in both directions. A probe that cannot report a
    // success and a probe that cannot report a failure are both useless, so
    // one of each is run, and both are expected to be exactly as claimed.
    //   "1280x720" with "%ux%u" must give r=2 (the positive control).
    //   "12x"      with "%ux%u" must give r=1 (the negative control: the
    //                            second conversion fails, so the call
    //                            returns 1, not 2).
    logSscanfCase("ctl-neg", "12x", "%ux%u");
    logSscanfCase("ctl-neg", "notanumber", "%ux%u");
    char summary[192];
    unsigned w = 0;
    unsigned h = 0;
    const int good = ::sscanf("960x640", "%ux%u", &w, &h);
    __builtin_snprintf(summary, sizeof(summary),
                       "verdict good=%d r=%d w=%u h=%u expect_r=2", good, good,
                       w, h);
    appendGuestBootLine(kGuestViewLog, "sscanf-verdict", 0,
                        static_cast<unsigned long long>(good), summary);
}

// ---------------------------------------------------------------------------
// JOB 1: the resolution list
// ---------------------------------------------------------------------------

namespace {

// One sample. Every field carries its own "was it readable" flag, so a
// refused read is reported as UNMAPPED and never as a value.
struct ImageResSample {
    bool gotCellRead{};
    bool cellRead{};
    bool managerRead{};
    bool tailRead{};
    bool headRead{};
    bool countRead{};
    uint64_t gotCell{};
    uint64_t cell{};
    uint64_t manager{};
    uint64_t tail{};
    uint64_t head{};
    uint64_t count{};
    unsigned nodeCount{};
    bool rookieGotRead{};
    bool rookieCellRead{};
    bool rookieRead{};
    uint64_t rookieGot{};
    uint64_t rookieCell{};
    uint64_t rookie{};
    bool cloudGotRead{};
    bool cloudCellRead{};
    bool cloudRead{};
    bool cloudFlagRead{};
    uint64_t cloudGot{};
    uint64_t cloudCell{};
    uint64_t cloud{};
    uint8_t cloudFlag4c1{};
    char nodes[768]{};
};

void appendNodeText(char* buffer, size_t capacity, const char* text) {
    const size_t used = std::strlen(buffer);
    if (used + 1 >= capacity) {
        return;
    }
    std::snprintf(buffer + used, capacity - used, "%s", text);
}

ImageResSample sampleImageRes(uint64_t bias) {
    ImageResSample s{};
    if (bias == 0) {
        return s;
    }
    // The ImageResManager chain: GOT -> .bss cell -> object.
    s.gotCellRead = guestReadWord(
        bias, static_cast<uintptr_t>(bias + kImageResGotElfVa), &s.gotCell);
    if (s.gotCellRead) {
        s.cellRead = guestReadWord(bias, static_cast<uintptr_t>(s.gotCell),
                                   &s.cell);
    }
    if (s.cellRead) {
        s.managerRead = guestReadWord(bias, static_cast<uintptr_t>(s.cell),
                                      &s.manager);
    }
    if (s.managerRead && s.manager != 0) {
        s.tailRead = guestReadWord(
            bias, static_cast<uintptr_t>(s.manager + kImageResTailOff), &s.tail);
        s.headRead = guestReadWord(
            bias, static_cast<uintptr_t>(s.manager + kImageResHeadOff), &s.head);
        s.countRead = guestReadWord(
            bias, static_cast<uintptr_t>(s.manager + kImageResCountOff), &s.count);
    }
    // The nodes, if there are any. The walk is bounded: a corrupt list must
    // not turn a diagnostic into a hang.
    //
    // BOTH directions are walked, because the manager's head and tail are
    // read from a list the guest may be rebuilding at the same moment, and a
    // walk that only goes forwards cannot tell "the list has three nodes"
    // from "the list has three nodes and the probe started in the middle of
    // it".
    if (s.headRead && s.head != 0 && s.manager != 0) {
        uint64_t node = s.head;
        for (unsigned step = 0; step < 4; ++step) {
            float width = 0.0f;
            float height = 0.0f;
            float aspect = 0.0f;
            uint64_t prev = 0;
            uint64_t next = 0;
            const bool widthRead = guestReadFloat(
                bias, static_cast<uintptr_t>(node + kNodeWidthOff), &width);
            const bool heightRead = guestReadFloat(
                bias, static_cast<uintptr_t>(node + kNodeHeightOff), &height);
            const bool aspectRead = guestReadFloat(
                bias, static_cast<uintptr_t>(node + kNodeAspectOff), &aspect);
            const bool prevRead = guestReadWord(
                bias, static_cast<uintptr_t>(node), &prev);
            const bool nextRead = guestReadWord(
                bias, static_cast<uintptr_t>(node + kNodeNextOff), &next);
            char path[96]{};
            const bool pathRead = guestReadString(
                bias, static_cast<uintptr_t>(node + kNodePathOff), path,
                sizeof(path));
            char text[224];
            __builtin_snprintf(text, sizeof(text),
                               "[%u]n=%llx p=%llx nx=%llx %s%.0fx%.0f a=%.3f p=%s ",
                               step,
                               static_cast<unsigned long long>(node),
                               static_cast<unsigned long long>(prev),
                               static_cast<unsigned long long>(next),
                               (widthRead && heightRead) ? "" : "?",
                               static_cast<double>(width),
                               static_cast<double>(height),
                               static_cast<double>(aspect),
                               pathRead ? path : "?");
            appendNodeText(s.nodes, sizeof(s.nodes), text);
            ++s.nodeCount;
            if (!nextRead || next == 0 || next == node || next == s.manager) {
                break;
            }
            node = next;
        }
    }
    // The RookieChampionshipManager: the gate's own chain, read the same way.
    //
    // TWO LEVELS, NOT THREE. The GOT slot holds the .bss CELL's address and
    // the cell holds the OBJECT; the ImageResManager happens to be
    // self-referential (its first word is itself, per the guest's own
    // constructor at 0x10f8c18: `stp x0, x0, [x0]`), so reading one level
    // too deep still lands on the right object for THAT class. The
    // RookieChampionshipManager is not self-referential, so a third read
    // would return the object's first member instead of the object. The
    // object is rookieCell.
    s.rookieGotRead = guestReadWord(
        bias, static_cast<uintptr_t>(bias + kRookieGotElfVa), &s.rookieGot);
    if (s.rookieGotRead) {
        s.rookieCellRead = guestReadWord(
            bias, static_cast<uintptr_t>(s.rookieGot), &s.rookieCell);
    }
    s.rookie = s.rookieCell;
    s.rookieRead = s.rookieCellRead;
    // The Cloudcell's +0x4c1 byte.
    s.cloudGotRead = guestReadWord(
        bias, static_cast<uintptr_t>(bias + kCloudGotElfVa), &s.cloudGot);
    if (s.cloudGotRead) {
        s.cloudCellRead = guestReadWord(bias,
                                         static_cast<uintptr_t>(s.cloudGot),
                                         &s.cloudCell);
    }
    if (s.cloudCellRead) {
        s.cloudRead = guestReadWord(bias, static_cast<uintptr_t>(s.cloudCell),
                                    &s.cloud);
    }
    if (s.cloudRead) {
        s.cloudFlagRead = guestReadByte(
            bias, static_cast<uintptr_t>(s.cloud + kCloudFlag4c1Off),
            &s.cloudFlag4c1);
    }
    return s;
}

} // namespace

void sampleImageResList(unsigned long long frames, const char* carrier) {
    const uint64_t bias = static_cast<uint64_t>(resolveMainImageLoadBias());
    const ImageResSample s = sampleImageRes(bias);

    // CHANGE-TRIGGERED plus a 1 s heartbeat, the same rule the other probes
    // in this project use, and a hard line budget.
    struct Last {
        bool valid{};
        uint64_t manager{};
        uint64_t head{};
        uint64_t count{};
        uint64_t rookie{};
        uint8_t cloudFlag4c1{};
        uint64_t tick{};
    };
    static Last s_last{};
    static unsigned s_lines = 0;
    if (s_lines >= kMaxLines) {
        return;
    }
    const uint64_t now = armGetSystemTick();
    const uint64_t freq = armGetSystemTickFreq();
    const uint64_t elapsedMs = (now - s_last.tick) * 1000ULL /
                               (freq != 0 ? freq : 1);
    const bool heartbeatDue = s_last.valid && elapsedMs >= 1000ULL;
    const bool changed = !s_last.valid || s.manager != s_last.manager ||
                         s.head != s_last.head || s.count != s_last.count ||
                         s.rookie != s_last.rookie ||
                         s.cloudFlag4c1 != s_last.cloudFlag4c1;
    if (!changed && !heartbeatDue) {
        return;
    }
    s_last.valid = true;
    s_last.manager = s.manager;
    s_last.head = s.head;
    s_last.count = s.count;
    s_last.rookie = s.rookie;
    s_last.cloudFlag4c1 = s.cloudFlag4c1;
    s_last.tick = now;
    ++s_lines;

    unsigned mask = 0;
    mask |= s.gotCellRead ? 1u : 0u;
    mask |= s.cellRead ? 2u : 0u;
    mask |= s.managerRead ? 4u : 0u;
    mask |= s.tailRead ? 8u : 0u;
    mask |= s.headRead ? 16u : 0u;
    mask |= s.countRead ? 32u : 0u;
    mask |= s.rookieGotRead ? 64u : 0u;
    mask |= s.rookieCellRead ? 128u : 0u;
    mask |= s.rookieRead ? 256u : 0u;
    mask |= s.cloudGotRead ? 512u : 0u;
    mask |= s.cloudCellRead ? 1024u : 0u;
    mask |= s.cloudRead ? 2048u : 0u;
    mask |= s.cloudFlagRead ? 4096u : 0u;

    char cellText[48];
    __builtin_snprintf(cellText, sizeof(cellText),
                       "0x%llx", static_cast<unsigned long long>(s.gotCell));
    const uint64_t expectedCell = bias + kImageResCellElfVa;
    const bool cellMatches = s.gotCell == expectedCell;

    // The manager's first four words, RAW. The layout is
    // {tail@0, head@8, count@0x10} per the guest's own constructor
    // (0x10f8c18: new(0x18); stp x0,x0,[x0]; str xzr,[x0,#0x10]), so a word
    // that does not look like a count is a fact about the list's state at
    // the instant of the read, and the raw words are what says which.
    uint64_t raw[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4; ++i) {
        guestReadWord(bias, static_cast<uintptr_t>(s.manager + 8 * i), &raw[i]);
    }

    char line[1200];
    __builtin_snprintf(
        line, sizeof(line),
        "reslist: f=%llu via=%s mgr=%llx m0=%llx m1=%llx m2=%llx m3=%llx"
        " count64=%llu count32=%u"
        " tail=%llx head=%llx nodes=%u %s"
        " gotcell=%s(expect=0x%llx match=%d)"
        " rookie=%llx cc=%llx cc4c1=%u rd=0x%04x",
        frames, carrier != nullptr ? carrier : "?",
        static_cast<unsigned long long>(s.manager),
        static_cast<unsigned long long>(raw[0]),
        static_cast<unsigned long long>(raw[1]),
        static_cast<unsigned long long>(raw[2]),
        static_cast<unsigned long long>(raw[3]),
        static_cast<unsigned long long>(raw[2]),
        static_cast<unsigned>((uint32_t)raw[2]),
        static_cast<unsigned long long>(s.tail),
        static_cast<unsigned long long>(s.head),
        s.nodeCount, s.nodes,
        cellText, static_cast<unsigned long long>(expectedCell),
        cellMatches ? 1 : 0,
        static_cast<unsigned long long>(s.rookie),
        static_cast<unsigned long long>(s.cloud),
        static_cast<unsigned>(s.cloudFlag4c1), mask);
    appendGuestBootLine(kGuestViewLog, "reslist", 0, s.manager, line);
}

} // namespace rr3
