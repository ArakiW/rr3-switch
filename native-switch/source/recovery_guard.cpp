#include "rr3/recovery_guard.hpp"
#include "rr3/libcxx_shims.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <switch.h>

extern "C" {
alignas(16) unsigned char __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* dump);
}

namespace {
FsFile g_crashFile{};
bool g_crashFileOpen = false;
unsigned int g_handlingCrash = 0;
char g_crashText[2048]{};
size_t g_crashLength = 0;

void crashText(const char* text) {
    while (*text != '\0' && g_crashLength < sizeof(g_crashText)) {
        g_crashText[g_crashLength++] = *text++;
    }
}

void crashHex(const char* name, u64 value) {
    crashText(name);
    crashText("=");
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (g_crashLength < sizeof(g_crashText)) {
            g_crashText[g_crashLength++] = digits[(value >> shift) & 15];
        }
    }
    crashText("\n");
}
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* dump) {
    if (__atomic_exchange_n(&g_handlingCrash, 1U, __ATOMIC_RELAXED) != 0) {
        svcExitProcess();
    }
    g_crashLength = 0;
    crashText("RR3 exception-v1\nstatus=exception\n");
    crashHex("handler", reinterpret_cast<uintptr_t>(&__libnx_exception_handler));
    crashHex("error_desc", dump->error_desc);
    crashHex("pc", dump->pc.x);
    crashHex("lr", dump->lr.x);
    crashHex("sp", dump->sp.x);
    crashHex("fp", dump->fp.x);
    crashHex("far", dump->far.x);
    crashHex("esr", dump->esr);
    crashHex("pstate", dump->pstate);
    u64 tpidr = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    crashHex("tpidr_el0", tpidr);
    for (unsigned int index = 0; index < 29; ++index) {
        char name[] = {'x', static_cast<char>('0' + index / 10),
                       static_cast<char>('0' + index % 10), '\0'};
        crashHex(name, dump->cpu_gprs[index].x);
    }
    svcOutputDebugString(g_crashText, g_crashLength);
    if (g_crashFileOpen) {
        fsFileWrite(&g_crashFile, 0, g_crashText, g_crashLength, FsWriteOption_Flush);
    }
    svcExitProcess();
}

namespace rr3 {

bool prepareGuestCrashCapture() {
    if (g_crashFileOpen) {
        return false;
    }
    FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    if (sd == nullptr) {
        return false;
    }
    char path[128]{};
    std::snprintf(path, sizeof(path), "/switch/rr3-native-rehost.crash-%016llx.txt",
                  static_cast<unsigned long long>(armGetSystemTick()));
    if (R_FAILED(fsFsCreateFile(sd, path, 0, 0)) ||
        R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &g_crashFile))) {
        return false;
    }
    androidSetAbortMessage(nullptr);
    g_crashFileOpen = true;
    constexpr char armed[] = "RR3 termination-v2\nstatus=armed\n";
    if (R_FAILED(fsFileWrite(&g_crashFile, 0, armed, sizeof(armed) - 1, FsWriteOption_Flush))) {
        finishGuestCrashCapture();
        return false;
    }
    return true;
}

void finishGuestCrashCapture() {
    if (g_crashFileOpen) {
        g_crashFileOpen = false;
        fsFileClose(&g_crashFile);
    }
}

[[noreturn]] void guestTracedAbort() {
    recordGuestTermination("abort", 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::abort();
}

[[noreturn]] void guestTracedExit(int status) {
    recordGuestTermination("exit", static_cast<unsigned long long>(status), reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::exit(status);
}

int guestTracedRaise(int signal) {
    recordGuestTermination("raise", static_cast<unsigned long long>(signal), reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    return std::raise(signal);
}
void recordGuestTermination(const char* kind, unsigned long long value,
                            unsigned long long caller) {
    if (!g_crashFileOpen) return;
    char text[2048]{};
    size_t size = 0;
    const auto append = [&](const char* input) {
        while (*input != '\0' && size < sizeof(text)) text[size++] = *input++;
    };
    const auto hex = [&](const char* name, unsigned long long number) {
        append(name);
        constexpr char digits[] = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) {
            if (size < sizeof(text)) text[size++] = digits[(number >> shift) & 15];
        }
        append("\n");
    };
    append("RR3 termination-v2\nstatus=termination-request\nkind=");
    append(kind != nullptr ? kind : "unknown");
    append("\n");
    hex("caller=", caller);
    hex("value=", value);
    append("abort_message_begin\n");
    append(guestAbortMessage());
    append("\nabort_message_end\n");
    svcOutputDebugString(text, size);
    while (size < sizeof(text)) text[size++] = ' ';
    text[sizeof(text) - 1] = '\n';
    fsFileWrite(&g_crashFile, 0, text, size, FsWriteOption_Flush);
}


bool recoveryMarkerExists(const char* path) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool beginGuardedAction(const char* path, const char* action) {
    return beginGuardedActionSnapshot(path, action, nullptr);
}

bool beginGuardedActionSnapshot(const char* path, const char* action,
                                const char* snapshot) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "pending=%s\n",
                 action != nullptr && action[0] != '\0' ? action : "unknown");
    if (snapshot != nullptr && snapshot[0] != '\0') {
        std::fprintf(file, "%s\n", snapshot);
    }
    std::fprintf(file, "clear_only_after_success=1\n");
    const bool flushed = std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

bool completeGuardedAction(const char* path) {
    if (path == nullptr) {
        return false;
    }
    if (!recoveryMarkerExists(path)) {
        return true;
    }
    return std::remove(path) == 0;
}

bool writeGuestBootProgress(const char* path, const char* phase,
                            unsigned long long ordinal,
                            unsigned long long target,
                            const char* details) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "phase=%s ordinal=%llu target=%016llx%s%s\n",
                 phase != nullptr && phase[0] != '\0' ? phase : "unknown",
                 ordinal, target, details != nullptr ? " " : "",
                 details != nullptr ? details : "");
    const bool flushed = std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

bool appendGuestBootLine(const char* path, const char* phase,
                         unsigned long long ordinal,
                         unsigned long long target,
                         const char* details) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "a");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "phase=%s ordinal=%llu target=%016llx%s%s\n",
                 phase != nullptr && phase[0] != '\0' ? phase : "unknown",
                 ordinal, target, details != nullptr ? " " : "",
                 details != nullptr ? details : "");
    const bool flushed = std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

bool readGuestBootProgress(const char* path, char* out, unsigned long capacity) {
    if (out != nullptr && capacity > 0) {
        out[0] = '\0';
    }
    if (path == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    const size_t read = std::fread(out, 1, capacity - 1, file);
    out[read] = '\0';
    std::fclose(file);
    // Trim a single trailing newline so the string prints cleanly in the UI.
    for (size_t index = read; index > 0; --index) {
        if (out[index - 1] == '\n' || out[index - 1] == '\r') {
            out[index - 1] = '\0';
        } else {
            break;
        }
    }
    return read > 0;
}

void clearGuestBootProgress(const char* path) {
    if (path != nullptr && recoveryMarkerExists(path)) {
        std::remove(path);
    }
}

} // namespace rr3
