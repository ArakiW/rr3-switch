// The guest's string-to-number conversions, with a BOUND.
//
// The reasoning, the measurement and the failure mode are in
// include/rr3/guest_strto.hpp.  The short version: the host's strtoul scans
// until it meets a non-digit, and on the guest's thread that scan is
// unbounded, so a guest pointer to an unterminated string hangs the whole
// port with nothing in the log.
//
// MEASURED (q89): the guest's root thread was sampled inside the host image
// at pc_off=0x5c6154 = `_strtoul_l.isra.0`, and the run hung for 120 s with
// the log frozen.
//
// ⇒ Here the string is copied out of the guest's memory under a readability
// check, with a hard length limit, and the host's parser only sees the copy.
#include "rr3/guest_strto.hpp"

#include "rr3/recovery_guard.hpp"

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// HOW FAR THE HOST MAY LOOK.
//
// A number is short.  512 bytes is enough for any legitimate conversion
// (including a long run of leading spaces) and short enough that a
// pathological input cannot cost more than one bounded copy.
//
// MEASURED 2026-11-01 (q90..q92): the guest parses DOTTED LISTS that are
// longer than 128 bytes ("50.12.3c9.7.0.19.3.3.7.26.7.11.1..."), 150 of
// them per round.  With a 128-byte window every one of them was TRUNCATED --
// which is legal, and the copy still parses to the same value -- but the
// first version of the shim answered 0 for a truncated string and the
// guest's race metrics came back empty.  The window is 512 so that the
// guest's own strings fit entirely and the truncation path is the rare one.
constexpr size_t kMaxScan = 512;

constexpr const char* kGuestViewLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";

// The guest's address space tops out well below 2^47 (the same gate
// guest_action.cpp's pointerPlausible uses).  A pointer outside that range
// is an integer that was mistaken for a pointer.
bool pointerPlausible(uintptr_t address) {
    return address >= 0x10000ULL && address < 0x0000800000000000ULL;
}

// Is this page mapped and readable RIGHT NOW?
//
// The same question, with the same answer, as guest_touch.cpp's
// touchMemoryReadable() and guest_view_probe.cpp's reader: ask the kernel
// with svcQueryMemory and check Perm_R.  A pointer the guest published can
// point at a page that is no longer there, and a diagnostic that can crash
// the run it is measuring is worse than no diagnostic.
bool pageReadable(uintptr_t address) {
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        return false;
    }
    return (info.perm & Perm_R) != 0;
}

enum class TextState {
    Ok,          // copied, and the NUL was found inside the window
    Truncated,   // the window ran out before the NUL: the string is LONGER
                 // than the window, which is legal
    Unreadable   // a page of the string is not mapped and readable
};

struct GuestText {
    char buffer[kMaxScan + 1]{};
    TextState state = TextState::Unreadable;
    size_t length = 0;
};

// Copies the guest's string out, byte by byte, with a readability check per
// page.  The read is of the GUEST's memory, so it is a plain load in this
// process; the check is what keeps a wild pointer from being a fault.
GuestText takeGuestText(const char* text) {
    GuestText out{};
    if (text == nullptr) {
        out.state = TextState::Unreadable;
        return out;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(text);
    if (!pointerPlausible(base)) {
        out.state = TextState::Unreadable;
        return out;
    }
    uintptr_t page = 0;
    bool pageOk = false;
    for (size_t i = 0; i < kMaxScan; ++i) {
        const uintptr_t address = base + i;
        const uintptr_t thisPage = address & ~static_cast<uintptr_t>(0xfff);
        if (thisPage != page) {
            page = thisPage;
            pageOk = pageReadable(address);
        }
        if (!pageOk) {
            out.state = TextState::Unreadable;
            return out;
        }
        const char c = text[i];
        if (c == '\0') {
            out.buffer[i] = '\0';
            out.length = i;
            out.state = TextState::Ok;
            return out;
        }
        out.buffer[i] = c;
    }
    out.buffer[kMaxScan] = '\0';
    out.length = kMaxScan;
    out.state = TextState::Truncated;
    return out;
}

// The refusal is a NAMED EVENT, not a silent zero.
//
// THE BYTES THAT WERE READ ARE PRINTED, because "the guest called strtod
// on an unterminated string" is a fact and "the guest called strtod on a
// string that starts with 37 30 2e 31 32 2e 33" is a fact that points at
// the asset.
//
// AND THE TWO CASES ARE NOT THE SAME THING, WHICH IS WHY THEY ARE NOT
// TREATED THE SAME WAY:
//
//   * TRUNCATED: the string is LEGAL and longer than the window.  The
//     host's parser would have read the same leading bytes and stopped at
//     the same place, so the copy parses to the SAME VALUE and the answer
//     is returned.  Only the scan is bounded.
//   * UNREADABLE: the pointer is not mapped.  The host's parser would have
//     faulted, so there is no value to be faithful to; the answer is 0 and
//     the refusal is logged.
//
// MEASURED 2026-11-01 (q90..q92): the first version treated BOTH as a
// refusal and answered 0, and the guest then got 0 where it expected a
// number -- the guest parses long dotted strings ("50.12.3c9.7.0.19...",
// 150 of them per round), and the race's metrics came back EMPTY: the car's
// pointer was 0 and the lap distance was -1.  **A BOUND ON THE SCAN MUST
// NOT CHANGE THE ANSWER.**
void logRefusal(const char* function, const char* text, const GuestText& copy) {
    char head[96]{};
    const size_t shown = copy.length < 32 ? copy.length : 32;
    for (size_t i = 0; i < shown; ++i) {
        std::snprintf(head + i * 2, 3, "%02x",
                      static_cast<unsigned>(static_cast<unsigned char>(copy.buffer[i])));
    }
    const bool unreadable = copy.state == TextState::Unreadable;
    char details[320];
    std::snprintf(details, sizeof(details),
                  "fn=%s ptr=0x%llx state=%s bytes_read=%zu head=%s "
                  "note=%s",
                  function, static_cast<unsigned long long>(
                               reinterpret_cast<uintptr_t>(text)),
                  unreadable ? "unreadable" : "truncated",
                  copy.length, head,
                  unreadable
                      ? "the_pointer_is_not_readable_the_host_parser_would_have_faulted"
                      : "the_string_is_longer_than_the_window_the_value_is_the_copy_s_value");
    rr3::appendGuestBootLine(kGuestViewLog, unreadable ? "guest-strto-refused"
                                                       : "guest-strto-truncated",
                              0, 0, details);
}

}  // namespace

// ---------------------------------------------------------------------------
// The conversions themselves.
//
// Each one: take the bounded copy, parse the COPY, and translate the end
// pointer back into the guest's string.  A refused string answers 0.
// ---------------------------------------------------------------------------

long guestStrtol(const char* text, char** end, int base) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtol", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtol", text, copy);
    }
    char* stop = nullptr;
    const long value = std::strtol(copy.buffer, &stop, base);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

long long guestStrtoll(const char* text, char** end, int base) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtoll", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtoll", text, copy);
    }
    char* stop = nullptr;
    const long long value = std::strtoll(copy.buffer, &stop, base);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

unsigned long guestStrtoul(const char* text, char** end, int base) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtoul", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtoul", text, copy);
    }
    char* stop = nullptr;
    const unsigned long value = std::strtoul(copy.buffer, &stop, base);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

unsigned long long guestStrtoull(const char* text, char** end, int base) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtoull", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtoull", text, copy);
    }
    char* stop = nullptr;
    const unsigned long long value = std::strtoull(copy.buffer, &stop, base);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

double guestStrtod(const char* text, char** end) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtod", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0.0;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtod", text, copy);
    }
    char* stop = nullptr;
    const double value = std::strtod(copy.buffer, &stop);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

float guestStrtof(const char* text, char** end) {
    const GuestText copy = takeGuestText(text);
    if (copy.state == TextState::Unreadable) {
        // There is no value to be faithful to: the host's parser would
        // have faulted on this pointer.
        logRefusal("strtof", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0.0f;
    }
    if (copy.state == TextState::Truncated) {
        // The string is LEGAL and just longer than the window.  The
        // host's parser would have read the same leading bytes and
        // stopped in the same place, so the COPY parses to the same
        // value and it is returned.  Only the scan is bounded.
        logRefusal("strtof", text, copy);
    }
    char* stop = nullptr;
    const float value = std::strtof(copy.buffer, &stop);
    if (end != nullptr) {
        *end = (stop == copy.buffer) ? const_cast<char*>(text)
                                     : const_cast<char*>(text + (stop - copy.buffer));
    }
    return value;
}

int guestAtoi(const char* text) {
    return static_cast<int>(guestStrtol(text, nullptr, 10));
}

long guestAtol(const char* text) {
    return guestStrtol(text, nullptr, 10);
}

double guestAtof(const char* text) {
    return guestStrtod(text, nullptr);
}
