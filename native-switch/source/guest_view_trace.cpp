#include "rr3/guest_view_trace.hpp"

#include "rr3/gfx_backend.hpp"
#include "rr3/gfx_text_probe.hpp"
#include "rr3/guest_filesystem.hpp"
#include "rr3/guest_frame_probe.hpp"
#include "rr3/guest_teardown_repair.hpp"
#include "rr3/recovery_guard.hpp"

#include <EGL/egl.h>
#include <switch.h>

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

// THE CONTEXT-LESS GL CALL COUNTER.
//
// It is declared HERE, above checkpointPhase(), because the count has to ride
// along with every progress checkpoint: the summary line written at the end
// of the run is useless for the case this instrument exists for, because the
// device froze INSIDE onViewChangedJNI and never reached that line.  The
// progress file's LAST LINE is the only thing that survives a freeze.
std::atomic<uint32_t> g_glNoContextCount{0};
std::atomic<uint32_t> g_glNoContextRecorded{0};
std::atomic<bool> g_glNoContextBudgetSpent{false};
// The thread that owns the EGL context, so that a call from anywhere else is
// recognisable as "a call from a thread the context is not on".
std::atomic<uint32_t> g_glContextOwnerHandle{0};

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
            " gltrace=shader-v1 gl_caller=%016llx gl_object=%x"
            " gl_no_ctx_calls=%u gl_no_ctx_recorded=%u",
            static_cast<unsigned long long>(g_second40Caller),
            static_cast<unsigned long long>(g_copyCaller),
            static_cast<unsigned long long>(g_copyDestination),
            static_cast<unsigned long long>(g_copySource),
            static_cast<unsigned long long>(g_copyLength),
            static_cast<unsigned long long>(g_freeCaller),
            static_cast<unsigned long long>(g_freeAddress),
            static_cast<unsigned long long>(g_glCaller), g_glObject,
            g_glNoContextCount.load(std::memory_order_relaxed),
            g_glNoContextRecorded.load(std::memory_order_relaxed));
    } else {
        // THE COUNT RIDES ALONG WITH EVERY CHECKPOINT, not only with the
        // stage>=30 ones.  The progress file keeps ONE line and overwrites it,
        // so the line that survives a freeze is whichever checkpoint ran
        // last -- and if the count were only on the stage>=30 checkpoints, a
        // freeze before the first of those would leave a line with no count
        // on it, which is exactly the case where the number is wanted.
        __builtin_snprintf(details, sizeof(details),
            "gl_no_ctx_calls=%u gl_no_ctx_recorded=%u",
            g_glNoContextCount.load(std::memory_order_relaxed),
            g_glNoContextRecorded.load(std::memory_order_relaxed));
    }
    // Only the blocked-wait breadcrumb passes durable=false: it is the one
    // progress write that repeats on a guest-blocking timer, and it must not
    // pay for the whole-card fsync that the durable stage checkpoints need.
    if (durable) {
        writeGuestBootProgress(g_progressPath, phase, stage, target, details);
    } else {
        writeGuestBootProgressNoSync(g_progressPath, phase, stage, target, details);
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

// ---------------------------------------------------------------------------
// A GL CALL FROM A THREAD WITH NO EGL CONTEXT.
//
// MEASURED 2026-11-09 on the console: the whole system froze solid about
// 2.8 minutes into the port's life.  The guest had started, it had created
// seven framebuffers (all GL_FRAMEBUFFER_COMPLETE), it had NOT rendered a
// single frame, and it died inside its own onViewChangedJNI with the
// progress file's last line reading "view-sync-blocked-cond-wait".  The last
// GL thing the tracer saw was a shader program.
//
// THE BLIND SPOT THIS CLOSES.  `g_enabled` above is thread_local, and it is
// only turned on by the thread that calls enableGuestViewGlTrace() -- the
// main thread.  So every GL call a guest WORKER thread makes is invisible:
// the tracers take their early-out and forward straight to the driver.
//
// WHY THAT MATTERS ON HARDWARE AND NOT ON THE EMULATOR.  The port makes the
// EGL context current ON THE MAIN THREAD ONLY, and EGL binds a context to
// one thread at a time, so a guest worker thread has NO current context.
// On the emulator's software Mesa a context-less GL call is a harmless
// no-op; on the real NVN driver it reaches driver internals with no
// dispatch table, and a wedged NVN channel takes the compositor -- and
// therefore the whole console -- with it.  The emulator cannot show this,
// because the emulator never has the problem.
//
// SO THIS IS A NARROW OBSERVER, NOT A GLOBAL TRACE.  Making g_enabled
// global would turn on the existing checkpoint writer (an fsync per
// checkpoint) on every worker thread, and this project has already
// measured that a diagnostic which becomes the loudest thing in the system
// is a second outage: 2026-11-09's own fix grew the guest log to 186 MB
// in four minutes before it was given a total budget.
//
// What this does instead: a boolean check (is there a current context?) and,
// only when the answer is NO, one bounded record and one log line.  The
// line is written BEFORE the real GL call, so if that call is what wedges
// the GPU, the evidence is already on the card.
// ---------------------------------------------------------------------------
constexpr size_t kGlNoContextRecords = 32;

struct GlNoContextRecord {
    uint32_t threadHandle{};
    uintptr_t callerPc{};
    uintptr_t eglContext{};
    const char* fn{};
};

GlNoContextRecord g_glNoContextRecords[kGlNoContextRecords];
std::atomic<uint32_t> g_glNoContextRecordIndex{0};

// THE OBSERVER.  Called from the GL tracers BEFORE the real call.
void noteGlCallWithoutContext(const char* fn, uintptr_t callerPc) {
    static thread_local bool s_inObserver = false;
    if (s_inObserver) {
        return;
    }
    // The check has to be cheap: it runs on every traced GL call.
    if (::eglGetCurrentContext() != EGL_NO_CONTEXT) {
        return;
    }
    s_inObserver = true;

    const uint32_t seen = g_glNoContextCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t slot = g_glNoContextRecordIndex.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kGlNoContextRecords) {
        if (!g_glNoContextBudgetSpent.exchange(true, std::memory_order_relaxed)) {
            char details[160];
            __builtin_snprintf(details, sizeof(details),
                               "budget=%u calls=%u "
                               "note=the_observer_stopped_recording_but_the_calls_continue",
                               static_cast<unsigned>(kGlNoContextRecords), seen);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "gl-no-context-budget", 0, seen, details);
        }
        s_inObserver = false;
        return;
    }

    g_glNoContextRecords[slot].threadHandle = threadGetCurHandle();
    g_glNoContextRecords[slot].callerPc = callerPc;
    g_glNoContextRecords[slot].eglContext = 0;
    g_glNoContextRecords[slot].fn = fn;
    g_glNoContextRecorded.fetch_add(1, std::memory_order_relaxed);

    const uint32_t owner = g_glContextOwnerHandle.load(std::memory_order_relaxed);
    const uint32_t self = threadGetCurHandle();
    char details[256];
    __builtin_snprintf(details, sizeof(details),
                       "fn=%s caller_pc=0x%llx egl_ctx=0x%llx thread=0x%x "
                       "context_owner_thread=0x%x is_owner=%d calls=%u",
                       fn != nullptr ? fn : "?",
                       (unsigned long long)callerPc,
                       (unsigned long long)0,
                       self, owner,
                       (owner != 0 && owner == self) ? 1 : 0, seen);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "gl-no-context", seen, self, details);
    s_inObserver = false;
}

} // namespace

// THE TWO PUBLIC READ-OUTS.  They live OUTSIDE the anonymous namespace on
// purpose: the caller is in another translation unit
// (guest_view_probe.cpp), so they need external linkage.
//
// They are written so that a count of ZERO is a fact, not an absence: the
// caller prints the number every run, so "the guest made no context-less GL
// call" and "the observer never ran" cannot be mistaken for each other.
uint32_t guestViewGlNoContextCount() {
    return g_glNoContextCount.load(std::memory_order_relaxed);
}

uint32_t guestViewGlNoContextRecorded() {
    return g_glNoContextRecorded.load(std::memory_order_relaxed);
}

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

// ---------------------------------------------------------------------------
// GL Failure Budget and Diagnostics.
//
// MEASURED: Per-call logging in the render loop severely penalizes frame rate.
// The failure diagnostics for guest shaders and texture uploads are therefore:
//   1. COMPLETELY SILENT on success (no log line for successful compiles or uploads).
//   2. Logged ONLY on failure (non-zero compile/link error, glGetError != NO_ERROR).
//   3. Strictly rate-limited to a per-process budget of 32 lines. Once the budget
//      is exhausted, a single warning line is emitted and no further disk I/O occurs.
// ---------------------------------------------------------------------------
constexpr uint32_t kGlFailureLogBudget = 32;
static std::atomic<uint32_t> g_glFailureLogCount{0};
static std::atomic<bool> g_glFailureBudgetSpent{false};

static void reportGlFailure(const char* tag, unsigned long long ordinal,
                            unsigned long long target, const char* details) {
    uint32_t count = g_glFailureLogCount.fetch_add(1, std::memory_order_relaxed);
    if (count < kGlFailureLogBudget) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            tag, ordinal, target, details);
        if (count + 1 == kGlFailureLogBudget) {
            g_glFailureBudgetSpent.store(true, std::memory_order_relaxed);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "gl-fail-budget", kGlFailureLogBudget, 0,
                                "maximum GL failure log budget reached; suppressing further lines");
        }
    }
}

struct TrackedShaderEntry {
    GLuint shader;
    GLuint program;
    GLenum type;
};
constexpr size_t kMaxTrackedShaders = 256;
static TrackedShaderEntry g_trackedShaders[kMaxTrackedShaders]{};
static size_t g_trackedShaderCount = 0;

static void recordShaderCreated(GLuint shader, GLenum type) {
    if (shader == 0) return;
    for (size_t i = 0; i < g_trackedShaderCount; ++i) {
        if (g_trackedShaders[i].shader == shader) {
            g_trackedShaders[i].type = type;
            return;
        }
    }
    if (g_trackedShaderCount < kMaxTrackedShaders) {
        g_trackedShaders[g_trackedShaderCount++] = {shader, 0, type};
    }
}

static void recordShaderAttached(GLuint program, GLuint shader) {
    if (shader == 0) return;
    for (size_t i = 0; i < g_trackedShaderCount; ++i) {
        if (g_trackedShaders[i].shader == shader) {
            g_trackedShaders[i].program = program;
            return;
        }
    }
    if (g_trackedShaderCount < kMaxTrackedShaders) {
        g_trackedShaders[g_trackedShaderCount++] = {shader, program, 0};
    }
}

static GLuint findProgramForShader(GLuint shader) {
    for (size_t i = 0; i < g_trackedShaderCount; ++i) {
        if (g_trackedShaders[i].shader == shader) {
            return g_trackedShaders[i].program;
        }
    }
    return 0;
}

// THE PORT'S OWN BINDS.  Set while the port binds a framebuffer for its own
// reasons (the blit into the window), so the tracer does not redirect them.
static bool g_hostBinding = false;

void guestViewSetHostBinding(bool hostBinding) {
    g_hostBinding = hostBinding;
}

bool guestViewHostBinding() {
    return g_hostBinding;
}

// THE SIZE OF THE GUEST'S RENDER TARGETS, FROM THE GUEST'S OWN CALLS.
//
// Two small tables: the size of every texture the guest creates, and the
// texture every framebuffer has attached to it.  Together they give the size
// of the guest's scene framebuffer -- which is the number the present path
// has to scale, and the number that says whether the view is bigger than the
// screen.
//
// No GL query is involved on purpose.  The devkitPro GLES2 header has no
// glGetTexLevelParameteriv, and the alternative (binding the texture to a
// texture unit to ask it) would change state in the middle of the guest's
// frame.  The guest already told us everything, in calls we are wrapping
// anyway.
namespace {
struct GuestTextureSize {
    uint32_t id;
    uint32_t width;
    uint32_t height;
};
constexpr size_t kMaxGuestTextures = 64;
GuestTextureSize g_guestTextures[kMaxGuestTextures];
size_t g_guestTextureCount = 0;

struct GuestFramebufferAttachment {
    uint32_t fbo;
    uint32_t texture;
};
constexpr size_t kMaxGuestAttachments = 32;
GuestFramebufferAttachment g_guestAttachments[kMaxGuestAttachments];
size_t g_guestAttachmentCount = 0;

void guestRememberTextureSize(uint32_t id, uint32_t width, uint32_t height) {
    for (size_t i = 0; i < g_guestTextureCount; ++i) {
        if (g_guestTextures[i].id == id) {
            g_guestTextures[i].width = width;
            g_guestTextures[i].height = height;
            return;
        }
    }
    if (g_guestTextureCount < kMaxGuestTextures) {
        g_guestTextures[g_guestTextureCount].id = id;
        g_guestTextures[g_guestTextureCount].width = width;
        g_guestTextures[g_guestTextureCount].height = height;
        g_guestTextureCount += 1;
    }
}

void guestRememberAttachment(uint32_t fbo, uint32_t texture) {
    for (size_t i = 0; i < g_guestAttachmentCount; ++i) {
        if (g_guestAttachments[i].fbo == fbo) {
            g_guestAttachments[i].texture = texture;
            return;
        }
    }
    if (g_guestAttachmentCount < kMaxGuestAttachments) {
        g_guestAttachments[g_guestAttachmentCount].fbo = fbo;
        g_guestAttachments[g_guestAttachmentCount].texture = texture;
        g_guestAttachmentCount += 1;
    }
}

uint32_t guestTextureWidth(uint32_t texture) {
    for (size_t i = 0; i < g_guestTextureCount; ++i) {
        if (g_guestTextures[i].id == texture) {
            return g_guestTextures[i].width;
        }
    }
    return 0;
}

uint32_t guestTextureHeight(uint32_t texture) {
    for (size_t i = 0; i < g_guestTextureCount; ++i) {
        if (g_guestTextures[i].id == texture) {
            return g_guestTextures[i].height;
        }
    }
    return 0;
}
} // namespace

uint32_t guestFramebufferTextureWidth(uint32_t fbo) {
    for (size_t i = 0; i < g_guestAttachmentCount; ++i) {
        if (g_guestAttachments[i].fbo == fbo) {
            return guestTextureWidth(g_guestAttachments[i].texture);
        }
    }
    return 0;
}

uint32_t guestFramebufferTextureHeight(uint32_t fbo) {
    for (size_t i = 0; i < g_guestAttachmentCount; ++i) {
        if (g_guestAttachments[i].fbo == fbo) {
            return guestTextureHeight(g_guestAttachments[i].texture);
        }
    }
    return 0;
}

void enableGuestViewGlTrace(const char* progressPath) {
    g_progressPath = progressPath;
    g_lastStage = 0;
    g_seenStages = 0;
    // THE THREAD THAT OWNS THE EGL CONTEXT.  The port makes the context
    // current on this thread only, so a GL call from any other thread has no
    // context -- and that is the fact the no-context observer reports.
    g_glContextOwnerHandle.store(threadGetCurHandle(), std::memory_order_relaxed);
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
    noteGlCallWithoutContext("glGetString",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
        if (rr3::gfxPerCallLog() && (firstFew || milestone)) {
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
    // BEFORE the early-out: a worker thread takes exactly this path, and a
    // worker thread is the one with no EGL context.  See the observer's
    // comment above.
    noteGlCallWithoutContext("glCreateShader",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (!g_enabled || g_checkpointInProgress) {
        const GLuint shader = ::glCreateShader(type);
        recordShaderCreated(shader, type);
        return shader;
    }
    g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    g_glObject = 0;
    checkpointPhase("view-gl-create-shader-enter", 38, type);
    const GLuint shader = ::glCreateShader(type);
    recordShaderCreated(shader, type);
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
    noteGlCallWithoutContext("glShaderSource",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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

void guestViewTracedGlAttachShader(GLuint program, GLuint shader) {
    noteGlCallWithoutContext("glAttachShader",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    recordShaderAttached(program, shader);
    ::glAttachShader(program, shader);
}

void guestViewTracedGlCompileShader(GLuint shader) {
    // BEFORE the early-out: see the observer's comment.  A shader compiled on
    // a thread with no context is the single most suspicious thing a guest
    // can do to an NVN driver.
    noteGlCallWithoutContext("glCompileShader",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    const bool probeTrace = g_enabled && !g_checkpointInProgress;
    if (probeTrace) {
        g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        g_glObject = shader;
        checkpointPhase("view-gl-compile-shader-enter", 42, shader);
    }
    ::glCompileShader(shader);
    GLint compiled = 0;
    ::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        ++g_glStats.shaderCompileFails;
        g_glStats.lastFailedShader = shader;

        GLint logLen = 0;
        ::glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        char infoLog[512] = {0};
        if (logLen > 0) {
            GLsizei actual = 0;
            ::glGetShaderInfoLog(shader, sizeof(infoLog) - 1, &actual, infoLog);
            for (GLsizei i = 0; i < actual; ++i) {
                if (infoLog[i] == '\r' || infoLog[i] == '\n') {
                    infoLog[i] = ' ';
                }
            }
        }

        const GLuint prog = findProgramForShader(shader);
        char details[640];
        std::snprintf(details, sizeof(details),
                      "shader=%u program=%u status=%d log=\"%s\"",
                      static_cast<unsigned>(shader), static_cast<unsigned>(prog),
                      static_cast<int>(compiled), infoLog[0] ? infoLog : "none");
        reportGlFailure("shader-compile-fail", shader, prog, details);
    }
    if (probeTrace) {
        checkpointPhase("view-gl-compile-shader-return", 43, shader);
    }
}

void guestViewTracedGlLinkProgram(GLuint program) {
    // BEFORE the early-out: see the observer's comment.
    noteGlCallWithoutContext("glLinkProgram",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    const bool probeTrace = g_enabled && !g_checkpointInProgress;
    if (probeTrace) {
        g_glCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        g_glObject = program;
        checkpointPhase("view-gl-link-program-enter", 44, program);
    }
    ::glLinkProgram(program);
    GLint linked = 0;
    ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        ++g_glStats.programLinkFails;
        g_glStats.lastFailedProgram = program;

        GLint logLen = 0;
        ::glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        char infoLog[512] = {0};
        if (logLen > 0) {
            GLsizei actual = 0;
            ::glGetProgramInfoLog(program, sizeof(infoLog) - 1, &actual, infoLog);
            for (GLsizei i = 0; i < actual; ++i) {
                if (infoLog[i] == '\r' || infoLog[i] == '\n') {
                    infoLog[i] = ' ';
                }
            }
        }

        GLint attachedCount = 0;
        GLuint attached[8] = {0};
        ::glGetAttachedShaders(program, 8, &attachedCount, attached);
        char attachedStr[64] = {0};
        if (attachedCount > 0) {
            int off = 0;
            for (GLint i = 0; i < attachedCount && off < static_cast<int>(sizeof(attachedStr)) - 12; ++i) {
                off += std::snprintf(attachedStr + off, sizeof(attachedStr) - off, "%s%u", i > 0 ? "," : "", attached[i]);
            }
        } else {
            std::snprintf(attachedStr, sizeof(attachedStr), "none");
        }

        char details[640];
        std::snprintf(details, sizeof(details),
                      "program=%u shaders=[%s] status=%d log=\"%s\"",
                      static_cast<unsigned>(program), attachedStr,
                      static_cast<int>(linked), infoLog[0] ? infoLog : "none");
        reportGlFailure("gl-link-fail", program, 0, details);
    }
    if (probeTrace) {
        checkpointPhase("view-gl-link-program-return", 45, program);
    }
}

void guestViewTracedGlShaderBinary(GLsizei count, const GLuint* shaders,
                                   GLenum binaryformat, const void* binary,
                                   GLsizei length) {
    noteGlCallWithoutContext("glShaderBinary",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glClear",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.clears;
    }
    ::glClear(mask);
}

void guestViewTracedGlDrawArrays(GLenum mode, GLint first, GLsizei count) {
    noteGlCallWithoutContext("glDrawArrays",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.drawArrays;
        g_glStats.lastDrawMode = mode;
        g_glStats.lastDrawElementsCount = static_cast<uint32_t>(count);
        // DID THIS DRAW LAND IN THE PORT'S OFFSCREEN TARGET?  See the note
        // in guestViewTracedGlDrawElements: the port's blit may only scale a
        // target the guest actually drew into.
        {
            const uint32_t offscreen = guestViewOffscreenTargetFbo();
            if (offscreen != 0 &&
                g_glStats.lastBoundFramebuffer == static_cast<GLuint>(offscreen)) {
                ++g_glStats.drawsIntoOffscreenTarget;
            }
        }
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
    noteGlCallWithoutContext("glDrawElements",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.drawElements;
        g_glStats.lastDrawMode = mode;
        g_glStats.lastDrawElementsCount = static_cast<uint32_t>(count);
        // DID THIS DRAW LAND IN THE PORT'S OFFSCREEN TARGET?
        //
        // The port's blit scales that target into the window, so it may only
        // run when the guest really drew into it this frame.  Otherwise the
        // blit copies a stale or cleared target over the picture the guest
        // drew into the window -- which is exactly what made the screen go
        // flat in q76 and q78.
        {
            const uint32_t offscreen = guestViewOffscreenTargetFbo();
            if (offscreen != 0 &&
                g_glStats.lastBoundFramebuffer == static_cast<GLuint>(offscreen)) {
                ++g_glStats.drawsIntoOffscreenTarget;
            }
        }
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
    noteGlCallWithoutContext("glTexImage2D",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.texImage2D;
        // The size of the texture the guest is creating, against the texture
        // it has bound.  This is how the size of the guest's scene
        // framebuffer is known without asking GL anything.
        guestRememberTextureSize(g_glStats.lastBoundTexture,
                                 static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height));
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
    // Drain any prior error so it is not misattributed to this texture upload
    while (::glGetError() != GL_NO_ERROR) {}

    ::glTexImage2D(target, level, internalformat, width, height, border,
                   format, type, pixels);

    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) {
        ++g_glStats.texUploadFails;
        g_glStats.lastTexUploadError = err;
        char details[256];
        std::snprintf(details, sizeof(details),
                      "op=glTexImage2D target=0x%x level=%d internal=0x%x w=%d h=%d fmt=0x%x type=0x%x err=0x%x",
                      static_cast<unsigned>(target), static_cast<int>(level),
                      static_cast<unsigned>(internalformat),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<unsigned>(format), static_cast<unsigned>(type),
                      static_cast<unsigned>(err));
        reportGlFailure("gl-tex-upload-fail", g_glStats.lastBoundTexture, err, details);
    }
}

void guestViewTracedGlCompressedTexImage2D(GLenum target, GLint level,
                                           GLenum internalformat, GLsizei width,
                                           GLsizei height, GLint border,
                                           GLsizei imageSize, const void* data) {
    noteGlCallWithoutContext("glCompressedTexImage2D",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.compressedTexImage2D;
        // Same as glTexImage2D: the size of the texture being created, so the
        // framebuffer it is attached to has a size that is known without a
        // GL query.
        guestRememberTextureSize(g_glStats.lastBoundTexture,
                                 static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height));
        if (g_glStats.compressedTexImage2D <= 8) {
            guestViewWideLog("ctexImage2D",
                             (static_cast<unsigned long long>(width) << 16) |
                                 static_cast<unsigned>(height),
                             static_cast<unsigned long long>(internalformat),
                             data != nullptr ? "data" : "null");
        }
    }

    while (::glGetError() != GL_NO_ERROR) {}

    ::glCompressedTexImage2D(target, level, internalformat, width, height,
                             border, imageSize, data);

    const GLenum err = ::glGetError();

    // Defect B diagnostic: link every compressed upload to recent served texture
    // and report glGetError() even on success, with a hard budget of 32 lines.
    static unsigned s_ctexDiagLines = 0;
    constexpr unsigned kMaxCtexDiagLines = 32;
    if (s_ctexDiagLines < kMaxCtexDiagLines) {
        ++s_ctexDiagLines;
        ServedTextureRecord recent{};
        const bool hasRecent = getRecentServedTexture(0, &recent);
        const char* internalStr = "unknown";
        if (internalformat == 0x83f0) internalStr = "DXT1_RGB";
        else if (internalformat == 0x83f1) internalStr = "DXT1_RGBA";
        else if (internalformat == 0x83f2) internalStr = "DXT3_RGBA";
        else if (internalformat == 0x83f3) internalStr = "DXT5_RGBA";
        else if (internalformat == 0x8d64) internalStr = "ETC1_RGB8";

        char detail[320];
        std::snprintf(detail, sizeof(detail),
                      "call=%u level=%d w=%d h=%d size=%d internal=0x%x(%s) err=0x%x "
                      "recent_file=\"%s\" recent_fourcc=%.4s recent_dim=%ux%u recent_mips=%u",
                      static_cast<unsigned>(g_glStats.compressedTexImage2D),
                      static_cast<int>(level),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(imageSize),
                      static_cast<unsigned>(internalformat), internalStr,
                      static_cast<unsigned>(err),
                      hasRecent ? recent.path : "-",
                      hasRecent ? recent.fourcc : "-",
                      hasRecent ? recent.width : 0,
                      hasRecent ? recent.height : 0,
                      hasRecent ? recent.mips : 0);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "ctex-upload", s_ctexDiagLines,
                            g_glStats.lastBoundTexture, detail);
        if (s_ctexDiagLines == kMaxCtexDiagLines) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "ctex-upload", 0, 0, "budget exhausted (32 lines)");
        }
    }

    if (err != GL_NO_ERROR) {
        ++g_glStats.texUploadFails;
        g_glStats.lastTexUploadError = err;
        char details[256];
        std::snprintf(details, sizeof(details),
                      "op=glCompressedTexImage2D target=0x%x level=%d internal=0x%x w=%d h=%d size=%d err=0x%x",
                      static_cast<unsigned>(target), static_cast<int>(level),
                      static_cast<unsigned>(internalformat),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(imageSize),
                      static_cast<unsigned>(err));
        reportGlFailure("gl-tex-upload-fail", g_glStats.lastBoundTexture, err, details);
    }
}

void guestViewTracedGlTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLsizei width, GLsizei height,
                                    GLenum format, GLenum type, const void* pixels) {
    noteGlCallWithoutContext("glTexSubImage2D",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    while (::glGetError() != GL_NO_ERROR) {}

    ::glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);

    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) {
        ++g_glStats.texUploadFails;
        g_glStats.lastTexUploadError = err;
        char details[256];
        std::snprintf(details, sizeof(details),
                      "op=glTexSubImage2D target=0x%x level=%d xoff=%d yoff=%d w=%d h=%d fmt=0x%x type=0x%x err=0x%x",
                      static_cast<unsigned>(target), static_cast<int>(level),
                      static_cast<int>(xoffset), static_cast<int>(yoffset),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<unsigned>(format), static_cast<unsigned>(type),
                      static_cast<unsigned>(err));
        reportGlFailure("gl-tex-upload-fail", g_glStats.lastBoundTexture, err, details);
    }
}

void guestViewTracedGlCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                                        GLint yoffset, GLint x, GLint y,
                                        GLsizei width, GLsizei height) {
    noteGlCallWithoutContext("glCopyTexSubImage2D",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    while (::glGetError() != GL_NO_ERROR) {}

    ::glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);

    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) {
        ++g_glStats.texUploadFails;
        g_glStats.lastTexUploadError = err;
        char details[256];
        std::snprintf(details, sizeof(details),
                      "op=glCopyTexSubImage2D target=0x%x level=%d xoff=%d yoff=%d x=%d y=%d w=%d h=%d err=0x%x",
                      static_cast<unsigned>(target), static_cast<int>(level),
                      static_cast<int>(xoffset), static_cast<int>(yoffset),
                      static_cast<int>(x), static_cast<int>(y),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<unsigned>(err));
        reportGlFailure("gl-tex-upload-fail", g_glStats.lastBoundTexture, err, details);
    }
}

void guestViewTracedGlBindFramebuffer(GLenum target, GLuint framebuffer) {
    // BEFORE everything else, including the host-binding exclusion below:
    // the question is "did the guest make this call from a thread with no
    // context", and the port's own binds are a separate case (the port's
    // blit has the context).
    noteGlCallWithoutContext("glBindFramebuffer",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    // THE GUEST'S "BIND THE DEFAULT FRAMEBUFFER" MEANS "BIND THE PORT'S
    // TARGET", WHEN THE PORT HAS ONE.
    //
    // MEASURED 2026-11-01 (q76): the guest renders into its own scene
    // framebuffer and composites into the DEFAULT one.  The port bound its
    // target in front of the guest's frame, but the guest's own bind undid
    // that: the composite went to the 1280x720 window, and the port's blit
    // then wiped the window with the target's clear colour.  The window came
    // out empty (one colour, rgba(0,0,0,0)) while the target held the
    // guest's clear colour -- the guest's picture had been painted over.
    //
    // Redirecting the guest's bind is what makes the two agree on ONE
    // framebuffer: the guest composites into the target (a surface of the
    // view size, with the guest's own viewport), and the port scales that
    // into the window.
    //
    // Only GL_FRAMEBUFFER and GL_DRAW_FRAMEBUFFER are redirected.  A READ
    // bind of the default framebuffer must stay the default framebuffer: the
    // guest reading the window back is a different question from the guest
    // drawing into it.
    //
    // The port's own binds are excluded: the port's blit has to reach the
    // real window, and it says so with guestViewSetHostBinding(true).
    static constexpr GLenum kGlFramebuffer = 0x8D40;
    static constexpr GLenum kGlDrawFramebuffer = 0x8CA9;
    GLuint bindFrame = framebuffer;
    bool redirected = false;
    if (!g_hostBinding && framebuffer == 0 &&
        (target == kGlFramebuffer || target == kGlDrawFramebuffer)) {
        const uint32_t offscreen = guestViewOffscreenTargetFbo();
        if (offscreen != 0) {
            bindFrame = offscreen;
            redirected = true;
        }
    }
    if (g_glStatsEnabled) {
        ++g_glStats.bindFramebuffer;
        // THE COMPARISON HAS TO HAPPEN BEFORE THE ASSIGNMENT.
        //
        // MEASURED 2026-11-01: the old order assigned lastBoundFramebuffer
        // first and compared afterwards, so `framebuffer != lastBoundFramebuffer`
        // was ALWAYS false and the log only ever held the first four binds.
        // The conclusion drawn from it -- "the guest binds fb=0 once at
        // start-up and never binds an FBO again" -- was an artefact of the
        // instrument, and it is what the whole offscreen-target design was
        // built on.  A criterion that is always false is a criterion that was
        // never tested.
        const GLuint previous = g_glStats.lastBoundFramebuffer;
        const bool changed = (bindFrame != previous);
        g_glStats.lastBoundFramebuffer = bindFrame;
        if (changed) {
            ++g_glStats.fboBindChanges;
        }
        if (bindFrame != 0) {
            // Remember the guest's scene target and how big it is.  This is
            // the framebuffer the port's present path has to scale, and its
            // size is what decides whether the view is bigger than the
            // screen (a downscale) or smaller (an upscale).
            //
            // THE SIZE COMES FROM A TABLE, NOT FROM A GL QUERY.  The devkitPro
            // GLES2 header has no glGetTexLevelParameteriv and no
            // GL_TEXTURE_WIDTH, and querying a texture's size would mean
            // binding it to a texture unit -- a state change in the middle of
            // the guest's frame.  The guest's own calls already say the
            // answer: it creates the texture (with its size) and then attaches
            // it to the framebuffer.  Two small tables turn that into the
            // framebuffer's size, with no perturbation at all.
            g_glStats.lastNonZeroFramebuffer = bindFrame;
            const uint32_t w = guestFramebufferTextureWidth(bindFrame);
            const uint32_t h = guestFramebufferTextureHeight(bindFrame);
            g_glStats.lastNonZeroFboWidth = w;
            g_glStats.lastNonZeroFboHeight = h;
        }
        // The volume is bounded: a change record per bind per frame is
        // thousands of lines a run, and the first two hundred cover the
        // shape of the frame.  The CENSUS (fboBindChanges) covers the rest.
        if (g_glStats.fboBindChanges <= 200) {
            GLenum status = GL_NONE;
            if (bindFrame != 0) {
                status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
                g_glStats.fboChecks += 1;
                g_glStats.lastFboStatus = status;
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    ++g_glStats.fboIncomplete;
                }
            }
            char diag[224];
            __builtin_snprintf(diag, sizeof(diag),
                "bindFramebuffer target=0x%x fbo=%u%s status=0x%x vp=%u,%u,%u,%u"
                " sc=%u,%u,%u,%u draws=%u size=%ux%u",
                target, bindFrame, redirected ? "(redirected)" : "",
                status,
                g_glStats.lastViewport[0], g_glStats.lastViewport[1],
                g_glStats.lastViewport[2], g_glStats.lastViewport[3],
                g_glStats.lastScissor[0], g_glStats.lastScissor[1],
                g_glStats.lastScissor[2], g_glStats.lastScissor[3],
                g_glStats.drawArrays,
                g_glStats.lastNonZeroFboWidth, g_glStats.lastNonZeroFboHeight);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fbo-bind", 249, 0, diag);
        }
    }
    ::glBindFramebuffer(target, bindFrame);
}

void guestViewTracedGlGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    noteGlCallWithoutContext("glGenFramebuffers",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        g_glStats.genFramebuffers += static_cast<uint32_t>(n);
    }
    ::glGenFramebuffers(n, framebuffers);
}

void guestViewTracedGlFramebufferTexture2D(GLenum target, GLenum attachment,
                                           GLenum textarget, GLuint texture,
                                           GLint level) {
    noteGlCallWithoutContext("glFramebufferTexture2D",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.framebufferTexture2D;
        // WHICH TEXTURE IS ON WHICH FRAMEBUFFER.  Together with the texture
        // sizes this gives the size of the guest's scene framebuffer.
        if (g_glStats.lastBoundFramebuffer != 0) {
            guestRememberAttachment(g_glStats.lastBoundFramebuffer, texture);
        }
    }
    ::glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void guestViewTracedGlUseProgram(GLuint program) {
    noteGlCallWithoutContext("glUseProgram",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.useProgram;
        if (g_glStats.useProgram <= 6) {
            guestViewWideLog("useProgram", program, g_glStats.useProgram);
        }
    }
    // The text probe follows the selected program so it can report every
    // draw the HUD's own 2D program makes.
    gfxTextProbeUseProgram(program);
    ::glUseProgram(program);
}

void guestViewTracedGlViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    noteGlCallWithoutContext("glViewport",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.viewports;
        const bool changed =
            (g_glStats.lastViewport[2] != static_cast<uint32_t>(width) ||
             g_glStats.lastViewport[3] != static_cast<uint32_t>(height));
        g_glStats.lastViewport[0] = static_cast<uint32_t>(x);
        g_glStats.lastViewport[1] = static_cast<uint32_t>(y);
        g_glStats.lastViewport[2] = static_cast<uint32_t>(width);
        g_glStats.lastViewport[3] = static_cast<uint32_t>(height);
        if (changed) {
            ++g_glStats.viewportChanges;
        }
        // The viewport the guest uses, and WHICH framebuffer it is aimed at.
        //
        // MEASURED 2026-11-01 (q76/q77): the guest's viewport came out
        // 1280x720 while its layout was 2048x1152 (the port told it so in
        // onViewChangedJNI, and the guest printed "Res base: 2048x1152" and
        // scissored in 2048x1152 coordinates).  A viewport smaller than the
        // layout is the "the picture lands in a corner" symptom, so the
        // viewport has to be measured against the framebuffer it is aimed at
        // -- which is what this line says.
        if (rr3::gfxPerCallLog() && g_glStats.viewportChanges <= 200) {
            char diag[192];
            __builtin_snprintf(diag, sizeof(diag),
                "glViewport x=%d y=%d w=%d h=%d (change #%u, fbo=%u)",
                x, y, width, height, g_glStats.viewportChanges,
                g_glStats.lastBoundFramebuffer);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "viewport", 249, 0, diag);
        }
    }
    // THE VIEWPORT MUST MATCH THE FRAMEBUFFER IT DRAWS INTO.
    //
    // MEASURED 2026-11-01 (q77/q78): the guest lays its 2D UI out in a
    // 2048x1152 space -- its scissor rectangles are in 2048x1152
    // coordinates -- and then draws it through a 1280x720 viewport, which
    // is the size of the WINDOW.  The UI therefore lands outside the
    // viewport and the screen shows nothing but the background: the HD view
    // came out as a flat sky blue, every frame.
    //
    // The guest cannot know the right number, because the framebuffer it is
    // drawing into is the PORT's offscreen target -- the guest thinks it is
    // drawing into the 1280x720 window.  So when the framebuffer bound at
    // this moment IS the port's target, the viewport is the target's size.
    //
    // Only that one framebuffer is touched.  The guest's own framebuffers
    // are the guest's business: it sizes them itself and it has been
    // getting that right (its scene target is 2048x1152, measured).
    GLint useWidth = width;
    GLint useHeight = height;
    if (!g_hostBinding) {
        const uint32_t offscreen = guestViewOffscreenTargetFbo();
        if (offscreen != 0 &&
            g_glStats.lastBoundFramebuffer == static_cast<GLuint>(offscreen)) {
            uint32_t targetWidth = 0;
            uint32_t targetHeight = 0;
            guestViewOffscreenTargetSize(&targetWidth, &targetHeight);
            if (targetWidth != 0 && targetHeight != 0 &&
                (useWidth != static_cast<GLint>(targetWidth) ||
                 useHeight != static_cast<GLint>(targetHeight))) {
                useWidth = static_cast<GLint>(targetWidth);
                useHeight = static_cast<GLint>(targetHeight);
            }
        }
    }
    ::glViewport(x, y, useWidth, useHeight);
}

void guestViewTracedGlScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    noteGlCallWithoutContext("glScissor",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
        if (rr3::gfxPerCallLog()) {
            char diag[192];
            __builtin_snprintf(diag, sizeof(diag),
                "glScissor x=%d y=%d w=%d h=%d (call #%u, viewport=%u,%u,%u,%u)",
                x, y, width, height, g_glStats.scissors,
                g_glStats.lastViewport[0], g_glStats.lastViewport[1],
                g_glStats.lastViewport[2], g_glStats.lastViewport[3]);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "scissor", 249, 0, diag);
        }
    }
    ::glScissor(x, y, width, height);
}

GLenum guestViewTracedGlCheckFramebufferStatus(GLenum target) {
    noteGlCallWithoutContext("glCheckFramebufferStatus",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glBindBuffer",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glBindTexture",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glGenTextures",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glGenBuffers",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
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
    noteGlCallWithoutContext("glVertexAttribPointer",
                              reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (g_glStatsEnabled) {
        ++g_glStats.vertexAttribPointer;
    }
    // The guest submits CLIENT-SIDE vertex arrays: this pointer is the
    // address of the guest's own memory, and it is the only place the
    // vertices of a quad can be read from. The text probe needs it.
    gfxTextProbeVertexAttribPointer(index, size, type, normalized, stride, pointer);
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
    if (interesting || cloudcell) {
        uint32_t hash = 2166136261u;
        for (const char* p = text; *p != '\0' && p < text + 120; ++p) {
            hash ^= static_cast<uint8_t>(*p);
            hash *= 16777619u;
        }
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "guest-log", 76, hash, text);
        return;
    }
    if (!rr3::gfxPerCallLog()) {
        return;
    }
    uint32_t hash = 2166136261u;
    for (const char* p = text; *p != '\0' && p < text + 120; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
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
