#pragma once

#include <switch.h>

#include <cstdint>

namespace rr3 {

struct RegisterMemory {
    uintptr_t value{};
    Result queryResult{};
    MemoryInfo memory{};
};

struct TlsProbe {
    RegisterMemory tpidrEl0{};
    RegisterMemory tpidrroEl0{};
    RegisterMemory stackPointer{};
    uintptr_t libnxTls{};
    uintptr_t threadSelf{};
    bool androidCanaryAddressReadable{};
    uint64_t androidCanaryCandidate{};
};

class GuestTlsScope {
public:
    GuestTlsScope();
    ~GuestTlsScope();
    GuestTlsScope(const GuestTlsScope&) = delete;
    GuestTlsScope& operator=(const GuestTlsScope&) = delete;

    bool active() const;
    uintptr_t base() const;
    uint64_t canary() const;

private:
    uintptr_t previous_{};
    alignas(16) unsigned char block_[0x100]{};
    bool active_{};
};

TlsProbe captureTlsProbe();
void printTlsProbe(const TlsProbe& probe);
uint64_t guestTlsScopeSelfTest();

} // namespace rr3
