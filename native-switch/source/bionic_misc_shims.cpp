#include "rr3/recovery_guard.hpp"
#include "rr3/bionic_misc_shims.hpp"

#include "rr3/bionic_shims.hpp"
#include "rr3/libcxx_shims.hpp"

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
    errno = ENOENT;
    return 0;
}

uid_t bionicGeteuid() { return 0; }

int bionicGetpwuidR(uid_t, void*, char*, size_t, void** result) {
    if (result != nullptr) *result = nullptr;
    return ENOENT;
}

unsigned int bionicIfNametoindex(const char* name) {
    if (name != nullptr && std::strcmp(name, "lo") == 0) return 1;
    errno = ENODEV;
    return 0;
}

off_t bionicLseek64(int fd, off_t offset, int origin) { return ::lseek(fd, offset, origin); }
int bionicMadvise(void*, size_t, int) { return 0; }
int bionicMlock(const void*, size_t) { return 0; }

void* bionicMmap(void*, size_t length, int protection, int, int fd, off_t offset) {
    if (length == 0 || (protection & 4) != 0) {
        errno = protection & 4 ? ENOTSUP : EINVAL;
        return reinterpret_cast<void*>(-1);
    }
    const size_t allocationSize = (length + 0xfff) & ~size_t(0xfff);
    void* allocation = memalign(0x1000, allocationSize);
    if (allocation == nullptr) return reinterpret_cast<void*>(-1);
    std::memset(allocation, 0, allocationSize);
    if (fd >= 0) {
        const off_t original = ::lseek(fd, 0, SEEK_CUR);
        const bool positioned = original >= 0 && ::lseek(fd, offset, SEEK_SET) >= 0;
        const ssize_t readBytes = positioned ? ::read(fd, allocation, length) : -1;
        if (original >= 0) {
            ::lseek(fd, original, SEEK_SET);
        }
        if (readBytes < 0) {
            std::free(allocation);
            return reinterpret_cast<void*>(-1);
        }
    }
    mutexLock(&gMappingMutex);
    for (MappingEntry& entry : gMappings) {
        if (!entry.used) {
            entry = {allocation, allocationSize, true};
            mutexUnlock(&gMappingMutex);
            return allocation;
        }
    }
    mutexUnlock(&gMappingMutex);
    std::free(allocation);
    errno = ENOMEM;
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
        errno = EINVAL;
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
        errno = EINVAL;
        return -1;
    }
    std::free(entry->address);
    *entry = {};
    mutexUnlock(&gMappingMutex);
    return 0;
}

int bionicPipe(int descriptors[2]) {
    if (descriptors == nullptr) { errno = EINVAL; return -1; }
    return socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors);
}

long bionicPtrace(int, ...) { errno = ENOSYS; return -1; }
int bionicSigaction(int, const void*, void*) { return 0; }

int bionicStatfs(const char* path, void* output) {
    if (output == nullptr) { errno = EINVAL; return -1; }
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
    return passed;
}

} // namespace rr3
