#pragma once

#include <cstdint>

#include "rr3/elf_preflight.hpp"
#include "rr3/fmod_audio_stub.hpp"
#include "rr3/jit_probe.hpp"
#include "rr3/graphics_probe.hpp"
#include "rr3/guest_filesystem.hpp"
#include "rr3/jni_bridge.hpp"
#include "rr3/tls_probe.hpp"

namespace rr3 {

struct DashboardData {
    TlsProbe tls{};
    JitProbe jit{};
    JitImageProbe imageJit{};
    ElfDryLoad game{};
    ElfDryLoad libcxx{};
    ElfLoaderSession loader{};
    ElfLoaderUnload unload{};
    GuestStartupPlan startupPlan{};
    GuestInitializationResult guestInitialization{};
    GuestInitializationResult mainInitialization{};
    GuestJniOnLoadProbe jniOnLoad{};
    GuestEntryProbe entryProbe{};
    GuestViewProbe viewProbe{};
    char bootProgress[128]{};
    uint64_t bionicTest{};
    uint64_t pthreadTest{};
    uint64_t libcxxTest{};
    uint64_t lifecycleTest{};
    uint64_t syscallTest{};
    uint64_t stdioTest{};
    uint64_t rollbackTest{};
    uint64_t phdrTest{};
    uint64_t dynamicLinkTest{};
    uint64_t optionalPlatformTest{};
    uint64_t semaphoreTest{};
    uint64_t jumpTest{};
    uint64_t miscTest{};
    uint64_t guestTlsTest{};
    uint64_t filesystemTest{};
    uint64_t fmodAudioTest{};
    FmodAudioStubStats fmodAudio{};
    uint32_t socketInitResult{};
    GraphicsProbe graphics{};
    ExternalDataStatus externalData{};
    bool recoveryMode{};
};

constexpr int kDashboardPages = 4;

void renderDashboard(const DashboardData& data, int page, const char* notice);
void renderLoadingDashboard(const char* stage, unsigned int percent);
bool writeDashboardReport(const DashboardData& data, const char* path);

} // namespace rr3
