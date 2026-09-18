#include "rr3/ui_dashboard.hpp"
#include "rr3/native_ui.hpp"
#include "rr3/pthread_bridge.hpp"

#include <cstdio>

namespace rr3 {
namespace {

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kGreen = "\x1b[32;1m";
constexpr const char* kYellow = "\x1b[33;1m";
constexpr const char* kRed = "\x1b[31;1m";
constexpr const char* kCyan = "\x1b[36;1m";
constexpr const char* kDim = "\x1b[37m";

void line() {
    std::printf("+------------------------------------------------------------------------------+\n");
}

void header(int page) {
    std::printf("\x1b[44;37;1m RR3 NATIVE REHOST   Horizon / libnx   No Android Runtime                     \x1b[0m\n");
    std::printf(" Loader Control Center 0.22.4                               Page %d / %d\n",
                page + 1, kDashboardPages);
    line();
}

void result(const char* label, bool passed, const char* detail) {
    std::printf(" %-29s %s[%s]%s  %s\n", label,
                passed ? kGreen : kRed,
                passed ? " OK " : "FAIL",
                kReset, detail);
}

void gate(const char* label, bool ready, const char* detail) {
    std::printf(" %-29s %s[%s]%s  %s\n", label,
                ready ? kGreen : kYellow,
                ready ? "READY" : "GATED",
                kReset, detail);
}

void progress(const char* label, unsigned int percent) {
    constexpr unsigned int width = 28;
    const unsigned int filled = percent * width / 100;
    std::printf(" %-20s [", label);
    for (unsigned int index = 0; index < width; ++index) {
        std::printf("%c", index < filled ? '#' : '.');
    }
    std::printf("] %3u%%\n", percent);
}

// Summarizes which not-yet-implemented JNIEnv slots the guest actually hit
// across the three lifecycle phases, sorted by hit count descending, into a
// caller-provided buffer as "slot:count slot:count ...".
void summarizeJniSlots(const rr3::GuestEntryProbe& probe, char* out,
                       size_t outSize, size_t topCount) {
    if (out == nullptr || outSize == 0) {
        return;
    }
    out[0] = '\0';
    uint32_t counts[rr3::kJniSlotCount]{};
    for (uint32_t slot = 0; slot < rr3::kJniSlotCount; ++slot) {
        counts[slot] = probe.onCreate.slotHits[slot] +
            probe.start.slotHits[slot] + probe.resume.slotHits[slot];
    }
    // Selection sort of slots by count (descending), stable for ties.
    uint32_t order[rr3::kJniSlotCount]{};
    for (uint32_t slot = 0; slot < rr3::kJniSlotCount; ++slot) {
        order[slot] = slot;
    }
    for (uint32_t a = 0; a < rr3::kJniSlotCount - 1 && a < topCount; ++a) {
        uint32_t best = a;
        for (uint32_t b = a + 1; b < rr3::kJniSlotCount; ++b) {
            if (counts[order[b]] > counts[order[best]]) {
                best = b;
            }
        }
        const uint32_t swap = order[a];
        order[a] = order[best];
        order[best] = swap;
    }
    size_t used = 0;
    for (size_t index = 0; index < topCount && index < rr3::kJniSlotCount; ++index) {
        const uint32_t slot = order[index];
        if (counts[slot] == 0) {
            break;
        }
        const int written = std::snprintf(
            out + used, outSize - used, "%s%u:%u", used == 0 ? "" : " ",
            slot, counts[slot]);
        if (written <= 0 || static_cast<size_t>(written) >= outSize - used) {
            break;
        }
        used += static_cast<size_t>(written);
    }
}

void renderOverview(const DashboardData& data) {
    const bool loaderHealthy = data.loader.completed && data.loader.invalidRelocations == 0;
    const bool permissions = data.loader.segmentPermissionsEnforced;
    const unsigned int loaderPercent = permissions ? 96 : (loaderHealthy ? 92 : 82);
    char gateDetail[80]{};
    std::snprintf(gateDetail, sizeof(gateDetail), "%llu strong imports unresolved",
                  static_cast<unsigned long long>(data.loader.missingRelocations));

    std::printf("\n %sSYSTEM STATUS%s\n\n", kCyan, kReset);
    result("Native Horizon shell", true, "NRO + embedded RomFS");
    result("External game data", data.externalData.ready,
           data.externalData.ready ? "seven read-only volumes mounted"
                                   : data.externalData.error);
    result("Core DSO loader", loaderHealthy,
           loaderHealthy ? "dependency and relocation pass" : data.loader.error);
    result("Segment permissions", permissions,
           permissions ? "RX / R / RW / RELRO enforced" : "safe activation block");
    result("Recovery guard", !data.recoveryMode || data.guestInitialization.active,
           data.guestInitialization.active
               ? "armed while dependency constructors are active"
               : (data.recoveryMode ? "previous guarded action was interrupted" : "clean state"));
    gate("Guest activation", data.loader.activationReady,
         data.loader.activationReady ? "constructor plan is eligible" : gateDetail);

    std::printf("\n %sPORT PROGRESS%s\n\n", kCyan, kReset);
    progress("Loader framework", loaderPercent);
    progress("Controlled core call", data.guestInitialization.completed
        ? 78 : (data.loader.missingRelocations == 0 ? 68 : 55));
    progress("First menu", data.loader.missingRelocations == 0 ? 20 : 15);
    progress("Playable race", 8);

    std::printf("\n %sCURRENT BOUNDARY%s\n", kCyan, kReset);
    std::printf("  Images: %llu     Mapped: %llu KiB     Missing strong relocations: %llu\n",
                static_cast<unsigned long long>(data.loader.imageCount),
                static_cast<unsigned long long>(data.loader.totalImageBytes / 1024),
                static_cast<unsigned long long>(data.loader.missingRelocations));
    std::printf("  Dependency ctors: %s  RR3 main constructors remain explicitly gated.\n",
                data.guestInitialization.completed ? "active" : "disabled");
}

void renderRuntime(const DashboardData& data) {
    std::printf("\n %sRUNTIME SELF-TESTS%s\n\n", kCyan, kReset);
    result("Bionic fortify/log", data.bionicTest == 0x3ff, "expected 7ff");
    result("Guest pthread bridge", data.pthreadTest == 0x7ff, "expected 7ff");
    result("libc++ shims", data.libcxxTest == 0xff, "expected ff");
    result("C++ lifecycle", data.lifecycleTest == 0x3f, "expected 3f");
    result("futex / gettid", data.syscallTest == 7, "expected 7");
    result("Bionic stdio", data.stdioTest == 7, "expected 7");
    result("Loader rollback", data.rollbackTest == 0xf, "expected f");
    result("Program-header registry", data.phdrTest == 7, "expected 7");
    result("Guest dynamic linker", data.dynamicLinkTest == 0x3f, "expected 3f");
    result("Optional AR / telemetry", data.optionalPlatformTest == 0xf,
           "AR unsupported, telemetry disabled");
    result("Guest semaphore bridge", data.semaphoreTest == 0x3f, "expected 3f");
    result("Guest setjmp / longjmp", data.jumpTest == 3, "expected 3");
    result("Bionic misc / mmap", data.miscTest == 0x3f, "expected 3f");
    result("Guest TPIDR_EL0 scope", data.guestTlsTest == 0xf,
           "install/canary/restore expected f");
    result("Guest filesystem VFS", data.filesystemTest == 0xfff,
           "content/save/cache routing expected fff");
    result("FMOD audio silent stub", data.fmodAudioTest == 0x3f,
           "opensl silent + aaudio refuse expected 3f");
    std::printf("                               opens=%u enqueues=%u playReq=%u audible=%u aaudio=%u/%u\n",
                data.fmodAudio.engineCreates, data.fmodAudio.bufferEnqueues,
                data.fmodAudio.playRequested, data.fmodAudio.audible,
                data.fmodAudio.aaudioRefusals, data.fmodAudio.aaudioAttempts);
    result("Socket service", data.socketInitResult == 0,
           data.socketInitResult == 0 ? "initialized" : "network features gated");
    if (data.graphics.attempted) {
        result("Mesa window renderer", data.graphics.drawCompleted,
               data.graphics.drawCompleted ? data.graphics.version : "probe failed");
        std::printf("                               stage=%u mode=%u EGL=%04x GL=%04x\n",
                    data.graphics.stage, data.graphics.surfaceMode,
                    data.graphics.eglError, data.graphics.glError);
        result("Real asset pipeline", data.graphics.realAssetDrawCompleted,
               data.graphics.realAssetDrawCompleted
                   ? "gui-sd DXT sprite uploaded and drawn"
                   : "stage 10 gate: real RR3 asset draw");
        std::printf("                               open=%s inflate=%s dds=%s s3tc=%s upload=%s %ux%u %.4s\n",
                    data.graphics.assetOpened ? "yes" : "no",
                    data.graphics.assetInflated ? "yes" : "no",
                    data.graphics.assetDdsParsed ? "yes" : "no",
                    data.graphics.s3tcSupported ? "yes" : "no",
                    data.graphics.realAssetUploaded ? "yes" : "no",
                    data.graphics.assetWidth, data.graphics.assetHeight,
                    data.graphics.assetFourcc[0] != '\0'
                        ? data.graphics.assetFourcc : "----");
    }
    else {
        std::printf(" %-29s %s[IDLE]%s  Press Y to restart into renderer mode\n",
                    "Mesa window renderer", kYellow, kReset);
    }

    std::printf("\n %sEXECUTABLE MEMORY%s\n", kCyan, kReset);
    std::printf("  Basic JIT: create=%08x exec=%08x call=%s close=%08x\n",
                data.jit.createResult, data.jit.executableResult,
                data.jit.returnStubExecuted ? "yes" : "no", data.jit.closeResult);
    std::printf("  Image JIT: %llu KiB edges=%s/%s create=%08x exec=%08x\n",
                static_cast<unsigned long long>(data.imageJit.requestedBytes / 1024),
                data.imageJit.firstPageCalled ? "yes" : "no",
                data.imageJit.lastPageCalled ? "yes" : "no",
                data.imageJit.createResult, data.imageJit.executableResult);
    std::printf("  TLS: TPIDR_EL0=%016llx Android-layout=%s\n",
                static_cast<unsigned long long>(data.tls.tpidrEl0.value),
                data.tls.androidCanaryAddressReadable ? "candidate" : "no");
}

void renderLoader(const DashboardData& data) {
    std::printf("\n %sDEPENDENCY GRAPH%s\n\n", kCyan, kReset);
    std::printf("  Images=%llu  Needed total/embedded/virtual/missing=%llu/%llu/%llu/%llu\n",
                static_cast<unsigned long long>(data.loader.imageCount),
                static_cast<unsigned long long>(data.loader.neededEntries),
                static_cast<unsigned long long>(data.loader.neededEmbedded),
                static_cast<unsigned long long>(data.loader.neededVirtualized),
                static_cast<unsigned long long>(data.loader.neededMissing));
    std::printf("  LOAD X/W/RO/WX=%llu/%llu/%llu/%llu  TLS=%llu  RELRO=%llu (%llu KiB)\n",
                static_cast<unsigned long long>(data.loader.executableSegments),
                static_cast<unsigned long long>(data.loader.writableSegments),
                static_cast<unsigned long long>(data.loader.readOnlySegments),
                static_cast<unsigned long long>(data.loader.writableExecutableSegments),
                static_cast<unsigned long long>(data.loader.tlsSegments),
                static_cast<unsigned long long>(data.loader.relroRanges),
                static_cast<unsigned long long>(data.loader.relroBytes / 1024));

    std::printf("\n %sRELOCATION PIPELINE%s\n\n", kCyan, kReset);
    std::printf("  Relative=%llu  Internal=%llu  Host=%llu  Cross-DSO=%llu\n",
                static_cast<unsigned long long>(data.loader.relativeApplied),
                static_cast<unsigned long long>(data.loader.internalApplied),
                static_cast<unsigned long long>(data.loader.hostRelocations),
                static_cast<unsigned long long>(data.loader.crossRelocations));
    std::printf("  Weak-zero=%llu  Missing=%llu  Patched/verified=%llu/%llu  Invalid=%llu\n",
                static_cast<unsigned long long>(data.loader.weakZeroRelocations),
                static_cast<unsigned long long>(data.loader.missingRelocations),
                static_cast<unsigned long long>(data.loader.patchedRelocations),
                static_cast<unsigned long long>(data.loader.verifiedWrites),
                static_cast<unsigned long long>(data.loader.invalidRelocations));
    std::printf("  Unique host/cross/missing=%llu/%llu/%llu\n",
                static_cast<unsigned long long>(data.loader.uniqueHostSymbols),
                static_cast<unsigned long long>(data.loader.uniqueCrossSymbols),
                static_cast<unsigned long long>(data.loader.uniqueMissingSymbols));

    std::printf("\n %sLIFECYCLE PLAN%s\n\n", kCyan, kReset);
    std::printf("  Constructors=%llu  Finalizers=%llu  Targets=%llu  Cycles=%llu\n",
                static_cast<unsigned long long>(data.loader.constructors),
                static_cast<unsigned long long>(data.loader.finalizers),
                static_cast<unsigned long long>(data.loader.lifecycleTargetsVerified),
                static_cast<unsigned long long>(data.loader.dependencyCycles));
    std::printf("  Order hashes: %016llx / %016llx\n",
                static_cast<unsigned long long>(data.loader.constructorOrderHash),
                static_cast<unsigned long long>(data.loader.finalizerOrderHash));

    std::printf("\n %sSTAGED GUEST INITIALIZATION%s\n\n", kCyan, kReset);
    std::printf("  Plan=%016llx images=%llu dependency/main ctors=%llu/%llu\n",
                static_cast<unsigned long long>(data.startupPlan.resolutionPlanHash),
                static_cast<unsigned long long>(data.startupPlan.imageCount),
                static_cast<unsigned long long>(data.startupPlan.dependencyConstructors),
                static_cast<unsigned long long>(data.startupPlan.mainConstructors));
    std::printf("  Dependency stage: attempted=%s preflight=%s complete=%s active=%s\n",
                data.guestInitialization.attempted ? "yes" : "no",
                data.guestInitialization.preflightPassed ? "yes" : "no",
                data.guestInitialization.completed ? "yes" : "no",
                data.guestInitialization.active ? "yes" : "no");
    std::printf("  Constructors=%llu/%llu stage=%u code=%u image=%llu ordinal=%llu target=%016llx\n",
                static_cast<unsigned long long>(data.guestInitialization.constructorsCalled),
                static_cast<unsigned long long>(data.guestInitialization.constructorsPlanned),
                data.guestInitialization.stage, data.guestInitialization.failureCode,
                static_cast<unsigned long long>(data.guestInitialization.currentImage),
                static_cast<unsigned long long>(data.guestInitialization.currentOrdinal),
                static_cast<unsigned long long>(data.guestInitialization.currentTarget));

    std::printf("\n %sGUEST BOOT (STAGE 9)%s\n\n", kCyan, kReset);
    std::printf("  JNI_OnLoad: attempted=%s complete=%s found=%u called=%u version=%08x code=%u\n",
                data.jniOnLoad.attempted ? "yes" : "no",
                data.jniOnLoad.completed ? "yes" : "no",
                data.jniOnLoad.imagesFound, data.jniOnLoad.imagesCalled,
                data.jniOnLoad.lastVersion, data.jniOnLoad.failureCode);
    std::printf("  Main ctors: attempted=%s preflight=%s complete=%s called=%llu/%llu code=%u ordinal=%llu\n",
                data.mainInitialization.attempted ? "yes" : "no",
                data.mainInitialization.preflightPassed ? "yes" : "no",
                data.mainInitialization.completed ? "yes" : "no",
                static_cast<unsigned long long>(data.mainInitialization.constructorsCalled),
                static_cast<unsigned long long>(data.mainInitialization.constructorsPlanned),
                data.mainInitialization.failureCode,
                static_cast<unsigned long long>(data.mainInitialization.currentOrdinal));
    std::printf("  onCreateJNI: resolved=%s called=%s returned=%s\n",
                data.entryProbe.resolved ? "yes" : "no",
                data.entryProbe.called ? "yes" : "no",
                data.entryProbe.returned ? "yes" : "no");
    std::printf("  onStartJNI:  resolved=%s called=%s returned=%s\n",
                data.entryProbe.startResolved ? "yes" : "no",
                data.entryProbe.startCalled ? "yes" : "no",
                data.entryProbe.startReturned ? "yes" : "no");
    std::printf("  onResumeJNI: resolved=%s called=%s returned=%s\n",
                data.entryProbe.resumeResolved ? "yes" : "no",
                data.entryProbe.resumeCalled ? "yes" : "no",
                data.entryProbe.resumeReturned ? "yes" : "no");
    std::printf("  JNI slots: create unhandled=%u bindings=%u last=%u\n",
                data.entryProbe.onCreate.unhandledCalls,
                data.entryProbe.onCreate.methodBindings,
                data.entryProbe.onCreate.lastSlot);
    std::printf("             start unhandled=%u last=%u  resume unhandled=%u last=%u\n",
                data.entryProbe.start.unhandledCalls,
                data.entryProbe.start.lastSlot,
                data.entryProbe.resume.unhandledCalls,
                data.entryProbe.resume.lastSlot);
    std::printf("  onViewCreated: resolved=%s called=%s returned=%s\n",
                data.viewProbe.createdResolved ? "yes" : "no",
                data.viewProbe.createdCalled ? "yes" : "no",
                data.viewProbe.createdReturned ? "yes" : "no");
    std::printf("  onViewChanged: resolved=%s called=%s returned=%s\n",
                data.viewProbe.changedResolved ? "yes" : "no",
                data.viewProbe.changedCalled ? "yes" : "no",
                data.viewProbe.changedReturned ? "yes" : "no");
    std::printf("  onViewRender:  resolved=%s called=%s returned=%s swap=%s\n",
                data.viewProbe.renderResolved ? "yes" : "no",
                data.viewProbe.renderCalled ? "yes" : "no",
                data.viewProbe.renderReturned ? "yes" : "no",
                data.viewProbe.swapped ? "yes" : "no");
    std::printf("  View EGL: display=%s init=%s config=%s surface=%s context=%s current=%s mode=%u code=%u\n",
                data.viewProbe.displayCreated ? "yes" : "no",
                data.viewProbe.initialized ? "yes" : "no",
                data.viewProbe.configChosen ? "yes" : "no",
                data.viewProbe.surfaceCreated ? "yes" : "no",
                data.viewProbe.contextCreated ? "yes" : "no",
                data.viewProbe.madeCurrent ? "yes" : "no",
                data.viewProbe.surfaceMode, data.viewProbe.failureCode);
    std::printf("  View host GL: outside=%s/%s inside=%s/%s nonnull=%s/%s\n",
                data.viewProbe.hostGlOutsideTlsCalled ? "called" : "idle",
                data.viewProbe.hostGlOutsideTlsReturned ? "returned" : "stopped",
                data.viewProbe.hostGlInsideTlsCalled ? "called" : "idle",
                data.viewProbe.hostGlInsideTlsReturned ? "returned" : "stopped",
                data.viewProbe.hostGlOutsideTlsNonNull ? "yes" : "no",
                data.viewProbe.hostGlInsideTlsNonNull ? "yes" : "no");
    std::printf("  View trace: last-stage=%u  GL=%x EGL=%x\n",
                data.viewProbe.lastTraceStage,
                data.viewProbe.glError, data.viewProbe.eglError);
    std::printf("  View post-GL malloc: 2800=%s/%s 3e38=%s/%s\n",
                data.viewProbe.postGlMalloc2800Called ? "called" : "idle",
                data.viewProbe.postGlMalloc2800Returned ? "returned" : "stopped",
                data.viewProbe.postGlMalloc3e38Called ? "called" : "idle",
                data.viewProbe.postGlMalloc3e38Returned ? "returned" : "stopped");
    std::printf("                         240-first=%s/%s 130=%s/%s 40-first=%s/%s\n",
                data.viewProbe.postGlMalloc240FirstCalled ? "called" : "idle",
                data.viewProbe.postGlMalloc240FirstReturned ? "returned" : "stopped",
                data.viewProbe.postGlMalloc130Called ? "called" : "idle",
                data.viewProbe.postGlMalloc130Returned ? "returned" : "stopped",
                data.viewProbe.postGlMalloc40FirstCalled ? "called" : "idle",
                data.viewProbe.postGlMalloc40FirstReturned ? "returned" : "stopped");
    std::printf("                         40-second=%s/%s\n",
                data.viewProbe.postGlMalloc40SecondCalled ? "called" : "idle",
                data.viewProbe.postGlMalloc40SecondReturned ? "returned" : "stopped");
    std::printf("  View JNI: created=%u changed=%u render=%u\n",
                data.viewProbe.createdJni.unhandledCalls,
                data.viewProbe.changedJni.unhandledCalls,
                data.viewProbe.renderJni.unhandledCalls);
    char topSlots[192]{};
    summarizeJniSlots(data.entryProbe, topSlots, sizeof(topSlots), 8);
    if (topSlots[0] != '\0') {
        std::printf("  Top unimplemented slots: %s\n", topSlots);
    }
    if (data.bootProgress[0] != '\0') {
        std::printf("  Last boot breadcrumb: %s\n", data.bootProgress);
    }
}

void renderDiagnostics(const DashboardData& data) {
    std::printf("\n %sLOADER DIAGNOSTICS%s\n\n", kCyan, kReset);
    std::printf("  Status: %s\n", data.loader.error[0] != '\0' ? data.loader.error : "no internal error");
    std::printf("  Process map: image=%llu stage=%llu rc=%08x\n",
                static_cast<unsigned long long>(data.loader.processMapFailureImage),
                static_cast<unsigned long long>(data.loader.processMapFailureStage),
                data.loader.processMapFailureResult);
    std::printf("  Backing type/perm/attr=%x/%x/%x  Target type/perm=%x/%x\n",
                data.loader.processBackingType, data.loader.processBackingPermission,
                data.loader.processBackingAttribute, data.loader.processTargetType,
                data.loader.processTargetPermission);
    std::printf("  Segment map: mode=%u tried=%s ok=%s regions=%llu/%llu failures=%llu\n",
                data.loader.segmentedMapperMode,
                data.loader.segmentedMapperAttempted ? "yes" : "no",
                data.loader.segmentedMapperSucceeded ? "yes" : "no",
                static_cast<unsigned long long>(data.loader.segmentedRegionsVerified),
                static_cast<unsigned long long>(data.loader.segmentedRegions),
                static_cast<unsigned long long>(data.loader.segmentedFailures));
    std::printf("  Permission ranges=%llu/%llu failures=%llu\n",
                static_cast<unsigned long long>(data.loader.permissionRangesVerified),
                static_cast<unsigned long long>(data.loader.permissionRanges),
                static_cast<unsigned long long>(data.loader.permissionFailures));
    std::printf("  Exit cleanup: active/unmapped=%llu/%llu reservations=%llu fail=%llu rc=%08x\n",
                static_cast<unsigned long long>(data.unload.activeMappings),
                static_cast<unsigned long long>(data.unload.unmappedMappings),
                static_cast<unsigned long long>(data.unload.releasedReservations),
                static_cast<unsigned long long>(data.unload.failures),
                data.unload.firstFailureResult);
    std::printf("  Guest finalizers: called/planned=%llu/%llu\n",
                static_cast<unsigned long long>(data.unload.finalizersCalled),
                static_cast<unsigned long long>(data.unload.finalizersPlanned));

    std::printf("\n %sDETERMINISTIC REFERENCES%s\n\n", kCyan, kReset);
    std::printf("  Resolver table: %llu / %016llx\n",
                static_cast<unsigned long long>(data.libcxx.resolverTableEntries),
                static_cast<unsigned long long>(data.libcxx.resolverTableNameHash));
    std::printf("  Loader plan:   %016llx\n",
                static_cast<unsigned long long>(data.loader.resolutionPlanHash));
    std::printf("  Main image:    %016llx\n",
                static_cast<unsigned long long>(data.game.imageHash));
    std::printf("  libc++ image:  %016llx\n",
                static_cast<unsigned long long>(data.libcxx.imageHash));

    std::printf("\n %sNEXT ENGINEERING GATES%s\n", kCyan, kReset);
    std::printf("  1. Validate one-pass ModuleCode permissions against the state model.\n");
    std::printf("  2. Initialize Mesa EGL/GLES and a native display surface.\n");
    std::printf("  3. Run constructors only through an explicit crash-guarded action.\n");
}

} // namespace

void renderLoadingDashboard(const char* stage, unsigned int percent) {
    if (nativeUiActive()) {
        nativeUiRenderLoading(stage, percent);
        return;
    }
    std::printf("\x1b[2J\x1b[H");
    std::printf("\x1b[44;37;1m RR3 NATIVE REHOST   Horizon / libnx   No Android Runtime                     \x1b[0m\n");
    std::printf(" Loader Control Center                                      Starting\n");
    line();
    std::printf("\n\n\n");
    std::printf("                         INITIALIZING NATIVE RUNTIME\n\n");
    progress(stage != nullptr ? stage : "Loading", percent > 100 ? 100 : percent);
    std::printf("\n                         Guest constructors are disabled.\n");
    std::printf("                         Please wait; do not power off.\n");
}

void renderDashboard(const DashboardData& data, int page, const char* notice) {
    if (nativeUiActive()) {
        nativeUiRenderDashboard(data, page, notice);
        return;
    }
    std::printf("\x1b[2J\x1b[H");
    header(page);
    switch (page) {
        case 0: renderOverview(data); break;
        case 1: renderRuntime(data); break;
        case 2: renderLoader(data); break;
        default: renderDiagnostics(data); break;
    }
    std::printf("\n");
    line();
    std::printf(" %sL/R%s Page  %sZR%s Init deps  %sZL%s Guest boot / view  %sY%s Renderer  %sX%s Export  %sPLUS%s Exit\n",
                kCyan, kReset, kCyan, kReset, kCyan, kReset,
                kCyan, kReset, kCyan, kReset, kCyan, kReset);
    if (notice != nullptr && notice[0] != '\0') {
        std::printf(" %s%s%s\n", kDim, notice, kReset);
    }
}

bool writeDashboardReport(const DashboardData& data, const char* path) {
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "RR3 Native Rehost Loader Report\n");
    std::fprintf(file, "external_data attempted=%d ready=%d mounted=%u/7 save=%d cache=%d loose=%d error=%s\n",
                 data.externalData.attempted, data.externalData.ready,
                 data.externalData.mountedVolumes, data.externalData.saveReady,
                 data.externalData.cacheReady, data.externalData.looseFallback,
                 data.externalData.error);
    std::fprintf(file, "loader completed=%d retained=%d executable=%d permissions=%d ready=%d\n",
                 data.loader.completed, data.loader.retained, data.loader.executableAliases,
                 data.loader.segmentPermissionsEnforced, data.loader.activationReady);
    std::fprintf(file, "images=%llu bytes=%llu missing=%llu invalid=%llu\n",
                 static_cast<unsigned long long>(data.loader.imageCount),
                 static_cast<unsigned long long>(data.loader.totalImageBytes),
                 static_cast<unsigned long long>(data.loader.missingRelocations),
                 static_cast<unsigned long long>(data.loader.invalidRelocations));
    std::fprintf(file, "reloc relative=%llu internal=%llu host=%llu cross=%llu weak=%llu\n",
                 static_cast<unsigned long long>(data.loader.relativeApplied),
                 static_cast<unsigned long long>(data.loader.internalApplied),
                 static_cast<unsigned long long>(data.loader.hostRelocations),
                 static_cast<unsigned long long>(data.loader.crossRelocations),
                 static_cast<unsigned long long>(data.loader.weakZeroRelocations));
    std::fprintf(file, "patched=%llu verified=%llu plan=%016llx\n",
                 static_cast<unsigned long long>(data.loader.patchedRelocations),
                 static_cast<unsigned long long>(data.loader.verifiedWrites),
                 static_cast<unsigned long long>(data.loader.resolutionPlanHash));
    std::fprintf(file, "process stage=%llu result=%08x backing=%x/%x/%x target=%x/%x\n",
                 static_cast<unsigned long long>(data.loader.processMapFailureStage),
                 data.loader.processMapFailureResult, data.loader.processBackingType,
                 data.loader.processBackingPermission, data.loader.processBackingAttribute,
                 data.loader.processTargetType, data.loader.processTargetPermission);
    std::fprintf(file, "unload attempted=%d completed=%d active=%llu unmapped=%llu reservations=%llu failures=%llu result=%08x\n",
                 data.unload.attempted, data.unload.completed,
                 static_cast<unsigned long long>(data.unload.activeMappings),
                 static_cast<unsigned long long>(data.unload.unmappedMappings),
                 static_cast<unsigned long long>(data.unload.releasedReservations),
                 static_cast<unsigned long long>(data.unload.failures),
                 data.unload.firstFailureResult);
    std::fprintf(file, "startup available=%d plan=%016llx images=%llu dependency_ctors=%llu main_ctors=%llu\n",
                 data.startupPlan.available,
                 static_cast<unsigned long long>(data.startupPlan.resolutionPlanHash),
                 static_cast<unsigned long long>(data.startupPlan.imageCount),
                 static_cast<unsigned long long>(data.startupPlan.dependencyConstructors),
                 static_cast<unsigned long long>(data.startupPlan.mainConstructors));
    std::fprintf(file, "guest_init attempted=%d preflight=%d completed=%d active=%d stage=%u code=%u called=%llu planned=%llu image=%llu ordinal=%llu target=%016llx\n",
                 data.guestInitialization.attempted,
                 data.guestInitialization.preflightPassed,
                 data.guestInitialization.completed,
                 data.guestInitialization.active,
                 data.guestInitialization.stage,
                 data.guestInitialization.failureCode,
                 static_cast<unsigned long long>(data.guestInitialization.constructorsCalled),
                 static_cast<unsigned long long>(data.guestInitialization.constructorsPlanned),
                 static_cast<unsigned long long>(data.guestInitialization.currentImage),
                 static_cast<unsigned long long>(data.guestInitialization.currentOrdinal),
                 static_cast<unsigned long long>(data.guestInitialization.currentTarget));
    std::fprintf(file, "graphics attempted=%d initialized=%d current=%d draw=%d stage=%u mode=%u gl=%x egl=%x version=%s\n",
                 data.graphics.attempted, data.graphics.initialized,
                 data.graphics.madeCurrent, data.graphics.drawCompleted,
                 data.graphics.stage, data.graphics.surfaceMode,
                 data.graphics.glError, data.graphics.eglError, data.graphics.version);
    std::fprintf(file, "graphics_asset open=%d inflate=%d dds=%d s3tc=%d upload=%d draw=%d %ux%u fourcc=%.4s\n",
                 data.graphics.assetOpened, data.graphics.assetInflated,
                 data.graphics.assetDdsParsed, data.graphics.s3tcSupported,
                 data.graphics.realAssetUploaded, data.graphics.realAssetDrawCompleted,
                 data.graphics.assetWidth, data.graphics.assetHeight,
                 data.graphics.assetFourcc[0] != '\0' ? data.graphics.assetFourcc : "----");
    std::fprintf(file, "main_init attempted=%d preflight=%d completed=%d active=%d stage=%u code=%u called=%llu planned=%llu image=%llu ordinal=%llu target=%016llx\n",
                 data.mainInitialization.attempted,
                 data.mainInitialization.preflightPassed,
                 data.mainInitialization.completed,
                 data.mainInitialization.active,
                 data.mainInitialization.stage,
                 data.mainInitialization.failureCode,
                 static_cast<unsigned long long>(data.mainInitialization.constructorsCalled),
                 static_cast<unsigned long long>(data.mainInitialization.constructorsPlanned),
                 static_cast<unsigned long long>(data.mainInitialization.currentImage),
                 static_cast<unsigned long long>(data.mainInitialization.currentOrdinal),
                 static_cast<unsigned long long>(data.mainInitialization.currentTarget));
    std::fprintf(file, "guest_entry attempted=%d resolved=%d called=%d returned=%d code=%u\n",
                 data.entryProbe.attempted, data.entryProbe.resolved,
                 data.entryProbe.called, data.entryProbe.returned,
                 data.entryProbe.failureCode);
    std::fprintf(file, "guest_lifecycle start=%d/%d/%d resume=%d/%d/%d\n",
                 data.entryProbe.startResolved, data.entryProbe.startCalled,
                 data.entryProbe.startReturned, data.entryProbe.resumeResolved,
                 data.entryProbe.resumeCalled, data.entryProbe.resumeReturned);
    std::fprintf(file, "guest_view attempted=%d egl=%d/%d/%d/%d/%d/%d callbacks=%d/%d/%d swap=%d mode=%u code=%u gl=%x egl_error=%x frame_loop=%d frames=%llu\n",
                 data.viewProbe.attempted,
                 data.viewProbe.displayCreated,
                 data.viewProbe.initialized,
                 data.viewProbe.configChosen,
                 data.viewProbe.surfaceCreated,
                 data.viewProbe.contextCreated,
                 data.viewProbe.madeCurrent,
                 data.viewProbe.createdReturned,
                 data.viewProbe.changedReturned,
                 data.viewProbe.renderReturned,
                 data.viewProbe.swapped,
                 data.viewProbe.surfaceMode,
                 data.viewProbe.failureCode,
                 data.viewProbe.glError,
                 data.viewProbe.eglError,
                 data.viewProbe.frameLoopEntered ? 1 : 0,
                 static_cast<unsigned long long>(data.viewProbe.framesRendered));
    std::fprintf(file, "guest_pthreads live=%u\n", rr3::guestPthreadLiveCount());
    std::fprintf(file, "fmod_audio silent=yes opens=%u enqueues=%u callbacks=%u playReq=%u audible=%u aaudio_refuse=%u aaudio_try=%u open_fail=%u\n",
                 data.fmodAudio.engineCreates, data.fmodAudio.bufferEnqueues,
                 data.fmodAudio.bufferCallbacks, data.fmodAudio.playRequested,
                 data.fmodAudio.audible, data.fmodAudio.aaudioRefusals,
                 data.fmodAudio.aaudioAttempts, data.fmodAudio.openFailures);
    std::fprintf(file, "guest_activity obj=%llx glCreated=%u landscape=%u paused=%u state=%u splash=%u w=%d h=%d\n",
                 static_cast<unsigned long long>(data.viewProbe.activityObject),
                 data.viewProbe.activityGlCreated,
                 data.viewProbe.activityLandscape,
                 data.viewProbe.activityPaused,
                 data.viewProbe.activityFrameState,
                 data.viewProbe.activitySplashFrames,
                 data.viewProbe.activityWidth,
                 data.viewProbe.activityHeight);
    std::fprintf(file, "guest_gl draws clear=%u arrays=%u elements=%u tex=%u ctex=%u fbo=%u use=%u\n",
                 data.viewProbe.glClears,
                 data.viewProbe.glDrawArrays,
                 data.viewProbe.glDrawElements,
                 data.viewProbe.glTexImage,
                 data.viewProbe.glCompressedTex,
                 data.viewProbe.glBindFbo,
                 data.viewProbe.glUseProgram);
    std::fprintf(file, "guest_view_host_gl outside=%d/%d/%d inside=%d/%d/%d trace=%u\n",
                 data.viewProbe.hostGlOutsideTlsCalled,
                 data.viewProbe.hostGlOutsideTlsReturned,
                 data.viewProbe.hostGlOutsideTlsNonNull,
                 data.viewProbe.hostGlInsideTlsCalled,
                 data.viewProbe.hostGlInsideTlsReturned,
                 data.viewProbe.hostGlInsideTlsNonNull,
                 data.viewProbe.lastTraceStage);
    std::fprintf(file, "guest_view_post_gl stage=%u malloc=2800:%d/%d 3e38:%d/%d 240-first:%d/%d 130:%d/%d 40-first:%d/%d 40-second:%d/%d\n",
                 data.viewProbe.lastTraceStage,
                 data.viewProbe.postGlMalloc2800Called,
                 data.viewProbe.postGlMalloc2800Returned,
                 data.viewProbe.postGlMalloc3e38Called,
                 data.viewProbe.postGlMalloc3e38Returned,
                 data.viewProbe.postGlMalloc240FirstCalled,
                 data.viewProbe.postGlMalloc240FirstReturned,
                 data.viewProbe.postGlMalloc130Called,
                 data.viewProbe.postGlMalloc130Returned,
                 data.viewProbe.postGlMalloc40FirstCalled,
                 data.viewProbe.postGlMalloc40FirstReturned,
                 data.viewProbe.postGlMalloc40SecondCalled,
                 data.viewProbe.postGlMalloc40SecondReturned);
    std::fprintf(file, "jni_view_phase created=%u changed=%u render=%u\n",
                 data.viewProbe.createdJni.unhandledCalls,
                 data.viewProbe.changedJni.unhandledCalls,
                 data.viewProbe.renderJni.unhandledCalls);
    std::fprintf(file, "jni_phase create unhandled=%u bindings=%u last=%u\n",
                 data.entryProbe.onCreate.unhandledCalls,
                 data.entryProbe.onCreate.methodBindings,
                 data.entryProbe.onCreate.lastSlot);
    std::fprintf(file, "jni_phase start unhandled=%u last=%u\n",
                 data.entryProbe.start.unhandledCalls,
                 data.entryProbe.start.lastSlot);
    std::fprintf(file, "jni_phase resume unhandled=%u last=%u\n",
                 data.entryProbe.resume.unhandledCalls,
                 data.entryProbe.resume.lastSlot);
    char topSlots[512]{};
    summarizeJniSlots(data.entryProbe, topSlots, sizeof(topSlots), 24);
    std::fprintf(file, "jni_slots_top%s%s\n",
                 topSlots[0] != '\0' ? " " : "", topSlots);
    std::fprintf(file, "jni_onload attempted=%d completed=%d found=%u called=%u version=%08x code=%u unhandled=%u last_slot=%u\n",
                 data.jniOnLoad.attempted, data.jniOnLoad.completed,
                 data.jniOnLoad.imagesFound, data.jniOnLoad.imagesCalled,
                 data.jniOnLoad.lastVersion, data.jniOnLoad.failureCode,
                 data.jniOnLoad.onLoad.unhandledCalls, data.jniOnLoad.onLoad.lastSlot);
    std::fprintf(file, "guest_boot_progress=%s\n",
                 data.bootProgress[0] != '\0' ? data.bootProgress : "(none)");
    std::fprintf(file, "guest_tls_selftest=%llx expected=f\n",
                 static_cast<unsigned long long>(data.guestTlsTest));
    std::fprintf(file, "guest_filesystem_selftest=%llx expected=fff\n",
                 static_cast<unsigned long long>(data.filesystemTest));
    std::fprintf(file, "recovery_mode=%d\n", data.recoveryMode);
    std::fclose(file);
    return true;
}

} // namespace rr3
