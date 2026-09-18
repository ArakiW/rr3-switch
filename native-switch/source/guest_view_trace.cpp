#include "rr3/guest_view_trace.hpp"

#include "rr3/recovery_guard.hpp"

#include <EGL/egl.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace {

const char* g_progressPath = nullptr;
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
uintptr_t g_second40Caller = 0;
uintptr_t g_copyCaller = 0;
uintptr_t g_copyDestination = 0;
uintptr_t g_copySource = 0;
size_t g_copyLength = 0;
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
                     unsigned long long target) {
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
    writeGuestBootProgress(g_progressPath, phase, stage, target,
                           stage >= 30 ? details : nullptr);
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

void* guestViewTracedMemcpy(void* destination, const void* source, size_t length) {
    if (!g_enabled || g_checkpointInProgress || !guestViewTraceReached(30) ||
        g_postStage30MemcpyReturned) {
        return std::memcpy(destination, source, length);
    }

    const uintptr_t caller = reinterpret_cast<uintptr_t>(
        __builtin_return_address(0));
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
    checkpointPhase(phase, stage, 0);
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
        if (g_glStats.drawArrays <= 3) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "draw-arrays",
                                static_cast<unsigned long long>(g_glStats.drawArrays),
                                (static_cast<unsigned long long>(mode) << 32) |
                                    static_cast<unsigned>(count));
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

void guestViewNoteAndroidLog(const char* text) {
    if (text == nullptr) {
        return;
    }
    bool interesting = false;
    for (const char* p = text; *p != '\0'; ++p) {
        if ((p[0] == 'V' && p[1] == 'I' && p[2] == 'E' && p[3] == 'W') ||
            (p[0] == 's' && p[1] == 'p' && p[2] == 'l' && p[3] == 'a') ||
            (p[0] == 'h' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e') ||
            (p[0] == 'o' && p[1] == 'r' && p[2] == 'i' && p[3] == 'e')) {
            interesting = true;
            break;
        }
    }
    if (!interesting) {
        return;
    }
    uint32_t hash = 2166136261u;
    for (const char* p = text; *p != '\0' && p < text + 120; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "guest-log", 76, hash, text);
}

void guestViewWideLog(const char* phase, unsigned long long a,
                      unsigned long long b, const char* details) {
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        phase, a, b, details);
}

} // namespace rr3
