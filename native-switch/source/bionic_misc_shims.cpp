#include "rr3/recovery_guard.hpp"
#include "rr3/bionic_misc_shims.hpp"
#include "rr3/guest_filesystem.hpp"

#include "rr3/bionic_shims.hpp"
#include "rr3/libcxx_shims.hpp"
#include "bionic_errno.hpp"

#include <switch.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <net/if.h>
#include <malloc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace rr3 {
namespace {

char gCtypeStorage[257]{};
char gBasenameBuffer[256]{};
bool gCtypeInitialized = false;

struct MappingEntry {
    void* address{};
    size_t size{};
    bool used{};
};

constexpr size_t kMaximumMappings = 128;
MappingEntry gMappings[kMaximumMappings]{};
Mutex gMappingMutex{};

// ---------------------------------------------------------------------------
// File-backed mmap probe
// ---------------------------------------------------------------------------
// WHY: the guest logs "Failed memory-mapping file: %s" and then tears down
// asset loading, and on this path the first thing that can return -1 is ours.
// Until this probe existed, bionicMmap had no observation on the file-backed
// branch at all: bionicMiscSelfTest only ever mapped with fd == -1, so the
// branch that actually fails could not fail in a test, and therefore never did.
//
// Successes are recorded as well as failures, deliberately. A probe that only
// prints when it breaks structurally cannot show the successful call we are
// comparing against, and "log only the failures" is exactly how the working
// case stays invisible.
//
// Successes are budgeted and then sampled, because the guest may map far more
// files than a log should hold. Failures are never budgeted, never sampled and
// never dropped, because the entire reason this probe exists is the failure.
constexpr unsigned kFileMapSuccessBudget = 400;
constexpr unsigned kFileMapSampleEvery = 100;
constexpr const char* kMmapProbeLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";

struct FileMapTally {
    unsigned long long calls;
    unsigned long long failures;
    unsigned long long anonymous;
    unsigned long long successes;
    size_t highWater;
};
FileMapTally gFileMap{};

// fd -> guest path, so a probe line can name the file instead of leaving the
// reader to correlate two logs by call ordering. The ring is written only on
// the open path, never on the map path.
//
// The SEQUENCE NUMBER is not decoration. A descriptor number is reused: the
// guest closes a file, opens the next one and the kernel hands back the same
// fd, so a ring indexed by fd alone silently reports the NEW file's slot with
// the OLD file's name whenever the map is not preceded by an open through
// guestOpen. Measured: 12 consecutive file mappings of 12 different files
// (lengths 125783, 129131, 130182, 196660, 125999, 71441, 52955, 6937, 342,
// 235, 342, 235) all printed the same path
// rr3veh0:/files/.depot/vehicles/shaders/spec_rotor.rgb.pvr.
//
// So every probe line carries the sequence the path was recorded at, and the
// reader can see that a path is stale instead of believing it. A ring lookup
// with a mismatched sequence prints fd-path-unverified rather than a name.
constexpr size_t kFdPathRingSize = 64;
struct FdPathNote {
    int fd;
    unsigned long long sequence;   // monotonic; 0 = never written
    char path[112];
};
FdPathNote gFdPaths[kFdPathRingSize]{};
unsigned gFdPathCursor = 0;
unsigned long long gFdPathSequence = 0;

struct BionicStatfs {
    uint64_t type;
    uint64_t blockSize;
    uint64_t blocks;
    uint64_t blocksFree;
    uint64_t blocksAvailable;
    uint64_t files;
    uint64_t filesFree;
    int32_t fsid[2];
    uint64_t nameLength;
    uint64_t fragmentSize;
    uint64_t flags;
    uint64_t spare[4];
};

[[noreturn]] void miscFortifyFailure(const char* operation) {
    recordGuestTermination(operation, 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::printf("RR3 Bionic misc failure: %s\n", operation);
    std::fflush(stdout);
    std::abort();
}

void initializeCtype() {
    if (gCtypeInitialized) {
        return;
    }
    constexpr unsigned char upper = 0x01;
    constexpr unsigned char lower = 0x02;
    constexpr unsigned char digit = 0x04;
    constexpr unsigned char space = 0x08;
    constexpr unsigned char punct = 0x10;
    constexpr unsigned char control = 0x20;
    constexpr unsigned char hex = 0x40;
    constexpr unsigned char blank = 0x80;
    for (int value = 0; value < 256; ++value) {
        unsigned char bits = 0;
        if (value >= 'A' && value <= 'Z') bits |= upper;
        if (value >= 'a' && value <= 'z') bits |= lower;
        if (value >= '0' && value <= '9') bits |= digit;
        if ((value >= '0' && value <= '9') ||
            (value >= 'A' && value <= 'F') ||
            (value >= 'a' && value <= 'f')) bits |= hex;
        if (value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
            value == '\f' || value == '\v') bits |= space;
        if (value == ' ' || value == '\t') bits |= blank;
        if (value < 0x20 || value == 0x7f) bits |= control;
        if (value >= 0x21 && value <= 0x7e &&
            (bits & (upper | lower | digit)) == 0) bits |= punct;
        gCtypeStorage[value + 1] = static_cast<char>(bits);
    }
    gCtypeInitialized = true;
}

MappingEntry* findMapping(void* address) {
    for (MappingEntry& entry : gMappings) {
        if (entry.used && entry.address == address) {
            return &entry;
        }
    }
    return nullptr;
}

// Returns the recorded path for an fd. `sequenceOut` gets the sequence the
// name was recorded at, and `verifiedOut` is false when the slot is empty or
// was overwritten by a later open -- i.e. when the name cannot be trusted.
const char* openedPathForFd(int fd, unsigned long long recordSequence,
                            bool* verifiedOut) {
    *verifiedOut = false;
    for (size_t i = 0; i < kFdPathRingSize; ++i) {
        if (gFdPaths[i].fd == fd && gFdPaths[i].path[0] != '\0') {
            *verifiedOut = (recordSequence != 0 && gFdPaths[i].sequence == recordSequence);
            return gFdPaths[i].path;
        }
    }
    return "<fd-not-in-open-ring>";
}

size_t occupiedMappingSlots() {
    size_t used = 0;
    for (const MappingEntry& entry : gMappings) {
        if (entry.used) ++used;
    }
    return used;
}

void logFileMapping(const char* outcome, const char* reason, size_t length,
                   int protection, int flags, int fd, off_t offset,
                   long long readBytes, void* result) {
    // The sequence is taken from the CURRENT ring entry, so it always matches
    // the name printed beside it. What it proves is that the entry was not
    // overwritten between the guest's open and this map -- not that the fd
    // was ever opened through guestOpen at all, which it may not have been.
    unsigned long long seq = 0;
    bool verified = false;
    const char* path = openedPathForFd(fd, 0, &verified);
    for (size_t i = 0; i < kFdPathRingSize && !verified; ++i) {
        if (gFdPaths[i].fd == fd && gFdPaths[i].path[0] != '\0') seq = gFdPaths[i].sequence;
    }
    const char* shown = verified ? path : "<fd-path-unverified>";
    char details[352];
    // "high" is the peak table occupancy, not the current one: a table that
    // filled to 128 and then emptied would look empty at the end of the run,
    // which is precisely the shape a capacity failure has.
    __builtin_snprintf(details, sizeof(details),
                       "len=%llu prot=0x%x flags=0x%x fd=%d off=%lld read=%lld "
                       "used=%zu/%zu high=%zu errno=%d ret=%s reason=%s seq=%llu path=%s",
                       static_cast<unsigned long long>(length), protection, flags,
                       fd, static_cast<long long>(offset), readBytes,
                       occupiedMappingSlots(), kMaximumMappings, gFileMap.highWater,
                       result == nullptr ? 0 : 1, result != nullptr ? "ok" : "FAIL",
                       reason, verified ? seq : 0ULL, shown);
    appendGuestBootLine(kMmapProbeLog, outcome, gFileMap.calls,
                        gFileMap.failures, details);
}

} // namespace

const char* bionicCtypePointer = gCtypeStorage + 1;
long bionicTimezone = 0;

int androidLogWrite(int priority, const char* tag, const char* text) {
    return androidLogPrint(priority, tag, "%s", text != nullptr ? text : "");
}

[[noreturn]] void bionicAssert2(const char* file, int line,
                                const char* function, const char* expression) {
    recordGuestTermination("assert2", static_cast<unsigned long long>(line),
                            reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::printf("Assertion failed: %s:%d %s: %s\n",
                file != nullptr ? file : "?", line,
                function != nullptr ? function : "?",
                expression != nullptr ? expression : "?");
    std::abort();
}

struct cmsghdr* bionicCmsgNextHeader(struct msghdr* message, struct cmsghdr* current) {
    if (message == nullptr || current == nullptr || current->cmsg_len < sizeof(cmsghdr)) {
        return nullptr;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(message->msg_control);
    const uintptr_t end = start + message->msg_controllen;
    const uintptr_t next = (reinterpret_cast<uintptr_t>(current) + current->cmsg_len +
                            sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
    return next <= end && sizeof(cmsghdr) <= end - next
        ? reinterpret_cast<cmsghdr*>(next)
        : nullptr;
}

int bionicFdIsSetChk(int fd, const fd_set* set, size_t setSize) {
    if (fd < 0 || fd >= FD_SETSIZE || set == nullptr || setSize < sizeof(fd_set)) {
        miscFortifyFailure("__FD_ISSET_chk");
    }
    return FD_ISSET(fd, set);
}

void bionicFdSetChk(int fd, fd_set* set, size_t setSize) {
    if (fd < 0 || fd >= FD_SETSIZE || set == nullptr || setSize < sizeof(fd_set)) {
        miscFortifyFailure("__FD_SET_chk");
    }
    FD_SET(fd, set);
}

size_t bionicFwriteChk(const void* data, size_t size, size_t count,
                       std::FILE* stream, size_t sourceSize) {
    if (size != 0 && count > SIZE_MAX / size) {
        miscFortifyFailure("__fwrite_chk overflow");
    }
    const size_t bytes = size * count;
    if (sourceSize != SIZE_MAX && bytes > sourceSize) {
        miscFortifyFailure("__fwrite_chk bounds");
    }
    return bionicFwrite(data, size, count, stream);
}

char* bionicStrchrChk(const char* string, int character, size_t size) {
    if (std::memchr(string, '\0', size) == nullptr) miscFortifyFailure("__strchr_chk");
    return const_cast<char*>(std::strchr(string, character));
}

char* bionicStrrchrChk(const char* string, int character, size_t size) {
    if (std::memchr(string, '\0', size) == nullptr) miscFortifyFailure("__strrchr_chk");
    return const_cast<char*>(std::strrchr(string, character));
}

int bionicRegisterAtFork(void (*)(), void (*)(), void (*)(), void*) { return 0; }

char* bionicBasename(const char* path) {
    if (path == nullptr || path[0] == '\0') return const_cast<char*>(".");
    const char* end = path + std::strlen(path);
    while (end > path && end[-1] == '/') --end;
    const char* start = end;
    while (start > path && start[-1] != '/') --start;
    const size_t length = std::min<size_t>(end - start, sizeof(gBasenameBuffer) - 1);
    std::memcpy(gBasenameBuffer, start, length);
    gBasenameBuffer[length] = '\0';
    return gBasenameBuffer;
}

int bionicFchown(int, uid_t, gid_t) { return 0; }

unsigned long bionicGetauxval(unsigned long type) {
    if (type == AT_PAGESZ) return 0x1000;
    if (type == AT_HWCAP || type == AT_HWCAP2) return 0;
    errno = kGuestErrnoENOENT;
    return 0;
}

uid_t bionicGeteuid() { return 0; }

int bionicGetpwuidR(uid_t, void*, char*, size_t, void** result) {
    if (result != nullptr) *result = nullptr;
    return ENOENT;
}

unsigned int bionicIfNametoindex(const char* name) {
    if (name != nullptr && std::strcmp(name, "lo") == 0) return 1;
    errno = kGuestErrnoENODEV;
    return 0;
}

off_t bionicLseek64(int fd, off_t offset, int origin) { return ::lseek(fd, offset, origin); }
int bionicMadvise(void*, size_t, int) { return 0; }
int bionicMlock(const void*, size_t) { return 0; }

void* bionicMmap(void*, size_t length, int protection, int flags, int fd, off_t offset) {
    const bool fileBacked = fd >= 0;
    if (fileBacked) {
        ++gFileMap.calls;
    } else {
        ++gFileMap.anonymous;
    }
    // Each rejection below is logged with its own reason. They are separate
    // branches that all collapse to the same -1, and the guest only reports
    // "failed to map" -- so without the reason tag there is no way to tell
    // them apart from the return value alone.
    if (length == 0 || (protection & 4) != 0) {
        errno = protection & 4 ? ENOTSUP : EINVAL;
        if (fileBacked) {
            ++gFileMap.failures;
            logFileMapping("mmap-file-fail",
                           (protection & 4) != 0 ? "prot-exec" : "zero-length",
                           length, protection, flags, fd, offset, -1, nullptr);
        }
        return reinterpret_cast<void*>(-1);
    }
    const size_t allocationSize = (length + 0xfff) & ~size_t(0xfff);
    void* allocation = memalign(0x1000, allocationSize);
    if (allocation == nullptr) {
        if (fileBacked) {
            ++gFileMap.failures;
            logFileMapping("mmap-file-fail", "memalign", length, protection, flags,
                           fd, offset, -1, nullptr);
        }
        return reinterpret_cast<void*>(-1);
    }
    std::memset(allocation, 0, allocationSize);
    ssize_t readBytes = -1;
    if (fileBacked) {
        const off_t original = ::lseek(fd, 0, SEEK_CUR);
        const bool positioned = original >= 0 && ::lseek(fd, offset, SEEK_SET) >= 0;
        readBytes = positioned ? ::read(fd, allocation, length) : -1;
        if (original >= 0) {
            ::lseek(fd, original, SEEK_SET);
        }
        if (readBytes < 0) {
            if (!positioned) {
                logFileMapping("mmap-file-fail", "lseek", length, protection, flags,
                               fd, offset, -1, nullptr);
            } else {
                logFileMapping("mmap-file-fail", "read", length, protection, flags,
                               fd, offset, readBytes, nullptr);
            }
            ++gFileMap.failures;
            std::free(allocation);
            return reinterpret_cast<void*>(-1);
        }
    }
    mutexLock(&gMappingMutex);
    for (MappingEntry& entry : gMappings) {
        if (!entry.used) {
            entry = {allocation, allocationSize, true};
            mutexUnlock(&gMappingMutex);
            ++gFileMap.successes;
            if (gFileMap.highWater < occupiedMappingSlots()) {
                gFileMap.highWater = occupiedMappingSlots();
            }
            // A short read is not a failure here: mmap of a region past EOF
            // legitimately zero-fills the tail, and the allocation was already
            // memset. It is still recorded, because "read returned fewer bytes
            // than we asked for" is the other thing worth seeing.
            if (fileBacked &&
                (gFileMap.successes <= kFileMapSuccessBudget ||
                 gFileMap.successes % kFileMapSampleEvery == 0)) {
                logFileMapping("mmap-file-ok", "ok", length, protection, flags, fd,
                               offset, static_cast<long long>(readBytes), allocation);
            }
            return allocation;
        }
    }
    mutexUnlock(&gMappingMutex);
    if (fileBacked) {
        ++gFileMap.failures;
        logFileMapping("mmap-file-fail", "table-full", length, protection, flags, fd,
                       offset, -1, nullptr);
    }
    std::free(allocation);
    errno = kGuestErrnoENOMEM;
    return reinterpret_cast<void*>(-1);
}

int bionicMprotect(void* address, size_t, int protection) {
    mutexLock(&gMappingMutex);
    const bool found = findMapping(address) != nullptr;
    mutexUnlock(&gMappingMutex);
    if (!found || (protection & 4) != 0) {
        errno = !found ? EINVAL : ENOTSUP;
        return -1;
    }
    return 0;
}

void* bionicMremap(void* oldAddress, size_t, size_t newSize, int, ...) {
    mutexLock(&gMappingMutex);
    MappingEntry* entry = findMapping(oldAddress);
    if (entry == nullptr) {
        mutexUnlock(&gMappingMutex);
        errno = kGuestErrnoEINVAL;
        return reinterpret_cast<void*>(-1);
    }
    const size_t allocationSize = (newSize + 0xfff) & ~size_t(0xfff);
    void* replacement = memalign(0x1000, allocationSize);
    if (replacement == nullptr) {
        mutexUnlock(&gMappingMutex);
        return reinterpret_cast<void*>(-1);
    }
    std::memset(replacement, 0, allocationSize);
    std::memcpy(replacement, entry->address, std::min(entry->size, allocationSize));
    std::free(entry->address);
    entry->address = replacement;
    entry->size = allocationSize;
    mutexUnlock(&gMappingMutex);
    return replacement;
}

int bionicMunmap(void* address, size_t) {
    mutexLock(&gMappingMutex);
    MappingEntry* entry = findMapping(address);
    if (entry == nullptr) {
        mutexUnlock(&gMappingMutex);
        errno = kGuestErrnoEINVAL;
        return -1;
    }
    std::free(entry->address);
    *entry = {};
    mutexUnlock(&gMappingMutex);
    return 0;
}

int bionicPipe(int descriptors[2]) {
    if (descriptors == nullptr) { errno = kGuestErrnoEINVAL; return -1; }
    return socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors);
}

long bionicPtrace(int, ...) { errno = kGuestErrnoENOSYS; return -1; }
// Deliberately still a no-op, and the return value is deliberately still 0.
//
// Checked before changing this, and the result was decisive:
//  * devkitA64's newlib DECLARES sigaction in sys/signal.h:184 but defines no
//    sigaction symbol in any of its archives (libc.a, libnosys.a, libg.a all
//    checked with nm), so there is no host sigaction to forward to. Only
//    signal/raise exist.
//  * bionic and newlib disagree about struct sigaction and about sigset_t
//    numbering, and this file has no bionic struct definition to convert from.
//    Guessing the offset would register the wrong function pointer.
//  * SA_ONSTACK / sigaltstack would be silently dropped by a signal()-based
//    fallback, so the guest could believe it has an altstack it does not have.
//  * And the case that actually matters is unreachable anyway: a guest SIGSEGV
//    is a CPU exception on a guest thread, so __libnx_exception_handler
//    (recovery_guard.cpp) runs first and calls svcExitProcess(). The guest's
//    SIGSEGV handler could never be reached, by this shim or any other, unless
//    the deliberate exit-on-exception policy is replaced by a resuming
//    exception stack, which is out of scope here.
//
// A half-correct forwarding path would be worse than this: it would look
// installed and still never run. Revisit only alongside a real exception path.
int bionicSigaction(int, const void*, void*) { return 0; }

// Defined out here rather than in the anonymous namespace above because
// guestOpen (guest_filesystem.cpp) calls it; an anonymous-namespace definition
// has internal linkage and would not match the header declaration.
void bionicNoteOpenedFd(int fd, const char* path) {
    if (fd < 0 || path == nullptr) return;
    const size_t index = gFdPathCursor++ % kFdPathRingSize;
    gFdPaths[index].fd = fd;
    gFdPaths[index].sequence = ++gFdPathSequence;
    const size_t length =
        std::min(std::strlen(path), sizeof(gFdPaths[index].path) - 1);
    std::memcpy(gFdPaths[index].path, path, length);
    gFdPaths[index].path[length] = '\0';
}

int bionicStatfs(const char* path, void* output) {
    if (output == nullptr) { errno = kGuestErrnoEINVAL; return -1; }
    struct statvfs source{};
    if (statvfs(path, &source) != 0) return -1;
    auto* target = static_cast<BionicStatfs*>(output);
    *target = {};
    target->blockSize = source.f_bsize;
    target->fragmentSize = source.f_frsize;
    target->blocks = source.f_blocks;
    target->blocksFree = source.f_bfree;
    target->blocksAvailable = source.f_bavail;
    target->files = source.f_files;
    target->filesFree = source.f_ffree;
    target->nameLength = source.f_namemax;
    target->flags = source.f_flag;
    return 0;
}

// A file-backed mapping, end to end: create, write, open, map, verify the
// bytes came back, unmap, unlink.
//
// It writes its own input rather than opening a file the project happens to
// ship, for two reasons. A hard-coded path is a guess that goes stale the day
// the data volumes move, and a self-test whose expectation depends on a file it
// does not control fails for reasons unrelated to what it is testing. Writing
// the file makes the input a known constant AND lets the test compare the
// mapped bytes against it -- "did not return -1" cannot tell a correct mapping
// from a zero-filled one, because this shim memsets the allocation either way.
//
// The rejection branches (zero length, PROT_EXEC, table full) are deliberately
// NOT exercised here. Each one returns -1 through a path that the mmap probe
// logs as a real failure, and a self-test that manufactures those failures at
// boot would poison the very log this coverage gap was opened to read.
bool fileMappingSelfTest() {
    constexpr const char* kPath = "sdmc:/rr3-mmap-selftest.tmp";
    unsigned char payload[0x2000];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31u + 7u) & 0xff);
    }
    payload[0] = 0xa5;  // never a zero byte, so a zeroed mapping is detectable

    const int writeFd = ::open(kPath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (writeFd < 0) return false;
    const bool wrote = ::write(writeFd, payload, sizeof(payload)) ==
                       static_cast<ssize_t>(sizeof(payload));
    ::close(writeFd);
    if (!wrote) { ::unlink(kPath); return false; }

    const int readFd = ::open(kPath, O_RDONLY);
    if (readFd < 0) { ::unlink(kPath); return false; }
    bionicNoteOpenedFd(readFd, kPath);
    void* mapping = bionicMmap(nullptr, sizeof(payload), 1 /* PROT_READ */, 0,
                               readFd, 0);
    ::close(readFd);
    if (mapping == reinterpret_cast<void*>(-1)) { ::unlink(kPath); return false; }

    bool ok = std::memcmp(mapping, payload, sizeof(payload)) == 0;
    ok = ok && bionicMprotect(mapping, sizeof(payload), 1) == 0;
    ok = ok && bionicMunmap(mapping, sizeof(payload)) == 0;
    ::unlink(kPath);
    return ok;
}

uint64_t bionicMiscSelfTest() {
    initializeCtype();
    uint64_t passed = 0;
    if ((static_cast<unsigned char>(bionicCtypePointer['A']) & 0x01) != 0 &&
        (static_cast<unsigned char>(bionicCtypePointer['9']) & 0x04) != 0) passed |= 1ULL;
    if (std::strcmp(bionicBasename("/switch/rr3/file.bin"), "file.bin") == 0)
        passed |= 1ULL << 1;
    fd_set set{};
    bionicFdSetChk(3, &set, sizeof(set));
    if (bionicFdIsSetChk(3, &set, sizeof(set)) != 0) passed |= 1ULL << 2;
    void* mapping = bionicMmap(nullptr, 0x1800, 3, 0, -1, 0);
    if (mapping != reinterpret_cast<void*>(-1) &&
        bionicMprotect(mapping, 0x1800, 3) == 0 && bionicMunmap(mapping, 0x1800) == 0)
        passed |= 1ULL << 3;
    if (bionicGetauxval(AT_PAGESZ) == 0x1000 && bionicGeteuid() == 0)
        passed |= 1ULL << 4;
    if (bionicStdoutObject != nullptr && bionicStderrObject != nullptr)
        passed |= 1ULL << 5;
    if (fileMappingSelfTest()) passed |= 1ULL << 6;
    if (bionicStatLayoutSelfTest()) passed |= 1ULL << 7;
    return passed;
}

} // namespace rr3
