#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace rr3 {

extern const char* bionicCtypePointer;
extern long bionicTimezone;

int androidLogWrite(int priority, const char* tag, const char* text);
[[noreturn]] void bionicAssert2(const char* file, int line,
                                const char* function, const char* expression);
struct cmsghdr* bionicCmsgNextHeader(struct msghdr* message, struct cmsghdr* current);
int bionicFdIsSetChk(int fd, const fd_set* set, size_t setSize);
void bionicFdSetChk(int fd, fd_set* set, size_t setSize);
size_t bionicFwriteChk(const void* data, size_t size, size_t count,
                       std::FILE* stream, size_t sourceSize);
char* bionicStrchrChk(const char* string, int character, size_t size);
char* bionicStrrchrChk(const char* string, int character, size_t size);
int bionicRegisterAtFork(void (*prepare)(), void (*parent)(), void (*child)(),
                         void* dsoHandle);
char* bionicBasename(const char* path);
int bionicFchown(int fd, uid_t owner, gid_t group);
unsigned long bionicGetauxval(unsigned long type);
uid_t bionicGeteuid();
int bionicGetpwuidR(uid_t uid, void* password, char* buffer, size_t bufferSize,
                    void** result);
unsigned int bionicIfNametoindex(const char* name);
off_t bionicLseek64(int fd, off_t offset, int origin);
int bionicMadvise(void* address, size_t length, int advice);
int bionicMlock(const void* address, size_t length);
void* bionicMmap(void* address, size_t length, int protection, int flags,
                 int fd, off_t offset);
int bionicMprotect(void* address, size_t length, int protection);
void* bionicMremap(void* oldAddress, size_t oldSize, size_t newSize,
                   int flags, ...);
int bionicMunmap(void* address, size_t length);
int bionicPipe(int descriptors[2]);
long bionicPtrace(int request, ...);
int bionicSigaction(int signalNumber, const void* action, void* oldAction);
int bionicStatfs(const char* path, void* output);
uint64_t bionicMiscSelfTest();

} // namespace rr3
