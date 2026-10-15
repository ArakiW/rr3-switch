#include "rr3/guest_view_trace.hpp"

#include "rr3/guest_frame_probe.hpp"
#include "rr3/guest_teardown_repair.hpp"
#include "rr3/recovery_guard.hpp"

#include <EGL/egl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace {

const char* g_progressPath = nullptr;
// Content-independent quota for guest __android_log_print lines that no filter
// predicate would keep. Rationale and the resulting bound are stated at
// guestViewNoteAndroidLog.
//
// RAISED 200 -> 4000, 2026-10-04, because the cap was a measurement limit, not
// a safety limit. 200 was set when the guest never got past the splash, to keep
// the archived run's 570-line log from growing much; the guest now renders its
// splash and keeps going, and the quota is spent long before it stops. The
// archived runs prove the truncation is real and not theoretical -- they carry
//   phase=guest-log-drop ordinal=1..4 cap=200 reason=quota_spent
// which is this file correctly reporting that it threw away guest lines, and
// the lines it throws away are the ones the guest prints AFTER it goes quiet,
// i.e. the exact evidence for the open question. 4000 lines is ~600 KB of
// guest-view.log, which the archive already handles for the eden_log tail.
constexpr unsigned kAndroidLogAnyBudget = 4000;
// Worker faults use exception capture; only the probe thread owns this breadcrumb stream.
thread_local bool g_enabled = false;
bool g_checkpointInProgress = false;
unsigned int g_lastStage = 0;
uint64_t g_seenStages = 0;
uintptr_t g_traceTpidrEl0 = 0;
uint64_t g_traceCanary = 0;
bool g_traceCanaryReadable = false;
unsigned int g_vectorGrowth240Calls = 0;
unsigned int g_vectorStorage40Calls = 0;
bool g_postStage30MemcpyReturned = false;
bool g_postStage30FreeTraced = false;

// ---------------------------------------------------------------------------
// Deallocation-sequence observer.
//
// The container-teardown loop in the guest is a REVERSE walk, 0x30 per step,
// whose exit test is plain pointer equality:
//     0xcd2abc  cmp  x20, x23      ; begin vs current
//     0xcd2ac4  b.eq #0xcd2b00      ; equal -> done
//     0xcd2ad4  sub  x23, x21, #0x30
// with no modulo guard anywhere. If (end - begin) is not a multiple of 0x30 --
// and the runtime reads say it is 0x25A77C, i.e. remainder 0x1c -- then the
// pointer can NEVER equal begin, so the loop never exits and walks down
// through memory calling operator delete until it faults.
//
// This observer exists to make that visible WITHOUT touching any allocator
// path and WITHOUT patching guest .text. It watches only the POINTER SEQUENCE
// handed to free(): a legitimate teardown frees a bounded, scattered set; the
// runaway loop frees a long run with a CONSTANT STRIDE, going down. That
// difference is the proof, and it comes from the pointer values alone.
//
// Deliberately on `free` and not on `_ZdlPv`: free is already shimmed and
// already forwards to the same std::free the guest allocator uses, so this adds
// no heap risk and no new row to the frozen symbol table. A `_ZdlPv` shim
// would have to forward to libc++'s operator delete to stay heap-correct, and
// whether it preserves the caller's x29 depends on whether libc++'s operator
// delete establishes a frame of its own -- unknowable without testing, and
// therefore strictly worse evidence than the stride.
// ---------------------------------------------------------------------------
std::atomic<uintptr_t> g_freeSeqLast{0};
std::atomic<uintptr_t> g_freeSeqStride{0};
std::atomic<uintptr_t> g_freeSeqRun{0};
std::atomic<uintptr_t> g_freeSeqTotal{0};
std::atomic<uintptr_t> g_freeSeqLongestRun{0};
std::atomic<uintptr_t> g_freeSeqLongestStride{0};
uintptr_t g_second40Caller = 0;
uintptr_t g_copyCaller = 0;
uintptr_t g_copyDestination = 0;
uintptr_t g_copySource = 0;size_t g_copyLength = 0;
uintptr_t g_freeCaller = 0;
uintptr_t g_freeAddress = 0;
uintptr_t g_glCaller = 0;
GLuint g_glObject = 0;
// Host-owned texture used when the guest binds 0 or uploads a 0-sized image.
// Proves whether the GL bridge can put a real texture on screen (Kimi plan C).
GLuint g_hostFallbackTex = 0;
unsigned g_hostTexInject = 0;
// Host fullscreen-quad draw after guest Render when the guest issued no
// indexed draws — independent of the guest vertex layout.
GLuint g_hostQuadProgram = 0;
GLuint g_hostQuadVbo = 0;
bool g_hostQuadReady = false;

void ensureHostFallbackTexture() {
    if (g_hostFallbackTex != 0) {
        return;
    }
    ::glGenTextures(1, &g_hostFallbackTex);
    if (g_hostFallbackTex == 0) {
        return;
    }
    ::glBindTexture(GL_TEXTURE_2D, g_hostFallbackTex);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    constexpr int kN = 16;
    static unsigned char pixels[kN * kN * 4];
    for (int y = 0; y < kN; ++y) {
        for (int x = 0; x < kN; ++x) {
            const bool dark = ((x / 2) + (y / 2)) % 2 == 0;
            unsigned char* p = &pixels[(y * kN + x) * 4];
            // Large magenta / black blocks — unmistakable vs Mesa cyan checker.
            p[0] = dark ? 0 : 255;
            p[1] = dark ? 0 : 0;
            p[2] = dark ? 0 : 255;
            p[3] = 255;
        }
    }
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kN, kN, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "host-fallback-tex",
                        static_cast<unsigned long long>(g_hostFallbackTex),
                        static_cast<unsigned long long>(kN));
}

bool ensureHostQuadPipeline() {
    if (g_hostQuadReady) {
        return true;
    }
    ensureHostFallbackTexture();
    if (g_hostFallbackTex == 0) {
        return false;
    }
    const GLchar* vs =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }\n";
    const GLchar* fs =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main(){ gl_FragColor=texture2D(uTex,vUV); }\n";
    const GLuint vsh = ::glCreateShader(GL_VERTEX_SHADER);
    const GLuint fsh = ::glCreateShader(GL_FRAGMENT_SHADER);
    ::glShaderSource(vsh, 1, &vs, nullptr);
    ::glCompileShader(vsh);
    ::glShaderSource(fsh, 1, &fs, nullptr);
    ::glCompileShader(fsh);
    g_hostQuadProgram = ::glCreateProgram();
    ::glAttachShader(g_hostQuadProgram, vsh);
    ::glAttachShader(g_hostQuadProgram, fsh);
    ::glBindAttribLocation(g_hostQuadProgram, 0, "aPos");
    ::glBindAttribLocation(g_hostQuadProgram, 1, "aUV");
    ::glLinkProgram(g_hostQuadProgram);
    ::glDeleteShader(vsh);
    ::glDeleteShader(fsh);
    // pos.xy, uv.xy — fullscreen triangle-strip quad
    static const float kVerts[] = {
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    ::glGenBuffers(1, &g_hostQuadVbo);
    ::glBindBuffer(GL_ARRAY_BUFFER, g_hostQuadVbo);
    ::glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    g_hostQuadReady = g_hostQuadProgram != 0 && g_hostQuadVbo != 0;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "host-quad-ready",
                        static_cast<unsigned long long>(g_hostQuadProgram),
                        static_cast<unsigned long long>(g_hostQuadVbo));
    return g_hostQuadReady;
}

uintptr_t readTpidrEl0() {
    uintptr_t value{};
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

uint64_t readGuestCanary(uintptr_t tpidrEl0) {
    if (tpidrEl0 == 0) {
        return 0;
    }
    return *reinterpret_cast<volatile const uint64_t*>(tpidrEl0 + 0x28);
}

const char* glStringName(GLenum name) {
    switch (name) {
        case GL_VERSION: return "version";
        case GL_VENDOR: return "vendor";
        case GL_RENDERER: return "renderer";
        case GL_EXTENSIONS: return "extensions";
        default: return nullptr;
    }
}

unsigned int stageFor(GLenum name, bool returned) {
    const unsigned int base = name == GL_VERSION ? 1U
        : name == GL_VENDOR ? 3U
        : name == GL_RENDERER ? 5U
        : 7U;
    return base + (returned ? 1U : 0U);
}

void checkpointPhase(const char* phase, unsigned int stage,
                     unsigned long long target, bool durable = true) {
    if (phase == nullptr || g_progressPath == nullptr ||
        g_checkpointInProgress) {
        return;
    }
    g_lastStage = stage;
    g_seenStages |= 1ULL << stage;
    g_checkpointInProgress = true;
    char details[384]{};
    if (stage >= 30) {
        __builtin_snprintf(details, sizeof(details),
            "trace=callers-v1 malloc40_caller=%016llx copy_caller=%016llx "
            "dst=%016llx src=%016llx len=%llx free_caller=%016llx ptr=%016llx"
            " gltrace=shader-v1 gl_caller=%016llx gl_object=%x",
            static_cast<unsigned long long>(g_second40Caller),
            static_cast<unsigned long long>(g_copyCaller),
            static_cast<unsigned long long>(g_copyDestination),
            static_cast<unsigned long long>(g_copySource),
            static_cast<unsigned long long>(g_copyLength),
            static_cast<unsigned long long>(g_freeCaller),
            static_cast<unsigned long long>(g_freeAddress),
            static_cast<unsigned long long>(g_glCaller), g_glObject);
    }
    // Only the blocked-wait breadcrumb passes durable=false: it is the one
    // progress write that repeats on a guest-blocking timer, and it must not
    // pay for the whole-card fsync that the durable stage checkpoints need.
    if (durable) {
        writeGuestBootProgress(g_progressPath, phase, stage, target,
                               stage >= 30 ? details : nullptr);
    } else {
        writeGuestBootProgressNoSync(g_progressPath, phase, stage, target,
                                     stage >= 30 ? details : nullptr);
    }
    g_checkpointInProgress = false;
}

void checkpoint(GLenum name, bool returned) {
    const char* kind = glStringName(name);
    if (kind == nullptr || g_progressPath == nullptr) {
        return;
    }
    const unsigned int stage = stageFor(name, returned);
    char phase[64]{};
    const int written = __builtin_snprintf(
        phase, sizeof(phase), "view-gl-%s-%s", kind,
        returned ? "return" : "enter");
    if (written > 0 && static_cast<unsigned int>(written) < sizeof(phase)) {
        checkpointPhase(phase, stage, 0);
    }
}

const char* allocationKind(size_t size, unsigned int occurrence) {
    switch (size) {
        case 0x2800: return "2800";
        case 0x3e38: return "3e38";
        case 0x478: return "478";
        case 0x240: return occurrence == 1 ? "240-first" : nullptr;
        case 0x130: return "130";
        case 0x40: return occurrence == 1 ? "40-first"
            : occurrence == 2 ? "40-second" : nullptr;
        default: return nullptr;
    }
}

unsigned int allocationStage(size_t size, unsigned int occurrence,
                             bool returned) {
    const unsigned int base = size == 0x2800 ? 9U
        : size == 0x3e38 ? 11U
        : size == 0x478 ? 19U
        : size == 0x240 ? 23U
        : size == 0x130 ? 25U
        : size == 0x40 ? (occurrence == 1 ? 27U : 29U)
        : 0U;
    return base + (returned ? 1U : 0U);
}

const char* memsetKind(size_t length) {
    switch (length) {
        case 0x1550: return "1550";
        case 0x320: return "320";
        case 0xe98: return "e98";
        case 0xc01: return "c01";
        default: return nullptr;
    }
}

unsigned int memsetStage(size_t length, bool returned) {
    const unsigned int base = length == 0x1550 ? 13U
        : length == 0x320 ? 15U
        : length == 0xe98 ? 17U
        : 21U;
    return base + (returned ? 1U : 0U);
}

} // namespace

void hostDrawMagentaQuad() {
    if (!ensureHostQuadPipeline()) {
        return;
    }
    ::glDisable(GL_DEPTH_TEST);
    ::glDisable(GL_CULL_FACE);
    ::glDisable(GL_SCISSOR_TEST);
    ::glDisable(GL_BLEND);
    ::glUseProgram(g_hostQuadProgram);
    ::glActiveTexture(GL_TEXTURE0);
    ::glBindTexture(GL_TEXTURE_2D, g_hostFallbackTex);
    const GLint texLoc = ::glGetUniformLocation(g_hostQuadProgram, "uTex");
    if (texLoc >= 0) {
        ::glUniform1i(texLoc, 0);
    }
    ::glBindBuffer(GL_ARRAY_BUFFER, g_hostQuadVbo);
    ::glEnableVertexAttribArray(0);
    ::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    ::glEnableVertexAttribArray(1);
    ::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16,
                            reinterpret_cast<const void*>(8));
    ::glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    static unsigned s_hostDraws = 0;
    if (s_hostDraws < 4) {
        ++s_hostDraws;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "host-draw-quad", s_hostDraws,
                            static_cast<unsigned long long>(g_hostFallbackTex));
    }
}

bool g_glStatsEnabled = false;
GuestViewGlStats g_glStats{};

void enableGuestViewGlTrace(const char* progressPath) {
    g_progressPath = progressPath;
    g_lastStage = 0;
    g_seenStages = 0;
    g_traceTpidrEl0 = readTpidrEl0();
    g_traceCanary = readGuestCanary(g_traceTpidrEl0);
    g_traceCanaryReadable = g_traceTpidrEl0 != 0;
    g_vectorGrowth240Calls = 0;
    g_vectorStorage40Calls = 0;
    g_postStage30MemcpyReturned = false;
    g_postStage30FreeTraced = false;
    g_second40Caller = 0;
    g_copyCaller = 0;
    g_copyDestination = 0;
    g_copySource = 0;
    g_copyLength = 0;
    g_freeCaller = 0;
    g_freeAddress = 0;
    g_glCaller = 0;
    g_glObject = 0;
    g_glStats = {};
    // Draw counters stay armed after disableGuestViewGlTrace so the frame
    // loop still attributes guest GL traffic.
    g_glStatsEnabled = true;
    g_enabled = true;
}

void disableGuestViewGlTrace() {
    g_enabled = false;
    g_progressPath = nullptr;
    g_traceTpidrEl0 = 0;
    g_traceCanary = 0;
    g_traceCanaryReadable = false;
    g_vectorGrowth240Calls = 0;
    g_vectorStorage40Calls = 0;
    g_postStage30MemcpyReturned = false;
    g_postStage30FreeTraced = false;
    g_second40Caller = 0;
    g_copyCaller = 0;
    g_copyDestination = 0;
    g_copySource = 0;
    g_copyLength = 0;
    g_freeCaller = 0;
    g_freeAddress = 0;
    g_glCaller = 0;
    g_glObject = 0;
}

const GLubyte* guestViewTracedGlGetString(GLenum name) {
    if (!g_enabled || glStringName(name) == nullptr) {
        return ::glGetString(name);
    }
    checkpoint(name, false);
    const GLubyte* result = ::glGetString(name);
    checkpoint(name, true);
    return result;
}

void* guestViewTracedMalloc(size_t size) {
    if (!g_enabled || g_checkpointInProgress) {
        return std::malloc(size);
    }

    unsigned int vectorCall = 0;
    if (size == 0x240) {
        vectorCall = ++g_vectorGrowth240Calls;
        if (vectorCall > 1) {
            return std::malloc(size);
        }
    }
    else if (size == 0x40) {
        if (!guestViewTraceReached(26)) {
            return std::malloc(size);
        }
        vectorCall = ++g_vectorStorage40Calls;
        if (vectorCall > 2) {
            return std::malloc(size);
        }
    }
    const char* kind = allocationKind(size, vectorCall);
    if (kind == nullptr) {
        return std::malloc(size);
    }

    const uintptr_t caller = reinterpret_cast<uintptr_t>(
        __builtin_return_address(0));
    if (size == 0x40 && vectorCall == 2) {
        g_second40Caller = caller;
    }
    char enterPhase[64]{};
    const int enterWritten = __builtin_snprintf(
        enterPhase, sizeof(enterPhase), "view-post-gl-malloc-%s-enter", kind);
    if (enterWritten > 0 &&
        static_cast<unsigned int>(enterWritten) < sizeof(enterPhase)) {
        checkpointPhase(enterPhase, allocationStage(size, vectorCall, false),
                        static_cast<unsigned long long>(caller));
    }

    void* result = std::malloc(size);

    char returnPhase[64]{};
    const int returnWritten = __builtin_snprintf(
        returnPhase, sizeof(returnPhase), "view-post-gl-malloc-%s-return", kind);
    if (returnWritten > 0 &&
        static_cast<unsigned int>(returnWritten) < sizeof(returnPhase)) {
        checkpointPhase(returnPhase, allocationStage(size, vectorCall, true),
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(result)));
    }
    return result;
}

extern "C" void* guestViewTracedMemcpyBody(void* destination, const void* source,
                                            size_t length, uintptr_t guestFp,
                                            uintptr_t guestLr) {
    // The caller's frame pointer and return address, captured by the assembly
    // stub in source/guest_frame_probe.s BEFORE it established a frame of its
    // own. For a guest call through the PLT these are the guest's own values,
    // which is the whole point: guestLr lets the frame probe ask "is the caller
    // inside the container-teardown function?" and guestFp is that function's
    // live frame pointer. Costs two relaxed atomic increments and a range
    // compare on the guest's hot path, and nothing else.
    guestFrameObserve(guestFp, guestLr, reinterpret_cast<uintptr_t>(destination),
                      "memcpy");

    if (!g_enabled || g_checkpointInProgress || !guestViewTraceReached(30) ||
        g_postStage30MemcpyReturned) {
        return std::memcpy(destination, source, length);
    }

    const uintptr_t caller = guestLr;
    g_copyCaller = caller;
    g_copyDestination = reinterpret_cast<uintptr_t>(destination);
    g_copySource = reinterpret_cast<uintptr_t>(source);
    g_copyLength = length;
    checkpointPhase("view-post-gl-memcpy-enter", 31,
                    static_cast<unsigned long long>(caller));
    void* result = std::memcpy(destination, source, length);
    g_postStage30MemcpyReturned = true;
    checkpointPhase("view-post-gl-memcpy-return", 32,
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(result)));
    return result;
}

void guestViewTracedFree(void* allocation) {
    // Pointer-sequence observation, first statement, and it never changes what
    // the function does: std::free is called on every path exactly as before.
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(allocation);
        const uintptr_t total =
            g_freeSeqTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        const uintptr_t last =
            g_freeSeqLast.load(std::memory_order_relaxed);
        uintptr_t run = 1;
        if (last != 0 && p != 0) {
            const uintptr_t delta = last - p;
            if (delta == g_freeSeqStride.load(std::memory_order_relaxed)) {
                run = g_freeSeqRun.fetch_add(1, std::memory_order_relaxed) + 1;
            } else {
                g_freeSeqStride.store(delta, std::memory_order_relaxed);
                g_freeSeqRun.store(1, std::memory_order_relaxed);
            }
        }
        g_freeSeqLast.store(p, std::memory_order_relaxed);
        uintptr_t best = g_freeSeqLongestRun.load(std::memory_order_relaxed);
        if (run > best) {
            g_freeSeqLongestRun.store(run, std::memory_order_relaxed);
            g_freeSeqLongestStride.store(
                g_freeSeqStride.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        // Milestones only, and only while a run is actually growing, so the log
        // shows the shape of the runaway instead of a firehose.
        const bool firstFew = total <= 8;
        const bool milestone = run == 64 || run == 1024 || run == 16384 ||
                               run == 131072 || run == 1048576;
        if (firstFew || milestone) {
            char line[256];
            __builtin_snprintf(line, sizeof(line),
                "total=%llu ptr=0x%llx prev=0x%llx stride=0x%llx run=%llu "
                "longest_run=%llu longest_stride=0x%llx caller=0x%llx "
                "note=constant_stride_run_means_a_reverse_teardown_walking_down",
                (unsigned long long)total, (unsigned long long)p,
                (unsigned long long)last,
                (unsigned long long)g_freeSeqStride.load(std::memory_order_relaxed),
                (unsigned long long)run,
                (unsigned long long)g_freeSeqLongestRun.load(std::memory_order_relaxed),
                (unsigned long long)g_freeSeqLongestStride.load(std::memory_order_relaxed),
                (unsigned long long)reinterpret_cast<uintptr_t>(
                    __builtin_return_address(0)));
            (void)appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "guest-free-seq", 0, 0, line);
        }
    }

    if (!g_enabled || g_checkpointInProgress ||
        !g_postStage30MemcpyReturned || g_postStage30FreeTraced) {
        std::free(allocation);
        return;
    }

    g_postStage30FreeTraced = true;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(
        __builtin_return_address(0));
    g_freeCaller = caller;
    g_freeAddress = reinterpret_cast<uintptr_t>(allocation);
    checkpointPhase("view-post-gl-free-enter", 33,
                    static_cast<unsigned long long>(caller));
    std::free(allocation);
    checkpointPhase("view-post-gl-free-return", 34,
                    static_cast<unsigned long long>(g_freeAddress));
}

void* guestViewTracedMemset(void* destination, int value, size_t length) {
    const char* kind = memsetKind(length);
    if (!g_enabled || kind == nullptr) {
        return std::memset(destination, value, length);
    }

    const uintptr_t caller = reinterpret_cast<uintptr_t>(
        __builtin_return_address(0));
    char enterPhase[64]{};
    const int enterWritten = __builtin_snprintf(
        enterPhase, sizeof(enterPhase), "view-post-gl-memset-%s-enter", kind);
    if (enterWritten > 0 &&
        static_cast<unsigned int>(enterWritten) < sizeof(enterPhase)) {
        checkpointPhase(enterPhase, memsetStage(length, false),
                        static_cast<unsigned long long>(caller));
    }

    void* result = std::memset(destination, value, length);

    char returnPhase[64]{};
    const int returnWritten = __builtin_snprintf(
        returnPhase, sizeof(returnPhase), "view-post-gl-memset-%s-return", kind);
    if (returnWritten > 0 &&
        static_cast<unsigned int>(returnWritten) < sizeof(returnPhase)) {
        checkpointPhase(returnPhase, memsetStage(length, true),
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(result)));
    }
    return result;
}

GLuint guestViewTracedGlCreateShader(GLenum type) {
    if (!g_enabled || g_checkpointInProgress) {
        return ::glCreateShader(type);
    }
    g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    g_glObject = 0;
    checkpointPhase("view-gl-create-shader-enter", 38, type);
    const GLuint shader = ::glCreateShader(type);
    g_glObject = shader;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "glCreateShader",
                        static_cast<unsigned long long>(shader),
                        static_cast<unsigned long long>(type));
    checkpointPhase("view-gl-create-shader-return", 39, shader);
    return shader;
}

void guestViewTracedGlShaderSource(GLuint shader, GLsizei count,
                                   const GLchar* const* strings, const GLint* lengths) {
    if (!g_enabled || g_checkpointInProgress) {
        ::glShaderSource(shader, count, strings, lengths);
        return;
    }
    g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    g_glObject = shader;
    uint32_t total = 0;
    for (GLsizei i = 0; i < count && strings != nullptr; ++i) {
        if (strings[i] == nullptr) continue;
        if (lengths != nullptr && lengths[i] > 0) {
            total += static_cast<uint32_t>(lengths[i]);
        } else {
            const GLchar* s = strings[i];
            while (*s++) ++total;
        }
    }
    char preview[48]{};
    if (count > 0 && strings != nullptr && strings[0] != nullptr) {
        const GLchar* s = strings[0];
        for (int i = 0; i < 40 && s[i] != '\0'; ++i) {
            const char c = s[i];
            preview[i] = (c >= 32 && c < 127) ? c : '.';
        }
    }
    guestViewWideLog("glShaderSource",
                     static_cast<unsigned long long>(shader),
                     static_cast<unsigned long long>(total), preview);
    checkpointPhase("view-gl-shader-source-enter", 40, static_cast<uint64_t>(count));
    ::glShaderSource(shader, count, strings, lengths);
    checkpointPhase("view-gl-shader-source-return", 41, shader);
}

void guestViewTracedGlCompileShader(GLuint shader) {
    if (!g_enabled || g_checkpointInProgress) {
        ::glCompileShader(shader);
        return;
    }
    g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    g_glObject = shader;
    checkpointPhase("view-gl-compile-shader-enter", 42, shader);
    ::glCompileShader(shader);
    GLint compiled = 0;
    ::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "shader-compile-fail",
                            static_cast<unsigned long long>(shader), 0);
    }
    checkpointPhase("view-gl-compile-shader-return", 43, shader);
}

void guestViewTracedGlLinkProgram(GLuint program) {
    if (!g_enabled || g_checkpointInProgress) {
        ::glLinkProgram(program);
        return;
    }
    g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    g_glObject = program;
    checkpointPhase("view-gl-link-program-enter", 44, program);
    ::glLinkProgram(program);
    GLint linked = 0;
    ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gl-link-fail",
                            static_cast<unsigned long long>(program), 0);
    }
    checkpointPhase("view-gl-link-program-return", 45, program);
}

void guestViewTracedGlShaderBinary(GLsizei count, const GLuint* shaders,
                                   GLenum binaryformat, const void* binary,
                                   GLsizei length) {
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "glShaderBinary",
                        static_cast<unsigned long long>(count),
                        static_cast<unsigned long long>(length),
                        binary != nullptr ? "bin" : "null");
    ::glShaderBinary(count, shaders, binaryformat, binary, length);
}

bool guestViewTraceReached(unsigned int stage) {
    return stage < 64 && (g_seenStages & (1ULL << stage)) != 0;
}

void guestViewTraceSyncBlocked(const char* primitive, unsigned int stage) {
    if (g_progressPath == nullptr || primitive == nullptr || g_checkpointInProgress) {
        return;
    }
    char phase[96]{};
    const int written = __builtin_snprintf(phase, sizeof(phase),
        "view-sync-blocked-%s", primitive);
    if (written <= 0 || static_cast<unsigned int>(written) >= sizeof(phase)) {
        return;
    }
    // durable=false: see checkpointPhase. This is the only caller that can fire
    // twice a second per blocked guest thread, and it is the caller that turned
    // a diagnostic breadcrumb into a whole-card fsync on the emulated SDMC.
    checkpointPhase(phase, stage, 0, false);
}

unsigned int guestViewTraceLastStage() {
    return g_lastStage;
}

void guestViewTraceStackCheckFailure(uintptr_t caller) {
    if (!g_enabled || g_progressPath == nullptr || g_checkpointInProgress) {
        return;
    }

    const uintptr_t currentTpidrEl0 = readTpidrEl0();
    uint64_t currentCanary = 0;
    if (currentTpidrEl0 == g_traceTpidrEl0 && g_traceCanaryReadable) {
        currentCanary = readGuestCanary(currentTpidrEl0);
    }
    unsigned int stage = 35;
    const char* state = "tls-same-canary-same";
    if (currentTpidrEl0 != g_traceTpidrEl0) {
        stage = 36;
        state = "tls-changed";
    }
    else if (!g_traceCanaryReadable || currentCanary != g_traceCanary) {
        stage = 37;
        state = "tls-same-canary-changed";
    }

    char phase[64]{};
    const int written = __builtin_snprintf(
        phase, sizeof(phase), "view-stack-chk-fail-%s", state);
    if (written > 0 && static_cast<unsigned int>(written) < sizeof(phase)) {
        checkpointPhase(phase, stage,
                        static_cast<unsigned long long>(caller));
    }
}

GuestViewGlStats guestViewGlStats() {
    return g_glStats;
}

void resetGuestViewGlStats() {
    g_glStats = {};
}

void guestViewTracedGlClear(GLbitfield mask) {
    if (g_glStatsEnabled) {
        ++g_glStats.clears;
    }
    ::glClear(mask);
}

void guestViewTracedGlDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (g_glStatsEnabled) {
        ++g_glStats.drawArrays;
        g_glStats.lastDrawMode = mode;
        g_glStats.lastDrawElementsCount = static_cast<uint32_t>(count);
        // The arguments, not just the tally. With flr=1 (archive
        // 20261012-021130-flr1) the guest draws 46,630 times -- about two per
        // presented frame -- and the full-buffer readback is a single flat
        // colour, RGB(13,31,89), identical at all five probe points. "It draws"
        // has stopped being informative; "what does it draw" is the question.
        //
        // mode/count separate the two worlds: a 4-vertex triangle strip is a
        // fullscreen quad and means "background only"; a few thousand vertices
        // means real geometry is being submitted and the problem is downstream
        // (blend, depth, texture, or the vertex buffer it is reading).
        if (g_glStats.drawArrays <= 6 || g_glStats.drawArrays % 4096 == 0) {
            GLint tex = -1;
            GLint prog = -1;
            ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
            ::glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            // The bound texture's dimensions, because "it draws 46,690 times"
            // has stopped being informative and the framebuffer is a single
            // flat colour (rgb 13,31,89 at every probe point). A quad with a
            // real 2D texture would show that texture. A uniform fill is what a
            // 1x1 texture, an uncompressed-but-empty texture, or a texture that
            // was never uploaded all look like -- and the host's own counters
            // already say 34 textures were generated, so WHICH one this quad
            // samples is the question, not whether textures exist.
            // This portlib's <GLES2/gl2.h> declares neither glGetTexLevelParameteriv nor
        // GL_TEXTURE_WIDTH/HEIGHT, and pulling in <GLES3/gl3.h> for two enum
        // values is not worth the header fight. Resolve the entry point the
        // same way guest_view_probe.cpp resolves glBlitFramebuffer.
        using PFNGetTexLevel = void (*)(GLenum, GLint, GLenum, GLint*);
        static PFNGetTexLevel getTexLevel = nullptr;
        static bool getTexLevelResolved = false;
        if (!getTexLevelResolved) {
            getTexLevelResolved = true;
            getTexLevel = reinterpret_cast<PFNGetTexLevel>(
                eglGetProcAddress("glGetTexLevelParameteriv"));
        }
        GLint tw = -1;
        GLint th = -1;
        if (tex > 0 && getTexLevel != nullptr) {
            static constexpr GLenum kTexWidth = 0x1000;   // ES2 GL_TEXTURE_WIDTH
            static constexpr GLenum kTexHeight = 0x1001;  // ES2 GL_TEXTURE_HEIGHT
            getTexLevel(GL_TEXTURE_2D, 0, kTexWidth, &tw);
            getTexLevel(GL_TEXTURE_2D, 0, kTexHeight, &th);
        }
            char diag[288];
            __builtin_snprintf(diag, sizeof(diag),
                "drawArrays #%u mode=0x%x first=%d count=%d tex=%d texWH=%dx%d"
                " prog=%d vp=%u,%u,%u,%u fbo=%u",
                g_glStats.drawArrays, mode, first, count, tex, tw, th, prog,
                g_glStats.lastViewport[0], g_glStats.lastViewport[1],
                g_glStats.lastViewport[2], g_glStats.lastViewport[3],
                g_glStats.lastBoundFramebuffer);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "draw-detail", 249, 0, diag);
        }
    }
    ::glDrawArrays(mode, first, count);
}

void guestViewTracedGlDrawElements(GLenum mode, GLsizei count, GLenum type,
                                   const void* indices) {
    if (g_glStatsEnabled) {
        ++g_glStats.drawElements;
        g_glStats.lastDrawMode = mode;
        g_glStats.lastDrawElementsCount = static_cast<uint32_t>(count);
        if (g_glStats.drawElements <= 8) {
            guestViewWideLog("drawElements",
                             (static_cast<unsigned long long>(mode) << 32) |
                                 static_cast<unsigned>(count),
                             static_cast<unsigned long long>(type));
        }
    }
    ::glDrawElements(mode, count, type, indices);
}

void guestViewTracedGlTexImage2D(GLenum target, GLint level, GLint internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLenum format, GLenum type, const void* pixels) {
    if (g_glStatsEnabled) {
        ++g_glStats.texImage2D;
        // 512x0 is the broken guest upload (empty atlas slot). Replace with a
        // valid 128x128 magenta/black image so the texture object is complete.
        if ((width <= 0 || height <= 0) && pixels == nullptr) {
            ensureHostFallbackTexture();
            width = 128;
            height = 128;
            // Reuse the already-uploaded fallback as the source of truth:
            // upload a solid color derived from the same magenta pattern.
            static unsigned char fill[128 * 128 * 4];
            for (int y = 0; y < 128; ++y) {
                for (int x = 0; x < 128; ++x) {
                    const bool dark = ((x / 16) + (y / 16)) % 2 == 0;
                    unsigned char* p = &fill[(y * 128 + x) * 4];
                    p[0] = dark ? 20 : 255;
                    p[1] = dark ? 20 : 0;
                    p[2] = dark ? 20 : 255;
                    p[3] = 255;
                }
            }
            pixels = fill;
            guestViewWideLog("texImage2D-fixup",
                             (static_cast<unsigned long long>(width) << 16) |
                                 static_cast<unsigned>(height),
                             static_cast<unsigned long long>(internalformat));
        }
        else if (g_glStats.texImage2D <= 6) {
            guestViewWideLog("texImage2D",
                             (static_cast<unsigned long long>(width) << 16) |
                                 static_cast<unsigned>(height),
                             static_cast<unsigned long long>(internalformat),
                             pixels != nullptr ? "pixels" : "null");
        }
    }
    ::glTexImage2D(target, level, internalformat, width, height, border,
                   format, type, pixels);
}

void guestViewTracedGlCompressedTexImage2D(GLenum target, GLint level,
                                           GLenum internalformat, GLsizei width,
                                           GLsizei height, GLint border,
                                           GLsizei imageSize, const void* data) {
    if (g_glStatsEnabled) {
        ++g_glStats.compressedTexImage2D;
        if (g_glStats.compressedTexImage2D <= 8) {
            guestViewWideLog("ctexImage2D",
                             (static_cast<unsigned long long>(width) << 16) |
                                 static_cast<unsigned>(height),
                             static_cast<unsigned long long>(internalformat),
                             data != nullptr ? "data" : "null");
        }
    }
    ::glCompressedTexImage2D(target, level, internalformat, width, height,
                             border, imageSize, data);
}

void guestViewTracedGlBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (g_glStatsEnabled) {
        ++g_glStats.bindFramebuffer;
        g_glStats.lastBoundFramebuffer = framebuffer;
        // gui15 refuted the scissor explanation by logging the rectangle
        // instead of just the count: glScissor x=0 y=0 w=1280 h=720 covers
        // the whole screen and clips nothing, while the full-buffer readback
        // is still lit=0 of 921600 at both frame 30 and frame 3000.
        //
        // What survives that refutation: fbo=23/0 with fboChecks=0. The guest
        // binds a framebuffer 23 times and NEVER checks completeness. Our
        // readback samples the DEFAULT framebuffer (fb=0). If the guest draws
        // into an FBO and never blits it out, the screen stays black while
        // everything else -- 128 mmaps, 78 opens, 95 programs, 191 compressed
        // textures -- is genuinely fine.
        //
        // So name the target. The full-buffer readback is the independent check:
        // if fb 0 is black and this is non-zero, the guest is rendering into a
        // texture nobody presents.
        if (g_glStats.bindFramebuffer <= 4 || framebuffer != g_glStats.lastBoundFramebuffer) {
            GLenum status = GL_NONE;
            if (framebuffer != 0) {
                status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
                g_glStats.fboChecks += 1;
                g_glStats.lastFboStatus = status;
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    ++g_glStats.fboIncomplete;
                }
            }
            char diag[224];
            __builtin_snprintf(diag, sizeof(diag),
                "bindFramebuffer target=0x%x fbo=%u status=0x%x vp=%u,%u,%u,%u"
                " sc=%u,%u,%u,%u draws=%u",
                target, framebuffer, status,
                g_glStats.lastViewport[0], g_glStats.lastViewport[1],
                g_glStats.lastViewport[2], g_glStats.lastViewport[3],
                g_glStats.lastScissor[0], g_glStats.lastScissor[1],
                g_glStats.lastScissor[2], g_glStats.lastScissor[3],
                g_glStats.drawArrays);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fbo-bind", 249, 0, diag);
        }
    }
    ::glBindFramebuffer(target, framebuffer);
}

void guestViewTracedGlGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    if (g_glStatsEnabled) {
        g_glStats.genFramebuffers += static_cast<uint32_t>(n);
    }
    ::glGenFramebuffers(n, framebuffers);
}

void guestViewTracedGlFramebufferTexture2D(GLenum target, GLenum attachment,
                                           GLenum textarget, GLuint texture,
                                           GLint level) {
    if (g_glStatsEnabled) {
        ++g_glStats.framebufferTexture2D;
    }
    ::glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void guestViewTracedGlUseProgram(GLuint program) {
    if (g_glStatsEnabled) {
        ++g_glStats.useProgram;
        if (g_glStats.useProgram <= 6) {
            guestViewWideLog("useProgram", program, g_glStats.useProgram);
        }
    }
    ::glUseProgram(program);
}

void guestViewTracedGlViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (g_glStatsEnabled) {
        ++g_glStats.viewports;
        g_glStats.lastViewport[0] = static_cast<uint32_t>(x);
        g_glStats.lastViewport[1] = static_cast<uint32_t>(y);
        g_glStats.lastViewport[2] = static_cast<uint32_t>(width);
        g_glStats.lastViewport[3] = static_cast<uint32_t>(height);
    }
    ::glViewport(x, y, width, height);
}

void guestViewTracedGlScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (g_glStatsEnabled) {
        ++g_glStats.scissors;
        // The rectangle itself, not just the count. gui14 measured the whole
        // back buffer twice -- EARLY frame 30 and LATE frame 3000 -- and got
        // lit=0 of 921600 both times, with every one of the 921,600 red
        // channels in the lowest bin. The guest reports sc=1 for the entire
        // session against a full-screen lastViewport=0,0,1280,720, so a single
        // scissor box that is never resized is the cheapest explanation left:
        // enable GL_SCISSOR_TEST, discard every fragment outside it, and the
        // buffer stays exactly as glClear left it.
        //
        // This is the measurement that settles it either way. A box that covers
        // the screen refutes scissor; a box that does not confirms it.
        g_glStats.lastScissor[0] = static_cast<uint32_t>(x);
        g_glStats.lastScissor[1] = static_cast<uint32_t>(y);
        g_glStats.lastScissor[2] = static_cast<uint32_t>(width);
        g_glStats.lastScissor[3] = static_cast<uint32_t>(height);
        char diag[192];
        __builtin_snprintf(diag, sizeof(diag),
            "glScissor x=%d y=%d w=%d h=%d (call #%u, viewport=%u,%u,%u,%u)",
            x, y, width, height, g_glStats.scissors,
            g_glStats.lastViewport[0], g_glStats.lastViewport[1],
            g_glStats.lastViewport[2], g_glStats.lastViewport[3]);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "scissor", 249, 0, diag);
    }
    ::glScissor(x, y, width, height);
}

GLenum guestViewTracedGlCheckFramebufferStatus(GLenum target) {
    const GLenum status = ::glCheckFramebufferStatus(target);
    if (g_glStatsEnabled) {
        ++g_glStats.fboChecks;
        g_glStats.lastFboStatus = status;
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            ++g_glStats.fboIncomplete;
        }
    }
    return status;
}

void guestViewTracedGlBindBuffer(GLenum target, GLuint buffer) {
    if (g_glStatsEnabled) {
        ++g_glStats.bindBuffer;
        g_glStats.lastBoundBuffer = buffer;
        if (buffer == 0) {
            ++g_glStats.bindBufferZero;
        }
    }
    ::glBindBuffer(target, buffer);
}

void guestViewTracedGlBindTexture(GLenum target, GLuint texture) {
    if (g_glStatsEnabled) {
        ++g_glStats.bindTexture;
        g_glStats.lastBoundTexture = texture;
        if (texture == 0) {
            ++g_glStats.bindTextureZero;
            ensureHostFallbackTexture();
            if (g_hostFallbackTex != 0) {
                texture = g_hostFallbackTex;
                ++g_hostTexInject;
                g_glStats.hostTexInject = g_hostTexInject;
                g_glStats.lastBoundTexture = texture;
            }
        }
        else if (g_glStats.bindTexture <= 8) {
            guestViewWideLog("bind-tex", texture, target);
        }
    }
    ::glBindTexture(target, texture);
}

void guestViewTracedGlGenTextures(GLsizei n, GLuint* textures) {
    ::glGenTextures(n, textures);
    if (g_glStatsEnabled) {
        g_glStats.genTextures += static_cast<uint32_t>(n > 0 ? n : 0);
        if (n > 0 && textures != nullptr) {
            g_glStats.lastGenTexture = textures[0];
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "gen-textures",
                                static_cast<unsigned long long>(g_glStats.genTextures),
                                textures[0]);
        }
    }
}

void guestViewTracedGlGenBuffers(GLsizei n, GLuint* buffers) {
    ::glGenBuffers(n, buffers);
    if (g_glStatsEnabled) {
        g_glStats.genBuffers += static_cast<uint32_t>(n > 0 ? n : 0);
        if (n > 0 && buffers != nullptr) {
            g_glStats.lastGenBuffer = buffers[0];
            if (g_glStats.genBuffers <= 8) {
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "gen-buffers",
                                    static_cast<unsigned long long>(g_glStats.genBuffers),
                                    buffers[0]);
            }
        }
    }
}

void guestViewTracedGlVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                          GLboolean normalized, GLsizei stride,
                                          const void* pointer) {
    if (g_glStatsEnabled) {
        ++g_glStats.vertexAttribPointer;
    }
    ::glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

// Read-only sample of the guest's path-resolver gate.
//
// ELF 0x12bf4dc, the resolver the guest runs on every asset name:
//   ldrb w8,[x0,#1] ; cmp w8,#':' ; b.ne 0x12bf548     -- volume prefix?
//   0x12bf548: adrp x8,#0x1f78000 ; ldr x8,[x8,#0xd30] ; ldrb w8,[x8]
//   0x12bf554: cbnz w8, 0x12bf5fc                    -- gate byte set -> ok
//   0x12bf698: ldrb w8,[x20] ; cmp w8,#'/' ; b.ne 0x12bf6d8
//   0x12bf6d8: mov w0, wzr                            -- returns 0, failure
//
// The gate byte is the FIRST BYTE of whatever *(0x1f78d30) designates, and it is
// not necessarily a string: sampled in the frame loop, that address held
// [01 00 00 00 00 00 00 00 | ptr | ptr | ptr] -- a leading flag followed by
// three heap pointers. Reading those bytes as a NUL-terminated string and
// reporting the result as "the content root is empty" is precisely the mistake
// this comment exists to prevent, so it prints the byte and the raw head, never
// a string built by guessing a layout.
void noteContentRootGate(const char* site, uintptr_t renderTarget) {
    static constexpr uint64_t kRenderExportElfVa = 0x12B3614ULL;
    static constexpr uint64_t kContentRootElfVa = 0x1F78D30ULL;
    if (renderTarget == 0) {
        return;
    }
    const uint64_t bias = static_cast<uint64_t>(renderTarget) - kRenderExportElfVa;
    const auto slot = static_cast<uintptr_t>(bias + kContentRootElfVa);
    const auto slotValue = *reinterpret_cast<const volatile uintptr_t*>(slot);
    unsigned long long head[2] = {0, 0};
    unsigned gateByte = 0xFFFFFFFFu;
    if (slotValue != 0) {
        const auto obj = static_cast<uintptr_t>(slotValue);
        head[0] = *reinterpret_cast<const volatile unsigned long long*>(obj);
        head[1] = *reinterpret_cast<const volatile unsigned long long*>(obj + 8);
        gateByte = *reinterpret_cast<const volatile unsigned char*>(obj);
    }
    char line[208];
    __builtin_snprintf(line, sizeof(line),
                      "contentroot-gate: site=%s slot=0x%llx value=0x%llx"
                      " gatebyte=0x%02x(%s) head0=%016llx head1=%016llx",
                      site, static_cast<unsigned long long>(slot),
                      static_cast<unsigned long long>(slotValue), gateByte,
                      gateByte == 0xFF ? "NOSLOT"
                                       : (gateByte == 0 ? "ZERO-resolver-returns-0"
                                                        : "SET-ok"),
                      head[0], head[1]);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "contentroot-gate", 300, gateByte, line);
}

void guestViewNoteAndroidLog(const char* text) {
    // THE TEARDOWN REPAIR'S ANCHOR MOVED HERE.
    //
    // It used to live in androidLogVPrint, on the argument that the guest's NDK
    // shim 0x109fa18 ends in `bl __android_log_vprint@plt`. That is true of ONE
    // of the three ways a guest log reaches this function, and an unconditional
    // entry trace in front of the repair proved it with no ambiguity: over a run
    // that produced 687 guest log lines, the repair was entered exactly THREE
    // times, with settled=0 and t_in_attempt=0 on all three -- so nothing was
    // being eaten by an early return. Those three anchors matched the first
    // three guest log lines and then nothing. The other 684 lines arrive via
    // jniCallVoidMethod / jniCallStaticVoidMethodV.
    //
    // Hooking here instead is total: this is the ONLY function that can emit
    // phase=guest-log-any, so every guest log passes through it no matter which
    // import the caller used.
    //
    // And the anchor is still safe. The disassembly of the built ELF shows this
    // function has NO frame record at entry (cbz x0, ... at 1ebe0; the
    // stp x29,x30 lands 0x1a8 bytes later, on a conditional path), which is
    // exactly why reading x29 HERE yields the CALLER's frame pointer rather than
    // a half-built local one. That was the reason it was rejected as an anchor
    // when androidLogVPrint was available; on its own it is the correct place,
    // and the walk already tolerates host frames between here and the guest.
    {
        uintptr_t anchorFp = 0;
        __asm__("mov %0, x29" : "=r"(anchorFp));
        guestTeardownRepairOnLog(anchorFp);
    }
    if (text == nullptr) {
        return;
    }
    // Item 3. The predicate below is a size-control heuristic, and it swallowed
    // exactly the evidence this port needed: every guest __android_log_print
    // line from the Cloudcell document subsystem was dropped before reaching
    // the log, so the guest's own account of its failure was discarded. Two
    // bounded changes:
    //   1. keep lines whose text carries the Cloudcell document tag (ctd.), and
    //   2. keep a fixed quota of lines whatever their content, so no filter can
    //      again hide an entire subsystem.
    // The existing per-line budget (the 120-byte hash window) is unchanged.
    // Note guestViewNoteAndroidLog receives the FORMAT string only; the tag
    // never reaches this predicate, so rule 1 depends on the guest naming
    // ctd.dat in its message text. Rule 2 is the backstop for when it does not.
    bool interesting = false;
    bool cloudcell = false;
    for (const char* p = text; *p != '\0'; ++p) {
        if ((p[0] == 'V' && p[1] == 'I' && p[2] == 'E' && p[3] == 'W') ||
            (p[0] == 's' && p[1] == 'p' && p[2] == 'l' && p[3] == 'a') ||
            (p[0] == 'h' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e') ||
            (p[0] == 'o' && p[1] == 'r' && p[2] == 'i' && p[3] == 'e')) {
            interesting = true;
            break;
        }
        if (p[0] == 'c' && p[1] == 't' && p[2] == 'd' && p[3] == '.') {
            cloudcell = true;
            break;
        }
    }
    uint32_t hash = 2166136261u;
    for (const char* p = text; *p != '\0' && p < text + 120; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    if (interesting || cloudcell) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "guest-log", 76, hash, text);
        return;
    }
    // Content-independent quota. The guest boot is deterministic, so this count
    // is reproducible run to run; 200 bounds the growth of the archived run's
    // 570-line log by ~35% from a stream whose volume is not yet measured, and
    // it only ever ADDS lines that were previously dropped, so the VIEW / splash
    // evidence above keeps its existing share. Once the cap is hit, the filter
    // says so in at most 4 more lines, so a dropped line is never mistaken for
    // a guest that printed nothing.
    static unsigned s_quota = 0;
    static unsigned s_dropReports = 0;
    if (s_quota < kAndroidLogAnyBudget) {
        ++s_quota;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "guest-log-any", s_quota, hash, text);
    }
    else if (s_dropReports < 4) {
        ++s_dropReports;
        char details[96];
        std::snprintf(details, sizeof(details),
                      "dropped_by_content_filter reports=%u cap=%u reason=quota_spent",
                      s_dropReports, kAndroidLogAnyBudget);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "guest-log-drop", s_dropReports, 0, details);
    }
}

void guestViewWideLog(const char* phase, unsigned long long a,
                      unsigned long long b, const char* details) {
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        phase, a, b, details);
}

} // namespace rr3
