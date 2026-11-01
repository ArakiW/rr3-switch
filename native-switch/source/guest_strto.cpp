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
// A number is short.  128 bytes is enough for any legitimate conversion
// (including a long run of leading spaces) and short enough that a
// pathological input cannot cost more than one bounded copy.
constexpr size_t kMaxScan = 128;

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
    Unreadable,  // a page of the string is not mapped and readable
    Unterminated // the window ran out before the NUL
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
    out.state = TextState::Unterminated;
    return out;
}

// The refusal is a NAMED EVENT, not a silent zero.
//
// The bytes that WERE read are printed, because "the guest called strtoul
// on an unterminated string" is a fact and "the guest called strtoul on a
// string that starts with 7f 31 2e 30" is a fact that points at the asset.
void logRefusal(const char* function, const char* text, const GuestText& copy) {
    char head[96]{};
    const size_t shown = copy.length < 32 ? copy.length : 32;
    for (size_t i = 0; i < shown; ++i) {
        std::snprintf(head + i * 2, 3, "%02x",
                      static_cast<unsigned>(static_cast<unsigned char>(copy.buffer[i])));
    }
    char details[320];
    std::snprintf(details, sizeof(details),
                  "fn=%s ptr=0x%llx state=%s bytes_read=%zu head=%s "
                  "note=bounded_scan_refused_the_host_parser_never_saw_the_guest_string",
                  function, static_cast<unsigned long long>(
                               reinterpret_cast<uintptr_t>(text)),
                  copy.state == TextState::Unterminated ? "unterminated" : "unreadable",
                  copy.length, head);
    rr3::appendGuestBootLine(kGuestViewLog, "guest-strto-refused", 0, 0, details);
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtol", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtoll", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtoul", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtoull", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0;
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtod", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0.0;
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
    if (copy.state != TextState::Ok) {
        logRefusal("strtof", text, copy);
        if (end != nullptr) *end = const_cast<char*>(text);
        return 0.0f;
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
