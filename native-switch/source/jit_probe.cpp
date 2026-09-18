#include "rr3/jit_probe.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {

JitProbe captureJitProbe() {
    JitProbe probe{};
    Jit jit{};
    constexpr size_t kPageSize = 0x1000;
    constexpr uint32_t kAarch64Return = 0xd65f03c0;

    probe.createResult = jitCreate(&jit, kPageSize);
    if (R_FAILED(probe.createResult)) {
        return probe;
    }

    probe.type = jit.type;
    probe.writableAddress = reinterpret_cast<uintptr_t>(jitGetRwAddr(&jit));
    probe.executableAddress = reinterpret_cast<uintptr_t>(jitGetRxAddr(&jit));

    *reinterpret_cast<volatile uint32_t*>(jitGetRwAddr(&jit)) = kAarch64Return;
    armDCacheFlush(jitGetRwAddr(&jit), sizeof(kAarch64Return));
    armICacheInvalidate(jitGetRxAddr(&jit), sizeof(kAarch64Return));

    probe.executableResult = jitTransitionToExecutable(&jit);
    if (R_SUCCEEDED(probe.executableResult)) {
        using ReturnStub = void (*)();
        reinterpret_cast<ReturnStub>(jitGetRxAddr(&jit))();
        probe.returnStubExecuted = true;
        probe.writableResult = jitTransitionToWritable(&jit);
    }
    probe.closeResult = jitClose(&jit);
    return probe;
}

JitImageProbe captureImageSizedJitProbe(size_t requestedBytes) {
    JitImageProbe probe{};
    constexpr size_t kPageSize = 0x1000;
    constexpr uint32_t kAarch64Return = 0xd65f03c0;
    probe.requestedBytes = (requestedBytes + kPageSize - 1) & ~(kPageSize - 1);
    if (probe.requestedBytes < kPageSize) {
        probe.requestedBytes = kPageSize;
    }

    Jit jit{};
    probe.createResult = jitCreate(&jit, probe.requestedBytes);
    if (R_FAILED(probe.createResult)) {
        return probe;
    }
    probe.type = jit.type;
    auto* writable = static_cast<unsigned char*>(jitGetRwAddr(&jit));
    auto* executable = static_cast<unsigned char*>(jitGetRxAddr(&jit));
    const size_t lastOffset = probe.requestedBytes - sizeof(kAarch64Return);
    std::memcpy(writable, &kAarch64Return, sizeof(kAarch64Return));
    std::memcpy(writable + lastOffset, &kAarch64Return, sizeof(kAarch64Return));
    armDCacheFlush(writable, sizeof(kAarch64Return));
    armDCacheFlush(writable + lastOffset, sizeof(kAarch64Return));
    armICacheInvalidate(executable, sizeof(kAarch64Return));
    armICacheInvalidate(executable + lastOffset, sizeof(kAarch64Return));

    probe.executableResult = jitTransitionToExecutable(&jit);
    if (R_SUCCEEDED(probe.executableResult)) {
        using ReturnStub = void (*)();
        reinterpret_cast<ReturnStub>(executable)();
        probe.firstPageCalled = true;
        reinterpret_cast<ReturnStub>(executable + lastOffset)();
        probe.lastPageCalled = true;
        probe.writableResult = jitTransitionToWritable(&jit);
    }
    probe.closeResult = jitClose(&jit);
    return probe;
}

void printJitProbe(const JitProbe& probe) {
    std::printf("\nExecutable-memory probe\n");
    std::printf("create=%08x type=%u rw=%016llx rx=%016llx\n",
                probe.createResult,
                static_cast<unsigned int>(probe.type),
                static_cast<unsigned long long>(probe.writableAddress),
                static_cast<unsigned long long>(probe.executableAddress));
    std::printf("exec=%08x called=%s writable=%08x close=%08x\n",
                probe.executableResult,
                probe.returnStubExecuted ? "yes" : "no",
                probe.writableResult,
                probe.closeResult);
}

} // namespace rr3
