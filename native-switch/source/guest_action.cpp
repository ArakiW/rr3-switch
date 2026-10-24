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
bool guestReadable(uintptr_t address) {
    if (address == 0) {
        return false;
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        return false;
    }
    return (info.perm & Perm_R) != 0;
}

bool readGuestBytes(uintptr_t address, void* out, size_t bytes) {
    if (address == 0 || out == nullptr || bytes == 0) {
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
    if (length + 1 > outCapacity) {
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
    bool done{};
    bool refused{};
};

constexpr size_t kMaxPendingActions = 8;

PendingAction g_pending[kMaxPendingActions];
size_t g_pendingCount = 0;
bool g_confRead = false;
unsigned long long g_lastAttemptFrame = 0;

// How often the pending list is retried. The name table only exists once
// the guest has built it, so the first attempts are expected to fail; a
// short period keeps the run responsive without hammering the guest.
constexpr unsigned long long kAttemptPeriodFrames = 60ULL;

void readActionConf() {
    g_confRead = true;
    std::FILE* file = std::fopen(kActionConfPath, "rb");
    if (file == nullptr) {
        return;
    }
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
    if (allDone) {
        char details[96];
        std::snprintf(details, sizeof(details), "pending=%zu all-done=1",
                      g_pendingCount);
        logAction("guest-action", 109, details);
    }
}

} // namespace rr3
