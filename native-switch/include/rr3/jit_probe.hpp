#pragma once

#include <switch.h>

#include <cstdint>

namespace rr3 {

struct JitProbe {
    Result createResult{};
    Result executableResult{};
    Result writableResult{};
    Result closeResult{};
    JitType type{};
    uintptr_t writableAddress{};
    uintptr_t executableAddress{};
    bool returnStubExecuted{};
};

struct JitImageProbe {
    size_t requestedBytes{};
    Result createResult{};
    Result executableResult{};
    Result writableResult{};
    Result closeResult{};
    JitType type{};
    bool firstPageCalled{};
    bool lastPageCalled{};
};

JitProbe captureJitProbe();
JitImageProbe captureImageSizedJitProbe(size_t requestedBytes);
void printJitProbe(const JitProbe& probe);

} // namespace rr3
