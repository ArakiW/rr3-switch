#include "rr3/symbol_resolver.hpp"
#include "rr3/bionic_shims.hpp"
#include "rr3/bionic_misc_shims.hpp"
#include "rr3/cxx_lifecycle.hpp"
#include "rr3/elf_preflight.hpp"
#include "rr3/fmod_audio_stub.hpp"
#include "rr3/fmod_shim.hpp"
#include "rr3/guest_filesystem.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/libcxx_shims.hpp"
#include "rr3/jump_bridge.hpp"
#include "rr3/optional_platform_shims.hpp"
#include "rr3/pthread_bridge.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/semaphore_bridge.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <malloc.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
long double strtold_l(const char* input, char** end, locale_t locale);
long long strtoll_l(const char* input, char** end, int base, locale_t locale);
unsigned long long strtoull_l(const char* input, char** end, int base,
                             locale_t locale);
int vasprintf(char** output, const char* format, va_list arguments);
void* memrchr(const void* memory, int character, size_t length);
void sincosf(float angle, float* sine, float* cosine);
// OpenSL IID pointer cells (data symbols) from fmod_audio_stub.cpp.
extern const void* SL_IID_ENGINE;
extern const void* SL_IID_PLAY;
extern const void* SL_IID_BUFFERQUEUE;
extern const void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
}

namespace rr3 {

// ---------------------------------------------------------------------------
// Per-guest-thread activity census. Defined in pthread_bridge.cpp, declared
// here because this is the only translation unit that sees the whole symbol
// table. censusArm() is called from resolveHostSymbol, which the guest linker
// runs during image relocation: strictly after the bridge self-tests and
// strictly before the game boots.
// ---------------------------------------------------------------------------
void censusArm();
void censusNoteImportFrom(const void* returnAddress, const char* symbol);
void censusWatchNextImport(const char* tag);

// See pthread_bridge.cpp: the link register has to be read in the shim.
#define CENSUS_IMPORT(symbol) \
    ::rr3::censusNoteImportFrom(__builtin_return_address(0), symbol)

namespace {

#define RR3_BIND(symbol) {#symbol, reinterpret_cast<uintptr_t>(&::symbol)}
#define RR3_SHIM(symbolName, function) \
    {symbolName, reinterpret_cast<uintptr_t>(&function)}

// Offline: make connect() fail immediately so the guest HTTP client does not
// block the render thread waiting for a network that will never answer.
int guestConnectStub(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    errno = ECONNREFUSED;
    return -1;
}

// ---------------------------------------------------------------------------
// hbn read trace. /files/apk/res/hbn is a 4-byte file (00 4C 7B 00) and the
// last file the guest touches before the defect, so every stdio step the guest
// performs on it is logged with the exact offset, length and bytes, and the
// next import after the read is logged too.
// ---------------------------------------------------------------------------
constexpr const char* kHbnLogPath = "sdmc:/switch/rr3-native-rehost.guest-view.log";
constexpr size_t kHbnStreamCapacity = 16;
std::FILE* gHbnStreams[kHbnStreamCapacity]{};
unsigned int gHbnOpens = 0;
unsigned int gHbnReads = 0;

bool isHbnPath(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    return std::strcmp(base, "hbn") == 0;
}

bool hbnTracked(std::FILE* file) {
    for (std::FILE* candidate : gHbnStreams) {
        if (candidate == file) {
            return true;
        }
    }
    return false;
}

void hbnTrack(std::FILE* file) {
    if (file == nullptr || hbnTracked(file)) {
        return;
    }
    for (std::FILE*& candidate : gHbnStreams) {
        if (candidate == nullptr) {
            candidate = file;
            return;
        }
    }
    gHbnStreams[0] = file;
}

void hbnHex(const void* data, size_t length, char* out, size_t capacity) {
    static const char* kHexDigits = "0123456789abcdef";
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    const size_t shown = length < 24 ? length : 24;
    const size_t limit = capacity / 2 - 1;
    const size_t count = shown < limit ? shown : limit;
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = kHexDigits[(bytes[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHexDigits[bytes[i] & 0xf];
    }
    out[count * 2] = '\0';
}

std::FILE* censusFopen(const char* path, const char* mode) {
    CENSUS_IMPORT("fopen");
    const bool hbn = isHbnPath(path);
    std::FILE* file = guestFopen(path, mode);
    if (!hbn) {
        return file;
    }
    hbnTrack(file);
    long size = -1;
    if (file != nullptr && std::fseek(file, 0, SEEK_END) == 0) {
        size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);
    }
    char details[320];
    __builtin_snprintf(details, sizeof(details),
                       "path=%s mode=%s file=%p size=%ld errno=%d", path,
                       mode != nullptr ? mode : "(null)", file, size, errno);
    appendGuestBootLine(kHbnLogPath, file != nullptr ? "hbn-open" : "hbn-open-fail",
                        ++gHbnOpens,
                        static_cast<unsigned long long>(size < 0 ? 0 : size),
                        details);
    return file;
}

size_t censusFread(void* buffer, size_t size, size_t count, std::FILE* file) {
    CENSUS_IMPORT("fread");
    if (!hbnTracked(file)) {
        return std::fread(buffer, size, count, file);
    }
    const long offset = std::ftell(file);
    const size_t want = size * count;
    const size_t got = std::fread(buffer, size, count, file);
    char hex[96]{};
    hbnHex(buffer, got, hex, sizeof(hex));
    char details[320];
    __builtin_snprintf(details, sizeof(details),
                       "offset=%ld want=%zu got=%zu elem=%zu eof=%d err=%d "
                       "bytes=%s", offset, want, got, size,
                       std::feof(file) ? 1 : 0, std::ferror(file) ? 1 : 0, hex);
    appendGuestBootLine(kHbnLogPath, "hbn-read", ++gHbnReads,
                        static_cast<unsigned long long>(offset < 0 ? 0 : offset),
                        details);
    censusWatchNextImport("hbn-read");
    return got;
}

char* censusFgets(char* buffer, int size, std::FILE* file) {
    CENSUS_IMPORT("fgets");
    if (!hbnTracked(file)) {
        return std::fgets(buffer, size, file);
    }
    const long offset = std::ftell(file);
    char* line = std::fgets(buffer, size, file);
    const long after = std::ftell(file);
    char details[320];
    __builtin_snprintf(details, sizeof(details),
                       "offset=%ld after=%ld want=%d line=%s", offset, after,
                       size, line != nullptr ? line : "(null)");
    appendGuestBootLine(kHbnLogPath, "hbn-fgets", ++gHbnReads,
                        static_cast<unsigned long long>(offset < 0 ? 0 : offset),
                        details);
    censusWatchNextImport("hbn-fgets");
    return line;
}

int censusFseek(std::FILE* file, long offset, int whence) {
    CENSUS_IMPORT("fseek");
    if (!hbnTracked(file)) {
        return std::fseek(file, offset, whence);
    }
    const int result = std::fseek(file, offset, whence);
    const long position = std::ftell(file);
    char details[320];
    __builtin_snprintf(details, sizeof(details),
                       "offset=%ld whence=%d result=%d position=%ld", offset,
                       whence, result, position);
    appendGuestBootLine(kHbnLogPath, "hbn-fseek", 0,
                        static_cast<unsigned long long>(position < 0 ? 0 : position),
                        details);
    return result;
}

long censusFtell(std::FILE* file) {
    CENSUS_IMPORT("ftell");
    if (hbnTracked(file)) {
        const long position = std::ftell(file);
        appendGuestBootLine(kHbnLogPath, "hbn-ftell", 0,
                            static_cast<unsigned long long>(position < 0 ? 0 : position),
                            "tell");
    }
    return std::ftell(file);
}

// ---------------------------------------------------------------------------
// fd side table.
//
// The sock-* rows further down record what the guest asked for, but nothing
// remembers which fd the answer landed on, and a run log does not decode
// itself: the last run had three sock-socket rows, two sock-bind rows and a
// sock-accept row, and the listener fd, the outbound fd, the accepted fd and
// the fourth socket all have to be inferred by reading the numbers in order.
// "Which of the fds that poll() watches is the one that is always ready" is not
// answerable from that alone, which is the whole reason the poll tap needs
// this table.
//
// It is written only by the shims in this file and read only by the poll tap.
// It takes no lock: every field is single-writer, and a diagnostic that can
// block a guest thread is worse than one that can be a microsecond stale.
// Slots are keyed by fd number, so a close followed by a reuse shows up as the
// new descriptor's facts, which is the truth.
// ---------------------------------------------------------------------------
constexpr size_t kFdTableSize = 64;

struct FdRecord {
    int fd{-1};
    int domain{0};
    int type{0};
    int protocol{0};
    unsigned int port{0};
    const char* role{"unknown"};
};

FdRecord gFdTable[kFdTableSize];

// create=false is the read-only form, and is the only form the poll fast path
// is ever allowed to use.
FdRecord* fdFind(int fd, bool create) {
    if (fd < 0) {
        return nullptr;
    }
    FdRecord* freeSlot = nullptr;
    for (FdRecord& record : gFdTable) {
        if (record.fd == fd) {
            return &record;
        }
        if (freeSlot == nullptr && record.fd < 0) {
            freeSlot = &record;
        }
    }
    if (!create || freeSlot == nullptr) {
        return nullptr;
    }
    *freeSlot = FdRecord{};
    freeSlot->fd = fd;
    return freeSlot;
}

// The address family as the HOST stack understands it, which is not always the
// number the guest asked for: a family the host has no socket type for still
// comes back as a live fd. Printing the bare number is what made the last run's
// third socket ("domain=518") unreadable, so the table names the families libnx
// defines and falls back to domN for everything else.
const char* socketFamilyName(int domain) {
    switch (domain) {
        case AF_UNSPEC: return "unspec";
        case AF_UNIX: return "unix";
        case AF_INET: return "inet";
        case AF_INET6: return "inet6";
        case AF_ROUTE: return "route";
        case AF_BLUETOOTH: return "bluetooth";
        default: return nullptr;
    }
}

void describeFd(int fd, char* out, size_t capacity) {
    const FdRecord* record = fdFind(fd, false);
    if (record == nullptr) {
        __builtin_snprintf(out, capacity, "fd%d(unknown)", fd);
        return;
    }
    const char* family = socketFamilyName(record->domain);
    char familyText[24];
    if (family == nullptr) {
        __builtin_snprintf(familyText, sizeof(familyText), "dom%d", record->domain);
        family = familyText;
    }
    if (record->port != 0) {
        __builtin_snprintf(out, capacity, "%s %s:%u", record->role, family,
                           record->port);
        return;
    }
    __builtin_snprintf(out, capacity, "%s %s", record->role, family);
}

// The accepted descriptor inherits the listener's family, and the one fact
// worth carrying into the poll log is who it ended up connected to, so the poll
// tap can name it without the sock-* rows having to be read in order. Only an
// IPv4 peer port is extracted: that is the only shape this port ever produces
// (the listener is AF_INET in every run so far), and a wrong port is worse
// than none.
void fdRememberAccepted(int listener, int accepted) {
    FdRecord* record = fdFind(accepted, true);
    if (record == nullptr) {
        return;
    }
    const FdRecord* source = fdFind(listener, false);
    if (source != nullptr) {
        record->domain = source->domain;
        record->type = source->type;
        record->protocol = source->protocol;
    }
    struct sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    if (accepted >= 0 &&
        ::getpeername(accepted, reinterpret_cast<struct sockaddr*>(&peer),
                      &length) == 0 &&
        peer.sin_family == AF_INET) {
        record->port = static_cast<unsigned int>(ntohs(peer.sin_port));
    }
    record->role = "accepted";
}

// ---------------------------------------------------------------------------
// File-I/O census taps.
//
// Until now only the five stdio shims above were counted, so a read() or an
// open() that never returned left no trace whatsoever: the thread's ring just
// stopped, and the sweep printed delta=0 with some older entry as "last". That
// is the blind spot that made delta=0 on the frozen main thread uninformative.
//
// These wrappers are the identical call with the return address recorded first,
// so the blocking call -- if it is file I/O -- appears as the final ring entry
// together with the address it will resume at. Every other bind in the table
// is still uncounted; this closes the file-I/O family only.
// ---------------------------------------------------------------------------
ssize_t censusRead(int fd, void* buffer, size_t count) {
    CENSUS_IMPORT("read");
    return ::read(fd, buffer, count);
}

ssize_t censusWrite(int fd, const void* buffer, size_t count) {
    CENSUS_IMPORT("write");
    return ::write(fd, buffer, count);
}

int censusClose(int fd) {
    CENSUS_IMPORT("close");
    // The fd side table has to learn about this or a closed-then-reused fd
    // number is attributed to whatever it used to be. Marked, not erased: the
    // poll tap can then report "fd21, closed" instead of "fd21, unknown", which
    // is a different fact when a guest polls a descriptor it has already
    // dropped.
    FdRecord* record = fdFind(fd, false);
    const int result = ::close(fd);
    if (record != nullptr && result == 0) {
        record->role = "closed";
    }
    return result;
}

off_t censusLseek(int fd, off_t offset, int whence) {
    CENSUS_IMPORT("lseek");
    return ::lseek(fd, offset, whence);
}

int censusFstat(int fd, struct stat* output) {
    CENSUS_IMPORT("fstat");
    return ::fstat(fd, output);
}

int censusFsync(int fd) {
    CENSUS_IMPORT("fsync");
    return ::fsync(fd);
}

int censusFclose(std::FILE* file) {
    CENSUS_IMPORT("fclose");
    return std::fclose(file);
}

int censusFtruncate(int fd, off_t length) {
    CENSUS_IMPORT("ftruncate");
    return ::ftruncate(fd, length);
}

int censusClosedir(DIR* directory) {
    CENSUS_IMPORT("closedir");
    return ::closedir(directory);
}

// ---------------------------------------------------------------------------
// Socket census taps.
//
// connect() is refused for every destination that leaves the machine (see
// guestConnectStub, and the loopback exception in destinationIsLoopback), so
// the one socket call that is guaranteed to run on the render path was the one
// call the census could not see: a thread blocked in the guest's own socket
// code, or waiting on a socket fd, stopped with no breadcrumb at all. The
// family is wrapped wholesale so a delta=0 thread can be attributed to socket
// work rather than guessed at. Every row that changes what the guest sees also
// logs the arguments it was handed, so the destination is on record whether or
// not the answer was yes.
//
// Pure getters (inet_*, getsockname, getaddrinfo, ...) are counted too: the
// census answers "which host call is this thread sitting in", and a getaddrinfo
// is exactly the kind of call that can block on a resolver.
// ---------------------------------------------------------------------------
// Renders a guest sockaddr as "family=… addr=… port=…". The guest's bionic
// sockaddr layout is the host's layout (that is the whole premise of the faked
// loader), so the cast is safe and the arguments are logged verbatim rather
// than translated. Without this the listening socket's address and port were
// only inferable from the symbol ring, never read.
void describeSockaddr(const struct sockaddr* address, socklen_t length,
                      char* out, size_t capacity) {
    if (address == nullptr) {
        __builtin_snprintf(out, capacity, "addr=(null) len=%u",
                           static_cast<unsigned>(length));
        return;
    }
    if (address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* in4 =
            reinterpret_cast<const struct sockaddr_in*>(address);
        char text[46];
        text[0] = '\0';
        ::inet_ntop(AF_INET, &in4->sin_addr, text, sizeof(text));
        __builtin_snprintf(out, capacity, "family=AF_INET addr=%s port=%u", text,
                           static_cast<unsigned>(ntohs(in4->sin_port)));
        return;
    }
    if (address->sa_family == AF_INET6 &&
        length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6* in6 =
            reinterpret_cast<const struct sockaddr_in6*>(address);
        char text[46];
        text[0] = '\0';
        ::inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text));
        __builtin_snprintf(out, capacity, "family=AF_INET6 addr=%s port=%u", text,
                           static_cast<unsigned>(ntohs(in6->sin6_port)));
        return;
    }
    __builtin_snprintf(out, capacity, "family=%u raw=", address->sa_family);
}

// One appended line per socket lifecycle event, carrying the arguments the
// guest actually passed. The log is a handful of events per run, so the fsync
// in appendGuestBootLine is not on any hot path.
void socketLogEvent(const char* what, int fd, const char* detail) {
    char line[320];
    __builtin_snprintf(line, sizeof(line), "fd=%d %s",
                       fd, detail != nullptr ? detail : "");
    appendGuestBootLine(kHbnLogPath, what, 0, 0, line);
}

// Is this destination the guest talking to itself? 127.0.0.0/8 and ::1 only.
//
// This is the entire connect policy, and the distinction is the point: the port
// is offline, so a destination that leaves the machine is refused immediately
// exactly as guestConnectStub refuses it. A loopback destination is different
// in kind, not degree -- the guest built that listener itself a few hundred
// imports earlier, so satisfying it needs no network at all. Letting it through
// keeps the port self-consistent: the guest gets the connected socket its own
// code asked for, and still gets nothing from the internet.
//
// Anything this does not positively recognise as loopback -- an unparsable
// sockaddr, a bare hostname that never got resolved, a public address -- is
// refused. The test is allowlist, never denylist.
//
// A tighter policy was considered and rejected on evidence: allow only the port
// this process has itself bound or listened on. It cannot work, because the
// guest never binds that listener -- it goes socket, listen, getsockname, and
// lets listen's implicit bind pick the ephemeral port, which is why the sock-
// bind row is absent from the run log for the TCP socket and why the port is
// only ever knowable after the fact. The guest dials whatever getsockname
// reported, and the only way to know that in advance is to be the host network
// stack. So the policy is destination-based: this guest dials exactly one
// address, and it is itself.
bool destinationIsLoopback(const struct sockaddr* address, socklen_t length) {
    if (address == nullptr) {
        return false;
    }
    if (address->sa_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* in4 =
            reinterpret_cast<const struct sockaddr_in*>(address);
        return (ntohl(in4->sin_addr.s_addr) >> 24) == 127u;
    }
    if (address->sa_family == AF_INET6 &&
        length >= sizeof(struct sockaddr_in6)) {
        // Compared against ::1 through inet_pton rather than
        // IN6_IS_ADDR_LOOPBACK so this file gains no dependency it did not
        // already have.
        unsigned char loopback[16];
        if (::inet_pton(AF_INET6, "::1", loopback) != 1) {
            return false;
        }
        return std::memcmp(&reinterpret_cast<const struct sockaddr_in6*>(address)
                               ->sin6_addr,
                           loopback, sizeof(loopback)) == 0;
    }
    return false;
}

// accept() is the one host call in this family that parks a guest thread
// forever, and it is where ordinal=3 has been sitting with delta=0 for 108 s.
// The guest built a full server here -- socket, listen, a second socket,
// getsockname, connect, accept, in that order and all on the main thread at
// around its 600th import -- and then went on to a client it never used. Note
// there is no bind in that sequence: listen's implicit bind is what gives the
// listener its ephemeral port, which is why the port is only knowable after the
// fact and why a port-allowlist policy cannot be written ahead of time.
//
// The working diagnosis was that the client half is refused by
// guestConnectStub, so the guest's own accept could never be satisfied. The
// loopback exception in destinationIsLoopback is the attempt to make that
// accept satisfiable without opening a single non-loopback door. It works:
// with connect() instrumented, the run log shows the guest learning its own
// port with getsockname (127.0.0.1:6875, 127.0.0.1:7272 on two runs) and
// dialling it, so connect returns 0 and this accept returns a real connected
// fd instead of EAGAIN. The guest never dials anything else.
//
// A blocking accept here is a deadlock by construction whenever nothing dials:
// the thread that would satisfy it is the very thread blocked on it.
//
// So accept never parks. It reports "no connection is available yet" the way a
// non-blocking socket does. The real hardware path stays sane: this is exactly
// the semantics a listener gets from a non-blocking accept, and a server loop
// that already re-polls will simply come back. The guest is a bionic caller
// that never set O_NONBLOCK itself, so the honest equivalent is a bounded
// wait rather than an instant lie -- acceptWaitMs below picks between the two.
//
// acceptWaitMs is 50 ms, and that specific value is load-bearing. At 0
// (return -1/EAGAIN immediately) the guest does not park, but it also does not
// back off: it spawns a further thread that then spins on host imports ~77M
// times in 120 s and doubles host CPU. The bounded wait keeps the retry rate
// down to ~20/s, which is an ordinary poll rather than a burn. A longer wait
// would damp it further at the cost of a slower wake-up on real hardware, so
// this is the one knob to turn if the spin ever reappears.
constexpr int acceptWaitMs = 50;

// Variant knob for this row, so both behaviours are buildable and measured
// rather than argued about.
//
//   false -> the 50 ms bounded poll above: never parks, reports EAGAIN.
//   true  -> bionic's plain blocking accept: parks until a client arrives.
//
// Measured, both variants, 120 s runs on the same tree:
//   the guest DOES dial itself. getsockname returns 127.0.0.1:<ephemeral> on
//   the listener it created, connect to that address is made, and only then is
//   accept called -- connect precedes accept in every run log, so a blocking
//   accept returns the queued connection at once (blocking ret=23, the same fd
//   the bounded poll picks up) and parks for 0 ms.
//
// So the blocking form is now safe, and it is the faithful one. It is still not
// the default, because it buys nothing measurable -- with the loopback policy
// in place the bounded poll picks the connection up on its first poll, and the
// four signals that matter (ordinal=3 exits, ordinal=4 spins, ordinal=0 frozen
// at 637 imports, frames=1) are identical under both. The one thing the
// blocking form adds is the ability to park forever if the guest ever stops
// dialling, which is the exact failure this row exists to prevent. Flip this
// constant when a future measurement says the faithfulness is worth that risk.
constexpr bool acceptBlocksForever = false;

int censusAccept(int sockfd, struct sockaddr* address, socklen_t* length) {
    CENSUS_IMPORT("accept");
    if (acceptBlocksForever) {
        const int accepted = ::accept(sockfd, address, length);
        fdRememberAccepted(sockfd, accepted);
        char blocked[96];
        __builtin_snprintf(blocked, sizeof(blocked), "blocking ret=%d errno=%d",
                           accepted, errno);
        socketLogEvent("sock-accept", sockfd, blocked);
        return accepted;
    }
    // Wait a bounded time at most. This deliberately never becomes an
    // unbounded park, which is the defect being fixed.
    if (acceptWaitMs > 0) {
        struct pollfd waiter;
        waiter.fd = sockfd;
        waiter.events = POLLIN;
        waiter.revents = 0;
        const int ready = ::poll(&waiter, 1, acceptWaitMs);
        if (ready > 0) {
            const int accepted = ::accept(sockfd, address, length);
            fdRememberAccepted(sockfd, accepted);
            char detail[96];
            __builtin_snprintf(detail, sizeof(detail), "picked ret=%d errno=%d",
                               accepted, errno);
            socketLogEvent("sock-accept", sockfd, detail);
            return accepted;
        }
        // ready==0 is the bounded timeout: fall through and report EAGAIN.
    }
    errno = EAGAIN;
    char detail[96];
    __builtin_snprintf(detail, sizeof(detail),
                       "timeout wait_ms=%d ret=-1 errno=EAGAIN", acceptWaitMs);
    socketLogEvent("sock-accept", sockfd, detail);
    return -1;
}

int censusBind(int sockfd, const struct sockaddr* address, socklen_t length) {
    CENSUS_IMPORT("bind");
    char described[192];
    describeSockaddr(address, length, described, sizeof(described));
    const int result = ::bind(sockfd, address, length);
    FdRecord* record = fdFind(sockfd, true);
    if (record != nullptr && address != nullptr && address->sa_family == AF_INET &&
        length >= sizeof(struct sockaddr_in) && result == 0) {
        record->port = static_cast<unsigned int>(
            ntohs(reinterpret_cast<const struct sockaddr_in*>(address)->sin_port));
    }
    char detail[256];
    __builtin_snprintf(detail, sizeof(detail), "%s ret=%d errno=%d", described,
                       result, errno);
    socketLogEvent("sock-bind", sockfd, detail);
    return result;
}

// Variant knob, so the loopback exception above is measurable rather than
// argued about.
//
//   true  -> current behaviour: the guest's connect to its own listener is a
//            real loopback connect, so accept hands back a connected fd.
//   false -> the original offline policy: every connect, loopback included, is
//            refused with ECONNREFUSED and accept can only ever report EAGAIN.
//
// Measured with true: four signals (ordinal=3 exits, ordinal=4 spins, ordinal=0
// frozen at 637 imports, frames=1) are identical either way, so the exception
// bought nothing measurable. That is what makes false worth re-testing: it
// removes the accepted descriptor from the process, and if that descriptor is
// the one poll() reports ready forever, the loop stops. Flip this constant to
// test the hypothesis; nothing else in the file needs to change.
constexpr bool allowLoopbackConnect = true;

int censusConnect(int sockfd, const struct sockaddr* address, socklen_t length) {
    CENSUS_IMPORT("connect");
    // Logged unconditionally, before the policy runs: the destination the guest
    // asked for is the fact this whole family exists to capture, and it must be
    // on record whether or not the answer is yes.
    char described[192];
    describeSockaddr(address, length, described, sizeof(described));
    const bool loopback = destinationIsLoopback(address, length);
    int result;
    int failure;
    if (loopback && allowLoopbackConnect) {
        // A real connect. The peer is the guest's own listener, bound a few
        // hundred imports earlier by censusBind/censusListen, so this is a
        // loopback handshake with no network involved: it either completes
        // against a queue that is already there or fails at once with
        // ECONNREFUSED, and it cannot park the way a public connect would.
        result = ::connect(sockfd, address, length);
        failure = errno;
    } else {
        // The offline refusal, unchanged from the original stub: named here so
        // there is exactly one place that decides a connection is refused.
        result = guestConnectStub(sockfd, address, length);
        failure = errno;
    }
    char detail[288];
    __builtin_snprintf(detail, sizeof(detail),
                       "%s policy=%s ret=%d errno=%d", described,
                       loopback ? (allowLoopbackConnect ? "loopback-connect"
                                                        : "loopback-refuse")
                                : "offline-refuse",
                       result, failure);
    socketLogEvent("sock-connect", sockfd, detail);
    return result;
}

void censusFreeaddrinfo(struct addrinfo* result) {
    CENSUS_IMPORT("freeaddrinfo");
    ::freeaddrinfo(result);
}

int censusGetaddrinfo(const char* node, const char* service,
                      const struct addrinfo* hints, struct addrinfo** result) {
    CENSUS_IMPORT("getaddrinfo");
    return ::getaddrinfo(node, service, hints, result);
}

struct hostent* censusGethostbyname(const char* name) {
    CENSUS_IMPORT("gethostbyname");
    return ::gethostbyname(name);
}

// ---------------------------------------------------------------------------
// getsockname instrumentation.
//
// The last host->guest event of the whole run, on the socket the guest made
// with socket(518, SOCK_DGRAM, IPPROTO_UDP) and then bound to 0.0.0.0:60002,
// was this row answering -1. Three things about that line were unreadable, and
// all three are logging defects rather than behaviour, so nothing here changes
// what the guest sees:
//
//   1. "errno=22" was never this call's errno. libnx's getsockname returns -1
//      through a path that never writes errno (nx/socket.c: on the -1 path the
//      errno store is skipped), so whatever errno the guest's own last libc
//      call left behind is what the log printed. 22 is EINVAL, which is exactly
//      what a failed inet_pton leaves behind, and this guest is parsing address
//      strings. errno is now captured immediately after the call, before
//      describeSockaddr() gets a chance to run inet_ntop and disturb it.
//
//   2. "family=0" was read out of a buffer the call never wrote. describeSockaddr
//      runs after the fact, so on a failure it decodes whatever the guest left
//      in the buffer. It is evidence that the buffer is untouched, not evidence
//      of what the guest asked for. The bytes are now logged before and after.
//
//   3. The host's own result code was never recorded. socketGetLastResult()
//      returns the thread-local bsd Result that libnx's getsockname just stored,
//      which is the only thing in the system that can say why the Horizon
//      bsd service refused. It is read with nothing in between.
//
// The probe below is the one that separates the two remaining explanations. If
// the guest's own call fails, the identical query is re-issued on a private
// 16-byte buffer with addrlen=16 -- what a bionic caller always passes. The
// probe's answer is logged and discarded: the guest still receives the host's
// real result, and the guest's buffer is only ever written by the real call. So
// "probe succeeds" means our own argument handling was the obstacle, and
// "probe fails too" means the descriptor has no name the host will give, which
// would make the error faithful and this row a dead end.
// ---------------------------------------------------------------------------
constexpr bool probeGetsocknameOnFailure = true;

int censusGetsockname(int sockfd, struct sockaddr* address, socklen_t* length) {
    CENSUS_IMPORT("getsockname");
    const socklen_t lengthIn = length != nullptr ? *length : 0;
    // Eight bytes, not sixteen: offset 0-7 is the whole discriminating part of
    // any of the layouts in play -- sin_len/sin_family, or sin_family as a u16,
    // then sin_port, then sin_addr. sin_zero adds nothing but log width.
    unsigned char before[8] = {0};
    if (address != nullptr) {
        std::memcpy(before, address, sizeof(before));
    }
    const int result = ::getsockname(sockfd, address, length);
    // Captured before anything else can touch it: libnx does not set errno on
    // the failure path, so this is the caller's own leftover, and printing it
    // as though it were the result is what made this row unreadable.
    const int failure = errno;
    const Result hostResult = socketGetLastResult();
    const socklen_t lengthOut = length != nullptr ? *length : 0;
    // Logged because this is the one call that reveals which port the host
    // actually assigned to the guest's listener, and that port is what any
    // self-connect has to name. Silence here made the guest's bind look
    // portless in the log when it was not. The same port goes into the fd side
    // table, because the listener never binds explicitly -- listen's implicit
    // bind is the only thing that ever assigns it.
    char described[96];
    describeSockaddr(address, lengthOut, described, sizeof(described));
    FdRecord* record = fdFind(sockfd, true);
    if (record != nullptr && address != nullptr && address->sa_family == AF_INET &&
        length != nullptr && *length >= sizeof(struct sockaddr_in) && result == 0) {
        record->port = static_cast<unsigned int>(
            ntohs(reinterpret_cast<const struct sockaddr_in*>(address)->sin_port));
    }
    char identity[48];
    describeFd(sockfd, identity, sizeof(identity));
    // hbnHex dereferences unconditionally, and getsockname is allowed a null
    // buffer, so the null case is rendered here rather than passed in.
    char beforeHex[24];
    char afterHex[24];
    if (address != nullptr) {
        hbnHex(before, sizeof(before), beforeHex, sizeof(beforeHex));
        hbnHex(address, sizeof(before), afterHex, sizeof(afterHex));
    } else {
        __builtin_snprintf(beforeHex, sizeof(beforeHex), "(null)");
        __builtin_snprintf(afterHex, sizeof(afterHex), "(null)");
    }
    char detail[288];
    __builtin_snprintf(detail, sizeof(detail),
                       "%s ret=%d errno=%d result=0x%08x fd=%s len=%u->%u "
                       "pre=%s post=%s",
                       described, result, failure,
                       static_cast<unsigned>(hostResult), identity,
                       static_cast<unsigned>(lengthIn),
                       static_cast<unsigned>(lengthOut), beforeHex, afterHex);
    socketLogEvent("sock-getsockname", sockfd, detail);
    if (result != 0 && probeGetsocknameOnFailure) {
        // Diagnostic only. A private buffer, the length a bionic caller would
        // pass, and the answer thrown away: the guest above already got the
        // host's honest result and its buffer is untouched by this.
        struct sockaddr_in probe = {};
        socklen_t probeLength = sizeof(probe);
        const int probeResult = ::getsockname(
            sockfd, reinterpret_cast<struct sockaddr*>(&probe), &probeLength);
        const int probeFailure = errno;
        const Result probeHostResult = socketGetLastResult();
        char probeText[64];
        hbnHex(&probe, sizeof(probe), probeText, sizeof(probeText));
        char probeDetail[224];
        __builtin_snprintf(probeDetail, sizeof(probeDetail),
                           "probe ret=%d errno=%d result=0x%08x len=%u bytes=%s",
                           probeResult, probeFailure,
                           static_cast<unsigned>(probeHostResult),
                           static_cast<unsigned>(probeLength), probeText);
        socketLogEvent("sock-getsockname-probe", sockfd, probeDetail);
    }
    return result;
}

int censusGetsockopt(int sockfd, int level, int option, void* value,
                     socklen_t* length) {
    CENSUS_IMPORT("getsockopt");
    return ::getsockopt(sockfd, level, option, value, length);
}

in_addr_t censusInetAddr(const char* text) {
    CENSUS_IMPORT("inet_addr");
    return ::inet_addr(text);
}

const char* censusInetNtop(int family, const void* source, char* destination,
                           socklen_t size) {
    CENSUS_IMPORT("inet_ntop");
    return ::inet_ntop(family, source, destination, size);
}

int censusInetPton(int family, const char* text, void* destination) {
    CENSUS_IMPORT("inet_pton");
    return ::inet_pton(family, text, destination);
}

int censusListen(int sockfd, int backlog) {
    CENSUS_IMPORT("listen");
    const int result = ::listen(sockfd, backlog);
    FdRecord* record = fdFind(sockfd, true);
    if (record != nullptr && result == 0) {
        record->role = "listener";
    }
    char detail[96];
    __builtin_snprintf(detail, sizeof(detail), "backlog=%d ret=%d errno=%d",
                       backlog, result, errno);
    socketLogEvent("sock-listen", sockfd, detail);
    return result;
}

ssize_t censusRecv(int sockfd, void* buffer, size_t length, int flags) {
    CENSUS_IMPORT("recv");
    return ::recv(sockfd, buffer, length, flags);
}

ssize_t censusRecvfrom(int sockfd, void* buffer, size_t length, int flags,
                       struct sockaddr* from, socklen_t* fromLength) {
    CENSUS_IMPORT("recvfrom");
    return ::recvfrom(sockfd, buffer, length, flags, from, fromLength);
}

// libnx declares recvmmsg with the glibc 5th "timeout" argument, which bionic
// does not have. The guest is a bionic DSO and calls the 4-argument form, so
// x4 is not a timespec: forwarding nullptr is the only safe value and is
// exactly bionic's blocking, no-timeout behaviour. (The row used to bind
// &::recvmmsg directly, which would have forwarded whatever x4 held.)
int censusRecvmmsg(int sockfd, struct mmsghdr* messages, unsigned int count,
                   int flags) {
    CENSUS_IMPORT("recvmmsg");
    return ::recvmmsg(sockfd, messages, count, flags, nullptr);
}

ssize_t censusSend(int sockfd, const void* buffer, size_t length, int flags) {
    CENSUS_IMPORT("send");
    return ::send(sockfd, buffer, length, flags);
}

int censusSendmmsg(int sockfd, struct mmsghdr* messages, unsigned int count,
                   int flags) {
    CENSUS_IMPORT("sendmmsg");
    return ::sendmmsg(sockfd, messages, count, flags);
}

ssize_t censusSendto(int sockfd, const void* buffer, size_t length, int flags,
                     const struct sockaddr* to, socklen_t toLength) {
    CENSUS_IMPORT("sendto");
    return ::sendto(sockfd, buffer, length, flags, to, toLength);
}

int censusSetsockopt(int sockfd, int level, int option, const void* value,
                     socklen_t length) {
    CENSUS_IMPORT("setsockopt");
    return ::setsockopt(sockfd, level, option, value, length);
}

// Which address families this host stack can actually service.
//
// The allowlist is the whole point: a family that cannot be serviced must not
// come back as a live descriptor, because everything downstream then treats a
// dead thing as a live one. The evidence is a single row in the last run log:
//
//   sock-socket fd=21 domain=518 type=2 proto=17 fd=21
//
// libnx defines no family 518 (its AF_* table stops at AF_VENDOR49 = 147, and
// AF_PACKET is not defined in this toolchain at all), yet the socket() returned
// a descriptor. A family that does not exist cannot be serviced by anything,
// and on real hardware the request is refused at once -- EAFNOSUPPORT, no fd --
// so the faithful answer is the refusal, not the descriptor.
//
// The test is the number the guest passed, not a name this toolchain lacks.
// Only AF_UNIX, AF_INET and AF_INET6 pass; everything else is refused. That is
// stricter than a deny list of the one family seen so far, and it cannot be
// defeated by the guest asking for a second odd family later in the boot.
//
// Which errno number the refusal carries is not a detail. The guest reads errno
// through bionic's __errno, which this port binds to the host's own slot
// (bionic_shims.cpp: bionicErrno() returns &errno), so the number written here
// is the number the guest compares. The guest compares bionic/Linux values, not
// this toolchain's: at guest 0x12E5264 it tests errno against 0xb (11, EAGAIN)
// and 0x73 (115, EINPROGRESS). devkitA64's newlib happens to agree on 11 but
// numbers EINPROGRESS 119, and it numbers EAFNOSUPPORT 106 -- which is bionic's
// EISCONN, "socket is already connected". Writing the macro would hand the
// guest a connected-socket error for a refused family, and the guest's own
// recovery logic would then be steering on a fiction. The bionic value is
// therefore spelled out rather than taken from <errno.h>.
//
// Measured with the refusal on, census off, graphics backend null, 120 s
// (run 20261003-131325-dom518-refuse, against 20261003-124323-census-off):
//   the refusal lands (fd=-1 errno=97 policy=refuse-family), the guest absorbs
//   it -- its two binds and its getsockname all come back EBADF -- and Eden's
//   three asserts for this socket are gone (domain=518, errno=10014, errno=17:
//   5 asserts down to the 2 unrelated protocol=0 rows the loopback TCP pair
//   still raises). The run is otherwise byte-identical: the ordered phase-name
//   signature matches the baseline on all 499 rows bar the one extra sock-bind
//   the guest's port-0 retry makes, view-render-ret and gfx-stats are still
//   absent, and the log still stops growing while Eden CPU keeps climbing.
//   So the raw-socket refusal is correct and is not what is holding the guest
//   at ccsn.bin.
constexpr int kBionicEafnosupport = 97;

bool familyCanBeServiced(int domain) {
    return domain == AF_UNIX || domain == AF_INET || domain == AF_INET6;
}

int censusSocket(int domain, int type, int protocol) {
    CENSUS_IMPORT("socket");
    const bool servicable = familyCanBeServiced(domain);
    int result;
    int failure;
    if (!servicable) {
        // Refused here rather than one layer down. libnx's socket() forwards
        // the domain straight to the Horizon bsd service, which has no case for
        // it: Eden answers "assert Unimplemented domain=518" from
        // sockets_translate.cpp:141 and then errno=10014 from network.cpp:163,
        // and still returns a descriptor, so the guest walks on believing it
        // has a socket. Refusing at this layer means Eden is never asked, and
        // the guest gets the -1 a real kernel would give.
        result = -1;
        failure = errno = kBionicEafnosupport;
    } else {
        result = ::socket(domain, type, protocol);
        failure = errno;
    }
    FdRecord* record = fdFind(result, true);
    if (record != nullptr && result >= 0) {
        record->domain = domain;
        record->type = type;
        record->protocol = protocol;
        record->role = "new";
    }
    char detail[192];
    __builtin_snprintf(detail, sizeof(detail),
                       "domain=%d type=%d proto=%d fd=%d errno=%d policy=%s",
                       domain, type, protocol, result, failure,
                       servicable ? "passthrough" : "refuse-family");
    socketLogEvent("sock-socket", result, detail);
    return result;
}

int censusSocketpair(int domain, int type, int protocol, int pair[2]) {
    CENSUS_IMPORT("socketpair");
    const int result = ::socketpair(domain, type, protocol, pair);
    for (int index = 0; index < 2; ++index) {
        FdRecord* record = fdFind(pair != nullptr ? pair[index] : -1, true);
        if (record != nullptr && result == 0) {
            record->domain = domain;
            record->type = type;
            record->protocol = protocol;
            record->role = "pair";
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// poll() census tap.
//
// The burner is pinned to one guest thread: ordinal=4, guest entry 0x12E15E8,
// 75.8M host imports in 120 s with idle_ms=0, alternating between exactly two
// return addresses, holding one guest mutex the whole time (acquire_site
// 0x12E16C0). The guest function there is FUN_013E1600 -- a poll() loop over
// four descriptors. poll was the one host call in that loop with no census row
// at all (it was bound straight through), so the three facts that decide the
// whole question were unrecorded: which fds it watches, what timeout it asks
// for, and which fd answers ready every single time.
//
// This row records them and decides nothing. The call is forwarded verbatim;
// what is new is the counting, and the counting is bounded:
//   - the first kPollTraceCalls calls are logged one line each, which is where
//     nfds, the fd numbers, the requested events, the per-fd revents and the
//     timeout are on record verbatim. The first kPollTracePerCaller calls of
//     every newly seen calling site are logged too, so a site that appears late
//     cannot be the invisible one.
//   - after that, one aggregate line per calling site, on a stride that grows
//     with each round so the rounds spread across a whole run instead of
//     bunching into its first seconds. Capped at kPollSummaryLines rounds.
//   - steady-state cost is one 4-entry pointer scan plus one relaxed atomic add
//     per descriptor. No lock, no syscall, no allocation, nothing the guest can
//     block on.
//
// Bounded is not optional. appendGuestBootLine() is one fopen + fsync + fclose,
// and the loop under observation makes ~330k calls/s: a per-call log would spend
// its time in the SD card and would move the very number it is measuring.
//
// Aggregation is per calling site, keyed on the guest return address, because
// the guest has more than one poll site (libRealRacing3 imports both poll and
// select; libfmodex and libcrashlytics import poll and epoll_wait). A single
// global count would average the burner together with sites that block
// correctly, which is exactly the confusion this tap exists to remove.
// ---------------------------------------------------------------------------
constexpr unsigned kPollTraceCalls = 20;
constexpr unsigned kPollTracePerCaller = 2;
constexpr unsigned kPollTraceTotal = 48;
constexpr unsigned long long kPollSummaryStride = 250000;
constexpr unsigned kPollSummaryLines = 24;
constexpr size_t kPollCallerSlots = 4;
constexpr size_t kPollFdsPerSlot = 8;

struct PollCaller {
    bool used{false};
    const void* returnAddress{nullptr};
    unsigned long long polls{0};
    unsigned long long ready{0};
    unsigned long long blocked{0};
    unsigned long long errored{0};
    int timeoutMin{0};
    int timeoutMax{0};
    size_t fdCount{0};
    int fd[kPollFdsPerSlot]{};
    short events[kPollFdsPerSlot]{};
    short revents[kPollFdsPerSlot]{};
    unsigned int reventsSeen[kPollFdsPerSlot]{};
    unsigned long long polled[kPollFdsPerSlot]{};
    unsigned long long fdReady[kPollFdsPerSlot]{};
};

PollCaller gPollCallers[kPollCallerSlots];
unsigned long long gPollCalls = 0;
unsigned gPollTraces = 0;
unsigned gPollSummaryRounds = 0;
unsigned long long gPollNextSummary = 0;

PollCaller* pollCallerFor(const void* returnAddress) {
    for (PollCaller& slot : gPollCallers) {
        if (slot.used && slot.returnAddress == returnAddress) {
            return &slot;
        }
    }
    for (PollCaller& slot : gPollCallers) {
        if (!slot.used) {
            slot = PollCaller{};
            slot.used = true;
            slot.returnAddress = returnAddress;
            return &slot;
        }
    }
    return nullptr;
}

// "21/tl-inet:3883(ev1 rv0x1 rd7500000/7500000)" -- the fd as the fd table knows
// it, the events the guest asked for, the revents the host reported, and how
// often that fd was ready out of how often it was polled. Only ever called from
// the two log paths, never from the steady state.
void appendPollFdText(PollCaller& slot, size_t index, char* out, size_t capacity) {
    const int written = __builtin_snprintf(
        out, capacity, "%d/", slot.fd[index]);
    if (written < 0 || static_cast<size_t>(written) >= capacity) {
        return;
    }
    char identity[64];
    describeFd(slot.fd[index], identity, sizeof(identity));
    size_t used = static_cast<size_t>(written);
    used += static_cast<size_t>(__builtin_snprintf(
        out + used, capacity - used, "%s/ev%hx rv%hx rd%llu/%llu", identity,
        static_cast<unsigned>(slot.events[index] & 0xffff),
        static_cast<unsigned>(slot.revents[index] & 0xffff),
        slot.fdReady[index], slot.polled[index]));
    (void)used;
}

void censusPollTrace(PollCaller& slot, nfds_t nfds, int timeout, int result,
                     int failure) {
    // 256 so socketLogEvent's own 320-byte buffer cannot be overrun: the two
    // together are what gcc's format-truncation check compares.
    char line[256];
    int used = __builtin_snprintf(line, sizeof(line),
                                 "ra=%llx nfds=%u tmo=%d ret=%d errno=%d fds=",
                                 (unsigned long long)reinterpret_cast<uintptr_t>(
                                     slot.returnAddress),
                                 static_cast<unsigned>(nfds), timeout, result,
                                 failure);
    for (size_t index = 0;
         index < slot.fdCount && used > 0 &&
         static_cast<size_t>(used) < sizeof(line) - 1;
         ++index) {
        char text[96];
        appendPollFdText(slot, index, text, sizeof(text));
        used += __builtin_snprintf(line + used, sizeof(line) - used, "%s%s",
                                   index != 0 ? "," : "", text);
    }
    socketLogEvent("sock-poll", -1, line);
}

void censusPollSummary(PollCaller& slot) {
    char line[256];
    int used = __builtin_snprintf(
        line, sizeof(line),
        "ra=%llx polls=%llu ready=%llu blocked=%llu err=%llu tmo=%d/%d nfd=%u fds=",
        (unsigned long long)reinterpret_cast<uintptr_t>(slot.returnAddress),
        slot.polls, slot.ready, slot.blocked, slot.errored, slot.timeoutMin,
        slot.timeoutMax, static_cast<unsigned>(slot.fdCount));
    for (size_t index = 0;
         index < slot.fdCount && used > 0 &&
         static_cast<size_t>(used) < sizeof(line) - 1;
         ++index) {
        char text[96];
        appendPollFdText(slot, index, text, sizeof(text));
        used += __builtin_snprintf(line + used, sizeof(line) - used, "%s%s",
                                   index != 0 ? "," : "", text);
    }
    socketLogEvent("sock-poll-sum", -1, line);
}

// One-shot per-descriptor bisect, used only when the poll itself failed.
//
// "poll returned -1" and "fd 0 is not a socket" are different claims, and only
// the second one says which descriptor to fix. So when a call fails, each
// descriptor is polled ALONE, once, with a zero timeout, and its own result and
// errno are logged. Twelve extra host calls in the life of the process, all of
// them before the answer is known; there is no per-call cost and no lock. The
// fd table does the naming, so a row reads as a fact, not a number.
//
// errno is cleared before each single poll so a successful poll is
// distinguishable from a failed one that happened to leave errno at zero.
constexpr unsigned kPollBisectCalls = 3;
unsigned gPollBisects = 0;

void censusPollBisect(const PollCaller& slot, int failure) {
    ++gPollBisects;
    char line[256];
    int used = __builtin_snprintf(line, sizeof(line), "call=%u nfds=%u err=%d ",
                                 gPollBisects,
                                 static_cast<unsigned>(slot.fdCount), failure);
    for (size_t index = 0;
         index < slot.fdCount && used > 0 &&
         static_cast<size_t>(used) < sizeof(line) - 1;
         ++index) {
        struct pollfd single;
        single.fd = slot.fd[index];
        single.events = slot.events[index];
        single.revents = 0;
        errno = 0;
        const int one = ::poll(&single, 1, 0);
        char identity[64];
        describeFd(slot.fd[index], identity, sizeof(identity));
        used += __builtin_snprintf(line + used, sizeof(line) - used, "%s%s:r%d/e%d/rv%hx ",
                                   index != 0 ? "," : "", identity, one, errno,
                                   static_cast<unsigned>(single.revents & 0xffff));
    }
    socketLogEvent("sock-poll-bisect", -1, line);
}

int censusPoll(struct pollfd* fds, nfds_t nfds, int timeout) {
    CENSUS_IMPORT("poll");
    const void* caller = __builtin_return_address(0);
    PollCaller* slot = pollCallerFor(caller);
    const size_t before = slot != nullptr ? slot->polls : 0;

    // Linux poll() conformance: hand the host only the entries it would have
    // been given on a real device.
    //
    // Measured, this is the entire wedge. The guest's set is four entries and
    // one of them is a zero-filled slot: fd 0, events 0, revents 0. Polled on
    // its own that slot is the one descriptor the host refuses -- poll(&{0,0},1,0)
    // is -1/ENOTSOCK -- and because the host validates the whole array up front
    // it fails the entire call, 29,750,021 times in 112 s, with the timeout the
    // guest asked for (tmo=-1, i.e. block forever) never being honoured. The
    // guest's loop is lock -> poll(-1) -> unlock at ~265k/s, which is the burner.
    //
    // Linux does not do that. do_poll() skips any entry with fd < 0 or
    // events == 0 before it validates anything, and leaves that entry's revents
    // at 0, so an unfilled slot is invisible: the call sleeps on the three real
    // descriptors. The other three measure clean on their own (r0, no error), so
    // with the slot skipped the guest's poll blocks exactly as it does on a
    // device. Nothing else about the call changes -- same timeout, same
    // descriptors, same revents for every descriptor the host does service.
    //
    // The pass-through branch below is untouched: when every entry is a real
    // watched descriptor (the ordinary case, and every poll this port made
    // before the guest started using an unfilled slot) the array goes to the
    // host verbatim, exactly as RR3_BIND(poll) used to hand it over.
    constexpr bool pollSkipsUnwatchedEntries = true;

    struct pollfd filtered[kPollFdsPerSlot];
    nfds_t filteredCount = 0;
    bool dropped = false;
    if (pollSkipsUnwatchedEntries && fds != nullptr && nfds != 0 &&
        nfds <= kPollFdsPerSlot) {
        for (nfds_t index = 0; index < nfds; ++index) {
            if (fds[index].fd < 0 || fds[index].events == 0) {
                // Linux leaves a skipped entry's revents at 0, so the guest
                // never sees a stale value from a previous call.
                fds[index].revents = 0;
                dropped = true;
                continue;
            }
            filtered[filteredCount++] = fds[index];
        }
    }

    const int result = dropped ? ::poll(filtered, filteredCount, timeout)
                               : ::poll(fds, nfds, timeout);
    const int failure = errno;
    if (dropped) {
        // Scatter the host's answer back onto the guest's array, in order. A
        // kept entry beyond the filtered copy's length cannot happen (filtered
        // is sized for the whole array), but the guard is there so a future
        // change to the bound degrades to revents=0 rather than to a read past
        // the end.
        nfds_t kept = 0;
        for (nfds_t index = 0; index < nfds; ++index) {
            if (fds[index].fd < 0 || fds[index].events == 0) {
                continue;
            }
            fds[index].revents = kept < filteredCount ? filtered[kept].revents : 0;
            ++kept;
        }
    }
    if (slot == nullptr) {
        return result;
    }
    const bool trace =
        gPollTraces < kPollTraceTotal &&
        (gPollCalls < kPollTraceCalls || before < kPollTracePerCaller);
    if (slot->polls == 0) {
        slot->timeoutMin = timeout;
        slot->timeoutMax = timeout;
    } else {
        if (timeout < slot->timeoutMin) {
            slot->timeoutMin = timeout;
        }
        if (timeout > slot->timeoutMax) {
            slot->timeoutMax = timeout;
        }
    }
    ++slot->polls;
    if (result > 0) {
        ++slot->ready;
    } else if (result == 0) {
        ++slot->blocked;
    } else {
        ++slot->errored;
    }
    if (fds != nullptr) {
        const size_t watched = nfds < kPollFdsPerSlot ? nfds : kPollFdsPerSlot;
        slot->fdCount = watched;
        for (size_t index = 0; index < watched; ++index) {
            const int fd = fds[index].fd;
            if (slot->fd[index] != fd) {
                slot->fd[index] = fd;
                slot->polled[index] = 0;
                slot->fdReady[index] = 0;
                slot->reventsSeen[index] = 0;
            }
            slot->events[index] = fds[index].events;
            slot->revents[index] = fds[index].revents;
            slot->reventsSeen[index] |=
                static_cast<unsigned int>(fds[index].revents & 0xffff);
            __atomic_fetch_add(&slot->polled[index], 1ULL, __ATOMIC_RELAXED);
            if (fds[index].revents != 0) {
                __atomic_fetch_add(&slot->fdReady[index], 1ULL, __ATOMIC_RELAXED);
            }
        }
    }
    ++gPollCalls;
    if (result < 0 && gPollBisects < kPollBisectCalls && slot->fdCount != 0) {
        censusPollBisect(*slot, failure);
    }
    if (trace) {
        ++gPollTraces;
        censusPollTrace(*slot, nfds, timeout, result, failure);
        return result;
    }
    // The stride grows by one stride per round, so round N lands at roughly
    // stride*N*(N+1)/2 polls: with 24 rounds that spreads the summaries over
    // the whole run rather than the first twenty seconds of it.
    if (gPollCalls >= gPollNextSummary && gPollSummaryRounds < kPollSummaryLines) {
        ++gPollSummaryRounds;
        gPollNextSummary = gPollCalls + kPollSummaryStride * (gPollSummaryRounds + 1);
        for (PollCaller& other : gPollCallers) {
            if (other.used) {
                censusPollSummary(other);
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Allocator census taps: NOT APPLIED, blocked by a contract test.
//
// The task asked for taps on the malloc/free pair. They cannot be added without
// breaking test_graphics_isolation_contract.py, which asserts the literal
// source text 'RR3_SHIM("malloc", guestViewTracedMalloc)' and
// 'RR3_SHIM("free", guestViewTracedFree)' in this file (lines 113 and 115).
// That test is not ours to edit, and 11/11 is a hard acceptance criterion, so
// the rows stay exactly as they were.
//
// For the record, the wrappers below are the ones that would be used; they
// compose with the traced shim rather than replacing it, so both observers stay
// live. They are commented out rather than deleted so the next round can
// enable them the moment the assertion is relaxed to check that
// guestViewTracedMalloc/guestViewTracedFree are still reached from the table
// (which a wrapper preserves) rather than the exact table text (which it does
// not).
//
// void* censusMalloc(size_t size) {
//     CENSUS_IMPORT("malloc");
//     return guestViewTracedMalloc(size);
// }
//
// void censusFree(void* allocation) {
//     CENSUS_IMPORT("free");
//     guestViewTracedFree(allocation);
// }

const HostSymbol kHostSymbols[] = {
    RR3_SHIM("abort", guestTracedAbort),
    RR3_SHIM("access", guestAccess),
    RR3_BIND(acos),
    RR3_BIND(acosf),
    RR3_BIND(asinf),
    RR3_BIND(atan2),
    RR3_BIND(atan2f),
    RR3_BIND(atanf),
    RR3_BIND(atof),
    RR3_BIND(atoi),
    RR3_BIND(atol),
    RR3_BIND(calloc),
    RR3_BIND(clearerr),
    RR3_SHIM("close", censusClose),
    RR3_BIND(cos),
    RR3_BIND(cosf),
    RR3_BIND(difftime),
    RR3_SHIM("exit", guestTracedExit),
    RR3_BIND(exp2f),
    RR3_BIND(expf),
    RR3_SHIM("fclose", censusFclose),
    RR3_BIND(fcntl),
    RR3_BIND(fdopen),
    RR3_BIND(feof),
    RR3_BIND(ferror),
    RR3_SHIM("fgets", censusFgets),
    RR3_BIND(fileno),
    RR3_BIND(fmod),
    RR3_BIND(fmodf),
    RR3_SHIM("fopen", censusFopen),
    RR3_BIND(fputs),
    RR3_SHIM("fread", censusFread),
    RR3_SHIM("free", guestViewTracedFree),
    RR3_BIND(frexp),
    RR3_BIND(fscanf),
    RR3_SHIM("fseek", censusFseek),
    RR3_BIND(fseeko),
    RR3_SHIM("fstat", censusFstat),
    RR3_SHIM("ftell", censusFtell),
    RR3_BIND(ftello),
    RR3_BIND(getenv),
    RR3_BIND(gethostname),
    RR3_BIND(getpid),
    RR3_BIND(gettimeofday),
    RR3_BIND(gmtime),
    RR3_BIND(gmtime_r),
    RR3_BIND(ioctl),
    RR3_BIND(ldexp),
    RR3_BIND(localtime),
    RR3_BIND(localtime_r),
    RR3_BIND(log),
    RR3_BIND(log10f),
    RR3_BIND(logf),
    RR3_SHIM("malloc", guestViewTracedMalloc),
    RR3_BIND(memalign),
    RR3_BIND(memchr),
    RR3_BIND(memcmp),
    RR3_SHIM("memcpy", guestViewTracedMemcpy),
    RR3_BIND(memmove),
    RR3_SHIM("memset", guestViewTracedMemset),
    RR3_BIND(mktime),
    RR3_BIND(modf),
    RR3_BIND(modff),
    RR3_BIND(nextafterf),
    RR3_SHIM("open", guestOpen),
    RR3_BIND(perror),
    RR3_BIND(pow),
    RR3_BIND(powf),
    RR3_BIND(printf),
    RR3_BIND(puts),
    RR3_BIND(qsort),
    RR3_SHIM("raise", guestTracedRaise),
    RR3_BIND(rand),
    RR3_SHIM("read", censusRead),
    RR3_BIND(realloc),
    RR3_SHIM("remove", guestRemove),
    RR3_SHIM("rename", guestRename),
    RR3_BIND(rewind),
    RR3_BIND(signal),
    RR3_BIND(sinf),
    RR3_BIND(snprintf),
    RR3_BIND(sprintf),
    RR3_BIND(srand),
    RR3_BIND(sscanf),
    RR3_SHIM("stat", guestStat),
    RR3_BIND(strcasecmp),
    RR3_BIND(strcat),
    RR3_BIND(strchr),
    RR3_BIND(strcmp),
    RR3_BIND(strcpy),
    RR3_BIND(strcspn),
    RR3_BIND(strdup),
    RR3_BIND(strerror),
    RR3_BIND(strftime),
    RR3_BIND(strlen),
    RR3_BIND(strncmp),
    RR3_BIND(strncpy),
    RR3_BIND(strnlen),
    RR3_BIND(strpbrk),
    RR3_BIND(strrchr),
    RR3_BIND(strspn),
    RR3_BIND(strstr),
    RR3_BIND(strtod),
    RR3_BIND(strtof),
    RR3_BIND(strtok),
    RR3_BIND(strtok_r),
    RR3_BIND(strtol),
    RR3_BIND(strtoll),
    RR3_BIND(strtoul),
    RR3_BIND(strtoull),
    RR3_BIND(tanf),
    RR3_BIND(time),
    RR3_SHIM("unlink", guestUnlink),
    RR3_BIND(vprintf),
    RR3_BIND(vsnprintf),
    RR3_SHIM("write", censusWrite),
    RR3_BIND(eglGetDisplay),
    RR3_BIND(eglGetProcAddress),
    RR3_BIND(eglQueryString),
    RR3_BIND(glActiveTexture),
    RR3_BIND(glAttachShader),
    RR3_BIND(glBindAttribLocation),
    RR3_SHIM("glBindBuffer", guestViewTracedGlBindBuffer),
    RR3_SHIM("glBindFramebuffer", guestViewTracedGlBindFramebuffer),
    RR3_BIND(glBindRenderbuffer),
    RR3_SHIM("glBindTexture", guestViewTracedGlBindTexture),
    RR3_BIND(glBlendColor),
    RR3_BIND(glBlendEquation),
    RR3_BIND(glBlendEquationSeparate),
    RR3_BIND(glBlendFunc),
    RR3_BIND(glBlendFuncSeparate),
    RR3_BIND(glBufferData),
    RR3_BIND(glBufferSubData),
    RR3_SHIM("glCheckFramebufferStatus", guestViewTracedGlCheckFramebufferStatus),
    RR3_SHIM("glClear", guestViewTracedGlClear),
    RR3_BIND(glClearColor),
    RR3_BIND(glClearDepthf),
    RR3_BIND(glClearStencil),
    RR3_BIND(glColorMask),
    RR3_SHIM("glCompileShader", guestViewTracedGlCompileShader),
    RR3_SHIM("glCompressedTexImage2D", guestViewTracedGlCompressedTexImage2D),
    RR3_BIND(glCompressedTexSubImage2D),
    RR3_BIND(glCopyTexImage2D),
    RR3_BIND(glCopyTexSubImage2D),
    RR3_BIND(glCreateProgram),
    RR3_SHIM("glCreateShader", guestViewTracedGlCreateShader),
    RR3_BIND(glCullFace),
    RR3_BIND(glDeleteBuffers),
    RR3_BIND(glDeleteFramebuffers),
    RR3_BIND(glDeleteProgram),
    RR3_BIND(glDeleteRenderbuffers),
    RR3_BIND(glDeleteShader),
    RR3_BIND(glDeleteTextures),
    RR3_BIND(glDepthFunc),
    RR3_BIND(glDepthMask),
    RR3_BIND(glDepthRangef),
    RR3_BIND(glDetachShader),
    RR3_BIND(glDisable),
    RR3_BIND(glDisableVertexAttribArray),
    RR3_SHIM("glDrawArrays", guestViewTracedGlDrawArrays),
    RR3_SHIM("glDrawElements", guestViewTracedGlDrawElements),
    RR3_BIND(glEnable),
    RR3_BIND(glEnableVertexAttribArray),
    RR3_BIND(glFinish),
    RR3_BIND(glFlush),
    RR3_BIND(glFramebufferRenderbuffer),
    RR3_SHIM("glFramebufferTexture2D", guestViewTracedGlFramebufferTexture2D),
    RR3_BIND(glFrontFace),
    RR3_SHIM("glGenBuffers", guestViewTracedGlGenBuffers),
    RR3_BIND(glGenerateMipmap),
    RR3_SHIM("glGenFramebuffers", guestViewTracedGlGenFramebuffers),
    RR3_BIND(glGenRenderbuffers),
    RR3_SHIM("glGenTextures", guestViewTracedGlGenTextures),
    RR3_BIND(glGetActiveAttrib),
    RR3_BIND(glGetActiveUniform),
    RR3_BIND(glGetAttachedShaders),
    RR3_BIND(glGetAttribLocation),
    RR3_BIND(glGetBooleanv),
    RR3_BIND(glGetBufferParameteriv),
    RR3_BIND(glGetError),
    RR3_BIND(glGetFloatv),
    RR3_BIND(glGetFramebufferAttachmentParameteriv),
    RR3_BIND(glGetIntegerv),
    RR3_BIND(glGetProgramInfoLog),
    RR3_BIND(glGetProgramiv),
    RR3_BIND(glGetRenderbufferParameteriv),
    RR3_BIND(glGetShaderInfoLog),
    RR3_BIND(glGetShaderiv),
    RR3_BIND(glGetShaderPrecisionFormat),
    RR3_BIND(glGetShaderSource),
    RR3_SHIM("glGetString", guestViewTracedGlGetString),
    RR3_BIND(glGetTexParameterfv),
    RR3_BIND(glGetTexParameteriv),
    RR3_BIND(glGetUniformfv),
    RR3_BIND(glGetUniformiv),
    RR3_BIND(glGetUniformLocation),
    RR3_BIND(glGetVertexAttribfv),
    RR3_BIND(glGetVertexAttribiv),
    RR3_BIND(glGetVertexAttribPointerv),
    RR3_BIND(glHint),
    RR3_BIND(glIsBuffer),
    RR3_BIND(glIsEnabled),
    RR3_BIND(glIsFramebuffer),
    RR3_BIND(glIsProgram),
    RR3_BIND(glIsRenderbuffer),
    RR3_BIND(glIsShader),
    RR3_BIND(glIsTexture),
    RR3_BIND(glLineWidth),
    RR3_SHIM("glLinkProgram", guestViewTracedGlLinkProgram),
    RR3_BIND(glPixelStorei),
    RR3_BIND(glPolygonOffset),
    RR3_BIND(glReadPixels),
    RR3_BIND(glReleaseShaderCompiler),
    RR3_BIND(glRenderbufferStorage),
    RR3_BIND(glSampleCoverage),
    RR3_SHIM("glScissor", guestViewTracedGlScissor),
    RR3_SHIM("glShaderBinary", guestViewTracedGlShaderBinary),
    RR3_SHIM("glShaderSource", guestViewTracedGlShaderSource),
    RR3_BIND(glStencilFunc),
    RR3_BIND(glStencilFuncSeparate),
    RR3_BIND(glStencilMask),
    RR3_BIND(glStencilMaskSeparate),
    RR3_BIND(glStencilOp),
    RR3_BIND(glStencilOpSeparate),
    RR3_SHIM("glTexImage2D", guestViewTracedGlTexImage2D),
    RR3_BIND(glTexParameterf),
    RR3_BIND(glTexParameterfv),
    RR3_BIND(glTexParameteri),
    RR3_BIND(glTexParameteriv),
    RR3_BIND(glTexSubImage2D),
    RR3_BIND(glUniform1f),
    RR3_BIND(glUniform1fv),
    RR3_BIND(glUniform1i),
    RR3_BIND(glUniform1iv),
    RR3_BIND(glUniform2f),
    RR3_BIND(glUniform2fv),
    RR3_BIND(glUniform2i),
    RR3_BIND(glUniform2iv),
    RR3_BIND(glUniform3f),
    RR3_BIND(glUniform3fv),
    RR3_BIND(glUniform3i),
    RR3_BIND(glUniform3iv),
    RR3_BIND(glUniform4f),
    RR3_BIND(glUniform4fv),
    RR3_BIND(glUniform4i),
    RR3_BIND(glUniform4iv),
    RR3_BIND(glUniformMatrix2fv),
    RR3_BIND(glUniformMatrix3fv),
    RR3_BIND(glUniformMatrix4fv),
    RR3_SHIM("glUseProgram", guestViewTracedGlUseProgram),
    RR3_BIND(glValidateProgram),
    RR3_BIND(glVertexAttrib1f),
    RR3_BIND(glVertexAttrib1fv),
    RR3_BIND(glVertexAttrib2f),
    RR3_BIND(glVertexAttrib2fv),
    RR3_BIND(glVertexAttrib3f),
    RR3_BIND(glVertexAttrib3fv),
    RR3_BIND(glVertexAttrib4f),
    RR3_BIND(glVertexAttrib4fv),
    RR3_SHIM("glVertexAttribPointer", guestViewTracedGlVertexAttribPointer),
    RR3_SHIM("glViewport", guestViewTracedGlViewport),
    RR3_SHIM("accept", censusAccept),
    RR3_SHIM("bind", censusBind),
    RR3_BIND(chmod),
    RR3_SHIM("connect", censusConnect),
    RR3_BIND(dup2),
    RR3_BIND(exp2),
    RR3_SHIM("freeaddrinfo", censusFreeaddrinfo),
    RR3_SHIM("fsync", censusFsync),
    RR3_BIND(gai_strerror),
    RR3_SHIM("getaddrinfo", censusGetaddrinfo),
    RR3_SHIM("gethostbyname", censusGethostbyname),
    RR3_BIND(getnameinfo),
    RR3_BIND(getpeername),
    RR3_SHIM("getsockname", censusGetsockname),
    RR3_SHIM("getsockopt", censusGetsockopt),
    RR3_SHIM("inet_addr", censusInetAddr),
    RR3_SHIM("inet_ntop", censusInetNtop),
    RR3_SHIM("inet_pton", censusInetPton),
    RR3_BIND(isalnum),
    RR3_BIND(isspace),
    RR3_BIND(isupper),
    RR3_BIND(isxdigit),
    RR3_SHIM("listen", censusListen),
    RR3_SHIM("lseek", censusLseek),
    RR3_BIND(memrchr),
    RR3_SHIM("poll", censusPoll),
    RR3_SHIM("recv", censusRecv),
    RR3_SHIM("recvfrom", censusRecvfrom),
    RR3_SHIM("recvmmsg", censusRecvmmsg),
    RR3_BIND(rmdir),
    RR3_BIND(select),
    RR3_SHIM("send", censusSend),
    RR3_SHIM("sendmmsg", censusSendmmsg),
    RR3_SHIM("sendto", censusSendto),
    RR3_SHIM("setsockopt", censusSetsockopt),
    RR3_BIND(shutdown),
    RR3_BIND(sin),
    RR3_BIND(sincosf),
    RR3_BIND(sleep),
    RR3_SHIM("socket", censusSocket),
    RR3_SHIM("socketpair", censusSocketpair),
    RR3_BIND(strncasecmp),
    RR3_BIND(tolower),
    RR3_BIND(toupper),
    RR3_BIND(usleep),
    RR3_BIND(utimes),
    RR3_BIND(btowc),
    RR3_BIND(chdir),
    RR3_BIND(clock_gettime),
    RR3_SHIM("closedir", censusClosedir),
    RR3_BIND(fchmod),
    RR3_BIND(freelocale),
    RR3_SHIM("ftruncate", censusFtruncate),
    RR3_BIND(getcwd),
    RR3_BIND(isdigit_l),
    RR3_BIND(islower_l),
    RR3_BIND(isupper_l),
    RR3_BIND(iswalpha_l),
    RR3_BIND(iswblank_l),
    RR3_BIND(iswcntrl_l),
    RR3_BIND(iswdigit_l),
    RR3_BIND(iswlower_l),
    RR3_BIND(iswprint_l),
    RR3_BIND(iswpunct_l),
    RR3_BIND(iswspace_l),
    RR3_BIND(iswupper_l),
    RR3_BIND(iswxdigit_l),
    RR3_BIND(isxdigit_l),
    RR3_SHIM("link", guestLink),
    RR3_BIND(localeconv),
    RR3_SHIM("lstat", guestLstat),
    RR3_BIND(mbrlen),
    RR3_BIND(mbrtowc),
    RR3_BIND(mbsnrtowcs),
    RR3_BIND(mbsrtowcs),
    RR3_BIND(mbtowc),
    RR3_SHIM("mkdir", guestMkdir),
    RR3_BIND(nanosleep),
    RR3_BIND(newlocale),
    RR3_SHIM("opendir", guestOpendir),
    RR3_SHIM("pathconf", guestPathconf),
    RR3_SHIM("readdir", guestReaddir),
    RR3_SHIM("readlink", guestReadlink),
    RR3_SHIM("realpath", guestRealpath),
    RR3_BIND(sched_yield),
    RR3_BIND(setlocale),
    RR3_SHIM("statvfs", guestStatvfs),
    RR3_BIND(strcoll_l),
    RR3_BIND(strerror_r),
    RR3_BIND(strftime_l),
    RR3_BIND(strtold),
    RR3_BIND(strtold_l),
    RR3_BIND(strtoll_l),
    RR3_BIND(strtoull_l),
    RR3_BIND(strxfrm_l),
    RR3_BIND(swprintf),
    RR3_SHIM("symlink", guestSymlink),
    RR3_BIND(tolower_l),
    RR3_BIND(toupper_l),
    RR3_BIND(towlower_l),
    RR3_BIND(towupper_l),
    RR3_BIND(truncate),
    RR3_BIND(uselocale),
    RR3_BIND(vasprintf),
    RR3_BIND(vsscanf),
    RR3_BIND(wcrtomb),
    RR3_BIND(wcscoll_l),
    RR3_BIND(wcslen),
    RR3_BIND(wcsnrtombs),
    RR3_BIND(wcstod),
    RR3_BIND(wcstof),
    RR3_BIND(wcstol),
    RR3_BIND(wcstold),
    RR3_BIND(wcstoll),
    RR3_BIND(wcstoul),
    RR3_BIND(wcstoull),
    RR3_BIND(wcsxfrm_l),
    RR3_BIND(wctob),
    RR3_BIND(wmemchr),
    RR3_BIND(wmemcmp),
    RR3_BIND(wmemcpy),
    RR3_BIND(wmemmove),
    RR3_BIND(wmemset),
    RR3_SHIM("__android_log_print", androidLogPrint),
    RR3_SHIM("__android_log_vprint", androidLogVPrint),
    RR3_SHIM("__android_log_write", androidLogWrite),
    // FMOD Android audio: silent OpenSL device / clear AAudio errors. Never
    // claims audible output (fmodAudioStubStats.audible is always 0).
    RR3_SHIM("slCreateEngine", slCreateEngine),
    RR3_SHIM("AAudioStreamBuilder_create", aaudioCreateStreamBuilder),
    RR3_SHIM("AAudio_createStreamBuilder", aaudioCreateStreamBuilder),
    RR3_SHIM("AAudioStreamBuilder_openStream", aaudioStreamBuilderOpenStream),
    RR3_SHIM("AAudioStreamBuilder_delete", aaudioStreamBuilderDelete),
    RR3_SHIM("AAudioStream_requestStart", aaudioStream_requestStart),
    RR3_SHIM("AAudioStream_requestStop", aaudioStream_requestStop),
    RR3_SHIM("AAudioStream_close", aaudioStream_close),
    RR3_SHIM("AAudioStream_write", aaudioStream_write),
    RR3_SHIM("AAudioStream_getState", aaudioStream_getState),
    RR3_SHIM("SL_IID_ENGINE", SL_IID_ENGINE),
    RR3_SHIM("SL_IID_PLAY", SL_IID_PLAY),
    RR3_SHIM("SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE),
    RR3_SHIM("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE),
    // FMOD Ex object model. The guest imports 52 FMOD symbols (51 Itanium-
    // mangled members plus FMOD_System_Create); every one is implemented in
    // source/fmod_shim.cpp with real, non-null objects whose DSP node lists are
    // self-linked sentinels, so the engine can never walk a null list head.
    // Rows generated by .build/fmod/gen_rows.py from the objdump -T import list
    // and checked by .build/fmod/verify_fmod_symbols.py against the built ELF.
    RR3_SHIM("FMOD_System_Create", ::FMOD_System_Create),
    RR3_SHIM("_ZN4FMOD12ChannelGroup6addDSPEPNS_3DSPEPPNS_13DSPConnectionE", ::FMOD::fmodChannelGroupAddDSP),
    RR3_SHIM("_ZN4FMOD12ChannelGroup8addGroupEPS0_", ::FMOD::fmodChannelGroupAddGroup),
    RR3_SHIM("_ZN4FMOD12ChannelGroup9setVolumeEf", ::FMOD::fmodChannelGroupSetVolume),
    RR3_SHIM("_ZN4FMOD3DSP11getUserDataEPPv", ::FMOD::fmodDSPGetUserData),
    RR3_SHIM("_ZN4FMOD3DSP12setParameterEif", ::FMOD::fmodDSPSetParameter),
    RR3_SHIM("_ZN4FMOD3DSP15getSystemObjectEPPNS_6SystemE", ::FMOD::fmodDSPGetSystemObject),
    RR3_SHIM("_ZN4FMOD3DSP7releaseEv", ::FMOD::fmodDSPRelease),
    RR3_SHIM("_ZN4FMOD5Sound11getUserDataEPPv", ::FMOD::fmodSoundGetUserData),
    RR3_SHIM("_ZN4FMOD5Sound11setUserDataEPv", ::FMOD::fmodSoundSetUserData),
    RR3_SHIM("_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_", ::FMOD::fmodSoundGetOpenState),
    RR3_SHIM("_ZN4FMOD5Sound7releaseEv", ::FMOD::fmodSoundRelease),
    RR3_SHIM("_ZN4FMOD5Sound9getLengthEPjj", ::FMOD::fmodSoundGetLength),
    RR3_SHIM("_ZN4FMOD6System10getVersionEPj", ::FMOD::fmodSystemGetVersion),
    RR3_SHIM("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", ::FMOD::fmodSystemCreateSound),
    RR3_SHIM("_ZN4FMOD6System11getDSPClockEPjS1_", ::FMOD::fmodSystemGetDSPClock),
    RR3_SHIM("_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", ::FMOD::fmodSystemCreateStream),
    RR3_SHIM("_ZN4FMOD6System14setSpeakerModeE16FMOD_SPEAKERMODE", ::FMOD::fmodSystemSetSpeakerMode),
    RR3_SHIM("_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE", ::FMOD::fmodSystemCreateDSPByType),
    RR3_SHIM("_ZN4FMOD6System16getDSPBufferSizeEPjPi", ::FMOD::fmodSystemGetDSPBufferSize),
    RR3_SHIM("_ZN4FMOD6System16setDSPBufferSizeEji", ::FMOD::fmodSystemSetDSPBufferSize),
    RR3_SHIM("_ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_", ::FMOD::fmodSystemGetSoftwareFormat),
    RR3_SHIM("_ZN4FMOD6System17set3DNumListenersEi", ::FMOD::fmodSystemSet3DNumListeners),
    RR3_SHIM("_ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER", ::FMOD::fmodSystemSetSoftwareFormat),
    RR3_SHIM("_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE", ::FMOD::fmodSystemCreateChannelGroup),
    RR3_SHIM("_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_", ::FMOD::fmodSystemSet3DListenerAttributes),
    RR3_SHIM("_ZN4FMOD6System4initEijPv", ::FMOD::fmodSystemInit),
    RR3_SHIM("_ZN4FMOD6System6addDSPEPNS_3DSPEPPNS_13DSPConnectionE", ::FMOD::fmodSystemAddDSP),
    RR3_SHIM("_ZN4FMOD6System6updateEv", ::FMOD::fmodSystemUpdate),
    RR3_SHIM("_ZN4FMOD6System7releaseEv", ::FMOD::fmodSystemRelease),
    RR3_SHIM("_ZN4FMOD6System9createDSPEP20FMOD_DSP_DESCRIPTIONPPNS_3DSPE", ::FMOD::fmodSystemCreateDSP),
    RR3_SHIM("_ZN4FMOD6System9getOutputEP15FMOD_OUTPUTTYPE", ::FMOD::fmodSystemGetOutput),
    RR3_SHIM("_ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE", ::FMOD::fmodSystemPlaySound),
    RR3_SHIM("_ZN4FMOD7Channel11getPositionEPjj", ::FMOD::fmodChannelGetPosition),
    RR3_SHIM("_ZN4FMOD7Channel11getUserDataEPPv", ::FMOD::fmodChannelGetUserData),
    RR3_SHIM("_ZN4FMOD7Channel11setCallbackEPF11FMOD_RESULTP12FMOD_CHANNEL25FMOD_CHANNEL_CALLBACKTYPEPvS5_E", ::FMOD::fmodChannelSetCallback),
    RR3_SHIM("_ZN4FMOD7Channel11setPositionEjj", ::FMOD::fmodChannelSetPosition),
    RR3_SHIM("_ZN4FMOD7Channel11setUserDataEPv", ::FMOD::fmodChannelSetUserData),
    RR3_SHIM("_ZN4FMOD7Channel12setFrequencyEf", ::FMOD::fmodChannelSetFrequency),
    RR3_SHIM("_ZN4FMOD7Channel12setLoopCountEi", ::FMOD::fmodChannelSetLoopCount),
    RR3_SHIM("_ZN4FMOD7Channel13setLoopPointsEjjjj", ::FMOD::fmodChannelSetLoopPoints),
    RR3_SHIM("_ZN4FMOD7Channel15set3DAttributesEPK11FMOD_VECTORS3_", ::FMOD::fmodChannelSet3DAttributes),
    RR3_SHIM("_ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE", ::FMOD::fmodChannelSetChannelGroup),
    RR3_SHIM("_ZN4FMOD7Channel17set3DDopplerLevelEf", ::FMOD::fmodChannelSet3DDopplerLevel),
    RR3_SHIM("_ZN4FMOD7Channel19set3DMinMaxDistanceEff", ::FMOD::fmodChannelSet3DMinMaxDistance),
    RR3_SHIM("_ZN4FMOD7Channel4stopEv", ::FMOD::fmodChannelStop),
    RR3_SHIM("_ZN4FMOD7Channel6addDSPEPNS_3DSPEPPNS_13DSPConnectionE", ::FMOD::fmodChannelAddDSP),
    RR3_SHIM("_ZN4FMOD7Channel7getModeEPj", ::FMOD::fmodChannelGetMode),
    RR3_SHIM("_ZN4FMOD7Channel7setModeEj", ::FMOD::fmodChannelSetMode),
    RR3_SHIM("_ZN4FMOD7Channel9getPausedEPb", ::FMOD::fmodChannelGetPaused),
    RR3_SHIM("_ZN4FMOD7Channel9setPausedEb", ::FMOD::fmodChannelSetPaused),
    RR3_SHIM("_ZN4FMOD7Channel9setVolumeEf", ::FMOD::fmodChannelSetVolume),
    RR3_SHIM("__assert2", bionicAssert2),
    RR3_SHIM("__cmsg_nxthdr", bionicCmsgNextHeader),
    RR3_SHIM("__sF", bionicStandardFilesStorage),
    RR3_SHIM("__ctype_get_mb_cur_max", bionicCtypeGetMbCurMax),
    RR3_SHIM("__cxa_atexit", guestCxaAtexit),
    RR3_SHIM("__cxa_finalize", guestCxaFinalize),
    RR3_SHIM("__cxa_thread_atexit_impl", guestCxaThreadAtexitImpl),
    RR3_SHIM("__errno", bionicErrno),
    RR3_SHIM("__FD_ISSET_chk", bionicFdIsSetChk),
    RR3_SHIM("__FD_SET_chk", bionicFdSetChk),
    RR3_SHIM("__fwrite_chk", bionicFwriteChk),
    RR3_SHIM("__memcpy_chk", bionicMemcpyChk),
    RR3_SHIM("__memmove_chk", bionicMemmoveChk),
    RR3_SHIM("__memset_chk", bionicMemsetChk),
    RR3_SHIM("__open_2", guestOpen2),
    RR3_SHIM("__read_chk", bionicReadChk),
    RR3_SHIM("__register_atfork", bionicRegisterAtFork),
    RR3_SHIM("__stack_chk_fail", bionicStackChkFail),
    RR3_SHIM("__strcat_chk", bionicStrcatChk),
    RR3_SHIM("__strchr_chk", bionicStrchrChk),
    RR3_SHIM("__strcpy_chk", bionicStrcpyChk),
    RR3_SHIM("__strlen_chk", bionicStrlenChk),
    RR3_SHIM("__strrchr_chk", bionicStrrchrChk),
    RR3_SHIM("__strncpy_chk", bionicStrncpyChk),
    RR3_SHIM("__strncpy_chk2", bionicStrncpyChk2),
    RR3_SHIM("__vsnprintf_chk", bionicVsnprintfChk),
    RR3_SHIM("__vsprintf_chk", bionicVsprintfChk),
    RR3_SHIM("android_set_abort_message", androidSetAbortMessage),
    RR3_SHIM("_ctype_", bionicCtypePointer),
    RR3_SHIM("basename", bionicBasename),
    RR3_SHIM("ArCamera_getDisplayOrientedPose", arCameraGetDisplayOrientedPose),
    RR3_SHIM("ArCamera_getTrackingState", arCameraGetTrackingState),
    RR3_SHIM("ArConfig_create", arConfigCreate),
    RR3_SHIM("ArConfig_destroy", arConfigDestroy),
    RR3_SHIM("ArCoreApk_checkAvailability", arCoreApkCheckAvailability),
    RR3_SHIM("ArCoreApk_requestInstall", arCoreApkRequestInstall),
    RR3_SHIM("ArFrame_acquireCamera", arFrameAcquireCamera),
    RR3_SHIM("ArFrame_create", arFrameCreate),
    RR3_SHIM("ArFrame_destroy", arFrameDestroy),
    RR3_SHIM("ArPose_create", arPoseCreate),
    RR3_SHIM("ArPose_destroy", arPoseDestroy),
    RR3_SHIM("ArPose_getMatrix", arPoseGetMatrix),
    RR3_SHIM("ArSession_configure", arSessionConfigure),
    RR3_SHIM("ArSession_create", arSessionCreate),
    RR3_SHIM("ArSession_destroy", arSessionDestroy),
    RR3_SHIM("ArSession_pause", arSessionPause),
    RR3_SHIM("ArSession_resume", arSessionResume),
    RR3_SHIM("ArSession_setCameraTextureName", arSessionSetCameraTextureName),
    RR3_SHIM("ArSession_update", arSessionUpdate),
    RR3_SHIM("EchoCreate", echoCreate),
    RR3_SHIM("EchoDestroy", echoDestroy),
    RR3_SHIM("EchoLogin", echoLogin),
    RR3_SHIM("EchoResume", echoResume),
    RR3_SHIM("EchoSet", echoSet),
    RR3_SHIM("EchoSuspend", echoSuspend),
    RR3_SHIM("fflush", bionicFflush),
    RR3_SHIM("fprintf", bionicFprintf),
    RR3_SHIM("fputc", bionicFputc),
    RR3_SHIM("fwrite", bionicFwrite),
    RR3_SHIM("getc", bionicGetc),
    RR3_SHIM("getauxval", bionicGetauxval),
    RR3_SHIM("geteuid", bionicGeteuid),
    RR3_SHIM("getpwuid_r", bionicGetpwuidR),
    RR3_SHIM("if_nametoindex", bionicIfNametoindex),
    RR3_SHIM("longjmp", rr3GuestLongjmp),
    RR3_SHIM("lseek64", bionicLseek64),
    RR3_SHIM("madvise", bionicMadvise),
    RR3_SHIM("mlock", bionicMlock),
    RR3_SHIM("mmap", bionicMmap),
    RR3_SHIM("mprotect", bionicMprotect),
    RR3_SHIM("mremap", bionicMremap),
    RR3_SHIM("munmap", bionicMunmap),
    RR3_SHIM("closelog", libcxxCloseLog),
    RR3_SHIM("dladdr", guestDladdr),
    RR3_SHIM("dlclose", guestDlclose),
    RR3_SHIM("dlerror", guestDlerror),
    RR3_SHIM("dl_iterate_phdr", guestDlIteratePhdr),
    RR3_SHIM("dlopen", guestDlopen),
    RR3_SHIM("dlsym", guestDlsym),
    RR3_SHIM("fchmodat", libcxxFchmodAt),
    RR3_SHIM("fchown", bionicFchown),
    RR3_SHIM("openlog", libcxxOpenLog),
    RR3_SHIM("posix_memalign", libcxxPosixMemalign),
    RR3_SHIM("pipe", bionicPipe),
    RR3_SHIM("ptrace", bionicPtrace),
    RR3_SHIM("sendfile", libcxxSendfile),
    RR3_SHIM("setjmp", rr3GuestSetjmp),
    RR3_SHIM("setbuf", bionicSetbuf),
    RR3_SHIM("setvbuf", bionicSetvbuf),
    RR3_SHIM("sigaction", bionicSigaction),
    RR3_SHIM("statfs", guestBionicStatfs),
    RR3_SHIM("stderr", bionicStderrObject),
    RR3_SHIM("stdout", bionicStdoutObject),
    RR3_SHIM("sem_close", guestSemClose),
    RR3_SHIM("sem_destroy", guestSemDestroy),
    RR3_SHIM("sem_getvalue", guestSemGetValue),
    RR3_SHIM("sem_init", guestSemInit),
    RR3_SHIM("sem_open", guestSemOpen),
    RR3_SHIM("sem_post", guestSemPost),
    RR3_SHIM("sem_timedwait", guestSemTimedWait),
    RR3_SHIM("sem_trywait", guestSemTryWait),
    RR3_SHIM("sem_unlink", guestSemUnlink),
    RR3_SHIM("sem_wait", guestSemWait),
    RR3_SHIM("sysconf", libcxxSysconf),
    RR3_SHIM("syscall", libcxxSyscall),
    RR3_SHIM("syslog", libcxxSyslog),
    RR3_SHIM("timezone", bionicTimezone),
    RR3_SHIM("utimensat", libcxxUtimensAt),
    RR3_SHIM("ungetc", bionicUngetc),
    RR3_SHIM("vfprintf", bionicVfprintf),
    RR3_SHIM("pthread_attr_destroy", guestPthreadAttrDestroy),
    RR3_SHIM("pthread_attr_init", guestPthreadAttrInit),
    RR3_SHIM("pthread_attr_setdetachstate", guestPthreadAttrSetDetachState),
    RR3_SHIM("pthread_attr_setstacksize", guestPthreadAttrSetStackSize),
    RR3_SHIM("pthread_cond_broadcast", guestPthreadCondBroadcast),
    RR3_SHIM("pthread_cond_destroy", guestPthreadCondDestroy),
    RR3_SHIM("pthread_cond_init", guestPthreadCondInit),
    RR3_SHIM("pthread_cond_signal", guestPthreadCondSignal),
    RR3_SHIM("pthread_cond_timedwait", guestPthreadCondTimedWait),
    RR3_SHIM("pthread_cond_wait", guestPthreadCondWait),
    RR3_SHIM("pthread_create", guestPthreadCreate),
    RR3_SHIM("pthread_detach", guestPthreadDetach),
    RR3_SHIM("pthread_equal", guestPthreadEqual),
    RR3_SHIM("pthread_exit", guestPthreadExit),
    RR3_SHIM("pthread_getspecific", guestPthreadGetSpecific),
    RR3_SHIM("pthread_join", guestPthreadJoin),
    RR3_SHIM("pthread_key_create", guestPthreadKeyCreate),
    RR3_SHIM("pthread_key_delete", guestPthreadKeyDelete),
    RR3_SHIM("pthread_mutex_destroy", guestPthreadMutexDestroy),
    RR3_SHIM("pthread_mutex_init", guestPthreadMutexInit),
    RR3_SHIM("pthread_mutex_lock", guestPthreadMutexLock),
    RR3_SHIM("pthread_mutex_trylock", guestPthreadMutexTryLock),
    RR3_SHIM("pthread_mutex_unlock", guestPthreadMutexUnlock),
    RR3_SHIM("pthread_mutexattr_destroy", guestPthreadMutexAttrDestroy),
    RR3_SHIM("pthread_mutexattr_init", guestPthreadMutexAttrInit),
    RR3_SHIM("pthread_mutexattr_settype", guestPthreadMutexAttrSetType),
    RR3_SHIM("pthread_once", guestPthreadOnce),
    RR3_SHIM("pthread_rwlock_destroy", guestPthreadRwlockDestroy),
    RR3_SHIM("pthread_rwlock_init", guestPthreadRwlockInit),
    RR3_SHIM("pthread_rwlock_rdlock", guestPthreadRwlockReadLock),
    RR3_SHIM("pthread_rwlock_unlock", guestPthreadRwlockUnlock),
    RR3_SHIM("pthread_rwlock_wrlock", guestPthreadRwlockWriteLock),
    RR3_SHIM("pthread_self", guestPthreadSelf),
    RR3_SHIM("pthread_setspecific", guestPthreadSetSpecific),
};

#undef RR3_BIND
#undef RR3_SHIM

} // namespace

uintptr_t resolveHostSymbol(const char* name) {
    if (name == nullptr) {
        return 0;
    }
    // First resolution of the session happens during guest image relocation,
    // which is the earliest point at which the per-thread census is meaningful
    // and the latest point that is still before any game thread exists.
    censusArm();
    for (const HostSymbol& symbol : kHostSymbols) {
        if (std::strcmp(symbol.name, name) == 0) {
            return symbol.address;
        }
    }
    return 0;
}

size_t hostSymbolCount() {
    return sizeof(kHostSymbols) / sizeof(kHostSymbols[0]);
}

uint64_t hostSymbolNameHash() {
    uint64_t hash = 1469598103934665603ULL;
    for (const HostSymbol& symbol : kHostSymbols) {
        for (const unsigned char* cursor =
                 reinterpret_cast<const unsigned char*>(symbol.name);
             *cursor != 0;
             ++cursor) {
            hash ^= *cursor;
            hash *= 1099511628211ULL;
        }
        hash ^= 0;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace rr3
