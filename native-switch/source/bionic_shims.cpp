#include "rr3/recovery_guard.hpp"
#include "rr3/bionic_shims.hpp"
#include "rr3/guest_view_trace.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace rr3 {
namespace {

[[noreturn]] void fortifyFailure(const char* operation) {
    recordGuestTermination(operation, 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::printf("\nRR3 Bionic fortify failure: %s\n", operation);
    std::fflush(stdout);
    std::abort();
}

bool knownSize(size_t size) {
    return size != std::numeric_limits<size_t>::max();
}

void requireCapacity(const char* operation, size_t required, size_t available) {
    if (knownSize(available) && required > available) {
        fortifyFailure(operation);
    }
}

int testVsnprintf(char* destination, size_t suppliedSize, size_t destinationSize,
                  const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result =
        bionicVsnprintfChk(destination, suppliedSize, 0, destinationSize, format, arguments);
    va_end(arguments);
    return result;
}

int testVsprintf(char* destination, size_t destinationSize, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = bionicVsprintfChk(destination, 0, destinationSize, format, arguments);
    va_end(arguments);
    return result;
}

} // namespace

int* bionicErrno() {
    return &errno;
}

void* bionicMemcpyChk(void* destination, const void* source, size_t length,
                      size_t destinationSize) {
    requireCapacity("__memcpy_chk", length, destinationSize);
    return std::memcpy(destination, source, length);
}

void* bionicMemmoveChk(void* destination, const void* source, size_t length,
                       size_t destinationSize) {
    requireCapacity("__memmove_chk", length, destinationSize);
    return std::memmove(destination, source, length);
}

void* bionicMemsetChk(void* destination, int value, size_t length,
                      size_t destinationSize) {
    requireCapacity("__memset_chk", length, destinationSize);
    return std::memset(destination, value, length);
}

char* bionicStrcatChk(char* destination, const char* source, size_t destinationSize) {
    const size_t required = std::strlen(destination) + std::strlen(source) + 1;
    requireCapacity("__strcat_chk", required, destinationSize);
    return std::strcat(destination, source);
}

char* bionicStrcpyChk(char* destination, const char* source, size_t destinationSize) {
    requireCapacity("__strcpy_chk", std::strlen(source) + 1, destinationSize);
    return std::strcpy(destination, source);
}

char* bionicStrncpyChk(char* destination, const char* source, size_t length,
                       size_t destinationSize) {
    requireCapacity("__strncpy_chk", length, destinationSize);
    return std::strncpy(destination, source, length);
}

char* bionicStrncpyChk2(char* destination, const char* source, size_t length,
                        size_t destinationSize, size_t sourceSize) {
    requireCapacity("__strncpy_chk2 destination", length, destinationSize);
    if (knownSize(sourceSize) && length > sourceSize &&
        std::memchr(source, '\0', sourceSize) == nullptr) {
        fortifyFailure("__strncpy_chk2 source");
    }
    return std::strncpy(destination, source, length);
}

int bionicVsnprintfChk(char* destination, size_t suppliedSize, int flags,
                       size_t destinationSize, const char* format, va_list arguments) {
    (void)flags;
    requireCapacity("__vsnprintf_chk", suppliedSize, destinationSize);
    return std::vsnprintf(destination, suppliedSize, format, arguments);
}

int bionicVsprintfChk(char* destination, int flags, size_t destinationSize,
                      const char* format, va_list arguments) {
    (void)flags;
    if (!knownSize(destinationSize)) {
        return std::vsprintf(destination, format, arguments);
    }
    return std::vsnprintf(destination, destinationSize, format, arguments);
}

[[noreturn]] void bionicStackChkFail() {
    guestViewTraceStackCheckFailure(reinterpret_cast<uintptr_t>(
        __builtin_return_address(0)));
    fortifyFailure("__stack_chk_fail");
}

int androidLogVPrint(int priority, const char* tag, const char* format,
                     va_list arguments) {
    const char* safeTag = tag != nullptr ? tag : "RR3";
    std::printf("[A%d/%s] ", priority, safeTag);
    const int result = std::vprintf(format, arguments);
    std::printf("\n");
    // Durable breadcrumb for VIEW/splash lines while the view probe owns
    // the progress path. Host TLS is valid here (guest workers use the
    // TPIDRRO_EL0 path for newlib).
    if (format != nullptr) {
        rr3::guestViewNoteAndroidLog(format);
    }
    return result;
}

int androidLogPrint(int priority, const char* tag, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = androidLogVPrint(priority, tag, format, arguments);
    va_end(arguments);
    return result;
}

uint64_t bionicShimSelfTest() {
    uint64_t passed = 0;

    char copyDestination[16]{};
    bionicMemcpyChk(copyDestination, "RR3", 4, sizeof(copyDestination));
    if (std::strcmp(copyDestination, "RR3") == 0) {
        passed |= 1ULL << 0;
    }

    char moveBuffer[16] = "ABCDE";
    bionicMemmoveChk(moveBuffer + 1, moveBuffer, 6, sizeof(moveBuffer) - 1);
    if (std::strcmp(moveBuffer, "AABCDE") == 0) {
        passed |= 1ULL << 1;
    }

    char setBuffer[4]{};
    bionicMemsetChk(setBuffer, 0x5a, 3, sizeof(setBuffer));
    if (setBuffer[0] == 'Z' && setBuffer[1] == 'Z' && setBuffer[2] == 'Z') {
        passed |= 1ULL << 2;
    }

    char stringBuffer[32]{};
    bionicStrcpyChk(stringBuffer, "RR3", sizeof(stringBuffer));
    if (std::strcmp(stringBuffer, "RR3") == 0) {
        passed |= 1ULL << 3;
    }
    bionicStrcatChk(stringBuffer, "-NX", sizeof(stringBuffer));
    if (std::strcmp(stringBuffer, "RR3-NX") == 0) {
        passed |= 1ULL << 4;
    }

    char strncpyBuffer[8]{};
    bionicStrncpyChk(strncpyBuffer, "AB", 3, sizeof(strncpyBuffer));
    if (std::strcmp(strncpyBuffer, "AB") == 0) {
        passed |= 1ULL << 5;
    }
    std::memset(strncpyBuffer, 0, sizeof(strncpyBuffer));
    bionicStrncpyChk2(strncpyBuffer, "CD", 3, sizeof(strncpyBuffer), 3);
    if (std::strcmp(strncpyBuffer, "CD") == 0) {
        passed |= 1ULL << 6;
    }

    char formatBuffer[32]{};
    if (testVsnprintf(formatBuffer, sizeof(formatBuffer), sizeof(formatBuffer),
                      "%s-%d", "RR", 3) == 4 &&
        std::strcmp(formatBuffer, "RR-3") == 0) {
        passed |= 1ULL << 7;
    }
    std::memset(formatBuffer, 0, sizeof(formatBuffer));
    if (testVsprintf(formatBuffer, sizeof(formatBuffer), "%s-%d", "NX", 1) == 4 &&
        std::strcmp(formatBuffer, "NX-1") == 0) {
        passed |= 1ULL << 8;
    }

    if (bionicErrno() != nullptr) {
        passed |= 1ULL << 9;
    }
    return passed;
}

} // namespace rr3
