#include "rr3/guest_action.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/tls_probe.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {

// ---------------------------------------------------------------------------
// THE GUEST'S ACTION TABLE
// ---------------------------------------------------------------------------
//
//   ELF 0x1e78b70: 317 entries of 0x10 bytes, {function, plain word}.
//
// The function pointer is RELOCATED at load time (R_AARCH64_RELATIVE), so
// the file has zeros there and the RUNTIME image has the real address.
// The second word of an entry carries no relocation at all, so it is not a
// pointer: the argument a command receives arrives in x0, not in the
// table.
//
//   ELF 0x1f9d6d8: the NAMES, 317 entries of 0x18 bytes, in .bss.
//
// The name table exists only once the guest has built it, so it is read at
// RUN TIME and the index comes from the name -- the port never carries an
// index of its own.
constexpr uintptr_t kActionTableVa = 0x1E78B70ULL;
constexpr uintptr_t kActionTableStride = 0x10ULL;
constexpr uintptr_t kActionTableCount = 317ULL;
constexpr uintptr_t kActionNameTableVa = 0x1F9D6D8ULL;
constexpr uintptr_t kActionNameTableStride = 0x18ULL;

// THE GUEST'S TEXT ENDS HERE. The first PT_LOAD segment is r-x and covers
// 0x0..0x1e19ae0; everything above that in the image is data (the
// vtables start at 0x1e1aae0). So a function pointer inside
// [bias, bias + 0x1e19ae0) is a CODE pointer, and a pointer anywhere else
// is refused rather than called. This is the check that keeps a corrupt
// table entry from turning into a jump into the data segment.
constexpr uintptr_t kGuestTextEndVa = 0x1E19AE0ULL;

// The control file. One command per line; a line may carry "at=<frame>" to
// postpone the call to the port's own frame counter. '#' starts a comment.
constexpr const char* kActionConfPath =
    "sdmc:/switch/rr3-native-rehost/rr3-action.txt";

// ---------------------------------------------------------------------------
// THE ALLOWLIST
// ---------------------------------------------------------------------------
//
// A command is called with x0 = 0, so only commands that do NOT READ x0 may
// be called at all. That property is checked offline against the guest
// binary and pinned by tests/test_guest_action_contract.py, which re-derives
// it from the same binary, so the two cannot drift apart. 122 of the 317
// commands ignore their argument; the ones below are the content-unlocking
// ones.
//
// The names here are only a first filter. The call itself is selected by
// NAME against the guest's own runtime table, so even a wrong name here
// cannot invoke the wrong command: the name is looked up, and the index
// that comes back is the one that is called.
struct AllowedAction {
    const char* name;
};

constexpr AllowedAction kAllowedActions[] = {
    {"Character|Unlock All Tracks"},
    {"Career|Reload event data"},
    {"Career|Win all events"},
    {"Career|Win tapped event"},
    {"Assets|UPDATE EVENTS"},
};

constexpr size_t kAllowedActionCount =
    sizeof(kAllowedActions) / sizeof(kAllowedActions[0]);

bool isAllowed(const char* name) {
    if (name == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kAllowedActionCount; ++i) {
        if (std::strcmp(kAllowedActions[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// READING GUEST MEMORY SAFELY
// ---------------------------------------------------------------------------
//
// A name that is too long for the inline form keeps its characters on the
// GUEST'S HEAP, so reading it needs the readability check the other probes
// use: a plain dereference of a stale pointer is a fault, and a diagnostic
// that can fault the run it is measuring is worse than no diagnostic.
//
// THE PLAUSIBILITY CHECK COMES FIRST, AND IT IS NOT OPTIONAL. Measured
// 2026-10-25: this module called svcQueryMemory with a wild address
// (0x369B98C5D0, a value read out of a name entry the guest had not
// finished building) and Eden's kernel service handler faulted on it --
// the port died at frame 7, before the front end was even up. The crash
// address was inside libnx's svcSendSyncRequest, and the port's own
// sampler recorded it: "pc_img=host-nro pc_off=0x58b70c".
//
// The other probes in this project get the order right: the plausibility
// gate first, and only then the kernel query. The guest's address space
// tops out well below 2^47, so an address outside that range is an
// INTEGER that was mistaken for a pointer, and asking the kernel about it
// is how the port dies.
constexpr uintptr_t kPlausibleLo = 0x10000ULL;
constexpr uintptr_t kPlausibleHi = 0x0000800000000000ULL;

bool pointerPlausible(uintptr_t address) {
    return address >= kPlausibleLo && address < kPlausibleHi;
}

bool guestReadable(uintptr_t address) {
    if (!pointerPlausible(address)) {
        return false;
    }
    // THE PAGE CACHE. A long name's characters are read one at a time (a
    // bulk copy could cross into an unmapped page), and each character would
    // otherwise cost a kernel query. Sixteen entries is enough for the few
    // pages a name spans.
    //
    // The cache is keyed on the PAGE, not on the address, so a name that
    // crosses a page boundary costs two queries and not one per character.
    static uintptr_t s_page[16] = {0};
    static bool s_readable[16] = {false};
    static bool s_valid[16] = {false};
    constexpr uintptr_t kPageMask = ~static_cast<uintptr_t>(0xFFF);
    const uintptr_t page = address & kPageMask;
    int freeSlot = -1;
    for (int i = 0; i < 16; ++i) {
        if (s_valid[i] && s_page[i] == page) {
            return s_readable[i];
        }
        if (freeSlot < 0 && !s_valid[i]) {
            freeSlot = i;
        }
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    const bool readable =
        !R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address))) &&
        (info.perm & Perm_R) != 0;
    const int slot = (freeSlot >= 0) ? freeSlot : 0;
    s_page[slot] = page;
    s_readable[slot] = readable;
    s_valid[slot] = true;
    return readable;
}

bool readGuestBytes(uintptr_t address, void* out, size_t bytes) {
    if (address == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    if (!pointerPlausible(address)) {
        return false;
    }
    if (!guestImageContains(address, bytes)) {
        if (!guestReadable(address)) {
            return false;
        }
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}

// ---------------------------------------------------------------------------
// THE NAME TABLE
// ---------------------------------------------------------------------------
//
// The entry's first byte is a flag. Bit 0 set means the string is LONG: the
// length is a u64 at entry+8 and the characters live behind a heap pointer
// at entry+0x10. Bit 0 clear means it is SHORT: the length is that byte
// shifted right by one, and the characters are INLINE at entry+1.
//
// That is the same rule the port's own action-name dump uses, and it is
// reproduced here rather than shared, because that reader lives in another
// file's anonymous namespace and the two must be able to disagree loudly
// rather than silently.
bool readActionName(uintptr_t bias, uint64_t index, char* out,
                    size_t outCapacity) {
    if (out == nullptr || outCapacity == 0) {
        return false;
    }
    out[0] = '\0';
    const uintptr_t entry =
        bias + kActionNameTableVa + index * kActionNameTableStride;
    uint8_t flag = 0;
    if (!readGuestBytes(entry, &flag, 1)) {
        return false;
    }
    uintptr_t chars = 0;
    uint64_t length = 0;
    if ((flag & 1u) != 0u) {
        // LONG: length at entry+8, characters behind the pointer at +0x10.
        uint64_t stored = 0;
        uint64_t pointer = 0;
        if (!readGuestBytes(entry + 8, &stored, 8) ||
            !readGuestBytes(entry + 0x10, &pointer, 8)) {
            return false;
        }
        if (stored == 0 || pointer == 0) {
            return false;
        }
        length = stored;
        chars = static_cast<uintptr_t>(pointer);
    } else {
        // SHORT: the length is the flag byte shifted right by one, and the
        // characters are inline.
        length = static_cast<uint64_t>(flag >> 1);
        if (length == 0) {
            return false;
        }
        chars = entry + 1;
    }
    // THE LENGTH IS CAPPED WITHOUT OVERFLOW. The guest's length is a u64, and
    // the obvious test -- "length + 1 > outCapacity" -- overflows for a length
    // of 0xffffffffffffffff: the sum becomes 0, the test is false, and the
    // loop below walks 2^64 bytes. The comparison is written so that it
    // cannot.
    if (length >= outCapacity) {
        length = outCapacity - 1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        uint8_t c = 0;
        if (!readGuestBytes(chars + i, &c, 1)) {
            out[0] = '\0';
            return false;
        }
        if (c == 0) {
            break;
        }
        out[i] = static_cast<char>(c);
    }
    out[length] = '\0';
    return out[0] != '\0';
}

// Finds a command's index by NAME in the guest's own name table.
bool findActionIndex(uintptr_t bias, const char* name, uint64_t* outIndex) {
    if (name == nullptr || outIndex == nullptr) {
        return false;
    }
    // THE TABLE HAS TO BE BUILT FIRST. The guest fills the table during its
    // own start-up, and until it does every entry is zero. Reading the
    // FIRST entry's flag byte is therefore the cheap test for "the table
    // exists", and it saves walking 317 entries on every attempt.
    uint8_t first = 0;
    if (!readGuestBytes(bias + kActionNameTableVa, &first, 1) || first == 0) {
        return false;
    }
    char candidate[128];
    for (uint64_t index = 0; index < kActionTableCount; ++index) {
        if (!readActionName(bias, index, candidate, sizeof(candidate))) {
            continue;
        }
        if (std::strcmp(candidate, name) == 0) {
            *outIndex = index;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// THE PENDING LIST
// ---------------------------------------------------------------------------
namespace {

struct PendingAction {
    char name[128];
    unsigned long long atFrame{};
    unsigned attempts{};
    bool done{};
    bool refused{};
};

constexpr size_t kMaxPendingActions = 8;

PendingAction g_pending[kMaxPendingActions];
size_t g_pendingCount = 0;
bool g_confRead = false;
unsigned g_confAttempts = 0;
bool g_allDoneLogged = false;
unsigned long long g_lastAttemptFrame = 0;

// Defined below; the conf reader needs it to report a file it could not
// open, which is the only thing that says "the pump is doing nothing".
void logAction(const char* phase, unsigned long long ordinal,
               const char* details);

// How often the pending list is retried. The name table only exists once
// the guest has built it, so the first attempts are expected to fail; a
// short period keeps the run responsive without hammering the guest.
constexpr unsigned long long kAttemptPeriodFrames = 60ULL;

void readActionConf() {
    std::FILE* file = std::fopen(kActionConfPath, "rb");
    if (file == nullptr) {
        // NOT READ YET, SO NOT "read". The control file lives on the SD
        // card, and the card is not necessarily there on the first frame the
        // pump runs. The first version of this set the "read" flag before it
        // opened the file, so a card that was not mounted yet left the pump
        // permanently empty -- and it said nothing, because there was no
        // line to say it with. The flag is now set only when the file was
        // actually opened, and the attempt is logged either way.
        g_confAttempts++;
        if (g_confAttempts == 1U || g_confAttempts == 20U) {
            char details[192];
            std::snprintf(details, sizeof(details),
                          "conf=%s open=0 attempts=%u pending=%zu "
                          "reason=the-control-file-is-not-readable-yet",
                          kActionConfPath, g_confAttempts, g_pendingCount);
            logAction("guest-action", 108, details);
        }
        return;
    }
    g_confRead = true;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* hash = std::strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char* name = line;
        while (*name == ' ' || *name == '\t') {
            ++name;
        }
        char* end = name + std::strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t' ||
                              end[-1] == '\r' || end[-1] == '\n')) {
            --end;
        }
        *end = '\0';
        if (name[0] == '\0') {
            continue;
        }
        // An optional "at=<frame>" postpones the call to the port's own
        // frame counter, so a command can wait for the front end to be up.
        unsigned long long at = 0;
        char* atMarker = std::strstr(name, "at=");
        if (atMarker != nullptr) {
            at = std::strtoull(atMarker + 3, nullptr, 10);
            *atMarker = '\0';
            end = atMarker;
            while (end > name && (end[-1] == ' ' || end[-1] == '\t')) {
                --end;
            }
            *end = '\0';
        }
        if (name[0] == '\0' || g_pendingCount >= kMaxPendingActions) {
            continue;
        }
        PendingAction& entry = g_pending[g_pendingCount++];
        std::snprintf(entry.name, sizeof(entry.name), "%s", name);
        entry.atFrame = at;
        entry.done = false;
        entry.refused = false;
    }
    std::fclose(file);
}

void logAction(const char* phase, unsigned long long ordinal,
               const char* details) {
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", phase,
                        ordinal, 0, details);
}

} // namespace

void pumpGuestActions(unsigned long long frame) {
    if (!g_confRead) {
        readActionConf();
        if (g_pendingCount == 0) {
            return;
        }
        char details[128];
        std::snprintf(details, sizeof(details), "pending=%zu file=%s",
                      g_pendingCount, kActionConfPath);
        logAction("guest-action", 0, details);
    }
    if (g_pendingCount == 0) {
        return;
    }
    if (frame < g_lastAttemptFrame + kAttemptPeriodFrames) {
        return;
    }
    g_lastAttemptFrame = frame;

    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        return;
    }

    bool allDone = true;
    for (size_t i = 0; i < g_pendingCount; ++i) {
        PendingAction& entry = g_pending[i];
        if (entry.done || entry.refused) {
            allDone = allDone && entry.done;
            continue;
        }
        if (frame < entry.atFrame) {
            allDone = false;
            continue;
        }
        if (!isAllowed(entry.name)) {
            entry.refused = true;
            char details[256];
            std::snprintf(details, sizeof(details),
                          "name=\"%.120s\" refused=1 reason=not-in-allowlist "
                          "(the command may read its argument, and it is "
                          "called with x0=0)",
                          entry.name);
            logAction("guest-action", 110 + static_cast<unsigned>(i), details);
            allDone = false;
            continue;
        }
        uint64_t index = 0;
        if (!findActionIndex(bias, entry.name, &index)) {
            // A COMMAND THAT IS NEVER FOUND IS REPORTED, NOT SWALLOWED. The
            // name table only exists once the guest has built it, so the first
            // attempts are expected to fail; after a while, a name that is
            // still missing is a fact about the guest, and a silent retry
            // would hide it.
            ++entry.attempts;
            if (entry.attempts == 20U) {
                char details[192];
                std::snprintf(details, sizeof(details),
                              "name=\"%.120s\" found=0 attempts=%u "
                              "reason=not-in-the-guest-name-table",
                              entry.name, entry.attempts);
                logAction("guest-action", 110 + static_cast<unsigned>(i), details);
            }
            allDone = false;
            continue;
        }
        // The function pointer is RELOCATED, so it is read from the RUNTIME
        // image and not from the file.
        uint64_t function = 0;
        const uintptr_t slot = bias + kActionTableVa + index * kActionTableStride;
        if (!readGuestBytes(slot, &function, 8) || function == 0) {
            allDone = false;
            continue;
        }
        if (function < bias || function >= bias + kGuestTextEndVa) {
            char details[256];
            std::snprintf(details, sizeof(details),
                          "name=\"%.120s\" idx=%llu fn=0x%llx refused=1 "
                          "reason=not-in-the-guest-text-segment",
                          entry.name, static_cast<unsigned long long>(index),
                          static_cast<unsigned long long>(function));
            logAction("guest-action", 110 + static_cast<unsigned>(i), details);
            entry.refused = true;
            allDone = false;
            continue;
        }
        // CALL IT, with guest TLS and a zero argument.
        char before[256];
        std::snprintf(before, sizeof(before), "name=\"%.120s\" idx=%llu fn=0x%llx "
                      "arg=0", entry.name,
                      static_cast<unsigned long long>(index),
                      static_cast<unsigned long long>(function));
        // The line BEFORE the call, so a command that takes the guest down
        // still leaves a record of WHICH command it was.
        logAction("guest-action", 110 + static_cast<unsigned>(i), before);
        {
            GuestTlsScope tls;
            using GuestActionFn = void (*)(uintptr_t);
            reinterpret_cast<GuestActionFn>(function)(0);
        }
        entry.done = true;
        char after[320];
        std::snprintf(after, sizeof(after), "%s calling=1 called=1", before);
        logAction("guest-action", 110 + static_cast<unsigned>(i), after);
    }
    if (allDone && !g_allDoneLogged) {
        g_allDoneLogged = true;
        char details[96];
        std::snprintf(details, sizeof(details), "pending=%zu all-done=1",
                      g_pendingCount);
        logAction("guest-action", 109, details);
    }
}

} // namespace rr3
