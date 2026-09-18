#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace rr3 {

using GuestThread = uint64_t;
using GuestKey = int;
using GuestStartRoutine = void* (*)(void*);

int guestPthreadAttrInit(void* attribute);
int guestPthreadAttrDestroy(void* attribute);
int guestPthreadAttrSetDetachState(void* attribute, int state);
int guestPthreadAttrSetStackSize(void* attribute, size_t stackSize);
int guestPthreadCreate(GuestThread* thread, const void* attribute,
                       GuestStartRoutine start, void* argument);
int guestPthreadJoin(GuestThread thread, void** value);
int guestPthreadDetach(GuestThread thread);
int guestPthreadEqual(GuestThread left, GuestThread right);
GuestThread guestPthreadSelf();
[[noreturn]] void guestPthreadExit(void* value);

int guestPthreadMutexAttrInit(void* attribute);
int guestPthreadMutexAttrDestroy(void* attribute);
int guestPthreadMutexAttrSetType(void* attribute, int type);
int guestPthreadMutexInit(void* mutex, const void* attribute);
int guestPthreadMutexDestroy(void* mutex);
int guestPthreadMutexLock(void* mutex);
int guestPthreadMutexTryLock(void* mutex);
int guestPthreadMutexUnlock(void* mutex);

int guestPthreadCondInit(void* condition, const void* attribute);
int guestPthreadCondDestroy(void* condition);
int guestPthreadCondWait(void* condition, void* mutex);
int guestPthreadCondTimedWait(void* condition, void* mutex,
                              const struct timespec* absoluteTime);
int guestPthreadCondSignal(void* condition);
int guestPthreadCondBroadcast(void* condition);

int guestPthreadRwlockInit(void* lock, const void* attribute);
int guestPthreadRwlockDestroy(void* lock);
int guestPthreadRwlockReadLock(void* lock);
int guestPthreadRwlockWriteLock(void* lock);
int guestPthreadRwlockUnlock(void* lock);

int guestPthreadOnce(void* onceControl, void (*initializer)());
int guestPthreadKeyCreate(GuestKey* key, void (*destructor)(void*));
int guestPthreadKeyDelete(GuestKey key);
void* guestPthreadGetSpecific(GuestKey key);
int guestPthreadSetSpecific(GuestKey key, const void* value);

uint64_t pthreadBridgeSelfTest();

// Stops the detached reaper and returns the number of host threads joined.
// Guest workers created by the game (FMOD / View Changed) often run until
// process death; joining them would block handoff forever (device rounds 7-8).
// This call therefore NEVER joins guest workers. Use guestPthreadLiveCount()
// after it to decide whether image unload is safe.
//
// Must run BEFORE guest image unload/finalizers when live count is zero.
// A non-zero live count means unload would race workers still executing
// guest code.
unsigned int guestPthreadQuiesceAll();

// Number of guest-thread registry slots still in use (running joinable,
// detached-pending-reap, or the reaper). Zero means no recorded guest
// pthread remains.
unsigned int guestPthreadLiveCount();

} // namespace rr3
