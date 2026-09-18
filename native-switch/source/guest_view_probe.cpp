#include "rr3/jni_bridge.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_input.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/pthread_bridge.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/tls_probe.hpp"

namespace rr3 {
namespace {

// Minimal cloudcell vtable stubs. Known call sites:
//   vtable+48   main-render path (void, must preserve this/x0)
//   vtable+136  main-render path (returns bool in x0, 0 = proceed)
//   vtable+272  CloudcellResume (void)
// Fill all slots with safe stubs so no NULL dereference occurs.
// extern "C" + return self so x0 is preserved after the call.
extern "C" void* cloudcellVtableStubVoid(void* self) { return self; }
extern "C" uint64_t cloudcellVtableStubBool(void* self) { (void)self; return 0; }

// Menu-loader vtable stub. Called at 0x12af010 as:
//   obj->vtable[21](obj, 2, 0x60)
// Must preserve self in x0 and return 0.
extern "C" uint64_t menuLoaderStub(void* self, int, int) {
    return reinterpret_cast<uint64_t>(self);
}

constexpr const char* kCreatedExport =
    "Java_com_firemint_realracing_MainActivity_onViewCreatedJNI";
constexpr const char* kChangedExport =
    "Java_com_firemint_realracing_MainActivity_onViewChangedJNI";
constexpr const char* kRenderExport =
    "Java_com_firemint_realracing_MainActivity_onViewRenderJNI";

void recordEglFailure(GuestViewProbe& probe) {
    probe.eglError = static_cast<uint32_t>(eglGetError());
}

void releaseEgl(EGLDisplay display, EGLSurface surface, EGLContext context) {
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
        eglTerminate(display);
    }
}

// Ghidra DAT_02097030 is the ndActivity singleton pointer, stored at a fixed
// offset from the onViewRenderJNI export (both live in libRealRacing3.so).
// Ghidra addresses for this image are 0x00100000 above the ELF VAs used at
// runtime, so the delta below is (0x02097030 - 0x013B3614).
constexpr uint64_t kActivitySingletonDeltaFromRenderExport = 0x00CE3A1CULL;

struct GuestActivitySnapshot {
    bool valid{};
    uintptr_t activity{};
    uintptr_t engine{};
    uintptr_t container{};
    uintptr_t renderer{};
    uint8_t glObjectsCreated;   // +0x4a4
    uint8_t landscapeApplied;   // +0x4a5
    uint8_t isLandscape;        // +0x4a6
    uint8_t paused;             // +0x4d0
    uint8_t flag4d1;
    uint8_t engineReady;        // engine+0xa
    uint8_t containerActive;    // container+0xa — gates FUN_013b43a0/013b4448
    uint8_t containerReady;     // container+0x20 — gates FUN_013b4940
    uint8_t splashExit;         // renderer+0x1f5a9 — splash→menu trigger
    uint8_t splashFlagAb;       // renderer+0x1f5ab
    uint8_t splashFlagAc;       // renderer+0x1f5ac
    uint8_t renderer120;        // renderer+0x120
    uint8_t renderer38c;        // renderer+0x38c — early-return when !=0
    uint64_t containerQ0;       // *container — vtable or first field
    uint64_t containerQ1;
    uint64_t containerQ2;
    int32_t width;              // +0x4a8
    int32_t height;             // +0x4ac
    int32_t frameState;         // +0x4d8
    int32_t splashFrames;       // +0x4dc
};

// Ghidra DAT_020b2688 is the engine pointer used by the render loop
// (vtable+0x38 time, +0xa landscape-ready flag). Same image as the activity
// singleton; delta from onViewRenderJNI = 0x020b2688 - 0x013b3614.
constexpr uint64_t kEngineSingletonDeltaFromRenderExport = 0x00CFF074ULL;

GuestActivitySnapshot readGuestActivitySnapshot(uintptr_t renderExport) {
    GuestActivitySnapshot snap{};
    if (renderExport == 0) {
        return snap;
    }
    const uintptr_t singletonSlot =
        renderExport + kActivitySingletonDeltaFromRenderExport;
    const uintptr_t activity =
        *reinterpret_cast<const uintptr_t*>(singletonSlot);
    if (activity == 0) {
        return snap;
    }
    snap.valid = true;
    snap.activity = activity;
    const volatile uint8_t* bytes =
        reinterpret_cast<const volatile uint8_t*>(activity);
    snap.glObjectsCreated = bytes[0x4a4];
    snap.landscapeApplied = bytes[0x4a5];
    snap.isLandscape = bytes[0x4a6];
    snap.paused = bytes[0x4d0];
    snap.flag4d1 = bytes[0x4d1];
    const volatile int32_t* words =
        reinterpret_cast<const volatile int32_t*>(activity);
    snap.width = words[0x4a8 / 4];
    snap.height = words[0x4ac / 4];
    snap.frameState = words[0x4d8 / 4];
    snap.splashFrames = words[0x4dc / 4];
    const uintptr_t engineSlot =
        renderExport + kEngineSingletonDeltaFromRenderExport;
    const uintptr_t engine = *reinterpret_cast<const uintptr_t*>(engineSlot);
    if (engine != 0) {
        snap.engine = engine;
        snap.engineReady = *reinterpret_cast<const volatile uint8_t*>(engine + 0xa);
    }
    // activity+0x58 → container (small object with +0xa/+0x20 flags).
    // First qword of container is the large renderer (FUN_013aea7c does
    // lVar12 = **(long**)(activity+0x58)).
    const uintptr_t container =
        *reinterpret_cast<const uintptr_t*>(activity + 0x58);
    if (container != 0) {
        snap.container = container;
        snap.containerActive =
            *reinterpret_cast<const volatile uint8_t*>(container + 0xa);
        snap.containerReady =
            *reinterpret_cast<const volatile uint8_t*>(container + 0x20);
        const volatile uint64_t* qwords =
            reinterpret_cast<const volatile uint64_t*>(container);
        snap.containerQ0 = qwords[0];
        snap.containerQ1 = qwords[1];
        snap.containerQ2 = qwords[2];
        snap.renderer = snap.containerQ0;
        if (snap.renderer != 0) {
            snap.splashExit =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5a9);
            snap.splashFlagAb =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5ab);
            snap.splashFlagAc =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x1f5ac);
            snap.renderer120 =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x120);
            snap.renderer38c =
                *reinterpret_cast<const volatile uint8_t*>(snap.renderer + 0x38c);
        }
    }
    return snap;
}

void writeGuestViewState(const char* progressPath, uintptr_t renderExport,
                         unsigned long long ordinal) {
    if (progressPath == nullptr) {
        return;
    }
    const GuestActivitySnapshot snap = readGuestActivitySnapshot(renderExport);
    const GuestViewGlStats stats = guestViewGlStats();
    char details[384]{};
    // Append-only sidecar: writeGuestBootProgress truncates, and the final
    // handoff-process-exit line wiped every guest-activity dump (round 11).
    constexpr const char* kViewLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";
    if (!snap.valid) {
        appendGuestBootLine(kViewLog, "guest-activity-null", ordinal, 0);
        return;
    }
    const int written = __builtin_snprintf(
        details, sizeof(details),
        "a4=%u a5=%u a6=%u d0=%u d1=%u d8=%d dc=%d eng=%u engp=%llx w=%d h=%d "
        "ca=%u cr=%u se=%u c38c=%u cq0=%llx cq1=%llx cq2=%llx "
        "gl=%u/%u/%u/%u/%u/%u/%u vp=%u sc=%u fbo=%u/%u st=%x buf=%u texb=%u vap=%u "
        "t0=%u b0=%u lasttex=%u lastbuf=%u gentex=%u genbuf=%u lastgen=%u/%u hinj=%u lastvp=%u,%u,%u,%u",
        snap.glObjectsCreated, snap.landscapeApplied, snap.isLandscape,
        snap.paused, snap.flag4d1, snap.frameState, snap.splashFrames,
        snap.engineReady, static_cast<unsigned long long>(snap.engine),
        snap.width, snap.height,
        snap.containerActive, snap.containerReady, snap.splashExit, snap.renderer38c,
        static_cast<unsigned long long>(snap.containerQ0),
        static_cast<unsigned long long>(snap.containerQ1),
        static_cast<unsigned long long>(snap.containerQ2),
        stats.clears, stats.drawArrays,
        stats.drawElements, stats.texImage2D, stats.compressedTexImage2D,
        stats.bindFramebuffer, stats.useProgram, stats.viewports, stats.scissors,
        stats.fboChecks, stats.fboIncomplete, stats.lastFboStatus,
        stats.bindBuffer, stats.bindTexture, stats.vertexAttribPointer,
        stats.bindTextureZero, stats.bindBufferZero,
        stats.lastBoundTexture, stats.lastBoundBuffer,
        stats.genTextures, stats.genBuffers, stats.lastGenTexture,
        stats.lastGenBuffer, stats.hostTexInject,
        stats.lastViewport[0], stats.lastViewport[1], stats.lastViewport[2],
        stats.lastViewport[3]);
    if (written > 0) {
        appendGuestBootLine(kViewLog, "guest-activity", ordinal,
                            snap.activity, details);
    }
}

} // namespace

GuestViewProbe runGuestViewProbe(NWindow* window, const char* progressPath) {
    GuestViewProbe probe{};
    probe.attempted = true;
    probe.surfaceMode = 1; // default NWindow surface
    if (window == nullptr) {
        probe.failureCode = 5;
        probe.eglError = EGL_BAD_NATIVE_WINDOW;
        return probe;
    }

    // Resolve every callback before installing guest TLS. This keeps a missing
    // export a normal diagnostic result rather than a fault in the guest scope.
    const uintptr_t createdTarget = resolveMainImageExport(kCreatedExport);
    if (createdTarget == 0) {
        probe.failureCode = 1;
        return probe;
    }
    probe.createdResolved = true;
    const uintptr_t changedTarget = resolveMainImageExport(kChangedExport);
    if (changedTarget == 0) {
        probe.failureCode = 3;
        return probe;
    }
    probe.changedResolved = true;
    const uintptr_t renderTarget = resolveMainImageExport(kRenderExport);
    if (renderTarget == 0) {
        probe.failureCode = 4;
        return probe;
    }
    probe.renderResolved = true;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        return probe;
    }
    probe.displayCreated = true;

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        eglTerminate(display);
        return probe;
    }
    probe.initialized = true;

    constexpr EGLint configAttributes[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config{};
    EGLint configCount = 0;
    if (eglChooseConfig(display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
        configCount == 0) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        eglTerminate(display);
        return probe;
    }
    probe.configChosen = true;

    EGLSurface surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        eglTerminate(display);
        return probe;
    }
    probe.surfaceCreated = true;

    constexpr EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        releaseEgl(display, surface, EGL_NO_CONTEXT);
        return probe;
    }
    probe.contextCreated = true;

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        probe.failureCode = 5;
        recordEglFailure(probe);
        releaseEgl(display, surface, context);
        return probe;
    }
    probe.madeCurrent = true;

    // First exercise the exact host GLES call that the guest's first View
    // initialization uses. This is deliberately outside GuestTlsScope so a
    // bad NWindow/EGL handoff is separated from a Mesa/TLS interaction.
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-outside-enter", 0, 0);
    }
    probe.hostGlOutsideTlsCalled = true;
    const GLubyte* outsideVersion = ::glGetString(GL_VERSION);
    probe.hostGlOutsideTlsReturned = true;
    probe.hostGlOutsideTlsNonNull = outsideVersion != nullptr;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-outside-return",
                               outsideVersion != nullptr ? 1 : 0, 0);
    }

    // The callbacks use the Activity object and JNI table created by the
    // preceding guest-entry probe. EGL is deliberately established first so
    // every guest GL dereference sees the same live context and NWindow.
    const JniSlotHistogram beforeCreated = captureJniHistogram();
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        probe.failureCode = 2;
        releaseEgl(display, surface, context);
        return probe;
    }

    // Repeat the host query while TPIDR_EL0 has the Android-compatible layout.
    // If this is the last breadcrumb, Mesa/its driver path—not guest code—is
    // the first component that depends on the guest TLS register.
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-inside-enter", 0, 0);
    }
    probe.hostGlInsideTlsCalled = true;
    const GLubyte* insideVersion = ::glGetString(GL_VERSION);
    probe.hostGlInsideTlsReturned = true;
    probe.hostGlInsideTlsNonNull = insideVersion != nullptr;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "view-host-gl-inside-return",
                               insideVersion != nullptr ? 1 : 0, 0);
    }

    // Bind the trace only around guest callbacks. The resolver target remains
    // transparent everywhere else, including the independent renderer NRO.
    enableGuestViewGlTrace(progressPath);
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewCreatedJNI", 0,
            static_cast<unsigned long long>(createdTarget));
    }
    void* jniEnvironment = guestJniEnvironment();
    void* activityObject = guestActivityObject();
    probe.createdCalled = true;
    reinterpret_cast<void (*)(void*, void*)>(createdTarget)(
        jniEnvironment, activityObject);
    probe.createdReturned = true;
    const JniSlotHistogram afterCreated = captureJniHistogram();
    probe.createdJni = diffJniHistograms(afterCreated, beforeCreated);

    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewChangedJNI", 0,
            static_cast<unsigned long long>(changedTarget));
    }
    probe.changedCalled = true;
    reinterpret_cast<void (*)(void*, void*, int, int, int, int)>(changedTarget)(
        jniEnvironment, activityObject, 1280, 720, 0, 0);
    probe.changedReturned = true;
    const JniSlotHistogram afterChanged = captureJniHistogram();
    probe.changedJni = diffJniHistograms(afterChanged, afterCreated);

    // First-landscape Changed is supposed to run FUN_013b4328 which sets
    // engine+0xa (render early-outs / empty update while it is 0). If the
    // guest skipped that path, force the flag so the frame loop can draw.
    // Also force the three gates the resource FSM needs to leave splash:
    // container+0xa (menu draw), container+0x20 (ready), renderer+0x1f5a9
    // (splash→menu). Guest never sets 0x1f5a9; without it the menu layout
    // switch never runs and no sprite atlases are opened.
    {
        const GuestActivitySnapshot afterChangedSnap =
            readGuestActivitySnapshot(renderTarget);
        if (afterChangedSnap.valid) {
            const uintptr_t engine =
                *reinterpret_cast<const uintptr_t*>(afterChangedSnap.activity + 0x58);
            if (engine != 0) {
                volatile uint8_t* ready =
                    reinterpret_cast<volatile uint8_t*>(engine + 0xa);
                if (*ready == 0) {
                    *ready = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-engine-ready", 78, engine);
                }
            }
            if (afterChangedSnap.container != 0) {
                volatile uint8_t* active =
                    reinterpret_cast<volatile uint8_t*>(afterChangedSnap.container + 0xa);
                if (*active == 0) {
                    *active = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-container-active", 80,
                                        afterChangedSnap.container);
                }
                volatile uint8_t* containerReady =
                    reinterpret_cast<volatile uint8_t*>(afterChangedSnap.container + 0x20);
                if (*containerReady == 0) {
                    *containerReady = 1;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "force-container-ready", 81,
                                        afterChangedSnap.container);
                }
            }
            // Do NOT force frameState — let the guest run its natural
            // warmup(1/2/3)→GL-reset(4)→splash(5)→menu sequence. The
            // connect() stub already prevents HTTP blocking.
            // Force renderer splash→menu gate bytes. The main-render path
            // at 0x12aed14 reads renderer+0x1f5a9; if 0 it skips menu layout.
            // When non-zero it also checks +0x1f5ab, +0x1f5ac, +0x120.
            // renderer = *container (containerQ0). Only write when the
            // pointer is clearly on the heap (>4 GiB) — writing
            // container+0x1f5a9 is OOB and crashed (round 32).
            if (afterChangedSnap.renderer != 0 &&
                afterChangedSnap.renderer > 0x100000000ULL) {
                volatile uint8_t* r = reinterpret_cast<volatile uint8_t*>(
                    afterChangedSnap.renderer);
                const uint8_t before = r[0x1f5a9];
                if (before == 0) {
                    r[0x1f5a9] = 1;
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "force-splash-exit", 91,
                        static_cast<unsigned long long>(r[0x1f5a9]));
                }
                if (r[0x1f5ab] == 0) {
                    r[0x1f5ab] = 1;
                }
                if (r[0x1f5ac] == 0) {
                    r[0x1f5ac] = 1;
                }
                if (r[0x120] == 0) {
                    r[0x120] = 1;
                }
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "force-splash-flags", 92,
                    (static_cast<unsigned long long>(r[0x1f5a9]) << 24) |
                    (static_cast<unsigned long long>(r[0x1f5ab]) << 16) |
                    (static_cast<unsigned long long>(r[0x1f5ac]) << 8) |
                    static_cast<unsigned long long>(r[0x120]));
            }
        }
    }

    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, "onViewRenderJNI", 0,
            static_cast<unsigned long long>(renderTarget));
    }
    probe.renderCalled = true;
    // The guest render entry receives a render mode and flags, not dimensions;
    // mode 2 with no flags selects the normal foreground surface path.
    reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
        jniEnvironment, activityObject, 2, 0);
    probe.renderReturned = true;
    writeGuestBootProgress(progressPath, "onViewRenderJNI-return", 46, renderTarget);
    const JniSlotHistogram afterRender = captureJniHistogram();
    probe.renderJni = diffJniHistograms(afterRender, afterChanged);
    probe.lastTraceStage = guestViewTraceLastStage();
    probe.postGlMalloc2800Called = guestViewTraceReached(9);
    probe.postGlMalloc2800Returned = guestViewTraceReached(10);
    probe.postGlMalloc3e38Called = guestViewTraceReached(11);
    probe.postGlMalloc3e38Returned = guestViewTraceReached(12);
    probe.postGlMalloc240FirstCalled = guestViewTraceReached(23);
    probe.postGlMalloc240FirstReturned = guestViewTraceReached(24);
    probe.postGlMalloc130Called = guestViewTraceReached(25);
    probe.postGlMalloc130Returned = guestViewTraceReached(26);
    probe.postGlMalloc40FirstCalled = guestViewTraceReached(27);
    probe.postGlMalloc40FirstReturned = guestViewTraceReached(28);
    probe.postGlMalloc40SecondCalled = guestViewTraceReached(29);
    probe.postGlMalloc40SecondReturned = guestViewTraceReached(30);
    disableGuestViewGlTrace();

    writeGuestBootProgress(progressPath, "view-gl-finish-enter", 47, 0);
    glFinish();
    writeGuestBootProgress(progressPath, "view-gl-finish-return", 48, 0);
    probe.glError = static_cast<uint32_t>(glGetError());
    writeGuestBootProgress(progressPath, "view-swap-enter", 49, probe.glError);
    probe.swapped = eglSwapBuffers(display, surface) == EGL_TRUE;
    writeGuestBootProgress(progressPath, "view-swap-return", 50, probe.swapped);
    if (!probe.swapped) {
        recordEglFailure(probe);
        writeGuestBootProgress(progressPath, "view-egl-release-enter", 51, 0);
        releaseEgl(display, surface, context);
        if (progressPath != nullptr) {
            writeGuestBootProgress(
                progressPath, "guest-view-swap-failed", 3,
                static_cast<unsigned long long>(renderTarget));
        }
        return probe;
    }

    // P3 persistent frame loop: keep the EGL context/surface alive and drive
    // the guest Render callback each frame instead of releasing after one swap.
    // The guest worker threads created during Changed keep using this context,
    // so it must stay current until the operator exits the game state.
    probe.frameLoopEntered = true;
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "frame-loop-enter", 70, 0);
    }

    // Host clear once so a guest that draws nothing shows solid blue instead
    // of Mesa's undefined checkerboard. If the screen stays checkerboard the
    // guest itself is presenting garbage.
    glClearColor(0.05f, 0.12f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    setGuestInputProgressPath(progressPath);
    prepareGuestControllerInput();
    connectGuestController(jniEnvironment, activityObject, 1);

    // Create fake cloudcell + menu-loader objects BEFORE the first render
    // call. The guest loads these pointers at function entry; if they are
    // NULL on frame 0, the guest crashes dereferencing them.
    {
        // Cloudcell: GOT at ELF 0x1f6d5d0; delta from onViewRenderJNI = +0xCB9FBC.
        constexpr uint64_t kCloudcellGotDelta = 0x00CB9FBCULL;
        const uintptr_t ccGot = renderTarget + kCloudcellGotDelta;
        const uintptr_t ccGlobal = *reinterpret_cast<const uintptr_t*>(ccGot);
        // Diagnostic: dump GOT entry value.
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "cc-got-diag", 110,
            (static_cast<unsigned long long>(ccGot & 0xFFFFFFFFULL) << 32) |
            (ccGlobal & 0xFFFFFFFFULL));
        if (ccGlobal != 0 && ccGlobal > 0x100000000ULL) {
            uintptr_t cloudcell = *reinterpret_cast<const uintptr_t*>(ccGlobal);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "cc-ptr-diag", 111,
                (static_cast<unsigned long long>(ccGlobal & 0xFFFFFFFFULL) << 32) |
                (cloudcell & 0xFFFFFFFFULL));
            if (cloudcell == 0 || cloudcell < 0x100000000ULL) {
                static uint64_t s_ccBuf[256] = {};
                static uint64_t s_ccVtable[64] = {};
                for (int i = 0; i < 64; ++i) {
                    s_ccVtable[i] = (i % 2 == 0)
                        ? reinterpret_cast<uint64_t>(&cloudcellVtableStubBool)
                        : reinterpret_cast<uint64_t>(&cloudcellVtableStubVoid);
                }
                s_ccBuf[0] = reinterpret_cast<uint64_t>(s_ccVtable);
                reinterpret_cast<volatile uint8_t*>(s_ccBuf)[1217] = 1;
                cloudcell = reinterpret_cast<uint64_t>(s_ccBuf);
                *reinterpret_cast<volatile uintptr_t*>(ccGlobal) = cloudcell;
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "create-cloudcell", 101, cloudcell);
            }
        }
        // Menu loader: GOT entry at ELF 0x1f6df50; delta = +0xCBA93C.
        // TWO levels of indirection:
        //   x23 = *(0x1f6df50)  = pointer to global variable
        //   x0  = *x23          = object pointer
        // The GOT entry is in RELRO; the global variable it points to is writable.
        constexpr uint64_t kMenuLoaderDelta = 0x00CBA93CULL;
        const uintptr_t gotEntry = renderTarget + kMenuLoaderDelta;
        const uintptr_t globalVarAddr =
            *reinterpret_cast<const uintptr_t*>(gotEntry);
        // Diagnostic: dump GOT entry value.
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "ml-got-diag", 112,
            (static_cast<unsigned long long>(gotEntry & 0xFFFFFFFFULL) << 32) |
            (globalVarAddr & 0xFFFFFFFFULL));
        if (globalVarAddr != 0 && globalVarAddr > 0x100000000ULL) {
            uintptr_t menuLoader =
                *reinterpret_cast<const uintptr_t*>(globalVarAddr);
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "ml-ptr-full", 115, menuLoader);
            if (menuLoader == 0 || menuLoader < 0x100000000ULL) {
                // Create new object.
                static uint64_t s_mlBuf[64] = {};
                static uint64_t s_mlVtable[32] = {};
                for (int i = 0; i < 32; ++i) {
                    s_mlVtable[i] = reinterpret_cast<uint64_t>(&menuLoaderStub);
                }
                s_mlBuf[0] = reinterpret_cast<uint64_t>(s_mlVtable);
                menuLoader = reinterpret_cast<uint64_t>(s_mlBuf);
                *reinterpret_cast<volatile uintptr_t*>(globalVarAddr) = menuLoader;
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "create-menu-loader", 102, menuLoader);
            } else {
                // Object exists: ensure vtable is valid.
                volatile uint64_t* vtableSlot =
                    reinterpret_cast<volatile uint64_t*>(menuLoader);
                const uint64_t currentVtable = *vtableSlot;
                if (currentVtable == 0 || currentVtable < 0x100000000ULL) {
                    static uint64_t s_mlVtableFix[32] = {};
                    for (int i = 0; i < 32; ++i) {
                        s_mlVtableFix[i] = reinterpret_cast<uint64_t>(&menuLoaderStub);
                    }
                    *vtableSlot = reinterpret_cast<uint64_t>(s_mlVtableFix);
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "fix-menu-vtable", 116,
                        (static_cast<unsigned long long>(menuLoader & 0xFFFFFFFFULL) << 32) |
                        (currentVtable & 0xFFFFFFFFULL));
                }
            }
        }
        // NOP patch removed: svcSetMemoryPermission fails on guest .text
        // (different memory block). The menu-load check at 0xdacf40 is
        // left intact; the fake cloudcell/menu-loader objects should be
        // sufficient for the guest to proceed.
    }

    PadState framePad{};
    padInitializeDefault(&framePad);
    uint64_t frames = 0;
    uint32_t lastGlError = 0;
    while (probe.frameLoopActive) {
        padUpdate(&framePad);
        pumpGuestControllerInput(jniEnvironment, activityObject,
                                 reinterpret_cast<GuestPadHandle*>(&framePad), 1);

        reinterpret_cast<void (*)(void*, void*, int, uint32_t)>(renderTarget)(
            jniEnvironment, activityObject, 2, 0);

        // Re-assert splash-exit flags each frame. Do NOT force frameState —
        // let the guest run its natural warmup→splash→menu sequence.
        {
            const GuestActivitySnapshot snapNow =
                readGuestActivitySnapshot(renderTarget);
            if (snapNow.valid) {
                if (snapNow.renderer != 0 &&
                    snapNow.renderer > 0x100000000ULL) {
                    volatile uint8_t* r = reinterpret_cast<volatile uint8_t*>(
                        snapNow.renderer);
                    bool changed = false;
                    if (r[0x1f5a9] == 0) { r[0x1f5a9] = 1; changed = true; }
                    if (r[0x1f5ab] == 0) { r[0x1f5ab] = 1; changed = true; }
                    if (r[0x1f5ac] == 0) { r[0x1f5ac] = 1; changed = true; }
                    if (r[0x120] == 0) { r[0x120] = 1; changed = true; }
                    if (changed && frames < 5) {
                        appendGuestBootLine(
                            "sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "reassert-splash-flags", frames,
                            (static_cast<unsigned long long>(r[0x1f5a9]) << 24) |
                            (static_cast<unsigned long long>(r[0x1f5ab]) << 16) |
                            (static_cast<unsigned long long>(r[0x1f5ac]) << 8) |
                            static_cast<unsigned long long>(r[0x120]));
                    }
                }
            }
        }

        // Fire CloudcellResume at several frames after hideSplash so it
        // lands after cloudcell Constructor completes (which happens async).
        // ELF 0x1cf2ad8; delta from onViewRenderJNI (ELF 0x12B3614) = +0xA3F4C4.
        if (frames == 10 || frames == 20 || frames == 30 ||
            frames == 50 || frames == 80) {
            constexpr uint64_t kCloudcellResumeDelta = 0x00A3F4C4ULL;
            using CloudcellResumeFn = void (*)(void*, void*);
            const uintptr_t cloudcellResume = renderTarget + kCloudcellResumeDelta;
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "call-cloudcell-resume", 93,
                (static_cast<unsigned long long>(frames) << 32) |
                (cloudcellResume & 0xFFFFFFFFULL));
            reinterpret_cast<CloudcellResumeFn>(cloudcellResume)(
                jniEnvironment, activityObject);
        }

        const uint32_t glError = static_cast<uint32_t>(glGetError());
        if (glError != 0 && glError != lastGlError) {
            lastGlError = glError;
            if (progressPath != nullptr) {
                writeGuestBootProgress(progressPath, "frame-loop-gl-error",
                                       73, glError);
            }
        }
        if (frames % 30 == 0) {
            if (progressPath != nullptr) {
                writeGuestBootProgress(progressPath, "frame-loop-heartbeat",
                                       74, frames);
            }
            writeGuestViewState(progressPath, renderTarget,
                                static_cast<unsigned long long>(77 + frames / 30));
        }

        if (eglSwapBuffers(display, surface) != EGL_TRUE) {
            recordEglFailure(probe);
            break;
        }
        else if (guestViewGlStats().drawElements == 0 &&
                 guestViewGlStats().drawArrays == 0) {
            // Guest never issues any draws. Paint the host quad so the
            // device screen is a positive GL test (magenta/black) instead of
            // Mesa's cyan undefined-texture checkerboard.
            hostDrawMagentaQuad();
            if (eglSwapBuffers(display, surface) != EGL_TRUE) {
                recordEglFailure(probe);
                break;
            }
        }
        ++frames;
        if ((padGetButtonsDown(&framePad) & HidNpadButton_Plus) != 0) {
            probe.frameLoopActive = false;
        }
        svcSleepThread(16666666LL); // ~60Hz
        if (frames >= 360000ULL) { // defensive cap ~100 minutes
            break;
        }
    }
    probe.framesRendered = frames;
    writeGuestViewState(progressPath, renderTarget, 79);
    {
        const GuestActivitySnapshot snap = readGuestActivitySnapshot(renderTarget);
        const GuestViewGlStats stats = guestViewGlStats();
        probe.activityObject = snap.activity;
        probe.activityGlCreated = snap.glObjectsCreated;
        probe.activityLandscape = snap.isLandscape;
        probe.activityPaused = snap.paused;
        probe.activityFrameState = static_cast<uint32_t>(snap.frameState);
        probe.activitySplashFrames = static_cast<uint32_t>(snap.splashFrames);
        probe.activityWidth = snap.width;
        probe.activityHeight = snap.height;
        probe.glClears = stats.clears;
        probe.glDrawArrays = stats.drawArrays;
        probe.glDrawElements = stats.drawElements;
        probe.glTexImage = stats.texImage2D;
        probe.glCompressedTex = stats.compressedTexImage2D;
        probe.glBindFbo = stats.bindFramebuffer;
        probe.glUseProgram = stats.useProgram;
    }
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "frame-loop-exit", 71, frames);
    }

    // Guest workers still execute guest code after PLUS. Destroying the EGL
    // context they use races and was the round-9 leave-app error path.
    const unsigned int liveThreads = guestPthreadLiveCount();
    if (liveThreads != 0) {
        if (progressPath != nullptr) {
            writeGuestBootProgress(progressPath, "view-egl-keep-workers", 75,
                                   liveThreads);
        }
        if (progressPath != nullptr) {
            writeGuestBootProgress(
                progressPath, probe.swapped ? "guest-view-complete"
                                            : "guest-view-swap-failed",
                3, static_cast<unsigned long long>(renderTarget));
        }
        return probe;
    }

    writeGuestBootProgress(progressPath, "view-egl-release-enter", 51, 0);
    releaseEgl(display, surface, context);
    if (progressPath != nullptr) {
        writeGuestBootProgress(
            progressPath, probe.swapped ? "guest-view-complete" : "guest-view-swap-failed",
            3, static_cast<unsigned long long>(renderTarget));
    }
    return probe;
}

} // namespace rr3
