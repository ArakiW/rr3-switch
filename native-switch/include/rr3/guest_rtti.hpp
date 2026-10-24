//
// GUEST RTTI: turn a vptr into the guest's own class name.
//
// WHAT THIS IS FOR
// ----------------
// The port has to find live C++ objects inside the guest's memory without
// guessing an address.  The guest is an Android arm64 binary built with the
// Itanium C++ ABI, so it keeps its own RTTI, and the RTTI is enough:
//
//        [vptr - 8]      -> the typeinfo object   (a relocated pointer)
//        [typeinfo + 8]  -> the __name C string   (a relocated pointer)
//
// The __name is the MANGLED class name, e.g. "8GameMode",
// "19RuleSet_RaceMetrics", "N9FrontEnd214EventMapScreenE".
//
// WHY A RUNTIME LOOKUP AND NOT A HARDCODED ADDRESS
// ------------------------------------------------
// The guest's "8GameMode" class has ONE vtable, but it has TWELVE derived
// classes (SoloMode, DragRaceMode, P2PMultiplayerMode, ...), and each
// derived class has its OWN typeinfo and its OWN vtable.  So there is no
// one vptr value that identifies "a game mode object": the only way to
// name the class of an arbitrary live object is to read the name out of
// the guest's own RTTI at runtime.
//
// EVIDENCE (re-derived from reverse/native/arm64/libRealRacing3.so)
// -----------------------------------------------------------------
//   * the GameMode typeinfo object is at ELF 0x1e983c8,
//   * its __name slot is at 0x1e983d0 and points at 0x4f0748 = "8GameMode",
//   * the GameMode vtable's typeinfo slot is at 0x1e98160, so that vtable's
//     ADDRESS POINT -- the value a live object's first word holds -- is
//     0x1e98168, and [0x1e98168] is its first virtual function
//     (relocation addend 0xf16e80, inside .text).
//   The same chain gives "19RuleSet_RaceMetrics" for vptr 0x1e4b378 and
//   "27RuleSetModifier_RaceMetrics" for vptr 0x1e4b3f8, which are the two
//   other vptr values native-switch/source/race_state_probe.cpp hard-codes.
//   So those constants ARE address points and the chain above IS the
//   convention this module implements.
//
// WHAT THIS MODULE DOES NOT DO
// ----------------------------
// It never writes to guest memory, never allocates, never locks and never
// makes a syscall: every read is of an address inside the guest image and
// is preceded by a guestImageContains() bounds check.  That is what makes
// it safe to call on the guest's own thread (a lock on that thread has
// deadlocked this project before).
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace rr3 {

// The guest image's RW segment, in ELF virtual addresses.  .data.rel.ro is
// the first thing in it, so EVERY vtable the guest has lives in
// [0x1e1aae0, 0x1f7f880).  A word that points there is a CANDIDATE vptr.
//
// THE BOUNDS COME FROM THE GUEST'S PT_LOAD TABLE, re-read from
// reverse/native/arm64/libRealRacing3.so (program header table at file
// offset 0x40, 9 entries, 3 of them PT_LOAD):
//
//   segment 0: 0x00000000 .. 0x1e19ae0  r-x   (.text .plt .rodata ...)
//   segment 1: 0x1e1aae0 .. 0x1f7f880  rw-   (.data.rel.ro .dynamic .got)
//   segment 2: 0x1f80880 .. 0x1fc7a99  rw-   (.data .bss)
//
// There are 0x1000-byte holes between the segments.  The loader maps one
// contiguous block for the whole image, so the holes are mapped and zero,
// but the segments are scanned SEPARATELY anyway: walking the holes would
// read zeroes for nothing.
//
// NOTE THE UPPER BOUND.  It is the end of the whole RW PT_LOAD segment, NOT
// the end of .data.rel.ro (0x1f6cd80), because real vtable ADDRESS POINTS
// live above .data.rel.ro's end as well: 34 address points that satisfy
// "[A-8] is a typeinfo pointer and [A] is a .text address" sit ABOVE the
// older 0x1f6d280 bound that native-switch/source/race_state_probe.cpp
// uses, and a range that is too small silently rejects real vtables.
//
// The 34 are in the .got tail of the same RW segment (the linker put them
// there), and they are real classes, e.g. 0x1f744d0 = "22RuleSetModifier_Replay"
// (first virtual function 0x1024ac4) and 0x1f70a38 = "19mtMaterialManagerGL".
// The highest is 0x1f7af20.
//
// (For the record, because the port's notes and the port's own test oracle
// describe that high entry differently: 0x1f6d6a8 is a slot that holds the
// GameMode typeinfo, but it is NOT a vtable's typeinfo slot -- the word
// after it, 0x1f6d6b0, holds another typeinfo (0x1ea9db0 =
// "13TimeTrialMode") where a vtable holds its first virtual function, and
// the word before it, 0x1f6d6a0, holds a .text address (0x10a2098).  An
// enumeration that only asks "does this slot hold a typeinfo pointer"
// therefore calls 0x1f6d6b0 an "address point", and that is where the
// "GameMode entry past the old bound" in
// native-switch/tests/test_guest_rtti.py comes from.  The GameMode
// vtable's own address point is 0x1e98168, and 0x1f6d6a0 is not an address
// point at all -- the [vptr-8] chain fails there, because [0x1f6d698]
// holds 0x1f95118, which is not a typeinfo object.)
constexpr uintptr_t kGuestVptrRegionLo = 0x1e1aae0ULL;
constexpr uintptr_t kGuestVptrRegionHi = 0x1f7f880ULL;

// The two writable segments of the guest image, in ELF virtual addresses.
// These are where live objects built IN PLACE by the guest's own
// constructors live (the guest's CGlobal is one of them).  Scan them to
// census the guest's global objects.
constexpr uintptr_t kGuestDataSegmentLo = 0x1e1aae0ULL;
constexpr uintptr_t kGuestDataSegmentHi = 0x1f7f880ULL;
constexpr uintptr_t kGuestBssSegmentLo  = 0x1f80880ULL;
constexpr uintptr_t kGuestBssSegmentHi  = 0x1fc7a99ULL;

// True when `word` points into the guest's vtable region.  `bias` is the
// main image's load bias.
//
// The fold is unsigned arithmetic only: `word` is a runtime address and
// `bias` is the image's load bias, so the ELF virtual address is
// word - bias.  A word below the bias cannot be an image address at all,
// and the re-folded runtime address is checked for wraparound, because a
// wrapped address would fold back into the range and look valid.
bool guestLooksLikeVptr(uintptr_t bias, uint64_t word);

// The guest's own mangled class name for the object whose vptr is
// `vptr`, read from the guest's RTTI at runtime:
//     [vptr-8] -> typeinfo, [typeinfo+8] -> __name
// Writes a NUL-terminated ASCII name into `out` and returns true.
// Returns false (and writes an empty string) when the name cannot be
// resolved or is not a plausible mangled type name.
bool guestResolveClassName(uintptr_t vptr, char* out, size_t outSize);

// True when `name` contains `needle` (a plain substring test, ASCII).
bool guestClassNameContains(const char* name, const char* needle);

// One live object the census found, with the class name the guest's own
// RTTI gave for it.
struct GuestClassObject {
    uintptr_t address{};   // where the object starts (the vptr's slot)
    uintptr_t vptr{};      // the vptr itself
    char name[96]{};        // the guest's mangled class name
};

// How many objects of each interesting class the census found.
struct GuestClassCensus {
    uint32_t scannedWords{};
    uint32_t vptrCandidates{};
    uint32_t namedObjects{};
    uint32_t gameModeObjects{};
    uint32_t ruleSetObjects{};
    uint32_t metricsObjects{};
    uint32_t otherObjects{};
    // Set when the candidate cap was reached, i.e. when the scan stopped
    // RESOLVING names because it had already resolved too many candidates.
    // The counts above are then a lower bound, not a total.
    uint32_t capped{};
};

// Walks the guest's writable segments, finds every word that could be a
// vptr, resolves the class name of each, and reports the objects whose
// class name contains "GameMode", "RuleSet" or "RaceMetrics".
//
// It is safe to call on the guest's own thread: every read is of an
// address inside the guest image, so there is no syscall, no lock and no
// allocation.
//
// `out` receives up to `max` objects; the census counters say how many
// were found in total. Returns the number written to `out`.
//
// The objects in `out` are the interesting ones first (ascending address
// order inside each group), then the "other" ones (also ascending).  A
// caller with a small `max` therefore still gets the objects it came for.
size_t guestCensusGlobalObjects(uintptr_t bias,
                                GuestClassObject* out, size_t max,
                                GuestClassCensus* census);

} // namespace rr3
