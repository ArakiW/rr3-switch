#pragma once

#include <cstdint>

// PadState is a libnx typedef; callers pass the real PadState*.
using GuestPadHandle = void;

namespace rr3 {

bool prepareGuestControllerInput();
void setGuestInputProgressPath(const char* path);
bool connectGuestController(void* jniEnvironment, void* activityObject,
                            int deviceId);
void pumpGuestControllerInput(void* jniEnvironment, void* activityObject,
                              GuestPadHandle* pad, int deviceId);
bool guestControllerInputReady();

} // namespace rr3
