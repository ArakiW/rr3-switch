#pragma once

#include <cstdint>

// PadState is a libnx typedef; callers pass the real PadState*.
using GuestPadHandle = void;

namespace rr3 {

// THE SWITCH CONTROLLER -> THE GUEST'S ControllerManager.
//
// This header is the contract for the two things a caller (and a script
// writer) has to get right.
//
// THE TRIGGERS, AND WHAT THEY ARE WORTH
//
// The guest's own Java
// (reverse/java/sources/com/firemint/realracing/ControllerManager.java)
// reads an Android trigger as a MotionEvent axis and hands it to the guest
// as SetJoystickValueJNI(id, value, ordinal) with the ordinals 4 and 5 of
// its ControllerAxis enum: AXIS_LTRIGGER and AXIS_RTRIGGER
// (ControllerManager.java:491-492). Android normalises a one-sided trigger
// axis to 0..1 with 1.0 = fully pressed, and the guest passes the value
// through unchanged.
//
// So THE PORT SENDS 0..1 for a TRIGGER: 0.0 released, 1.0 fully pressed,
// anything between a partial press. The STICKS are signed, -1..1.
//
// libnx has no analogue trigger in the ordinary Npad state --
// HidNpadCommonState (hid.h:1072) has no trigger field, so a Pro
// Controller's ZL/ZR are digital -- but it does expose the GameCube
// Controller's analogue trigger as PadState.gc_triggers[]
// (runtime/pad.h:25), raw 0x0..0x7FFF (hid.h:1094-1095). The port uses
// that value when the controller reports it and falls back to the digital
// ZL/ZR bit otherwise. See source/guest_input.cpp for the evidence.
//
// THE SCRIPTED PAD
//
// The scripted pad is how a reproducible experiment is driven inside an
// emulator. Its control file is on the card:
//
//     sdmc:/switch/rr3-native-rehost/rr3-pad.txt
//
// and its format is:
//
//     [at <frame>] lx=<f> ly=<f> rx=<f> ry=<f> lt=<f> rt=<f> b=<hex>
//
// EVERY KEY IS OPTIONAL. A key that is absent keeps the value it had
// before, so a script is a list of states over time and not a list of
// complete snapshots.
//
//   at <frame>   The PORT'S frame counter -- the same counter the touch
//                 script uses -- never a wall clock. The bracket form and
//                 the bare form both work.
//   lx, ly       AXIS_LTHUMB_X, AXIS_LTHUMB_Y   (ordinals 0, 1), signed.
//   rx, ry       AXIS_RTHUMB_X, AXIS_RTHUMB_Y   (ordinals 2, 3), signed.
//   lt, rt       AXIS_LTRIGGER, AXIS_RTRIGGER   (ordinals 4, 5), 0..1.
//   b            A HEX bitmask over the guest's ControllerButtons
//                 ORDINALS: bit 0 = BTN_A, bit 1 = BTN_B, bit 2 = BTN_X,
//                 bit 3 = BTN_Y, bit 4 = BTN_L1, bit 5 = BTN_R1,
//                 bit 6 = BTN_L2, bit 7 = BTN_R2, bit 8 = BTN_DPAD_LEFT,
//                 bit 9 = BTN_DPAD_RIGHT. The guest's own numbering, not
//                 the Switch's.
//
// THE LAST LINE WHOSE "at" FRAME IS <= THE CURRENT FRAME APPLIES. A
// script with no line at or before the current frame does not apply at
// all, and the physical controller is used instead. With no control file
// on the card nothing changes and the physical path is untouched.
//
// The format above is pinned by
// native-switch/tests/test_guest_pad_script_format.py, which reads the
// parser and this header and requires them to agree.

bool prepareGuestControllerInput();
void setGuestInputProgressPath(const char* path);
bool connectGuestController(void* jniEnvironment, void* activityObject,
                            int deviceId);
void pumpGuestControllerInput(void* jniEnvironment, void* activityObject,
                              GuestPadHandle* pad, int deviceId);
bool guestControllerInputReady();

} // namespace rr3
