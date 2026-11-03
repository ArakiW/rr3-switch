// The guest's string-to-number conversions, with a BOUND on how far the host
// is allowed to look.
//
// WHY THIS EXISTS
// ---------------
// The port binds the guest's libc imports to the HOST's implementations
// (`RR3_BIND(strtoul)` and friends in source/symbol_resolver.cpp).  The
// host's `strtoul` scans forward until it meets a byte that is not a digit,
// and on the guest's thread that scan has NO bound: if the guest hands it a
// pointer to a string that is not terminated, the scan walks the guest's
// address space, the port burns a CPU, and NOTHING is logged.  The run looks
// exactly like a freeze.
//
// MEASURED 2026-11-01 (round q89, archive 20261101-160153-eden-rr3-q89-
// ui2048): the guest's root thread was sampled inside the HOST image at
// pc_off=0x5c6154, and that offset is the link-time vaddr of
// `_strtoul_l.isra.0` -- newlib's strtoul -- running on the guest's thread.
// The run then hung for 120 s with the log frozen at 1.6 MB, no frame
// captured, and the guest's own last log line being the cloudcell sync.
//
// The offset is a LINK-TIME vaddr because `censusNroBase()` is `&_start`
// (pthread_bridge.cpp), and `_start` is at vaddr 0 in the link.
//
// WHAT THIS DOES
// --------------
// The string is COPIED out of the guest's memory under a readability check
// and with a hard length limit, and the host's parser only ever sees that
// copy.  A string that is not terminated inside the window is REFUSED, with
// a log line that says so and prints the bytes that were read -- so the
// failure becomes a named event instead of a hang.
//
// The refusal answers 0, which is what a failed conversion answers, and the
// end pointer is left pointing at the guest's own string, which is what the
// C standard asks for when no conversion was performed.
// ---------------------------------------------------------------------------
#pragma once

long guestStrtol(const char* text, char** end, int base);
long long guestStrtoll(const char* text, char** end, int base);
unsigned long guestStrtoul(const char* text, char** end, int base);
unsigned long long guestStrtoull(const char* text, char** end, int base);
double guestStrtod(const char* text, char** end);
float guestStrtof(const char* text, char** end);
int guestAtoi(const char* text);
long guestAtol(const char* text);
double guestAtof(const char* text);

// The guest's sscanf, with a bound on how far the host may look.
//
// MEASURED 2026-11-03 (round q123, archive 20261103-011044-eden-rr3-q123-
// icon-font): the guest's thread was sampled twice inside the HOST image, at
// pc_off=0x5ea1c0 and 0x5e8e1c, and both offsets are inside
// `__svfscanf_r` -- newlib's sscanf -- running on the GUEST's thread.  The
// run froze with the log at 1.62 MB and no frame captured.
//
// The guest's sscanf import was bound straight to the host's newlib
// (`RR3_BIND(sscanf)`), so the host's parser walked the GUEST's memory with
// no bound at all: the same shape as the q89 strtoul hang, one function over.
//
// ⇒ The input AND the format are copied out under a readability check with a
// hard limit, and the host's parser only ever sees the copies.  A string
// that is not terminated inside the window still parses to the SAME value
// (the copy has the same leading bytes), so the bound does not change the
// answer -- it only stops the scan.
int guestSscanf(const char* text, const char* format, ...);
