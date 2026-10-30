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
// WHAT A COMMAND RECEIVES. The guest's cheat commands are METHODS of the
// FrontEnd::MainMenuCheatScreen object. The guest's own dispatcher (the
// std::function invoke at 0xe3d958) computes
//
//     x0 = owner + (ctx >> 1)
//
// and 313 of the 317 commands carry ctx == 0, so the guest calls fn(owner).
// The port does the same, which is what lets it call the commands that READ
// their argument instead of only the ones that ignore it.
//
// The owner lives in a .bss cell the guest fills when it builds the cheat
// list: ELF 0x1fa1d00 (the GOT alias is 0x1f6dc88). The port checks the
// pointer before it calls anything with it and refuses the call if the
// object is not recognisable -- a wrong x0 takes the whole run down.
//
// The ctx word is the one thing the port cannot reproduce on its own: for a
// command whose ctx is NOT zero the guest calls fn(owner + ctx/2), which is
// not a method of the owner, and what that offset means is not established.
// In the FILE every ctx word is zero -- the function pointers are filled in
// by the loader from .rela.dyn, and the ctx slots carry no relocation -- but
// the table lives in writable data and the guest builds it at run time, so
// the port reads the ctx word from the RUNTIME image and refuses a command
// whose ctx is not zero. The refusal is logged, not guessed at.
//
// The names here are only a first filter. The call itself is selected by
// NAME against the guest's own runtime table, so even a wrong name here
// cannot invoke the wrong command: the name is looked up, and the index
// that comes back is the one that is called.
struct AllowedAction {
    const char* name;
    // True when the command READS its argument. Such a command is only
    // called when the owner could be identified, because calling it with a
    // zero argument is what takes the guest down.
    bool needsOwner;
};

constexpr AllowedAction kAllowedActions[] = {
    {"Character|Unlock All Tracks", false},
    {"Career|Reload event data", false},
    {"Career|Win all events", false},
    {"Career|Win tapped event", false},
    {"Assets|UPDATE EVENTS", false},
    // These three set a global flag or load the owner themselves, so they
    // never look at x0.
    {"Career|Bypass Locked Series", false},
    {"Soaks|START SOAK", false},
    // These read x0: they are called with the owner, and refused when the
    // owner cannot be identified.
    {"Character|Unlock + Own Everything", true},
    {"Debug Race Selector:", true},
    // THE GUEST'S OWN QA RACE FRAMEWORK.
    //
    // The guest ships a soak framework of its own: "Soaks|QUEUE SOAK:*"
    // pushes a test onto the guest's own test queue and "Soaks|START SOAK"
    // runs the queue's last entry, which builds a RACE REQUEST and hands it
    // to the guest's normal race flow.  That is a way into a race with no
    // pixels involved at all.
    //
    // REVERSE ENGINEERED 2026-10-30 (libRealRacing3.so):
    //   Soaks|QUEUE SOAK:RACE LOOP            fn 0xa6e22c  push_back(3)
    //   Soaks|QUEUE SOAK:RACE LOOP - fast fwd fn 0xa6e244  push_back(4)
    //   Soaks|START SOAK                      fn 0xa6e1e8  SoakMgr::Start(0)
    //   Soaks|CLEAR SOAKS                     fn 0xa6e218  clears the queue
    //
    // The two QUEUE commands OVERWRITE x0 before they use it
    // ("ldr x0,[x8,#0x78]"), so they do not read the owner and do not need
    // it: they are called with a zero argument.  That is why they carry
    // needsOwner == false here -- the owner's own dispatcher would pass the
    // owner, and the command would ignore it.
    //
    // The order matters: START takes the queue's LAST entry, so the QUEUE
    // has to run first.
    {"Soaks|QUEUE SOAK:RACE LOOP", false},
    {"Soaks|QUEUE SOAK:RACE LOOP - fast forward", false},
    {"Soaks|CLEAR SOAKS", false},
    {"Soaks|START SOAK LOOP", false},
    {"TestRecorder|Time Trial", true},
    {"Main Menu|Enter Deep Link", true},
};

constexpr size_t kAllowedActionCount =
    sizeof(kAllowedActions) / sizeof(kAllowedActions[0]);

bool isAllowed(const char* name, bool* needsOwner) {
    if (needsOwner != nullptr) {
        *needsOwner = false;
    }
    if (name == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kAllowedActionCount; ++i) {
        if (std::strcmp(kAllowedActions[i].name, name) == 0) {
            if (needsOwner != nullptr) {
                *needsOwner = kAllowedActions[i].needsOwner;
            }
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
// THE OWNER
// ---------------------------------------------------------------------------
//
// The cheat commands are METHODS of the FrontEnd::MainMenuCheatScreen, and
// the guest's dispatcher calls them with that object in x0. The object is
// reached by the chain the guest itself walks, and the port's own
// front-end probe already walks it:
//
//     .bss 0x1fb2688          the game global G
//     G + 0x17f8              the FrontEnd2::MainMenuManager
//     manager + 0x420         the state carrier: a MainMenuCheatScreen
//     ELF 0x1e78928           that class's vtable
//
// The vtable is the identity check. The constructor at 0xe1d374 installs
// it, and it is the only vtable in the image that carries the cheat
// screen's state machine, so an object whose first word is that vtable is
// the cheat screen and nothing else.
//
// Two checks have to pass before the port calls anything with the pointer,
// because a wrong x0 takes the whole run down:
//
//   1. the object's first word is the MainMenuCheatScreen vtable;
//   2. the cheat list it owns (owner+0x450/+0x458) holds the 317 entries
//      the guest registered.
//
// Both are reported in the log, so a run that refuses says WHY it refused,
// and a run that accepts says what it accepted.
constexpr uintptr_t kGameGlobalBssVa = 0x1FB2688ULL;
constexpr uintptr_t kManagerOff = 0x17F8ULL;
constexpr uintptr_t kStateCarrierOff = 0x420ULL;
constexpr uintptr_t kCheatScreenVtableVa = 0x1E78928ULL;
constexpr uintptr_t kOwnerListBeginOffset = 0x450ULL;
constexpr uintptr_t kOwnerListEndOffset = 0x458ULL;
constexpr uint64_t kExpectedCheatListSpan = 0x9E8ULL;   // 317 entries x 8 bytes

struct OwnerProbe {
    uint64_t game = 0;
    uint64_t manager = 0;
    uint64_t owner = 0;
    uint64_t vptr = 0;
    uint64_t listBegin = 0;
    uint64_t listEnd = 0;
    bool chainOk = false;
    bool classMatches = false;
    bool listMatches = false;
};

OwnerProbe readCheatScreenOwner(uintptr_t bias) {
    OwnerProbe probe{};
    uint64_t game = 0;
    if (!readGuestBytes(bias + kGameGlobalBssVa, &game, 8) || game == 0) {
        return probe;
    }
    probe.game = game;
    uint64_t manager = 0;
    if (!readGuestBytes(static_cast<uintptr_t>(game) + kManagerOff, &manager, 8) ||
        manager == 0) {
        return probe;
    }
    probe.manager = manager;
    uint64_t owner = 0;
    if (!readGuestBytes(static_cast<uintptr_t>(manager) + kStateCarrierOff, &owner, 8) ||
        owner == 0 || !pointerPlausible(static_cast<uintptr_t>(owner))) {
        return probe;
    }
    probe.owner = owner;
    probe.chainOk = true;
    if (!readGuestBytes(static_cast<uintptr_t>(owner), &probe.vptr, 8)) {
        return probe;
    }
    if (!readGuestBytes(static_cast<uintptr_t>(owner) + kOwnerListBeginOffset,
                        &probe.listBegin, 8)) {
        return probe;
    }
    if (!readGuestBytes(static_cast<uintptr_t>(owner) + kOwnerListEndOffset,
                        &probe.listEnd, 8)) {
        return probe;
    }
    probe.classMatches = (probe.vptr == bias + kCheatScreenVtableVa);
    probe.listMatches = (probe.listEnd >= probe.listBegin) &&
                        (probe.listEnd - probe.listBegin) == kExpectedCheatListSpan;
    return probe;
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

// Compares two command names, ignoring TRAILING whitespace.
//
// The guest's own names end with a colon AND A SPACE when the command takes
// a value ("Debug Race Selector: "), and the control file cannot carry that
// trailing space: the parser trims it, because a line's trailing whitespace
// is not part of its name. So the name that arrives from the control file is
// "Debug Race Selector:" and the name in the guest's table is "Debug Race
// Selector: ", and an exact comparison would never match.
bool namesMatch(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    size_t la = std::strlen(a);
    size_t lb = std::strlen(b);
    while (la > 0 && (a[la - 1] == ' ' || a[la - 1] == '\t')) {
        --la;
    }
    while (lb > 0 && (b[lb - 1] == ' ' || b[lb - 1] == '\t')) {
        --lb;
    }
    if (la != lb) {
        return false;
    }
    return std::memcmp(a, b, la) == 0;
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
        if (namesMatch(candidate, name)) {
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

// THE COMMAND TABLE.
//
// It used to hold EIGHT commands, and a ninth line in the control file was
// skipped SILENTLY. Two runs were wasted on that: the control file had
// eleven commands, the log said "pending=8", and the four commands that
// were supposed to start a race never ran.
//
// A command table is a table, not a queue: the control file is read ONCE,
// so a command that does not fit is lost for the whole run. Thirty-two is
// more than any experiment has needed, and the table is BSS.
constexpr size_t kMaxPendingActions = 32;

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
        // An optional "at=<frame>" postpones the call to the port's own
        // frame counter, so a command can wait for the front end to be up.
        //
        // IT MAY COME BEFORE THE NAME OR AFTER IT.
        //
        // The first version cut everything from "at=" onwards out of the
        // name, which only works when the frame comes LAST: a line written
        // the natural way round ("at=5950 Soaks|START SOAK") left an EMPTY
        // name, and the command was dropped SILENTLY -- a control file that
        // does nothing has to SAY that it did nothing.  The frame token is
        // removed wherever it is, and what is left is the name.
        //
        // The name may contain spaces ("Character|Unlock All Tracks"), so
        // the name is the whole rest of the line, not its first word.
        unsigned long long at = 0;
        bool haveAt = false;
        for (char* p = line; *p != '\0';) {
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            char* tokenEnd = p;
            while (*tokenEnd != '\0' && *tokenEnd != ' ' && *tokenEnd != '\t') {
                ++tokenEnd;
            }
            if (tokenEnd - p >= 3 && p[0] == 'a' && p[1] == 't' && p[2] == '=') {
                at = std::strtoull(p + 3, nullptr, 10);
                haveAt = true;
                std::memmove(p, tokenEnd, std::strlen(tokenEnd) + 1);
                p = line;
                continue;
            }
            p = tokenEnd;
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
            if (haveAt) {
                // A LINE THAT CARRYED A FRAME BUT NO NAME IS REPORTED, NOT
                // DROPPED.  A command that never runs and never says so is
                // indistinguishable from a command that was never written.
                char details[192];
                std::snprintf(details, sizeof(details),
                              "dropped=1 reason=no-command-name-after-the-frame "
                              "frame=%llu",
                              at);
                logAction("guest-action", 109, details);
            }
            continue;
        }
        // A COMMAND THAT DOES NOT FIT IS REPORTED, NOT DROPPED. The table
        // used to hold eight commands and the ninth line was skipped
        // SILENTLY, so a control file with eleven commands ran the first
        // eight and the last three never happened -- and the log said
        // "pending=8" as if that were the whole file. A command that is
        // dropped without a word is indistinguishable from a command that
        // was never written.
        if (g_pendingCount >= kMaxPendingActions) {
            char details[256];
            std::snprintf(details, sizeof(details),
                          "name=\"%.120s\" dropped=1 reason=the-command-table-is-full "
                          "(%u commands)",
                          name, static_cast<unsigned>(kMaxPendingActions));
            logAction("guest-action", 107, details);
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

    // THE OWNER, read once per pump. The commands are methods of it, so it
    // is what goes into x0 -- and a command that READS its argument needs it.
    // The probe is reported when it CHANGES, so a run that cannot call the
    // owner-reading commands says what it saw, and a run that can does not
    // repeat the same line every pump.
    const OwnerProbe owner = readCheatScreenOwner(bias);
    const bool ownerUsable = owner.chainOk && (owner.classMatches || owner.listMatches);
    {
        static uint64_t s_lastOwner = 0;
        static unsigned s_lastClass = 0;
        static unsigned s_lastList = 0;
        static unsigned s_lastUsable = 0;
        const unsigned classBit = owner.classMatches ? 1u : 0u;
        const unsigned listBit = owner.listMatches ? 1u : 0u;
        const unsigned usableBit = ownerUsable ? 1u : 0u;
        if (s_lastOwner != owner.owner || s_lastClass != classBit ||
            s_lastList != listBit || s_lastUsable != usableBit) {
            s_lastOwner = owner.owner;
            s_lastClass = classBit;
            s_lastList = listBit;
            s_lastUsable = usableBit;
            char details[384];
            std::snprintf(details, sizeof(details),
                          "owner=0x%llx chain=%d vptr=0x%llx class=%d "
                          "list=0x%llx-0x%llx span=0x%llx listmatch=%d usable=%d",
                          static_cast<unsigned long long>(owner.owner),
                          owner.chainOk ? 1 : 0,
                          static_cast<unsigned long long>(owner.vptr),
                          owner.classMatches ? 1 : 0,
                          static_cast<unsigned long long>(owner.listBegin),
                          static_cast<unsigned long long>(owner.listEnd),
                          static_cast<unsigned long long>(owner.listEnd >= owner.listBegin
                                                              ? owner.listEnd - owner.listBegin
                                                              : 0),
                          owner.listMatches ? 1 : 0,
                          ownerUsable ? 1 : 0);
            logAction("guest-action-owner", 108, details);
        }
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
        bool needsOwner = false;
        if (!isAllowed(entry.name, &needsOwner)) {
            entry.refused = true;
            char details[256];
            std::snprintf(details, sizeof(details),
                          "name=\"%.120s\" refused=1 reason=not-in-allowlist",
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
        // THE ctx WORD. The guest's dispatcher calls fn(owner + (ctx >> 1)),
        // so a command with a non-zero ctx is NOT a method of the owner and
        // the port cannot reproduce the call. Four commands are like that
        // ("Car|Damage Car" and friends); they are refused, and the refusal
        // is logged rather than guessed at.
        uint64_t ctx = 0;
        if (!readGuestBytes(slot + 8, &ctx, 8)) {
            allDone = false;
            continue;
        }
        if (ctx != 0) {
            entry.refused = true;
            char details[256];
            std::snprintf(details, sizeof(details),
                          "name=\"%.120s\" idx=%llu refused=1 "
                          "reason=non-zero-ctx-the-guest-would-call-owner+ctx/2",
                          entry.name, static_cast<unsigned long long>(index));
            logAction("guest-action", 110 + static_cast<unsigned>(i), details);
            allDone = false;
            continue;
        }
        // THE ARGUMENT. A command that reads x0 needs the owner; one that
        // does not is called with zero, which is what it was called with
        // before the owner was known.
        uintptr_t argument = 0;
        if (needsOwner) {
            if (!ownerUsable) {
                entry.refused = true;
                char details[256];
                std::snprintf(details, sizeof(details),
                              "name=\"%.120s\" idx=%llu refused=1 "
                              "reason=no-owner-the-command-reads-its-argument",
                              entry.name, static_cast<unsigned long long>(index));
                logAction("guest-action", 110 + static_cast<unsigned>(i), details);
                allDone = false;
                continue;
            }
            argument = static_cast<uintptr_t>(owner.owner);
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
        // CALL IT, with guest TLS and the argument the guest would use.
        char before[256];
        std::snprintf(before, sizeof(before), "name=\"%.120s\" idx=%llu fn=0x%llx "
                      "arg=0x%llx owner=%d",
                      entry.name,
                      static_cast<unsigned long long>(index),
                      static_cast<unsigned long long>(function),
                      static_cast<unsigned long long>(argument),
                      needsOwner ? 1 : 0);
        // The line BEFORE the call, so a command that takes the guest down
        // still leaves a record of WHICH command it was.
        logAction("guest-action", 110 + static_cast<unsigned>(i), before);
        {
            GuestTlsScope tls;
            using GuestActionFn = void (*)(uintptr_t);
            reinterpret_cast<GuestActionFn>(function)(argument);
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
