#pragma once

#include <cstdint>

extern "C" {
int rr3GuestSetjmp(void* buffer) __attribute__((returns_twice));
[[noreturn]] void rr3GuestLongjmp(void* buffer, int value);
}

namespace rr3 {
uint64_t jumpBridgeSelfTest();
}
