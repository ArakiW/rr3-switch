#pragma once

// ---------------------------------------------------------------------------
// A/B SWITCH -- added 2026-10-04, DEFAULT IS THE UNFIXED (newlib) NUMBERING.
//
// Why this exists: the bionic numbering below is correct as an ABI fact --
// newlib and bionic genuinely disagree from 35 onward -- but correctness of
// the number is not the same as usefulness to this guest, and the two have
// never been tested against each other on a run where the guest was healthy.
// There is exactly one archived run where the guest behaved
// (20261002-181947-zpallones, total_imports climbing 640 -> 5124 over 116 s),
// and it predates the fix. Every run since has been in the broken state, which
// means "the run did not regress" was measured against other broken runs and
// proved nothing.
//
// Setting RR3_ERRSYS_BIONIC to 0 restores the newlib numbers at every one of
// the 41 assignment sites without touching those sites. The shim layer is the
// only thing that writes guest-visible errno, so this is a complete A/B.
// Flip it, rebuild, compare fault volume and whether total_imports climbs.
// ---------------------------------------------------------------------------
#ifndef RR3_ERRSYS_BIONIC
#define RR3_ERRSYS_BIONIC 1
#endif

#if RR3_ERRSYS_BIONIC
#define RR3_ERRNO(bionic_value, newlib_value) (bionic_value)
#else
#define RR3_ERRNO(bionic_value, newlib_value) (newlib_value)
#endif

// ---------------------------------------------------------------------------
// errno values the GUEST will read, not the ones newlib defines.
//
// Every shim in this tree runs on newlib/libnx, whose <sys/errno.h> is a
// different numbering from the Android bionic ABI the guest was compiled
// against. The two agree exactly for 1..34 (EPERM..ERANGE) and diverge from
// there, because newlib carries its own EL2NSYNC/EL3HLT/EL3RST block that
// Linux does not have, and everything after it is shifted.
//
// Measured against
//   .tools/devkitPro/.../aarch64-none-elf/include/sys/errno.h
// on 2026-10-04:
//
//   errno            newlib   bionic   delta
//   ENAMETOOLONG         91       36      +55
//   ENOSYS               88       38      +50
//   ENOTSUP             134       95      +39
//   ETIMEDOUT           116      110       +6
//   ECONNREFUSED        111      111        0
//   EAGAIN               11       11        0
//   EINVAL               22       22        0
//
// The consequence is not cosmetic: a guest that asks for ENOSYS and gets a
// non-zero return reads errno == 88, which in bionic is a different error
// entirely. Anything that branches on errno -- retry loops, fallback paths,
// "is this fatal" checks -- silently takes the wrong arm. That is the whole
// class of bug the "ERRNO domain" work item was chasing, and it cannot be
// fixed by editing the value at the assignment site, because the macro itself
// is the wrong number. The shims must write these instead.
//
// Rule for this file: every name here is the BIONIC number. It must never be
// spelled as the newlib macro at a guest-visible assignment. Low-numbered
// errno may keep using the plain macro, but prefer the kGuest* form so the
// intent is explicit and a future audit is mechanical.
// ---------------------------------------------------------------------------

// Identical in both ABIs (1..34). Listed so the full set the tree uses is in
// one place and so an audit does not have to assume agreement.
constexpr int kGuestErrnoEPERM          = 1;
constexpr int kGuestErrnoENOENT         = 2;
constexpr int kGuestErrnoEINTR          = 4;
constexpr int kGuestErrnoEIO            = 5;
constexpr int kGuestErrnoEBADF          = 9;
constexpr int kGuestErrnoEAGAIN         = 11;
constexpr int kGuestErrnoENOMEM         = 12;
constexpr int kGuestErrnoEACCES         = 13;
constexpr int kGuestErrnoEFAULT         = 14;
constexpr int kGuestErrnoEBUSY          = 16;
constexpr int kGuestErrnoEEXIST         = 17;
constexpr int kGuestErrnoEXDEV          = 18;
constexpr int kGuestErrnoENODEV         = 19;
constexpr int kGuestErrnoENOTDIR        = 20;
constexpr int kGuestErrnoEISDIR         = 21;
constexpr int kGuestErrnoEINVAL         = 22;
constexpr int kGuestErrnoENFILE         = 23;
constexpr int kGuestErrnoEMFILE         = 24;
constexpr int kGuestErrnoEFBIG          = 27;
constexpr int kGuestErrnoENOSPC         = 28;
constexpr int kGuestErrnoESPIPE         = 29;
constexpr int kGuestErrnoEROFS          = 30;
constexpr int kGuestErrnoEPIPE          = 32;
constexpr int kGuestErrnoEDOM           = 33;
constexpr int kGuestErrnoERANGE         = 34;

// Divergent. These are the ones that are actually wrong today.
constexpr int kGuestErrnoENAMETOOLONG   = RR3_ERRNO(36, 91);   // newlib says 91
constexpr int kGuestErrnoENOSYS         = RR3_ERRNO(38, 88);   // newlib says 88
constexpr int kGuestErrnoENOTEMPTY      = 39;
constexpr int kGuestErrnoELOOP          = 40;
constexpr int kGuestErrnoENOSR          = 63;
constexpr int kGuestErrnoEPROTO         = 71;
constexpr int kGuestErrnoEBADMSG        = 74;
constexpr int kGuestErrnoEILSEQ         = 84;
constexpr int kGuestErrnoENOTSUP        = RR3_ERRNO(95, 134);   // newlib says 134
constexpr int kGuestErrnoEOPNOTSUPP     = 95;
constexpr int kGuestErrnoEAFNOSUPPORT   = 97;
constexpr int kGuestErrnoEADDRINUSE     = 98;
constexpr int kGuestErrnoEADDRNOTAVAIL  = 99;
constexpr int kGuestErrnoENETDOWN       = 100;
constexpr int kGuestErrnoENETUNREACH    = 101;
constexpr int kGuestErrnoECONNABORTED   = 103;
constexpr int kGuestErrnoECONNRESET     = 104;
constexpr int kGuestErrnoENOBUFS        = 105;
constexpr int kGuestErrnoEISCONN        = 106;
constexpr int kGuestErrnoENOTCONN       = 107;
constexpr int kGuestErrnoESHUTDOWN      = 108;
constexpr int kGuestErrnoETIMEDOUT      = RR3_ERRNO(110, 116);  // newlib says 116
constexpr int kGuestErrnoECONNREFUSED   = 111;  // newlib says 111 -- agrees
constexpr int kGuestErrnoEHOSTDOWN      = 112;
constexpr int kGuestErrnoEHOSTUNREACH   = 113;
constexpr int kGuestErrnoEALREADY       = 114;
constexpr int kGuestErrnoEINPROGRESS    = 115;
constexpr int kGuestErrnoECANCELED      = 125;
