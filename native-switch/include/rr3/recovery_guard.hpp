#pragma once

namespace rr3 {

bool prepareGuestCrashCapture();
void finishGuestCrashCapture();
[[noreturn]] void guestTracedAbort();
[[noreturn]] void guestTracedExit(int status);
int guestTracedRaise(int signal);
void recordGuestTermination(const char* kind, unsigned long long value,
                            unsigned long long caller);

bool recoveryMarkerExists(const char* path);
bool beginGuardedAction(const char* path, const char* action);
bool beginGuardedActionSnapshot(const char* path, const char* action,
                                const char* snapshot);
bool completeGuardedAction(const char* path);

// Durable one-line breadcrumb for the stage-9 guest boot. Rewritten (truncate)
// and fsync'd before each risky guest step so that, if the next step faults
// with no saved crash report, the last-reached phase/ordinal survives the
// power-off and can be read back on the next launch. Safe to call while a
// GuestTlsScope is installed: it only touches TPIDR_EL0, while libnx/newlib
// reach their thread vars via TPIDRRO_EL0.
bool writeGuestBootProgress(const char* path, const char* phase,
                            unsigned long long ordinal,
                            unsigned long long target,
                            const char* details = nullptr);
// The same single line in the same file, written WITHOUT the whole-card fsync.
// For progress writes that can repeat on a guest-blocking timer (the
// blocked-wait breadcrumb). See recovery_guard.cpp: fsync on an emulated SDMC
// card is a whole-card sync and the census hot log was already moved off it for
// exactly this reason.
bool writeGuestBootProgressNoSync(const char* path, const char* phase,
                                 unsigned long long ordinal,
                                 unsigned long long target,
                                 const char* details = nullptr);
// Appends one line and deliberately does NOT fsync. Used for guest-view
// diagnostics that must survive the final handoff breadcrumb overwrite of the
// progress file. See recovery_guard.cpp for the measured reason. The "a" handle
// is cached per path (one open, many appends); the format is unchanged and is
// still exactly one flushed line per call.
bool appendGuestBootLine(const char* path, const char* phase,
                         unsigned long long ordinal,
                         unsigned long long target,
                         const char* details = nullptr);
// Snapshot of the append-handle cache counters: how many times a handle was
// freshly opened, reused from the cache, and reopened after an error. Read this
// at an existing periodic point (the census/sweep in pthread_bridge.cpp) rather
// than from a new timer; appendGuestBootLine must not report on itself.
void guestBootLogCacheStats(unsigned long long& opens, unsigned long long& reuses,
                            unsigned long long& reopens, unsigned int& slots);
// Reads the breadcrumb back (up to capacity-1 bytes, NUL-terminated). Returns
// false and leaves an empty string if the file is absent.
bool readGuestBootProgress(const char* path, char* out, unsigned long capacity);
void clearGuestBootProgress(const char* path);

} // namespace rr3
