#pragma once

#include <cstdint>

struct NWindow;

namespace rr3 {

// Number of slots in the JNINativeInterface function table (JNI spec).
// The standard table includes GetObjectRefType at index 232.
constexpr uint32_t kJniSlotCount = 233;

// Per-slot hit histogram. slotHits[slot] counts calls that fell through to the
// not-yet-implemented stub for that slot. Snapshotting the counters at phase
// boundaries and diffing the snapshots attributes each phase's JNI traffic
// exactly (JNI_OnLoad vs onCreateJNI vs onStartJNI vs onResumeJNI), which is the
// work list for deciding which slots to implement next.
struct JniSlotHistogram {
    uint32_t slotHits[kJniSlotCount]{};
    uint32_t unhandledCalls{};
    uint32_t lastSlot{0xffffffffu};
    uint32_t methodBindings{};
};

struct GuestViewProbe {
    bool attempted{};
    bool createdResolved{};
    bool createdCalled{};
    bool createdReturned{};
    bool changedResolved{};
    bool changedCalled{};
    bool changedReturned{};
    bool renderResolved{};
    bool renderCalled{};
    bool renderReturned{};
    bool displayCreated{};
    bool initialized{};
    bool configChosen{};
    bool surfaceCreated{};
    bool contextCreated{};
    bool madeCurrent{};
    bool swapped{};
    bool hostGlOutsideTlsCalled{};
    bool hostGlOutsideTlsReturned{};
    bool hostGlOutsideTlsNonNull{};
    bool hostGlInsideTlsCalled{};
    bool hostGlInsideTlsReturned{};
    bool hostGlInsideTlsNonNull{};
    bool postGlMalloc2800Called{};
    bool postGlMalloc2800Returned{};
    bool postGlMalloc3e38Called{};
    bool postGlMalloc3e38Returned{};
    bool postGlMalloc240FirstCalled{};
    bool postGlMalloc240FirstReturned{};
    bool postGlMalloc130Called{};
    bool postGlMalloc130Returned{};
    bool postGlMalloc40FirstCalled{};
    bool postGlMalloc40FirstReturned{};
    bool postGlMalloc40SecondCalled{};
    bool postGlMalloc40SecondReturned{};
    uint32_t failureCode{}; // 1/3/4=export missing, 2=TLS, 5=EGL
    uint32_t glError{};
    uint32_t eglError{};
    uint32_t surfaceMode{}; // 1=default NWindow, 2=pbuffer
    uint32_t lastTraceStage{};
    // P3 persistent frame loop: set true once the loop is entered; the main
    // loop clears frameLoopActive to request the loop to exit on the next frame.
    bool frameLoopEntered{};
    bool frameLoopActive{true};
    uint64_t framesRendered{};
    // Guest activity singleton snapshot after the frame loop (0 = null).
    uint64_t activityObject{};
    uint32_t activityGlCreated{};
    uint32_t activityLandscape{};
    uint32_t activityPaused{};
    uint32_t activityFrameState{};
    uint32_t activitySplashFrames{};
    int32_t activityWidth{};
    int32_t activityHeight{};
    // Guest GL draw traffic attributed while the view probe owned the context.
    uint32_t glClears{};
    uint32_t glDrawArrays{};
    uint32_t glDrawElements{};
    uint32_t glTexImage{};
    uint32_t glCompressedTex{};
    uint32_t glBindFbo{};
    uint32_t glUseProgram{};
    JniSlotHistogram createdJni{};
    JniSlotHistogram changedJni{};
    JniSlotHistogram renderJni{};
};

// Result of calling the guest's first real runtime entry point, Java_com_firemint_realracing_MainActivity_onCreateJNI, through the minimal host-side JNIEnv, followed (same guarded run) by onStartJNI and onResumeJNI.
// onCreateJNI constructs the 0x540-byte platform Activity object and binds the Java method IDs; onStart/onResume walk it but do not touch GL (that is onViewCreatedJNI), so the whole probe runs in the main process.
struct GuestEntryProbe {
    bool attempted{};
    bool resolved{};         // onCreateJNI export found in the main image
    bool called{};           // entered the guest function
    bool returned{};         // guest function returned without a crash
    bool startResolved{};
    bool startCalled{};
    bool startReturned{};
    bool resumeResolved{};
    bool resumeCalled{};
    bool resumeReturned{};
    uint32_t failureCode{};      // 1=onCreate export missing 2=TLS scope 3=onStart 4=onResume
    JniSlotHistogram onCreate{}; // histogram of the onCreateJNI phase only
    JniSlotHistogram start{};
    JniSlotHistogram resume{};
};

// Result of calling each guest library's JNI_OnLoad(JavaVM*, reserved) with the
// minimal fake JavaVM. On Android the loader calls JNI_OnLoad at library-load
// time; it is what caches the JavaVM pointer that EA::Nimble::getEnv() (and, by
// extension, several main constructors) later dereference. This MUST run before
// initializeGuestMainConstructors — main ctor #157 null-derefs the uncached
// JavaVM otherwise.
struct GuestJniOnLoadProbe {
    bool attempted{};
    bool completed{};
    uint32_t failureCode{};
    uint32_t imagesFound{};     // images exporting JNI_OnLoad
    uint32_t imagesCalled{};    // JNI_OnLoad calls that returned
    uint32_t lastVersion{};     // JNI version the last JNI_OnLoad returned
    JniSlotHistogram onLoad{};  // histogram of the JNI_OnLoad phase only
};

// Builds the minimal JNIEnv/JavaVM if needed and calls JNI_OnLoad on every
// loaded guest image that exports it. progressPath (optional) receives a durable
// breadcrumb before each call for post-crash attribution.
GuestJniOnLoadProbe runGuestJniOnLoad(const char* progressPath = nullptr);

// Builds the minimal JNIEnv (233-slot JNINativeInterface table) if needed, then
// calls Java_..._onCreateJNI, onStartJNI and onResumeJNI under one guest TLS
// scope with the same fake activity jobject, writing a durable breadcrumb
// before each. Must be called only after JNI_OnLoad and the main image
// constructors have completed, and only under the recovery guard (the caller
// arms the snapshot marker first).
GuestEntryProbe runGuestEntryProbe(const char* progressPath = nullptr);

// Runs the first guest view lifecycle callbacks with a live EGL context in the
// same process that owns the guest mappings. The caller must have completed
// JNI_OnLoad and the activity lifecycle probe before invoking this function.
// Brings up a SURFACELESS GL context, asks glGetString(GL_VERSION) on it, and
// tears it down again. Called before the guest's early initialisation, which
// samples the GL version once and never re-samples it -- so if the host has no
// context at that moment the guest records major=0 for the rest of the process
// and every later check of that field is permanently on the wrong side.
//
// No surface is taken on the NWindow, so the view probe's own windowed ladder
// is unaffected. Controlled by "glctxearly" in rr3-gfx.conf; default on.
bool ensureEarlyGuestGlContext();

// Runs the first guest view lifecycle callbacks with a live EGL context in the
// same process that owns the guest mappings. The caller must have completed
// JNI_OnLoad and the activity lifecycle probe before invoking this function.
GuestViewProbe runGuestViewProbe(NWindow* window,
                                 const char* progressPath = nullptr);

// Stable handles used at the JNI callback boundary after ensureInitialized().
// They are opaque to the host and point at the same fake objects used by
// onCreateJNI/onStartJNI/onResumeJNI.
void* guestJniEnvironment();
void* guestActivityObject();

// Capture/diff helpers used to attribute unsupported JNI calls to a phase.
JniSlotHistogram captureJniHistogram();
JniSlotHistogram diffJniHistograms(const JniSlotHistogram& after,
                                   const JniSlotHistogram& before);

} // namespace rr3
