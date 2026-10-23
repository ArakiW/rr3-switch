#pragma once
//
// The Switch's real touchscreen, wired into the guest's touch path.
//
// WHAT THIS IS. The port can inject a touch from a control file
// (rr3/guest_touch.hpp), which is enough to PROVE that the guest's touch
// path works, but a scripted press is not a game a person can play. This is
// the hardware half: it reads the console's own digitizer and turns a finger
// into the same guest calls the file control makes.
//
// THE API. libnx 4.12 (the toolchain in .tools/devkitPro) does NOT have
// hidInitializeTouch() / hidTouchRead() / hidTouchCount(). What it has is
//     void     hidInitializeTouchScreen(void);
//     size_t   hidGetTouchScreenStates(HidTouchScreenState* states, size_t count);
// with, from switch/services/hid.h,
//     typedef struct HidTouchState {
//         u64 delta_time; u32 attributes; u32 finger_id;
//         u32 x; u32 y; u32 diameter_x; u32 diameter_y;
//         u32 rotation_angle; u32 reserved;
//     } HidTouchState;
//     typedef struct HidTouchScreenState {
//         u64 sampling_number; s32 count; u32 reserved;
//         HidTouchState touches[16];
//     } HidTouchScreenState;
//     HidTouchAttribute_Start = BIT(0), HidTouchAttribute_End = BIT(1).
//
// Read out of libnx.a's own hid.o (the oracle, not the header comments):
//   * hidGetTouchScreenStates() copies the touchscreen entries out of the HID
//     SHARED MEMORY with atomic loads and a sequence-number check, and
//     clamps each entry's count to 16. There is no hidScanInput() in 4.12 --
//     the HID state is read straight out of the shared memory -- so the call
//     is safe from the frame loop thread and costs one memcpy of the entry.
//   * hidInitializeTouchScreen() is _hidCmdInAruidNoOut(11) and it ABORTS the
//     process when that command fails (diagAbortWithResult). A port that
//     aborts at startup because a platform has no touchscreen is worse than a
//     port that reports it, so this code dispatches command 11 itself and
//     READS the result instead of calling the aborting wrapper.
//
// THE COORDINATES. HidTouchState.x / .y are the digitizer's raw pixels. On a
// Switch the digitizer and the screen are both 1280x720, and the guest's
// view is 1280x720 (the port reports it through onViewChangedJNI(env, self,
// 1280, 720, 0, 0)), so the mapping is the identity -- expressed as a
// TouchMapping with both sizes named, not as a hidden constant. The
// conversion itself lives in rr3/guest_touch_convert.hpp, free of libnx so a
// host test can drive it with synthetic samples.
//
// OFF BY DEFAULT? NO, and deliberately: a touchscreen that needs a switch
// flipped is not a touchscreen. What IS off by default is the capability
// probe (see guest_touch_hw.cpp), which pokes the HID debug service.
//
// PRECEDENCE. The file control owns the guest's touch path while its control
// file is on the card. The hardware path never presses into a scripted
// gesture; it only lets go of a finger it is holding, so the guest is never
// left with two pointers down.

#include "rr3/guest_touch_convert.hpp"

namespace rr3 {

// Brings up the digitizer and clears the tracker. Safe to call once at
// startup; returns false when this platform reports no touchscreen at all.
bool prepareGuestTouchHardware();

// Called once per frame from the port's frame loop, on the same thread that
// calls the guest's render entry point -- the same relationship the guest's
// own GLView.queueEvent() has on Android. Cheap: one shared-memory copy per
// frame, and the guest's own state is read only while a finger is down.
void pumpGuestTouchHardware(void* jniEnvironment, void* activityObject);

// True when the platform reported a touchscreen device.
bool guestTouchHardwareAvailable();

// The number of hardware touch events the port has injected since startup.
// For logs and tests.
unsigned int guestTouchHardwareEventCount();

}  // namespace rr3
