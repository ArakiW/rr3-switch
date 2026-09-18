#pragma once

#include <cstdint>

namespace rr3 {

using GuestDestructor = void (*)(void*);

int guestCxaAtexit(GuestDestructor destructor, void* argument, void* dsoHandle);
void guestCxaFinalize(void* dsoHandle);
int guestCxaThreadAtexitImpl(GuestDestructor destructor, void* argument,
                            void* dsoHandle);
uint64_t cxxLifecycleSelfTest();

} // namespace rr3
