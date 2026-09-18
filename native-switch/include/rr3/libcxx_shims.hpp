#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/types.h>
#include <time.h>

namespace rr3 {

constexpr size_t kBionicFileObjectSize = 152;
extern unsigned char bionicStandardFilesStorage[3 * kBionicFileObjectSize];
extern std::FILE* bionicStdoutObject;
extern std::FILE* bionicStderrObject;

size_t bionicCtypeGetMbCurMax();
int bionicOpen2(const char* path, int flags);
ssize_t bionicReadChk(int fd, void* destination, size_t count,
                      size_t destinationSize);
size_t bionicStrlenChk(const char* string, size_t stringSize);
void androidSetAbortMessage(const char* message);
const char* guestAbortMessage();
int bionicFflush(std::FILE* stream);
int bionicFprintf(std::FILE* stream, const char* format, ...);
int bionicFputc(int character, std::FILE* stream);
size_t bionicFwrite(const void* data, size_t size, size_t count, std::FILE* stream);
int bionicGetc(std::FILE* stream);
int bionicUngetc(int character, std::FILE* stream);
void bionicSetbuf(std::FILE* stream, char* buffer);
int bionicSetvbuf(std::FILE* stream, char* buffer, int mode, size_t size);
int bionicVfprintf(std::FILE* stream, const char* format, va_list arguments);

void libcxxCloseLog();
int libcxxFchmodAt(int directoryFd, const char* path, mode_t mode, int flags);
void libcxxOpenLog(const char* identity, int options, int facility);
int libcxxPosixMemalign(void** result, size_t alignment, size_t size);
ssize_t libcxxSendfile(int outputFd, int inputFd, off_t* offset, size_t count);
long libcxxSysconf(int name);
long libcxxSyscall(long number, ...);
void libcxxSyslog(int priority, const char* format, ...);
int libcxxUtimensAt(int directoryFd, const char* path,
                    const struct timespec times[2], int flags);

uint64_t libcxxShimSelfTest();
uint64_t libcxxSyscallSelfTest();
uint64_t bionicStdioSelfTest();

} // namespace rr3
