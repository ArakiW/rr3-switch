//
// GUEST RTTI: turn a vptr into the guest's own class name.
//
// THE CONVENTION THIS FILE IMPLEMENTS
// -----------------------------------
// The guest is an Android arm64 binary built with the Itanium C++ ABI, so
// it keeps its own RTTI.  A C++ object with virtual functions starts with
// a vptr: the ADDRESS POINT of its class's vtable.  The ABI lays a vtable
// group out like this, with offsets relative to the address point A:
//
//        A-16   offset-to-top    (0 for the primary vtable)
//        A-8    typeinfo pointer
//        A      first virtual function
//        A+8    second virtual function
//        ...
//
// so the class of an arbitrary live object can be recovered WITHOUT
// GUESSING ANY ADDRESS:
//
//        [vptr - 8]      -> the typeinfo object
//        [typeinfo + 8]  -> the __name C string
//
// The __name is the MANGLED class name: "8GameMode",
// "19RuleSet_RaceMetrics", "N9FrontEnd214EventMapScreenE".
//
// WHY THE RUNTIME LOOKUP IS NECESSARY
// ------------------------------------
// The guest's "8GameMode" class has ONE vtable, but it has TWELVE derived
// classes (SoloMode, DragRaceMode, P2PMultiplayerMode, StandardRaceMode_Base,
// ...), and each derived class has its OWN typeinfo and its OWN vtable.
// A single hard-coded vptr value therefore cannot name "a game mode
// object"; the name has to be read out of the guest's RTTI at runtime.
//
// THIRTEEN slots in the image hold the GameMode typeinfo, but only ONE of
// them (0x1e98160) is a vtable's typeinfo slot.  The other twelve are the
// base_type slots of the derived classes' own typeinfo objects (their
// names are readable: "16TrackPreviewMode", "8SoloMode",
// "26AiPerformanceMeasuringMode", "18CarDebugViewerMode",
// "12DragRaceMode", "15EliminationMode", "18P2PMultiplayerMode",
// "11SandboxMode", "21StandardRaceMode_Base",
// "29TrackPerformanceProfilingMode", "12TutorialMode", and one data slot
// in the .got tail).  So "the GameMode typeinfo has thirteen vtables
// pointing at it" would be wrong -- thirteen POINTERS point at it, one
// vtable uses it -- and the runtime lookup is what tells those apart.
//
// EVIDENCE (re-derived from reverse/native/arm64/libRealRacing3.so)
// ------------------------------------------------------------------
//   * the GameMode typeinfo object is at ELF 0x1e983c8,
//   * its __name slot is at 0x1e983d0 and points at 0x4f0748 = "8GameMode",
//   * the GameMode vtable's typeinfo slot is at 0x1e98160, so that vtable's
//     ADDRESS POINT -- the value a live object's first word holds -- is
//     0x1e98168, and [0x1e98168] is its first virtual function
//     (relocation addend 0xf16e80, inside .text).
//   The same chain gives "19RuleSet_RaceMetrics" for vptr 0x1e4b378 and
//   "27RuleSetModifier_RaceMetrics" for vptr 0x1e4b3f8, which are the two
//   other vptr values native-switch/source/race_state_probe.cpp hard-codes,
//   so those constants ARE address points and the chain above IS the
//   convention.
//
// WHAT THE FILE MAY NOT DO
// ------------------------
// No write to guest memory, no allocation, no lock, no syscall, no file
// I/O.  The probe runs on the guest's own thread, and a lock there has
// deadlocked this project before.  Every read in here goes through
// readGuestWord() or through a guestImageContains()-proven window, so the
// module cannot fault on a stale pointer.
//
#include "rr3/guest_rtti.hpp"

#include "rr3/elf_preflight.hpp"

namespace rr3 {
namespace {

// The capacity of GuestClassObject::name.  A mangled class name is short
// (the longest one in the guest is well under 32 characters), but the
// buffer is the contract, so the resolver never writes more than this.
constexpr size_t kGuestClassNameCapacity = 96;

// The hard cap on how many vptr candidates the census resolves.
//
// WHY A CAP AT ALL.  Resolving a name costs a few reads (the typeinfo
// pointer, the __name pointer and then the string itself), and the scan
// walks ~220k words of the guest's writable segments.  The number of words
// that merely LOOK like a vptr is not bounded by anything the port
// controls -- a word in the guest's data that points into .data.rel.ro is
// indistinguishable from a vptr by range alone -- so an unbounded resolve
// count would turn a diagnostic into a stall on the guest's thread.  The
// census stops RESOLVING past this many candidates and says so through the
// census' `capped` counter, instead of silently truncating.
constexpr uint32_t kCensusCandidateCap = 200000;

// The two writable PT_LOAD segments, in ELF virtual addresses.
//
// PROVENANCE: the guest's program header table (file offset 0x40, 9
// entries) says
//   segment 0: 0x00000000 .. 0x1e19ae0  r-x
//   segment 1: 0x1e1aae0 .. 0x1f7f880  rw-
//   segment 2: 0x1f80880 .. 0x1fc7a99  rw-
// There are 0x1000-byte holes between the segments.  The loader maps one
// contiguous block for the whole image so the holes are mapped and zero,
// but the segments are walked SEPARATELY anyway: walking the holes would
// read zeroes for nothing, and a zero is not a vptr.
struct SegmentSpan {
    uintptr_t lo;
    uintptr_t hi;
};

constexpr SegmentSpan kWritableSegments[2] = {
    {kGuestDataSegmentLo, kGuestDataSegmentHi},
    {kGuestBssSegmentLo, kGuestBssSegmentHi},
};

// The one and only place where this module reads guest memory.
//
// EVERY read is preceded by guestImageContains(), which folds the runtime
// address back to a guest virtual address and tests it against the image's
// PT_LOAD table.  That is what makes a stale or random pointer harmless:
// the read is refused instead of faulting the process.
bool readGuestWord(uintptr_t address, uint64_t* out) {
    if (out == nullptr || address == 0) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint64_t))) {
        return false;
    }
    // volatile: the compiler must not cache or elide a read of memory that
    // another thread (the guest) may change.
    *out = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
}

// Is this a plausible Itanium MANGLED type name?
//
// THE FIRST-CHARACTER RULE IS THE INTERESTING ONE, and the obvious rule
// was tried first and REJECTED THE GUEST'S OWN MOST IMPORTANT CLASS:
// "the first character is an upper-case letter" is wrong, because the
// Itanium mangling of a plain class is <length><identifier>, so the first
// character is the LENGTH -- a digit.  The guest's names are
//
//        "8GameMode"                      (ELF 0x4f0748)
//        "19RuleSet_RaceMetrics"         (ELF 0x4b9160)
//        "27RuleSetModifier_RaceMetrics"  (ELF 0x4b910c)
//        "N9FrontEnd214EventMapScreenE"   (nested class: 'N' then the
//                                           enclosing class, then the
//                                           nested class, then 'E')
//
// A rule that rejected "8GameMode" would reject the very class this module
// exists to find, so the rule is:
//
//   * length >= 2,
//   * every character in [A-Za-z0-9_],
//   * the first character is an upper-case letter OR a digit,
//   * at least one letter somewhere (a mangled name is never all digits,
//     and a random pointer into a table of decimal numbers must not be
//     accepted as a class name).
bool plausibleMangledName(const char* name, size_t length) {
    if (name == nullptr || length < 2 || length > kGuestClassNameCapacity - 1) {
        return false;
    }
    bool sawLetter = false;
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        const bool isAlpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool isDigit = (c >= '0' && c <= '9');
        if (!isAlpha && !isDigit && c != '_') {
            return false;
        }
        if (isAlpha) {
            sawLetter = true;
        }
    }
    if (!sawLetter) {
        return false;
    }
    const char first = name[0];
    const bool firstIsUpper = (first >= 'A' && first <= 'Z');
    const bool firstIsDigit = (first >= '0' && first <= '9');
    return firstIsUpper || firstIsDigit;
}

// The three classes the port cares about.  A name can match more than one
// of them ("19RuleSet_RaceMetrics" contains both "RuleSet" and
// "RaceMetrics"), and such a name is counted in EVERY bucket it matches.
bool isGameModeName(const char* name) {
    return guestClassNameContains(name, "GameMode");
}

bool isRuleSetName(const char* name) {
    return guestClassNameContains(name, "RuleSet");
}

bool isMetricsName(const char* name) {
    return guestClassNameContains(name, "RaceMetrics");
}

// The state of one pass over the writable segments.
struct CensusPass {
    uint32_t scannedWords = 0;
    uint32_t candidates = 0;
    uint32_t named = 0;
    uint32_t gameMode = 0;
    uint32_t ruleSet = 0;
    uint32_t metrics = 0;
    uint32_t others = 0;
    bool capped = false;
};

// One pass over the two writable segments.
//
// `collectOthers` says what the pass does with a resolved object:
//   false -> count it, and write it into `out` when it is an INTERESTING
//            class (GameMode / RuleSet / RaceMetrics);
//   true  -> write it into `out` when it is NOT interesting.
//
// WHY TWO PASSES.  The caller wants the interesting objects FIRST in `out`,
// and how many interesting objects there are is not known until the scan is
// over.  Staging them in a side buffer would need an allocation, and this
// module must not allocate on the guest's thread, so the scan is simply run
// twice: once to count and to collect the interesting objects, once to
// collect the rest.  Both passes walk the same memory in the same order, so
// the result is stable.
//
// `written` is the running index into `out`; it is shared between the two
// passes so the interesting objects keep the front of the array.
void runCensusPass(uintptr_t bias, bool collectOthers, GuestClassObject* out,
                   size_t max, CensusPass* pass, size_t* written) {
    for (const SegmentSpan& segment : kWritableSegments) {
        for (uintptr_t at = segment.lo; at + sizeof(uint64_t) <= segment.hi;
             at += sizeof(uint64_t)) {
            // The runtime address of this word.  The addition cannot wrap
            // for a loaded image (the bias is a mapped address and the
            // segment offsets are below 32 MiB), and if it somehow did,
            // readGuestWord() would refuse it because the wrapped address
            // is not inside the image.
            const uintptr_t runtime = bias + at;
            uint64_t word = 0;
            if (!readGuestWord(runtime, &word)) {
                continue;
            }
            pass->scannedWords++;

            if (!guestLooksLikeVptr(bias, word)) {
                continue;
            }
            pass->candidates++;

            if (pass->candidates > kCensusCandidateCap) {
                // The cap.  The scan keeps COUNTING candidates but stops
                // RESOLVING them, and the census reports the cap instead of
                // silently returning a partial answer.
                pass->capped = true;
                continue;
            }

            char name[kGuestClassNameCapacity];
            if (!guestResolveClassName(static_cast<uintptr_t>(word), name,
                                       sizeof(name))) {
                continue;
            }
            pass->named++;

            const bool gameMode = isGameModeName(name);
            const bool ruleSet = isRuleSetName(name);
            const bool metrics = isMetricsName(name);
            if (gameMode) {
                pass->gameMode++;
            }
            if (ruleSet) {
                pass->ruleSet++;
            }
            if (metrics) {
                pass->metrics++;
            }
            const bool interesting = gameMode || ruleSet || metrics;
            if (!interesting) {
                pass->others++;
            }

            const bool keep = collectOthers ? !interesting : interesting;
            if (!keep) {
                continue;
            }
            if (out == nullptr || *written >= max) {
                continue;
            }

            GuestClassObject& object = out[*written];
            object.address = runtime;
            object.vptr = static_cast<uintptr_t>(word);
            size_t i = 0;
            while (i + 1 < sizeof(object.name) && name[i] != '\0') {
                object.name[i] = name[i];
                ++i;
            }
            object.name[i] = '\0';
            (*written)++;
        }
    }
}

} // namespace

// True when `word` points into the guest's vtable region.
//
// WHY THE FOLD IS UNSIGNED ARITHMETIC ONLY.  `word` is a runtime address
// and `bias` is the image's load bias, so the ELF virtual address is
// word - bias.  A word below the bias cannot be an image address at all.
// The subtraction cannot underflow because the comparison above already
// rejected the smaller values, and the re-folded runtime address is
// checked for wraparound, because a wrapped address would fold back into
// the range and look valid when it is not.
bool guestLooksLikeVptr(uintptr_t bias, uint64_t word) {
    if (word == 0 || bias == 0) {
        return false;
    }
    const uint64_t imageBias = static_cast<uint64_t>(bias);
    if (word < imageBias) {
        return false;
    }
    const uint64_t folded = word - imageBias;
    if (folded < kGuestVptrRegionLo || folded >= kGuestVptrRegionHi) {
        return false;
    }
    const uint64_t runtime = imageBias + folded;
    if (runtime < imageBias) {
        // The fold wrapped.  Reject rather than trust a wrapped address.
        return false;
    }
    return true;
}

// The guest's own mangled class name for the object whose vptr is `vptr.
//
//     [vptr-8] -> typeinfo, [typeinfo+8] -> __name
bool guestResolveClassName(uintptr_t vptr, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }
    // Always leave an empty string behind on failure, so a caller that
    // ignores the return value cannot read a stale name.
    out[0] = '\0';

    if (vptr < sizeof(uint64_t)) {
        // vptr - 8 would underflow.
        return false;
    }

    // The typeinfo pointer sits in the slot 8 bytes below the address
    // point.  Both this read and the next one are bounds-checked.
    uint64_t typeinfo = 0;
    if (!readGuestWord(vptr - sizeof(uint64_t), &typeinfo) || typeinfo == 0) {
        return false;
    }
    const uintptr_t typeinfoAddress = static_cast<uintptr_t>(typeinfo);
    if (typeinfoAddress > UINTPTR_MAX - sizeof(uint64_t)) {
        // typeinfo + 8 would wrap.
        return false;
    }

    // The __name pointer sits in the typeinfo object's second word.
    uint64_t namePointer = 0;
    if (!readGuestWord(typeinfoAddress + sizeof(uint64_t), &namePointer) ||
        namePointer == 0) {
        return false;
    }
    const uintptr_t nameAddress = static_cast<uintptr_t>(namePointer);

    // Read the __name C string.  The buffer holds 96 bytes, so at most 95
    // characters plus the NUL; a longer name cannot be reported and is
    // rejected rather than truncated, because a truncated mangled name
    // would name a class that does not exist.
    size_t limit = kGuestClassNameCapacity - 1;
    if (limit > outSize - 1) {
        limit = outSize - 1;
    }
    if (limit < 2) {
        // The caller's buffer cannot hold a name at all.
        return false;
    }

    char buffer[kGuestClassNameCapacity];
    size_t length = 0;
    bool terminated = false;

    if (guestImageContains(nameAddress, limit)) {
        // FAST PATH: the whole window is inside the guest image, so every
        // byte read below is provably in bounds and needs no check of its
        // own.
        for (size_t i = 0; i < limit; ++i) {
            const char c =
                *reinterpret_cast<const volatile char*>(nameAddress + i);
            buffer[i] = c;
            if (c == '\0') {
                terminated = true;
                length = i;
                break;
            }
        }
    } else {
        // SLOW PATH: the window runs past the end of the image (or past the
        // end of a PT_LOAD segment), so every byte is checked on its own.
        // A name that runs off the end of the image must FAIL, not read
        // past it.
        for (size_t i = 0; i < limit; ++i) {
            if (!guestImageContains(nameAddress + i, 1)) {
                return false;
            }
            const char c =
                *reinterpret_cast<const volatile char*>(nameAddress + i);
            buffer[i] = c;
            if (c == '\0') {
                terminated = true;
                length = i;
                break;
            }
        }
    }

    if (!terminated || !plausibleMangledName(buffer, length)) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        out[i] = buffer[i];
    }
    out[length] = '\0';
    return true;
}

// True when `name` contains `needle`.
//
// Written out instead of calling std::strstr() so the test is plainly
// ASCII and locale-independent, and so the module needs no <cstring>.
bool guestClassNameContains(const char* name, const char* needle) {
    if (name == nullptr || needle == nullptr) {
        return false;
    }
    if (name[0] == '\0' || needle[0] == '\0') {
        return false;
    }
    for (const char* start = name; *start != '\0'; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

// Walks the guest's writable segments, finds every word that could be a
// vptr, resolves the class name of each, and reports the objects whose
// class name contains "GameMode", "RuleSet" or "RaceMetrics".
size_t guestCensusGlobalObjects(uintptr_t bias, GuestClassObject* out, size_t max,
                                GuestClassCensus* census) {
    GuestClassCensus local{};
    if (census == nullptr) {
        census = &local;
    }
    *census = GuestClassCensus{};

    if (bias == 0) {
        // The main image is not loaded, so there is nothing to scan.
        return 0;
    }

    size_t written = 0;

    // First pass: count everything and collect the interesting objects.
    CensusPass first{};
    runCensusPass(bias, false, out, max, &first, &written);

    census->scannedWords = first.scannedWords;
    census->vptrCandidates = first.candidates;
    census->namedObjects = first.named;
    census->gameModeObjects = first.gameMode;
    census->ruleSetObjects = first.ruleSet;
    census->metricsObjects = first.metrics;
    census->otherObjects = first.others;
    census->capped = first.capped ? 1u : 0u;

    if (out == nullptr || max == 0) {
        return written;
    }

    // Second pass: the objects that are not interesting, after the
    // interesting ones.  Its counters are the first pass' counters (the
    // scan is deterministic), so they are not counted twice.
    CensusPass second{};
    runCensusPass(bias, true, out, max, &second, &written);
    if (second.capped) {
        census->capped = 1u;
    }

    return written;
}

} // namespace rr3
