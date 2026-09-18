#include "rr3/recovery_guard.hpp"
#include "rr3/libcxx_shims.hpp"

#include <switch.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace rr3 {

alignas(16) unsigned char bionicStandardFilesStorage[3 * kBionicFileObjectSize]{};
std::FILE* bionicStdoutObject = reinterpret_cast<std::FILE*>(
    bionicStandardFilesStorage + kBionicFileObjectSize);
std::FILE* bionicStderrObject = reinterpret_cast<std::FILE*>(
    bionicStandardFilesStorage + 2 * kBionicFileObjectSize);

namespace {

constexpr int kAndroidAtFdcwd = -100;
constexpr int kTargetAtFdcwd = -2;
constexpr int kAndroidScClkTck = 0x0006;
constexpr int kAndroidScPageSize = 0x0027;
constexpr int kAndroidScPageSizeAlias = 0x0028;
constexpr int kAndroidScProcessorsConfigured = 0x0060;
constexpr int kAndroidScProcessorsOnline = 0x0061;
constexpr long kAarch64SysFutex = 98;
constexpr long kAarch64SysGetTid = 178;
constexpr int kFutexWait = 0;
constexpr int kFutexWake = 1;
constexpr int kFutexPrivateFlag = 128;

thread_local char gAbortMessage[1024]{};
char gLogIdentity[64] = "RR3-libc++";

[[noreturn]] void libcxxFortifyFailure(const char* operation) {
    recordGuestTermination(operation, 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::printf("\nRR3 libc++ fortify failure: %s\n", operation);
    std::fflush(stdout);
    std::abort();
}

bool isCurrentDirectoryFd(int directoryFd) {
    return directoryFd == kAndroidAtFdcwd || directoryFd == kTargetAtFdcwd;
}

void libcxxVsyslog(int priority, const char* format, va_list arguments) {
    std::printf("[S%d/%s] ", priority, gLogIdentity);
    std::vprintf(format != nullptr ? format : "(null)", arguments);
    std::printf("\n");
}

std::FILE* translateBionicStandardFile(std::FILE* stream) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(stream);
    const uintptr_t base = reinterpret_cast<uintptr_t>(bionicStandardFilesStorage);
    if (value == base) {
        return stdin;
    }
    if (value == base + kBionicFileObjectSize) {
        return stdout;
    }
    if (value == base + 2 * kBionicFileObjectSize) {
        return stderr;
    }
    return stream;
}

} // namespace

size_t bionicCtypeGetMbCurMax() {
    return MB_CUR_MAX;
}

int bionicOpen2(const char* path, int flags) {
    if ((flags & O_CREAT) != 0) {
        libcxxFortifyFailure("__open_2 called with O_CREAT");
    }
    return ::open(path, flags);
}

ssize_t bionicReadChk(int fd, void* destination, size_t count,
                      size_t destinationSize) {
    if (count > destinationSize) {
        libcxxFortifyFailure("__read_chk");
    }
    return ::read(fd, destination, count);
}

size_t bionicStrlenChk(const char* string, size_t stringSize) {
    const void* terminator = std::memchr(string, '\0', stringSize);
    if (terminator == nullptr) {
        libcxxFortifyFailure("__strlen_chk");
    }
    return static_cast<const char*>(terminator) - string;
}

const char* guestAbortMessage() {
    return gAbortMessage;
}

void androidSetAbortMessage(const char* message) {
    if (message == nullptr) {
        gAbortMessage[0] = '\0';
        return;
    }
    std::strncpy(gAbortMessage, message, sizeof(gAbortMessage) - 1);
    gAbortMessage[sizeof(gAbortMessage) - 1] = '\0';
}

int bionicFflush(std::FILE* stream) {
    return std::fflush(stream != nullptr ? translateBionicStandardFile(stream) : nullptr);
}

int bionicVfprintf(std::FILE* stream, const char* format, va_list arguments) {
    return std::vfprintf(translateBionicStandardFile(stream), format, arguments);
}

int bionicFprintf(std::FILE* stream, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = bionicVfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int bionicFputc(int character, std::FILE* stream) {
    return std::fputc(character, translateBionicStandardFile(stream));
}

size_t bionicFwrite(const void* data, size_t size, size_t count, std::FILE* stream) {
    return std::fwrite(data, size, count, translateBionicStandardFile(stream));
}

int bionicGetc(std::FILE* stream) {
    return std::getc(translateBionicStandardFile(stream));
}

int bionicUngetc(int character, std::FILE* stream) {
    return std::ungetc(character, translateBionicStandardFile(stream));
}

void bionicSetbuf(std::FILE* stream, char* buffer) {
    std::setbuf(translateBionicStandardFile(stream), buffer);
}

int bionicSetvbuf(std::FILE* stream, char* buffer, int mode, size_t size) {
    return std::setvbuf(translateBionicStandardFile(stream), buffer, mode, size);
}

void libcxxCloseLog() {
    std::strcpy(gLogIdentity, "RR3-libc++");
}

int libcxxFchmodAt(int directoryFd, const char* path, mode_t mode, int flags) {
    if (!isCurrentDirectoryFd(directoryFd) || flags != 0) {
        errno = ENOTSUP;
        return -1;
    }
    return ::chmod(path, mode);
}

void libcxxOpenLog(const char* identity, int options, int facility) {
    (void)options;
    (void)facility;
    if (identity == nullptr || identity[0] == '\0') {
        std::strcpy(gLogIdentity, "RR3-libc++");
        return;
    }
    std::strncpy(gLogIdentity, identity, sizeof(gLogIdentity) - 1);
    gLogIdentity[sizeof(gLogIdentity) - 1] = '\0';
}

int libcxxPosixMemalign(void** result, size_t alignment, size_t size) {
    if (result == nullptr || alignment < sizeof(void*) ||
        (alignment & (alignment - 1)) != 0 || alignment % sizeof(void*) != 0) {
        return EINVAL;
    }
    void* allocation = ::memalign(alignment, size);
    if (allocation == nullptr) {
        return ENOMEM;
    }
    *result = allocation;
    return 0;
}

ssize_t libcxxSendfile(int outputFd, int inputFd, off_t* offset, size_t count) {
    off_t originalPosition = -1;
    if (offset != nullptr) {
        originalPosition = ::lseek(inputFd, 0, SEEK_CUR);
        if (originalPosition < 0 || ::lseek(inputFd, *offset, SEEK_SET) < 0) {
            return -1;
        }
    }

    char buffer[16 * 1024];
    size_t remaining = count;
    ssize_t total = 0;
    int savedErrno = 0;
    while (remaining != 0) {
        const size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t received = ::read(inputFd, buffer, request);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            savedErrno = errno;
            if (total == 0) {
                total = -1;
            }
            break;
        }

        ssize_t written = 0;
        while (written < received) {
            const ssize_t result =
                ::write(outputFd, buffer + written, static_cast<size_t>(received - written));
            if (result <= 0) {
                savedErrno = result < 0 ? errno : EIO;
                if (total == 0 && written == 0) {
                    total = -1;
                }
                remaining = 0;
                break;
            }
            written += result;
            total += result;
            remaining -= static_cast<size_t>(result);
        }
        if (written != received) {
            break;
        }
    }

    if (offset != nullptr) {
        const off_t finalPosition = ::lseek(inputFd, 0, SEEK_CUR);
        if (finalPosition >= 0) {
            *offset = finalPosition;
        }
        if (::lseek(inputFd, originalPosition, SEEK_SET) < 0 && savedErrno == 0) {
            savedErrno = errno;
            if (total == 0) {
                total = -1;
            }
        }
    }
    if (savedErrno != 0) {
        errno = savedErrno;
    }
    return total;
}

long libcxxSysconf(int name) {
    switch (name) {
        case kAndroidScClkTck:
            return 100;
        case kAndroidScPageSize:
        case kAndroidScPageSizeAlias:
            return 0x1000;
        case kAndroidScProcessorsConfigured:
        case kAndroidScProcessorsOnline:
            return 4;
        default:
            errno = EINVAL;
            return -1;
    }
}

long libcxxSyscall(long number, ...) {
    if (number == kAarch64SysGetTid) {
        u64 threadId = 0;
        const Result result = svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
        if (R_FAILED(result)) {
            errno = EIO;
            return -1;
        }
        return static_cast<long>(threadId);
    }
    if (number != kAarch64SysFutex) {
        errno = ENOSYS;
        return -1;
    }

    va_list arguments;
    va_start(arguments, number);
    auto* address = va_arg(arguments, int32_t*);
    const int operation = va_arg(arguments, int);
    const int value = va_arg(arguments, int);
    const auto* timeout = va_arg(arguments, const struct timespec*);
    (void)va_arg(arguments, void*);
    (void)va_arg(arguments, int);
    va_end(arguments);

    if (address == nullptr || (reinterpret_cast<uintptr_t>(address) & 3U) != 0) {
        errno = EINVAL;
        return -1;
    }
    const int command = operation & ~kFutexPrivateFlag;
    if (command == kFutexWake) {
        const Result result = svcSignalToAddress(
            address, SignalType_Signal, 0, value < 0 ? 0 : value);
        if (R_FAILED(result)) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    if (command != kFutexWait) {
        errno = ENOSYS;
        return -1;
    }
    if (*reinterpret_cast<volatile int32_t*>(address) != value) {
        errno = EAGAIN;
        return -1;
    }

    s64 timeoutNanoseconds = -1;
    if (timeout != nullptr) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L ||
            timeout->tv_sec > (INT64_MAX - timeout->tv_nsec) / 1000000000LL) {
            errno = EINVAL;
            return -1;
        }
        timeoutNanoseconds = static_cast<s64>(timeout->tv_sec) * 1000000000LL +
            static_cast<s64>(timeout->tv_nsec);
    }
    const Result result = svcWaitForAddress(
        address, ArbitrationType_WaitIfEqual, value, timeoutNanoseconds);
    if (R_FAILED(result)) {
        errno = timeout != nullptr ? ETIMEDOUT : EAGAIN;
        return -1;
    }
    return 0;
}

void libcxxSyslog(int priority, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    libcxxVsyslog(priority, format, arguments);
    va_end(arguments);
}

int libcxxUtimensAt(int directoryFd, const char* path,
                    const struct timespec times[2], int flags) {
    if (!isCurrentDirectoryFd(directoryFd) || flags != 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (times == nullptr) {
        return ::utimes(path, nullptr);
    }

    struct timeval converted[2]{};
    for (size_t index = 0; index < 2; ++index) {
        if (times[index].tv_nsec < 0 || times[index].tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        converted[index].tv_sec = times[index].tv_sec;
        converted[index].tv_usec = static_cast<suseconds_t>(times[index].tv_nsec / 1000);
    }
    return ::utimes(path, converted);
}

uint64_t libcxxShimSelfTest() {
    uint64_t passed = 0;

    if (bionicCtypeGetMbCurMax() >= 1) {
        passed |= 1ULL << 0;
    }
    if (bionicStrlenChk("libc++", 7) == 6) {
        passed |= 1ULL << 1;
    }

    int fd = bionicOpen2("romfs:/libc++_shared.so", O_RDONLY);
    if (fd >= 0) {
        unsigned char magic[4]{};
        if (bionicReadChk(fd, magic, sizeof(magic), sizeof(magic)) == 4 &&
            magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            passed |= 1ULL << 2;
        }
        ::close(fd);
    }

    void* allocation = nullptr;
    if (libcxxPosixMemalign(&allocation, 64, 127) == 0 && allocation != nullptr &&
        (reinterpret_cast<uintptr_t>(allocation) & 63U) == 0) {
        passed |= 1ULL << 3;
    }
    std::free(allocation);

    if (libcxxSysconf(kAndroidScPageSize) == 0x1000 &&
        libcxxSysconf(kAndroidScProcessorsOnline) == 4) {
        passed |= 1ULL << 4;
    }

    androidSetAbortMessage("RR3-v8");
    if (std::strcmp(gAbortMessage, "RR3-v8") == 0) {
        passed |= 1ULL << 5;
    }

    errno = 0;
    if (libcxxFchmodAt(123, "unused", 0, 0) == -1 && errno == ENOTSUP) {
        passed |= 1ULL << 6;
    }
    errno = 0;
    if (libcxxUtimensAt(123, "unused", nullptr, 0) == -1 && errno == ENOTSUP) {
        passed |= 1ULL << 7;
    }

    return passed;
}

uint64_t libcxxSyscallSelfTest() {
    uint64_t passed = 0;
    const long firstThreadId = libcxxSyscall(kAarch64SysGetTid);
    const long secondThreadId = libcxxSyscall(kAarch64SysGetTid);
    if (firstThreadId > 0 && firstThreadId == secondThreadId) {
        passed |= 1ULL << 0;
    }

    alignas(4) int32_t futexWord = 5;
    errno = 0;
    if (libcxxSyscall(kAarch64SysFutex, &futexWord,
                      kFutexWait | kFutexPrivateFlag, 6,
                      static_cast<const struct timespec*>(nullptr), nullptr, 0) == -1 &&
        errno == EAGAIN) {
        passed |= 1ULL << 1;
    }
    if (libcxxSyscall(kAarch64SysFutex, &futexWord,
                      kFutexWake | kFutexPrivateFlag, 1,
                      static_cast<const struct timespec*>(nullptr), nullptr, 0) == 0) {
        passed |= 1ULL << 2;
    }
    return passed;
}

uint64_t bionicStdioSelfTest() {
    uint64_t passed = 0;
    auto* guestStdin = reinterpret_cast<std::FILE*>(bionicStandardFilesStorage);
    auto* guestStdout = reinterpret_cast<std::FILE*>(
        bionicStandardFilesStorage + kBionicFileObjectSize);
    auto* guestStderr = reinterpret_cast<std::FILE*>(
        bionicStandardFilesStorage + 2 * kBionicFileObjectSize);

    if (reinterpret_cast<uintptr_t>(guestStdout) - reinterpret_cast<uintptr_t>(guestStdin) ==
            kBionicFileObjectSize &&
        reinterpret_cast<uintptr_t>(guestStderr) - reinterpret_cast<uintptr_t>(guestStdin) ==
            2 * kBionicFileObjectSize) {
        passed |= 1ULL << 0;
    }
    if (bionicFwrite("", 0, 0, guestStdout) == 0 && bionicFflush(guestStdout) == 0) {
        passed |= 1ULL << 1;
    }
    if (bionicFprintf(guestStderr, "") == 0) {
        passed |= 1ULL << 2;
    }
    return passed;
}

} // namespace rr3
