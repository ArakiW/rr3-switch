#include <switch.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "rr3/elf_preflight.hpp"
#include "rr3/bionic_shims.hpp"
#include "rr3/bionic_misc_shims.hpp"
#include "rr3/cxx_lifecycle.hpp"
#include "rr3/fmod_audio_stub.hpp"
#include "rr3/jit_probe.hpp"
#include "rr3/jni_bridge.hpp"
#include "rr3/jump_bridge.hpp"
#include "rr3/graphics_probe.hpp"
#include "rr3/guest_filesystem.hpp"
#include "rr3/libcxx_shims.hpp"
#include "rr3/native_contract.hpp"
#include "rr3/native_ui.hpp"
#include "rr3/optional_platform_shims.hpp"
#include "rr3/pthread_bridge.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/semaphore_bridge.hpp"
#include "rr3/tls_probe.hpp"
#include "rr3/ui_dashboard.hpp"

namespace {

template <typename... Arguments>
int bootPrintf(const char* format, Arguments... arguments) {
    (void)format;
    (void)sizeof...(arguments);
    return 0;
}

bool pathExists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

bool hasArgument(int argc, char** argv, const char* expected) {
    if (argv == nullptr || expected == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && std::strcmp(argv[index], expected) == 0) {
            return true;
        }
    }
    return false;
}

void runtimeTrace(const char* message) {
    if (message != nullptr) {
        svcOutputDebugString(message, std::strlen(message));
    }
}

const char* findArgumentValue(int argc, char** argv, const char* prefix) {
    if (argv == nullptr || prefix == nullptr) {
        return nullptr;
    }
    const size_t prefixLength = std::strlen(prefix);
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr &&
            std::strncmp(argv[index], prefix, prefixLength) == 0) {
            return argv[index] + prefixLength;
        }
    }
    return nullptr;
}

Result scheduleSelfLoad(int argc, char** argv, const char* argument) {
    if (!envHasNextLoad() || argc <= 0 || argv == nullptr ||
        argv[0] == nullptr || argument == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    char reloadArguments[0x800]{};
    const int written = std::snprintf(
        reloadArguments, sizeof(reloadArguments),
        "\"%s\" %s", argv[0], argument);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(reloadArguments)) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    return envSetNextLoad(argv[0], reloadArguments);
}

uint32_t graphicsProbeFlags(const rr3::GraphicsProbe& probe) {
    return (probe.attempted ? 1U : 0U) |
        (probe.displayCreated ? 2U : 0U) |
        (probe.initialized ? 4U : 0U) |
        (probe.configChosen ? 8U : 0U) |
        (probe.contextCreated ? 16U : 0U) |
        (probe.surfaceCreated ? 32U : 0U) |
        (probe.madeCurrent ? 64U : 0U) |
        (probe.drawCompleted ? 128U : 0U) |
        (probe.vertexShaderCompiled ? 256U : 0U) |
        (probe.fragmentShaderCompiled ? 512U : 0U) |
        (probe.programLinked ? 1024U : 0U) |
        (probe.realisticShaderCompiled ? 2048U : 0U) |
        (probe.realisticProgramLinked ? 4096U : 0U) |
        (probe.textureUploaded ? 8192U : 0U) |
        (probe.texturedDrawCompleted ? 16384U : 0U) |
        (probe.assetOpened ? 32768U : 0U) |
        (probe.assetInflated ? 65536U : 0U) |
        (probe.assetDdsParsed ? 131072U : 0U) |
        (probe.s3tcSupported ? 262144U : 0U) |
        (probe.realAssetUploaded ? 524288U : 0U) |
        (probe.realAssetDrawCompleted ? 1048576U : 0U);
}

rr3::GraphicsProbe decodeGraphicsProbe(int argc, char** argv) {
    constexpr const char* kPrefix = "--rr3-renderer-result=";
    const char* encoded = findArgumentValue(argc, argv, kPrefix);
    rr3::GraphicsProbe probe{};
    unsigned int flags = 0;
    unsigned int stage = 0;
    unsigned int mode = 0;
    unsigned int eglError = 0;
    unsigned int glError = 0;
    int major = 0;
    int minor = 0;
    unsigned int assetWidth = 0;
    unsigned int assetHeight = 0;
    unsigned int fourcc = 0;
    const int parsed =
        encoded == nullptr
            ? 0
            : std::sscanf(encoded, "%x,%u,%u,%x,%x,%d,%d,%u,%u,%8x",
                          &flags, &stage, &mode, &eglError, &glError,
                          &major, &minor, &assetWidth, &assetHeight, &fourcc);
    // The stage-8 fields (fields 8-10) are optional so an older renderer
    // stage that only emits seven values still decodes cleanly.
    if (parsed < 7) {
        return probe;
    }

    probe.attempted = (flags & 1U) != 0;
    probe.displayCreated = (flags & 2U) != 0;
    probe.initialized = (flags & 4U) != 0;
    probe.configChosen = (flags & 8U) != 0;
    probe.contextCreated = (flags & 16U) != 0;
    probe.surfaceCreated = (flags & 32U) != 0;
    probe.madeCurrent = (flags & 64U) != 0;
    probe.drawCompleted = (flags & 128U) != 0;
    probe.vertexShaderCompiled = (flags & 256U) != 0;
    probe.fragmentShaderCompiled = (flags & 512U) != 0;
    probe.programLinked = (flags & 1024U) != 0;
    probe.realisticShaderCompiled = (flags & 2048U) != 0;
    probe.realisticProgramLinked = (flags & 4096U) != 0;
    probe.textureUploaded = (flags & 8192U) != 0;
    probe.texturedDrawCompleted = (flags & 16384U) != 0;
    probe.assetOpened = (flags & 32768U) != 0;
    probe.assetInflated = (flags & 65536U) != 0;
    probe.assetDdsParsed = (flags & 131072U) != 0;
    probe.s3tcSupported = (flags & 262144U) != 0;
    probe.realAssetUploaded = (flags & 524288U) != 0;
    probe.realAssetDrawCompleted = (flags & 1048576U) != 0;
    probe.assetWidth = assetWidth;
    probe.assetHeight = assetHeight;
    if (parsed >= 10) {
        probe.assetFourcc[0] = static_cast<char>((fourcc >> 24) & 0xFF);
        probe.assetFourcc[1] = static_cast<char>((fourcc >> 16) & 0xFF);
        probe.assetFourcc[2] = static_cast<char>((fourcc >> 8) & 0xFF);
        probe.assetFourcc[3] = static_cast<char>(fourcc & 0xFF);
    }
    probe.stage = stage;
    probe.surfaceMode = mode;
    probe.eglError = eglError;
    probe.glError = glError;
    probe.major = major;
    probe.minor = minor;
    std::snprintf(probe.version, sizeof(probe.version),
                  "EGL %d.%d renderer child", major, minor);
    return probe;
}

const char* selectGameLibrary(int argc, char** argv, bool romfsReady,
                              char* siblingPath, size_t siblingPathSize) {
    if (romfsReady && pathExists("romfs:/libRealRacing3.so")) {
        return "romfs:/libRealRacing3.so";
    }

    if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
        const char* slash = std::strrchr(argv[0], '/');
        if (slash != nullptr) {
            const size_t directoryLength = static_cast<size_t>(slash - argv[0] + 1);
            constexpr const char* kLibraryName = "libRealRacing3.so";
            const size_t libraryLength = std::strlen(kLibraryName);
            if (directoryLength + libraryLength + 1 <= siblingPathSize) {
                std::memcpy(siblingPath, argv[0], directoryLength);
                std::memcpy(siblingPath + directoryLength, kLibraryName, libraryLength + 1);
                if (pathExists(siblingPath)) {
                    return siblingPath;
                }
            }
        }
    }

    constexpr const char* kFixedPath =
        "sdmc:/switch/rr3-native-rehost/libRealRacing3.so";
    if (pathExists(kFixedPath)) {
        return kFixedPath;
    }
    return nullptr;
}

bool armDependencyInitialization(const char* markerPath,
                                 const rr3::GuestStartupPlan& plan,
                                 const char* action) {
    if (markerPath == nullptr || !plan.available || plan.imageCount == 0 ||
        plan.imageCount > rr3::kGuestStartupMaximumImages) {
        return false;
    }
    char snapshot[0x700]{};
    size_t used = 0;
    const int header = std::snprintf(
        snapshot, sizeof(snapshot),
        "plan=%016llx images=%llu dependency_ctors=%llu main_ctors=%llu",
        static_cast<unsigned long long>(plan.resolutionPlanHash),
        static_cast<unsigned long long>(plan.imageCount),
        static_cast<unsigned long long>(plan.dependencyConstructors),
        static_cast<unsigned long long>(plan.mainConstructors));
    if (header <= 0 || static_cast<size_t>(header) >= sizeof(snapshot)) {
        return false;
    }
    used = static_cast<size_t>(header);
    for (size_t index = 0; index < plan.imageCount; ++index) {
        const rr3::GuestStartupImage& image = plan.images[index];
        const int appended = std::snprintf(
            snapshot + used, sizeof(snapshot) - used,
            " image%u=%s@%016llx+%llx:c%llu:f%llu",
            static_cast<unsigned int>(index), image.soname,
            static_cast<unsigned long long>(image.base),
            static_cast<unsigned long long>(image.imageBytes),
            static_cast<unsigned long long>(image.constructors),
            static_cast<unsigned long long>(image.finalizers));
        if (appended <= 0 || static_cast<size_t>(appended) >= sizeof(snapshot) - used) {
            return false;
        }
        used += static_cast<size_t>(appended);
    }
    return rr3::beginGuardedActionSnapshot(
        markerPath, action, snapshot);
}

} // namespace

int main(int argc, char** argv) {
    runtimeTrace("[rr3] main entry\n");
    // The software console, native framebuffer, and Mesa EGL all own the
    // default NWindow. Never overlap them. The renderer runs in its own NRO
    // stage and returns telemetry through hbloader argv.
    if (hasArgument(argc, argv, "--rr3-renderer")) {
        // The stage-8 asset probe reads a real sprite from the gui-sd volume,
        // so the renderer child must mount SD + external data before probing;
        // the parent's mount at the bottom of main() does not carry into this
        // separately-loaded NRO stage.
        const bool childSd = R_SUCCEEDED(fsdevMountSdmc());
        const bool childRomfs = R_SUCCEEDED(romfsInit());
        const bool childSkipData = pathExists(
            "sdmc:/switch/rr3-native-rehost/debug-skip-data.flag");
        if (!childSkipData) {
            rr3::mountExternalData();
        }
        const rr3::GraphicsProbe childProbe =
            rr3::runWindowGraphicsProbe(nwindowGetDefault());
        rr3::unmountExternalData();
        if (childRomfs) {
            romfsExit();
        }
        if (childSd) {
            fsdevUnmountAll();
        }
        char resultArgument[0x140]{};
        std::snprintf(
            resultArgument, sizeof(resultArgument),
            "--rr3-renderer-result=%x,%u,%u,%x,%x,%d,%d,%u,%u,%02x%02x%02x%02x",
            graphicsProbeFlags(childProbe), childProbe.stage,
            childProbe.surfaceMode, childProbe.eglError, childProbe.glError,
            childProbe.major, childProbe.minor,
            childProbe.assetWidth, childProbe.assetHeight,
            static_cast<unsigned char>(childProbe.assetFourcc[0]),
            static_cast<unsigned char>(childProbe.assetFourcc[1]),
            static_cast<unsigned char>(childProbe.assetFourcc[2]),
            static_cast<unsigned char>(childProbe.assetFourcc[3]));
        const Result returnResult = scheduleSelfLoad(argc, argv, resultArgument);
        return R_SUCCEEDED(returnResult) ? 0 : static_cast<int>(returnResult);
    }

    const rr3::GraphicsProbe returnedGraphics = decodeGraphicsProbe(argc, argv);

    bool diagnosticConsole = hasArgument(argc, argv, "--rr3-console") ||
        pathExists("sdmc:/switch/rr3-native-rehost/debug-console.flag");
    if (diagnosticConsole) {
        consoleInit(nullptr);
    }
    runtimeTrace("[rr3] ui backend selected\n");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{};
    padInitializeDefault(&pad);
    const Result socketResult = socketInitializeDefault();
    runtimeTrace("[rr3] socket initialized\n");
    const Result sdMountResult = fsdevMountSdmc();
    const bool sdMountedHere = R_SUCCEEDED(sdMountResult);
    const bool sdAccessible = pathExists("sdmc:/");
    runtimeTrace("[rr3] sd mounted\n");
    const Result romfsResult = romfsInit();
    const bool romfsReady = R_SUCCEEDED(romfsResult);
    runtimeTrace("[rr3] embedded romfs mounted\n");
    const bool skipExternalData = pathExists(
        "sdmc:/switch/rr3-native-rehost/debug-skip-data.flag");
    runtimeTrace("[rr3] external data mount begin\n");
    const rr3::ExternalDataStatus externalData = skipExternalData
        ? rr3::ExternalDataStatus{}
        : rr3::mountExternalData();
    runtimeTrace("[rr3] external data mount end\n");
    char externalTrace[0x100]{};
    std::snprintf(externalTrace, sizeof(externalTrace),
                  "[rr3] external ready=%u loose=%u volumes=%u error=%s\n",
                  externalData.ready ? 1U : 0U,
                  externalData.looseFallback ? 1U : 0U,
                  externalData.mountedVolumes, externalData.error);
    runtimeTrace(externalTrace);
    const uint64_t filesystemSelfTest = rr3::guestFilesystemSelfTest();
    runtimeTrace("[rr3] filesystem selftest complete\n");
    if (!diagnosticConsole && (!romfsReady || !rr3::nativeUiInit())) {
        // Font availability is checked before normal presentation. A fully
        // closed failed framebuffer may safely fall back to diagnostics.
        rr3::nativeUiExit();
        consoleInit(nullptr);
        diagnosticConsole = true;
    }
    runtimeTrace("[rr3] presentation backend initialized\n");
    const auto presentUi = [&]() {
        if (diagnosticConsole) {
            consoleUpdate(nullptr);
        }
    };
    rr3::renderLoadingDashboard("Runtime services", 15);
    presentUi();
    runtimeTrace("[rr3] first frame presented\n");

    bootPrintf("Real Racing 3 native rehost\n");
    bootPrintf("Horizon/libnx only - no Android runtime\n\n");
    bootPrintf("Formal UI 0.18.7: unload-gated renderer handoff\n");
    bootPrintf("SD mount=%08x accessible=%s  RomFS=%08x\n",
                sdMountResult,
                sdAccessible ? "yes" : "no",
                romfsResult);

    const rr3::TlsProbe tls = rr3::captureTlsProbe();
    const uint64_t guestTlsSelfTest = rr3::guestTlsScopeSelfTest();
    const rr3::JitProbe jit = rr3::captureJitProbe();
    bootPrintf("TLS tp=%016llx ro=%016llx android=%s\n",
                static_cast<unsigned long long>(tls.tpidrEl0.value),
                static_cast<unsigned long long>(tls.tpidrroEl0.value),
                tls.androidCanaryAddressReadable ? "candidate" : "no");
    bootPrintf("JIT create=%08x exec=%08x call=%s close=%08x\n",
                jit.createResult,
                jit.executableResult,
                jit.returnStubExecuted ? "yes" : "no",
                jit.closeResult);
    bootPrintf("guest TLS scope selftest=%llx expected=f\n",
                static_cast<unsigned long long>(guestTlsSelfTest));

    const uint64_t bionicSelfTest = rr3::bionicShimSelfTest();
    bootPrintf("Bionic shim selftest=%03llx expected=3ff\n",
                static_cast<unsigned long long>(bionicSelfTest));
    const uint64_t pthreadSelfTest = rr3::pthreadBridgeSelfTest();
    bootPrintf("Pthread bridge selftest=%03llx expected=7ff\n",
                static_cast<unsigned long long>(pthreadSelfTest));
    const uint64_t libcxxSelfTest = rr3::libcxxShimSelfTest();
    bootPrintf("libc++ shim selftest=%02llx expected=ff\n",
                static_cast<unsigned long long>(libcxxSelfTest));
    const uint64_t lifecycleSelfTest = rr3::cxxLifecycleSelfTest();
    bootPrintf("CXX lifecycle selftest=%02llx expected=3f\n",
                static_cast<unsigned long long>(lifecycleSelfTest));
    const uint64_t syscallSelfTest = rr3::libcxxSyscallSelfTest();
    bootPrintf("syscall futex/tid selftest=%llx expected=7\n",
                static_cast<unsigned long long>(syscallSelfTest));
    const uint64_t stdioSelfTest = rr3::bionicStdioSelfTest();
    bootPrintf("Bionic stdio selftest=%llx expected=7\n",
                static_cast<unsigned long long>(stdioSelfTest));
    const uint64_t optionalPlatformSelfTest = rr3::optionalPlatformSelfTest();
    bootPrintf("optional platform selftest=%llx expected=f\n",
                static_cast<unsigned long long>(optionalPlatformSelfTest));
    const uint64_t semaphoreSelfTest = rr3::semaphoreBridgeSelfTest();
    bootPrintf("semaphore bridge selftest=%02llx expected=3f\n",
                static_cast<unsigned long long>(semaphoreSelfTest));
    const uint64_t jumpSelfTest = rr3::jumpBridgeSelfTest();
    bootPrintf("jump bridge selftest=%llx expected=3\n",
                static_cast<unsigned long long>(jumpSelfTest));
    const uint64_t miscSelfTest = rr3::bionicMiscSelfTest();
    bootPrintf("Bionic misc selftest=%02llx expected=3f\n",
                static_cast<unsigned long long>(miscSelfTest));
    const uint64_t fmodAudioSelfTest = rr3::fmodAudioStubSelfTest();
    bootPrintf("FMOD audio stub selftest=%02llx expected=3f silent device, no audible claim\n",
                static_cast<unsigned long long>(fmodAudioSelfTest));

    char siblingPath[0x400]{};
    const char* gameLibrary =
        selectGameLibrary(argc, argv, romfsReady, siblingPath, sizeof(siblingPath));
    rr3::renderLoadingDashboard("Main ELF audit", 35);
    presentUi();
    bootPrintf("\nELF preflight\n");
    bootPrintf("source: %s\n", gameLibrary != nullptr ? gameLibrary : "not found");
    rr3::ElfDryLoad gameDry{};
    if (gameLibrary == nullptr) {
        bootPrintf("status: embedded and external library missing\n");
    }
    else {
        const rr3::ElfPreflight elf = rr3::inspectElfFile(gameLibrary);
        if (!elf.opened || !elf.validElf || elf.error[0] != '\0') {
            bootPrintf("status: %s\n", elf.error);
        }
        else {
            bootPrintf("A64=%s DYN=%s ph=%u load=%u exec=%u tls=%u\n",
                        elf.isAarch64 ? "yes" : "no",
                        elf.isDynamicObject ? "yes" : "no",
                        elf.programHeaderCount,
                        elf.loadSegments,
                        elf.executableSegments,
                        elf.tlsSegments);
            bootPrintf("map=%llu KiB needed=%u rela=%llu plt-bytes=%llu\n",
                        static_cast<unsigned long long>(elf.mappedBytes / 1024),
                        elf.neededCount,
                        static_cast<unsigned long long>(elf.relaCount),
                        static_cast<unsigned long long>(elf.pltRelocationBytes));
            bootPrintf("rel=%llu abs64=%llu glob=%llu jump=%llu unsupported=%llu\n",
                        static_cast<unsigned long long>(elf.relativeRelocations),
                        static_cast<unsigned long long>(elf.absolute64Relocations),
                        static_cast<unsigned long long>(elf.globalDataRelocations),
                        static_cast<unsigned long long>(elf.jumpSlotRelocations),
                        static_cast<unsigned long long>(elf.unsupportedRelocations));
        }

        gameDry = rr3::dryLoadElfFile(gameLibrary);
        const rr3::ElfDryLoad& dry = gameDry;
        bootPrintf("\nELF dry load (no constructors)\n");
        if (!dry.opened || !dry.validElf || !dry.allocated || !dry.completed ||
            dry.error[0] != '\0') {
            bootPrintf("status: %s\n", dry.error[0] != '\0' ? dry.error : "failed");
        }
        else {
            bootPrintf("image=%llu KiB copy=%llu KiB zero=%llu KiB\n",
                        static_cast<unsigned long long>(dry.imageBytes / 1024),
                        static_cast<unsigned long long>(dry.copiedBytes / 1024),
                        static_cast<unsigned long long>(dry.zeroFilledBytes / 1024));
            bootPrintf("relative=%llu pending=%llu invalid=%llu\n",
                        static_cast<unsigned long long>(dry.relativeApplied),
                        static_cast<unsigned long long>(dry.externalPending),
                        static_cast<unsigned long long>(dry.invalidRelocations));
            bootPrintf("pre-reloc hash=%016llx\n",
                        static_cast<unsigned long long>(dry.imageHash));
            bootPrintf("symbols=%llu max=%llu internal-reloc=%llu\n",
                        static_cast<unsigned long long>(dry.uniqueExternalSymbols),
                        static_cast<unsigned long long>(dry.maximumSymbolIndex),
                        static_cast<unsigned long long>(dry.internalSymbolRelocations));
            bootPrintf("weak=%llu versioned=%llu names=%016llx\n",
                        static_cast<unsigned long long>(dry.weakExternalSymbols),
                        static_cast<unsigned long long>(dry.versionedExternalSymbols),
                        static_cast<unsigned long long>(dry.symbolNameHash));
            bootPrintf("GL=%llu EGL=%llu FMOD=%llu AR=%llu pthread=%llu\n",
                        static_cast<unsigned long long>(dry.glesSymbols),
                        static_cast<unsigned long long>(dry.eglSymbols),
                        static_cast<unsigned long long>(dry.fmodSymbols),
                        static_cast<unsigned long long>(dry.arcoreSymbols),
                        static_cast<unsigned long long>(dry.pthreadSymbols));
            bootPrintf("CXX=%llu android=%llu other=%llu\n",
                        static_cast<unsigned long long>(dry.cxxSymbols),
                        static_cast<unsigned long long>(dry.androidSymbols),
                        static_cast<unsigned long long>(dry.otherSymbols));
            bootPrintf("resolver=%llu table=%016llx\n",
                        static_cast<unsigned long long>(dry.resolverTableEntries),
                        static_cast<unsigned long long>(dry.resolverTableNameHash));
            bootPrintf("lookup sym=%llu/%llu reloc=%llu/%llu\n",
                        static_cast<unsigned long long>(dry.lookupMatchedSymbols),
                        static_cast<unsigned long long>(dry.lookupMissingSymbols),
                        static_cast<unsigned long long>(dry.lookupMatchedRelocations),
                        static_cast<unsigned long long>(dry.lookupMissingRelocations));
            bootPrintf("lookup names=%016llx\n",
                        static_cast<unsigned long long>(dry.lookupMatchedNameHash));
        }
    }

    constexpr const char* kCxxLibrary = "romfs:/libc++_shared.so";
    rr3::renderLoadingDashboard("libc++ closure", 55);
    presentUi();
    const rr3::ElfDryLoad cxx = rr3::dryLoadElfFile(kCxxLibrary);
    bootPrintf("\nlibc++ dependency dry audit\n");
    if (!cxx.opened || !cxx.validElf || !cxx.allocated || !cxx.completed ||
        cxx.error[0] != '\0') {
        bootPrintf("status: %s\n", cxx.error[0] != '\0' ? cxx.error : "failed");
    }
    else {
        bootPrintf("image=%llu KiB rel=%llu pending=%llu internal=%llu invalid=%llu\n",
                    static_cast<unsigned long long>(cxx.imageBytes / 1024),
                    static_cast<unsigned long long>(cxx.relativeApplied),
                    static_cast<unsigned long long>(cxx.externalPending),
                    static_cast<unsigned long long>(cxx.internalSymbolRelocations),
                    static_cast<unsigned long long>(cxx.invalidRelocations));
        bootPrintf("symbols=%llu weak=%llu versioned=%llu max=%llu\n",
                    static_cast<unsigned long long>(cxx.uniqueExternalSymbols),
                    static_cast<unsigned long long>(cxx.weakExternalSymbols),
                    static_cast<unsigned long long>(cxx.versionedExternalSymbols),
                    static_cast<unsigned long long>(cxx.maximumSymbolIndex));
        bootPrintf("lookup sym=%llu/%llu reloc=%llu/%llu\n",
                    static_cast<unsigned long long>(cxx.lookupMatchedSymbols),
                    static_cast<unsigned long long>(cxx.lookupMissingSymbols),
                    static_cast<unsigned long long>(cxx.lookupMatchedRelocations),
                    static_cast<unsigned long long>(cxx.lookupMissingRelocations));
        bootPrintf("hash=%016llx names=%016llx matched=%016llx\n",
                    static_cast<unsigned long long>(cxx.imageHash),
                    static_cast<unsigned long long>(cxx.symbolNameHash),
                    static_cast<unsigned long long>(cxx.lookupMatchedNameHash));
    }

    const rr3::JitImageProbe imageJit =
        rr3::captureImageSizedJitProbe(gameDry.imageBytes + cxx.imageBytes);
    bootPrintf("image-sized JIT=%llu KiB create=%08x exec=%08x edge=%s/%s close=%08x\n",
                static_cast<unsigned long long>(imageJit.requestedBytes / 1024),
                imageJit.createResult,
                imageJit.executableResult,
                imageJit.firstPageCalled ? "yes" : "no",
                imageJit.lastPageCalled ? "yes" : "no",
                imageJit.closeResult);

    const uint64_t rollbackSelfTest = rr3::loaderRollbackSelfTest();
    bootPrintf("loader rollback selftest=%llx expected=f\n",
                static_cast<unsigned long long>(rollbackSelfTest));

    const char* loaderPaths[] = {
        gameLibrary,
        "romfs:/libNimble.so",
        "romfs:/libfmodex.so",
        kCxxLibrary,
    };
    rr3::renderLoadingDashboard("Core DSO link and permissions", 75);
    presentUi();
    const rr3::ElfLoaderSession loader = gameLibrary != nullptr
        ? rr3::loadLinkElfSet(loaderPaths, sizeof(loaderPaths) / sizeof(loaderPaths[0]))
        : rr3::ElfLoaderSession{};
    bootPrintf("\nExecutable seven-DSO loader session (constructors gated)\n");
    if (!loader.completed || loader.error[0] != '\0') {
        bootPrintf("status: %s\n", loader.error[0] != '\0' ? loader.error : "failed");
        bootPrintf("diag patch/verify=%llu/%llu invalid=%llu relro=%llu life=%llu/%llu\n",
                    static_cast<unsigned long long>(loader.patchedRelocations),
                    static_cast<unsigned long long>(loader.verifiedWrites),
                    static_cast<unsigned long long>(loader.invalidRelocations),
                    static_cast<unsigned long long>(loader.relroRanges),
                    static_cast<unsigned long long>(loader.lifecycleTargetsVerified),
                    static_cast<unsigned long long>(loader.constructors + loader.finalizers));
        bootPrintf("segdiag image=%llu region=%llu stage=%llu rc=%08x perm=%x/%x failures=%llu\n",
                    static_cast<unsigned long long>(loader.segmentedFailureImage),
                    static_cast<unsigned long long>(loader.segmentedFailureRegion),
                    static_cast<unsigned long long>(loader.segmentedFailureStage),
                    loader.segmentedFailureResult,
                    loader.segmentedActualPermission,
                    loader.segmentedExpectedPermission,
                    static_cast<unsigned long long>(loader.segmentedFailures));
        bootPrintf("procdiag image=%llu stage=%llu rc=%08x backing=%x/%x/%x target=%x/%x\n",
                    static_cast<unsigned long long>(loader.processMapFailureImage),
                    static_cast<unsigned long long>(loader.processMapFailureStage),
                    loader.processMapFailureResult,
                    loader.processBackingType,
                    loader.processBackingPermission,
                    loader.processBackingAttribute,
                    loader.processTargetType,
                    loader.processTargetPermission);
    }
    else {
        bootPrintf("images=%llu total=%llu KiB retained=%s RX=%s ready=%s\n",
                    static_cast<unsigned long long>(loader.imageCount),
                    static_cast<unsigned long long>(loader.totalImageBytes / 1024),
                    loader.retained ? "yes" : "no",
                    loader.executableAliases ? "yes" : "no",
                    loader.activationReady ? "yes" : "no");
        bootPrintf("segments L/X/W/RO/WX=%llu/%llu/%llu/%llu/%llu TLS=%llu\n",
                    static_cast<unsigned long long>(loader.loadSegments),
                    static_cast<unsigned long long>(loader.executableSegments),
                    static_cast<unsigned long long>(loader.writableSegments),
                    static_cast<unsigned long long>(loader.readOnlySegments),
                    static_cast<unsigned long long>(loader.writableExecutableSegments),
                    static_cast<unsigned long long>(loader.tlsSegments));
        bootPrintf("permissions p73=%s fallback=%s enforced=%s ranges=%llu/%llu fail=%llu p73fail=%llu\n",
                    loader.permissionApiAvailable ? "yes" : "no",
                    loader.permissionFallbackUsed ? "yes" : "no",
                    loader.segmentPermissionsEnforced ? "yes" : "no",
                    static_cast<unsigned long long>(loader.permissionRangesVerified),
                    static_cast<unsigned long long>(loader.permissionRanges),
                    static_cast<unsigned long long>(loader.permissionFailures),
                    static_cast<unsigned long long>(loader.processPermissionFailures));
        bootPrintf("segmented tried=%s ok=%s mode=%u regions=%llu/%llu fail=%llu\n",
                    loader.segmentedMapperAttempted ? "yes" : "no",
                    loader.segmentedMapperSucceeded ? "yes" : "no",
                    loader.segmentedMapperMode,
                    static_cast<unsigned long long>(loader.segmentedRegionsVerified),
                    static_cast<unsigned long long>(loader.segmentedRegions),
                    static_cast<unsigned long long>(loader.segmentedFailures));
        bootPrintf("needed total/embedded/virtual/missing=%llu/%llu/%llu/%llu\n",
                    static_cast<unsigned long long>(loader.neededEntries),
                    static_cast<unsigned long long>(loader.neededEmbedded),
                    static_cast<unsigned long long>(loader.neededVirtualized),
                    static_cast<unsigned long long>(loader.neededMissing));
        bootPrintf("reloc R/I/H/X/W/M=%llu/%llu/%llu/%llu/%llu/%llu\n",
                    static_cast<unsigned long long>(loader.relativeApplied),
                    static_cast<unsigned long long>(loader.internalApplied),
                    static_cast<unsigned long long>(loader.hostRelocations),
                    static_cast<unsigned long long>(loader.crossRelocations),
                    static_cast<unsigned long long>(loader.weakZeroRelocations),
                    static_cast<unsigned long long>(loader.missingRelocations));
        bootPrintf("patched=%llu verified=%llu unique H/X/M=%llu/%llu/%llu\n",
                    static_cast<unsigned long long>(loader.patchedRelocations),
                    static_cast<unsigned long long>(loader.verifiedWrites),
                    static_cast<unsigned long long>(loader.uniqueHostSymbols),
                    static_cast<unsigned long long>(loader.uniqueCrossSymbols),
                    static_cast<unsigned long long>(loader.uniqueMissingSymbols));
        bootPrintf("RELRO=%llu/%llu KiB targets=%llu ctors=%llu fini=%llu life=%llu\n",
                    static_cast<unsigned long long>(loader.relroRanges),
                    static_cast<unsigned long long>(loader.relroBytes / 1024),
                    static_cast<unsigned long long>(loader.relroTargetsVerified),
                    static_cast<unsigned long long>(loader.constructors),
                    static_cast<unsigned long long>(loader.finalizers),
                    static_cast<unsigned long long>(loader.lifecycleTargetsVerified));
        bootPrintf("order C/F=%llu/%llu hash=%016llx/%016llx cycles=%llu\n",
                    static_cast<unsigned long long>(loader.constructorOrderEntries),
                    static_cast<unsigned long long>(loader.finalizerOrderEntries),
                    static_cast<unsigned long long>(loader.constructorOrderHash),
                    static_cast<unsigned long long>(loader.finalizerOrderHash),
                    static_cast<unsigned long long>(loader.dependencyCycles));
        bootPrintf("plan hash=%016llx\n",
                    static_cast<unsigned long long>(loader.resolutionPlanHash));
    }
    const uint64_t phdrSelfTest = rr3::guestPhdrRegistrySelfTest();
    bootPrintf("phdr registry selftest=%llx expected=7\n",
                static_cast<unsigned long long>(phdrSelfTest));
    const uint64_t dynamicLinkSelfTest = rr3::guestDynamicLinkSelfTest();
    bootPrintf("dynamic linker selftest=%02llx expected=3f\n",
                static_cast<unsigned long long>(dynamicLinkSelfTest));

    rr3::DashboardData dashboard{};
    dashboard.tls = tls;
    dashboard.jit = jit;
    dashboard.imageJit = imageJit;
    dashboard.game = gameDry;
    dashboard.libcxx = cxx;
    dashboard.loader = loader;
    dashboard.startupPlan = rr3::captureGuestStartupPlan();
    dashboard.bionicTest = bionicSelfTest;
    dashboard.pthreadTest = pthreadSelfTest;
    dashboard.libcxxTest = libcxxSelfTest;
    dashboard.lifecycleTest = lifecycleSelfTest;
    dashboard.syscallTest = syscallSelfTest;
    dashboard.stdioTest = stdioSelfTest;
    dashboard.rollbackTest = rollbackSelfTest;
    dashboard.phdrTest = phdrSelfTest;
    dashboard.dynamicLinkTest = dynamicLinkSelfTest;
    dashboard.optionalPlatformTest = optionalPlatformSelfTest;
    dashboard.semaphoreTest = semaphoreSelfTest;
    dashboard.jumpTest = jumpSelfTest;
    dashboard.miscTest = miscSelfTest;
    dashboard.guestTlsTest = guestTlsSelfTest;
    dashboard.filesystemTest = filesystemSelfTest;
    dashboard.fmodAudioTest = fmodAudioSelfTest;
    dashboard.fmodAudio = rr3::fmodAudioStubStats();
    dashboard.socketInitResult = socketResult;
    dashboard.graphics = returnedGraphics;
    dashboard.externalData = externalData;
    constexpr const char* kRecoveryMarker =
        "sdmc:/switch/rr3-native-rehost.guarded-action.pending";
    constexpr const char* kBootProgressPath =
        "sdmc:/switch/rr3-native-rehost.guest-boot.progress";
    dashboard.recoveryMode = rr3::recoveryMarkerExists(kRecoveryMarker);
    // Read back any breadcrumb from a prior guest boot that faulted with no
    // saved crash report. This survives the power-off and names the last phase
    // (main-ctor ordinal/target, or the pre-onCreateJNI point) that was reached.
    rr3::readGuestBootProgress(kBootProgressPath, dashboard.bootProgress,
                               sizeof(dashboard.bootProgress));
    const bool automaticDependencyTest =
        hasArgument(argc, argv, "--rr3-auto-deps");
    // Auto-boot by default for emulator debugging.
    const bool automaticGuestBoot = true;

    int dashboardPage = 0;
    bool developerMode = diagnosticConsole;
    const char* dashboardNotice = dashboard.recoveryMode
        ? "检测到上次受保护操作中断；请查看兼容性页，然后按 B 清除。"
        : returnedGraphics.attempted
        ? (returnedGraphics.drawCompleted
            ? (returnedGraphics.programLinked
                ? (returnedGraphics.texturedDrawCompleted
                    ? "独立渲染阶段已通过；真实感着色器、纹理上传与带纹理绘制全部成功。"
                    : returnedGraphics.realisticProgramLinked
                        ? "着色器与链接成功，但纹理上传或带纹理绘制失败；详细结果位于隐藏工程页。"
                        : returnedGraphics.realisticShaderCompiled
                            ? "基础着色器通过，但真实感着色器链接失败；详细结果位于隐藏工程页。"
                            : "基础着色器通过，但真实感着色器编译失败；详细结果位于隐藏工程页。")
                : "首帧已出图，但着色器编译或链接失败；详细结果位于隐藏工程页。")
            : "独立渲染阶段已安全返回；详细结果位于隐藏工程页。")
        : "已优先挂载原包简体中文与原版动效数据。";
    rr3::renderLoadingDashboard("Finalizing dashboard", 100);
    presentUi();
    rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
    bool reloadRequested = false;
    bool exitRequested = false;
    // When guest workers remain after View, image unload would race live
    // guest code. PLUS may still terminate the process (OS reclaims maps);
    // the recovery marker stays so the next launch is Recovery.
    bool skipUnloadAfterExit = false;
    const auto prepareCleanHandoff = [&](bool* dirtyWorkersOut = nullptr) {
        rr3::writeGuestBootProgress(kBootProgressPath, "handoff-quiesce-enter", 80, 0);
        const unsigned int quiesced = rr3::guestPthreadQuiesceAll();
        const unsigned int liveThreads = rr3::guestPthreadLiveCount();
        rr3::writeGuestBootProgress(kBootProgressPath, "handoff-quiesce-return", 81, quiesced);
        if (liveThreads != 0) {
            rr3::writeGuestBootProgress(kBootProgressPath, "handoff-quiesce-stuck", 83,
                                        liveThreads);
            if (dirtyWorkersOut != nullptr) {
                *dirtyWorkersOut = true;
            }
            return false;
        }
        dashboard.unload = rr3::unloadLinkElfSet();
        rr3::writeGuestBootProgress(kBootProgressPath, "handoff-unload-return", 82,
                                    dashboard.unload.completed ? 1 : 0);
        if (!dashboard.unload.completed) {
            return false;
        }
        if (dashboard.guestInitialization.active ||
            dashboard.mainInitialization.active) {
            dashboard.guestInitialization.active = false;
            dashboard.mainInitialization.active = false;
            const bool cleared = rr3::completeGuardedAction(kRecoveryMarker);
            dashboard.recoveryMode = !cleared;
            if (!cleared) {
                return false;
            }
        }
        return true;
    };
    const auto runGuardedDependencyInitialization = [&]() {
        if (dashboard.recoveryMode) {
            dashboardNotice =
                "恢复标记仍存在，依赖初始化已被阻止。";
        }
        else if (!dashboard.externalData.ready || dashboard.filesystemTest != 0xfff) {
            dashboardNotice =
                "完整外置数据卷或路径重定向尚未就绪，依赖初始化已阻止。";
        }
        else if (!dashboard.loader.activationReady ||
                 !dashboard.startupPlan.available) {
            dashboardNotice =
                "加载器尚未就绪，依赖初始化保持关闭。";
        }
        else if (dashboard.guestInitialization.active) {
            dashboardNotice =
                "依赖构造器已运行；安全退出时将执行终结器。";
        }
        else if (!armDependencyInitialization(kRecoveryMarker,
                                              dashboard.startupPlan,
                                              "dependency-constructors")) {
            dashboardNotice =
                "无法可靠写入一次性启动快照，操作已阻止。";
        }
        else {
            dashboard.recoveryMode = true;
            dashboardNotice =
                "正在调用依赖构造器；RR3 主构造器仍保持关闭……";
            rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
            presentUi();
            dashboard.guestInitialization =
                rr3::initializeGuestDependencies();
            dashboardNotice = dashboard.guestInitialization.completed
                ? "依赖构造器已通过；安全退出时将执行终结器。"
                : "依赖初始化未完成；恢复标记已保留。";
        }
    };
    const auto runGuardedGuestBootProbe = [&]() {
        if (dashboard.recoveryMode && !dashboard.guestInitialization.active) {
            dashboardNotice =
                "恢复标记仍存在，Guest 启动已被阻止。";
        }
        else if (!dashboard.guestInitialization.completed ||
                 !dashboard.guestInitialization.active) {
            dashboardNotice =
                "请先按 ZR 完成依赖构造器，再执行 Guest 启动。";
        }
        else if (!dashboard.externalData.ready ||
                 dashboard.filesystemTest != 0xfff) {
            dashboardNotice =
                "完整外置数据卷或路径重定向尚未就绪，Guest 启动已阻止。";
        }
        else if (dashboard.entryProbe.resumeReturned &&
                 !dashboard.viewProbe.attempted) {
            if (!rr3::prepareGuestCrashCapture()) {
                dashboardNotice = "无法准备异常现场文件；Guest View 未执行，恢复标记保留。";
                return;
            }
            dashboardNotice =
                "正在创建 Guest EGL 上下文并执行 onViewCreated/onViewChanged/onViewRender……";
            rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
            presentUi();
            // The native framebuffer and Mesa EGL both own the default
            // NWindow. Close the framebuffer first, then reacquire the default
            // window so EGL never receives a pointer captured before the UI
            // teardown.
            rr3::nativeUiExit();
            NWindow* viewWindow = nwindowGetDefault();
            dashboard.viewProbe =
                rr3::runGuestViewProbe(viewWindow, kBootProgressPath);
            rr3::finishGuestCrashCapture();
            rr3::writeGuestBootProgress(kBootProgressPath, "view-probe-returned", 60, 0);
            rr3::readGuestBootProgress(kBootProgressPath, dashboard.bootProgress,
                                       sizeof(dashboard.bootProgress));
            // Guest workers still own the EGL context. Restoring the native
            // framebuffer UI would steal NWindow from them; skip the dashboard
            // and dirty-exit so the OS reclaims everything.
            const unsigned int liveAfterView = rr3::guestPthreadLiveCount();
            if (liveAfterView != 0) {
                rr3::writeGuestBootProgress(kBootProgressPath,
                                            "view-probe-dirty-exit", 76,
                                            liveAfterView);
                skipUnloadAfterExit = true;
                exitRequested = true;
                return;
            }
            if (!rr3::nativeUiInit()) {
                rr3::writeGuestBootProgress(kBootProgressPath, "native-ui-init-failed", 61, 0);
                diagnosticConsole = true;
                consoleInit(nullptr);
            }
            else {
                rr3::writeGuestBootProgress(kBootProgressPath, "native-ui-restored", 62, 0);
            }
            if (dashboard.viewProbe.createdReturned &&
                dashboard.viewProbe.changedReturned &&
                dashboard.viewProbe.renderReturned &&
                dashboard.viewProbe.swapped) {
                dashboardNotice =
                    "Guest View 生命周期与一次 EGL 交换已完成；详情位于隐藏工程页。";
            }
            else {
                dashboardNotice =
                    "Guest View 阶段未完成；恢复标记与最后面包屑已保留。";
            }
        }
        else if (dashboard.entryProbe.resumeReturned) {
            dashboardNotice =
                "Guest View 阶段已运行；重复调用已被阻止。";
        }
        else if (dashboard.entryProbe.returned) {
            dashboardNotice =
                "onCreateJNI 已返回；onStart/onResume 未完成，重复调用已被阻止。";
        }
        else if (!armDependencyInitialization(kRecoveryMarker,
                                              dashboard.startupPlan,
                                              "guest-boot")) {
            dashboardNotice =
                "无法可靠写入一次性启动快照，操作已阻止。";
        }
        else {
            dashboard.recoveryMode = true;
            dashboardNotice =
                "正在运行 RR3 主构造器与 onCreateJNI……";
            rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
            presentUi();
            // Android's loader runs JNI_OnLoad on each library before any
            // activity code; that call caches the JavaVM pointer EA::Nimble's
            // getEnv() (reached from main ctor #157) dereferences. Run it first
            // or ctor #157 null-derefs the uncached pointer.
            if (!dashboard.jniOnLoad.completed) {
                dashboard.jniOnLoad = rr3::runGuestJniOnLoad(kBootProgressPath);
            }
            if (dashboard.jniOnLoad.completed &&
                !dashboard.mainInitialization.completed) {
                dashboard.mainInitialization =
                    rr3::initializeGuestMainConstructors(kBootProgressPath);
            }
            if (dashboard.mainInitialization.completed) {
                // Breadcrumb and phase attribution live inside the probe; it
                // calls onCreateJNI -> onStartJNI -> onResumeJNI, each recorded
                // separately so a fault pins the exact lifecycle stage.
                dashboard.entryProbe =
                    rr3::runGuestEntryProbe(kBootProgressPath);
            }
            // Refresh the in-UI breadcrumb from whatever was last written.
            rr3::readGuestBootProgress(kBootProgressPath, dashboard.bootProgress,
                                       sizeof(dashboard.bootProgress));
            if (dashboard.mainInitialization.completed &&
                dashboard.entryProbe.resumeReturned) {
                dashboardNotice =
                    "RR3 主构造器与 onCreate/onStart/onResume 已完成；再次按 ZL 进入 Guest View。";
            }
            else if (dashboard.mainInitialization.completed &&
                     dashboard.entryProbe.returned) {
                dashboardNotice =
                    "onCreateJNI 已完成（onStart/onResume 未完成）；JNI 详情位于隐藏工程页。";
            }
            else {
                dashboardNotice =
                    "Guest 启动未完成；恢复标记已保留。";
            }
        }
    };
    if (automaticDependencyTest || automaticGuestBoot) {
        // Wait for data volumes and loader to be ready before auto-boot.
        if (automaticGuestBoot) {
            dashboardNotice = "等待加载器就绪...";
            rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
            presentUi();
            // Poll until loader and plan are ready.
            for (int i = 0; i < 200; ++i) {
                if (dashboard.externalData.ready &&
                    dashboard.filesystemTest == 0xfff &&
                    dashboard.loader.activationReady &&
                    dashboard.startupPlan.available) {
                    break;
                }
                svcSleepThread(100000000ULL); // 100ms
            }
            // Log auto-boot status.
            char autoBootMsg[128];
            __builtin_snprintf(autoBootMsg, sizeof(autoBootMsg),
                "auto-boot: extData=%d fsTest=%x loader=%d plan=%d",
                dashboard.externalData.ready ? 1 : 0,
                dashboard.filesystemTest,
                dashboard.loader.activationReady ? 1 : 0,
                dashboard.startupPlan.available ? 1 : 0);
            rr3::appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                autoBootMsg, 200, 0);
        }
        runGuardedDependencyInitialization();
        if (automaticGuestBoot) {
            runGuardedGuestBootProbe();
        }
        rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
        presentUi();
    }
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 held = padGetButtons(&pad);
        bool redraw = false;
        const bool developerChord =
            (down & HidNpadButton_X) != 0 &&
            (held & HidNpadButton_L) != 0 &&
            (held & HidNpadButton_R) != 0;
        if (developerChord) {
            developerMode = !developerMode;
            dashboardPage = 0;
            dashboardNotice = developerMode
                ? "工程诊断页已开启。"
                : "工程诊断页已隐藏。";
            redraw = true;
        }
        const int visiblePages = developerMode ? rr3::kDashboardPages : 2;
        if ((down & (HidNpadButton_R | HidNpadButton_Right)) != 0) {
            dashboardPage = (dashboardPage + 1) % visiblePages;
            dashboardNotice = "页面已切换。";
            redraw = true;
        }
        if ((down & (HidNpadButton_L | HidNpadButton_Left)) != 0) {
            dashboardPage =
                (dashboardPage + visiblePages - 1) % visiblePages;
            dashboardNotice = "页面已切换。";
            redraw = true;
        }
        if (!developerChord && (down & HidNpadButton_X) != 0 && developerMode) {
            constexpr const char* kReportPath =
                "sdmc:/switch/rr3-native-rehost/loader-report.txt";
            dashboardNotice = rr3::writeDashboardReport(dashboard, kReportPath)
                ? "诊断报告已导出到 /switch/rr3-native-rehost/loader-report.txt"
                : "诊断报告导出失败；SD 路径不可写。";
            redraw = true;
        }
        if ((down & HidNpadButton_ZR) != 0) {
            runGuardedDependencyInitialization();
            redraw = true;
        }
        if ((down & HidNpadButton_ZL) != 0) {
            runGuardedGuestBootProbe();
            redraw = true;
        }
        if ((down & HidNpadButton_Y) != 0) {
            if (dashboard.recoveryMode && !dashboard.guestInitialization.active) {
                dashboardNotice =
                    "恢复标记清除前，渲染阶段保持关闭。";
            }
            else if (!prepareCleanHandoff()) {
                dashboardNotice =
                    "终结器或卸载清理失败，渲染阶段已阻止。";
            }
            else {
                const Result reloadResult =
                    scheduleSelfLoad(argc, argv, "--rr3-renderer");
                reloadRequested = R_SUCCEEDED(reloadResult);
                dashboardNotice = reloadRequested
                ? "Guest 映射已释放；正在启动独立渲染阶段……"
                    : "Guest 已安全卸载，但渲染阶段调度失败。";
            }
            redraw = true;
        }
        if ((down & HidNpadButton_B) != 0) {
            if (dashboard.guestInitialization.active ||
                dashboard.mainInitialization.active) {
                dashboardNotice =
                    "Guest 构造器仍活动时不能清除恢复状态。";
            }
            else {
                const bool cleared = rr3::completeGuardedAction(kRecoveryMarker);
                dashboard.recoveryMode = !cleared;
                if (cleared) {
                    // The boot breadcrumb only aids post-crash attribution; once
                    // the operator clears the marker, drop it so a stale phase
                    // does not linger on the next launch.
                    rr3::clearGuestBootProgress(kBootProgressPath);
                    dashboard.bootProgress[0] = '\0';
                }
                dashboardNotice = cleared
                    ? "恢复标记已清除，受保护操作现已可用。"
                    : "无法清除恢复标记。";
            }
            redraw = true;
        }
        if ((down & HidNpadButton_Plus) != 0) {
            bool dirtyWorkers = false;
            exitRequested = prepareCleanHandoff(&dirtyWorkers);
            if (!exitRequested && dirtyWorkers) {
                // Guest workers are still in the registry. Skip finalizers/
                // unmap (they would race live guest code), keep the recovery
                // marker, and return to hbloader so the OS reclaims mappings.
                skipUnloadAfterExit = true;
                exitRequested = true;
                rr3::writeGuestBootProgress(kBootProgressPath, "handoff-dirty-exit", 84,
                                            rr3::guestPthreadLiveCount());
                dashboardNotice =
                    "Guest 工作线程仍在运行；跳过卸载并退出，下次将进入恢复模式。";
                redraw = true;
            }
            else if (!exitRequested) {
                dashboardNotice =
                    "终结器或卸载清理失败；为保护 hbloader，退出已阻止。";
                redraw = true;
            }
        }
        if (redraw) {
            rr3::renderDashboard(dashboard, dashboardPage, dashboardNotice);
        }
        presentUi();
        if (redraw) {
            rr3::writeGuestBootProgress(kBootProgressPath, "main-loop-alive", 63, 0);
        }
        if (reloadRequested || exitRequested) {
            break;
        }
    }
    rr3::writeGuestBootProgress(kBootProgressPath, "main-loop-exit", 64, 0);

    if (skipUnloadAfterExit) {
        // Guest workers still execute guest code. Normal UI/FS teardown and
        // returning through libnx races them and produced the device error
        // after PLUS (round 9: live=6, progress reached main-loop-exit then
        // the process faulted with only armed crash). Terminate the process
        // the same way the exception path does; the OS reclaims ProcessCode,
        // FS, and UI. Recovery marker remains.
        rr3::writeGuestBootProgress(kBootProgressPath, "handoff-process-exit", 85,
                                    rr3::guestPthreadLiveCount());
        svcExitProcess();
    }

    if (!dashboard.unload.completed) {
        dashboard.unload = rr3::unloadLinkElfSet();
    }

    if (rr3::nativeUiActive()) {
        rr3::nativeUiExit();
    }
    rr3::unmountExternalData();
    if (romfsReady) {
        romfsExit();
    }
    if (sdMountedHere) {
        fsdevUnmountDevice("sdmc");
    }
    if (R_SUCCEEDED(socketResult)) {
        socketExit();
    }
    if (diagnosticConsole) {
        consoleExit(nullptr);
    }
    return 0;
}
