#pragma once

#include <cstdint>
#include <time.h>

namespace rr3 {

int guestSemInit(void* guestSemaphore, int processShared, unsigned int value);
int guestSemDestroy(void* guestSemaphore);
int guestSemPost(void* guestSemaphore);
int guestSemWait(void* guestSemaphore);
int guestSemTryWait(void* guestSemaphore);
int guestSemTimedWait(void* guestSemaphore, const struct timespec* absoluteTimeout);
int guestSemGetValue(void* guestSemaphore, int* value);
void* guestSemOpen(const char* name, int flags, ...);
int guestSemClose(void* guestSemaphore);
int guestSemUnlink(const char* name);
uint64_t semaphoreBridgeSelfTest();

} // namespace rr3
