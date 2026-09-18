#include "rr3/jump_bridge.hpp"

namespace rr3 {

uint64_t jumpBridgeSelfTest() {
    alignas(16) uint64_t buffer[32]{};
    const int value = rr3GuestSetjmp(buffer);
    if (value == 0) {
        rr3GuestLongjmp(buffer, 7);
    }
    uint64_t passed = value == 7 ? 1ULL : 0ULL;

    alignas(16) uint64_t zeroBuffer[32]{};
    const int zeroValue = rr3GuestSetjmp(zeroBuffer);
    if (zeroValue == 0) {
        rr3GuestLongjmp(zeroBuffer, 0);
    }
    if (zeroValue == 1) {
        passed |= 1ULL << 1;
    }
    return passed;
}

} // namespace rr3
