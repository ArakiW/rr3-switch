#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace rr3 {

int* bionicErrno();
void* bionicMemcpyChk(void* destination, const void* source, size_t length,
                      size_t destinationSize);
void* bionicMemmoveChk(void* destination, const void* source, size_t length,
                       size_t destinationSize);
void* bionicMemsetChk(void* destination, int value, size_t length,
                      size_t destinationSize);
char* bionicStrcatChk(char* destination, const char* source, size_t destinationSize);
char* bionicStrcpyChk(char* destination, const char* source, size_t destinationSize);
char* bionicStrncpyChk(char* destination, const char* source, size_t length,
                       size_t destinationSize);
char* bionicStrncpyChk2(char* destination, const char* source, size_t length,
                        size_t destinationSize, size_t sourceSize);
int bionicVsnprintfChk(char* destination, size_t suppliedSize, int flags,
                       size_t destinationSize, const char* format, va_list arguments);
int bionicVsprintfChk(char* destination, int flags, size_t destinationSize,
                      const char* format, va_list arguments);
[[noreturn]] void bionicStackChkFail();
int androidLogPrint(int priority, const char* tag, const char* format, ...);
int androidLogVPrint(int priority, const char* tag, const char* format,
                     va_list arguments);
uint64_t bionicShimSelfTest();

} // namespace rr3
