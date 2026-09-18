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
// Appends one line and fsyncs. Used for guest-view diagnostics that must
// survive the final handoff breadcrumb overwrite of the progress file.
bool appendGuestBootLine(const char* path, const char* phase,
                         unsigned long long ordinal,
                         unsigned long long target,
                         const char* details = nullptr);
// Reads the breadcrumb back (up to capacity-1 bytes, NUL-terminated). Returns
// false and leaves an empty string if the file is absent.
bool readGuestBootProgress(const char* path, char* out, unsigned long capacity);
void clearGuestBootProgress(const char* path);

} // namespace rr3
