#include "rr3/tls_probe.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

uintptr_t readTpidrEl0() {
    uintptr_t value{};
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

void writeTpidrEl0(uintptr_t value) {
    __asm__ volatile("msr tpidr_el0, %0\n\tisb" :: "r"(value) : "memory");
}

uintptr_t readTpidrroEl0() {
    uintptr_t value{};
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

uintptr_t readStackPointer() {
    uintptr_t value{};
    __asm__ volatile("mov %0, sp" : "=r"(value));
    return value;
}

RegisterMemory query(uintptr_t address) {
    RegisterMemory result{};
    result.value = address;
    if (address == 0) {
        result.queryResult = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return result;
    }
    u32 pageInfo = 0;
    result.queryResult = svcQueryMemory(&result.memory, &pageInfo, address);
    return result;
}

void printRegion(const char* name, const RegisterMemory& region) {
    std::printf("%-12s %016llx", name, static_cast<unsigned long long>(region.value));
    if (R_SUCCEEDED(region.queryResult)) {
        std::printf("  map=%016llx+%llx perm=%x type=%x\n",
                    static_cast<unsigned long long>(region.memory.addr),
                    static_cast<unsigned long long>(region.memory.size),
                    region.memory.perm,
                    region.memory.type);
    }
    else {
        std::printf("  query=%08x\n", region.queryResult);
    }
}

} // namespace

GuestTlsScope::GuestTlsScope() {
    previous_ = readTpidrEl0();
    std::memset(block_, 0, sizeof(block_));
    uint64_t value = 0x9e3779b97f4a7c15ULL ^ armGetSystemTick() ^
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block_));
    value = (value & ~0xffULL) | 0x5aULL;
    std::memcpy(block_ + 0x28, &value, sizeof(value));
    writeTpidrEl0(reinterpret_cast<uintptr_t>(block_));
    active_ = readTpidrEl0() == reinterpret_cast<uintptr_t>(block_);
}

GuestTlsScope::~GuestTlsScope() {
    if (active_) {
        writeTpidrEl0(previous_);
    }
}

bool GuestTlsScope::active() const {
    return active_;
}

uintptr_t GuestTlsScope::base() const {
    return reinterpret_cast<uintptr_t>(block_);
}

uint64_t GuestTlsScope::canary() const {
    uint64_t value = 0;
    std::memcpy(&value, block_ + 0x28, sizeof(value));
    return value;
}

TlsProbe captureTlsProbe() {
    TlsProbe probe{};
    probe.tpidrEl0 = query(readTpidrEl0());
    probe.tpidrroEl0 = query(readTpidrroEl0());
    probe.stackPointer = query(readStackPointer());
    probe.libnxTls = reinterpret_cast<uintptr_t>(armGetTls());
    probe.threadSelf = reinterpret_cast<uintptr_t>(threadGetSelf());

    constexpr uintptr_t kAndroidCanaryOffset = 0x28;
    const RegisterMemory& tcb = probe.tpidrEl0;
    if (R_SUCCEEDED(tcb.queryResult) && (tcb.memory.perm & Perm_R) != 0 &&
        tcb.value >= tcb.memory.addr &&
        tcb.value + kAndroidCanaryOffset + sizeof(uint64_t) <= tcb.memory.addr + tcb.memory.size) {
        probe.androidCanaryAddressReadable = true;
        probe.androidCanaryCandidate =
            *reinterpret_cast<volatile const uint64_t*>(tcb.value + kAndroidCanaryOffset);
    }
    return probe;
}

void printTlsProbe(const TlsProbe& probe) {
    std::printf("\nTLS register probe\n");
    printRegion("tpidr_el0", probe.tpidrEl0);
    printRegion("tpidrro_el0", probe.tpidrroEl0);
    printRegion("sp", probe.stackPointer);
    std::printf("armGetTls    %016llx\n", static_cast<unsigned long long>(probe.libnxTls));
    std::printf("threadSelf   %016llx\n", static_cast<unsigned long long>(probe.threadSelf));
    if (probe.androidCanaryAddressReadable) {
        std::printf("[tp+0x28]    %016llx (candidate only)\n",
                    static_cast<unsigned long long>(probe.androidCanaryCandidate));
    }
    else {
        std::printf("[tp+0x28]    unreadable; Android TLS ABI cannot be assumed\n");
    }
}

uint64_t guestTlsScopeSelfTest() {
    uint64_t passed = 0;
    const uintptr_t before = readTpidrEl0();
    uintptr_t guestBase = 0;
    uint64_t guestCanary = 0;
    {
        GuestTlsScope scope;
        guestBase = scope.base();
        guestCanary = scope.canary();
        if (scope.active()) {
            passed |= 1ULL << 0;
        }
        if (readTpidrEl0() == guestBase) {
            passed |= 1ULL << 1;
        }
        uint64_t observed = 0;
        std::memcpy(&observed,
                    reinterpret_cast<const void*>(guestBase + 0x28),
                    sizeof(observed));
        if (observed == guestCanary && observed != 0) {
            passed |= 1ULL << 2;
        }
    }
    if (readTpidrEl0() == before) {
        passed |= 1ULL << 3;
    }
    return passed;
}

} // namespace rr3
