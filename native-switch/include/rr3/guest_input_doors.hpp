#pragma once

#include <cstdint>

//
// The guest's input doors, and a scriptable virtual gamepad.
//
// WHY THIS EXISTS. The port maps the Switch controller onto the guest's
// ControllerManager JNI exports (guest_input.cpp). Whether that actually
// reaches the CAR is not visible from the port: the guest's gamepad state
// lives inside the guest image, and the only way to know whether a value
// arrived is to read the guest's own storage back.
//
// THE ORACLE IS THE GUEST IMAGE, not this comment:
//
//   Java_..._ControllerManager_SetJoystickValueJNI   ELF 0x12aa604
//   Java_..._ControllerManager_SetButtonValueJNI     ELF 0x12aa724
//   Java_..._ControllerManager_ControllerConnectedJNI ELF 0x12aa450
//
//   SetJoystickValueJNI:
//     adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5d8] ; ldr x8,[x8]   -> G (CGlobal)
//     ldr  x0,[x8,#0x1818]                                  -> the joystick manager
//     __dynamic_cast<fmJoystickManagerAndroid*>(x0)          -> null -> SILENT NO-OP
//     map lookup on the device id (manager+0x78)
//     ldrsw x8,[node,#0x20] ; ldr x9,[manager,#0xa0]
//     ldr  x0,[x9,x8,lsl #3]                                 -> the controller
//     bl    0x12a9334  -> add x8,x0,w1,sxtw #2 ; str s0,[x8,#0x84]
//
//   So the guest stores an AXIS at controller+0x84+4*axis (float) and a
//   BUTTON at controller+0x1c+4*ordinal (int), and the ORDINAL is the
//   ControllerButtons enum value, which is what guest_input.cpp already sends.
//
//   The controller is reached through a std::map<int,int> whose node layout is
//   read out of the guest's own lookup loop: left +0x00, right +0x08,
//   parent +0x10, key (int) +0x1c, value (int) +0x20; the vector of
//   controllers starts at manager+0xa0.
//
// THE VIRTUAL GAMEPAD. A real controller is not reproducible inside an
// emulator, so the steering/brake/accelerate experiment needs a scripted
// input. The script is the same shape as the touch control file (a text file
// on the card, a list of timed states) so that one script drives the whole
// experiment and the archive shows exactly what was pressed and when.
//
//   sdmc:/switch/rr3-native-rehost/rr3-pad.txt
//
//     [at <frame>] lx=<f> ly=<f> rx=<f> ry=<f> lt=<f> rt=<f> b=<hex>
//
//   The LAST line whose "at" frame is <= the current frame applies, so a
//   script is a list of states over time. `b` is a bitmask over the guest's
//   ControllerButtons ORDINALS (bit 0 = BTN_A ... bit 8 = BTN_DPAD_LEFT),
//   which is the guest's own numbering, not the Switch's.
//
//   With no file on the card nothing changes: the physical controller path
//   is untouched.
//
namespace rr3 {

// One virtual gamepad state, in the guest's own terms.
struct VirtualPadState {
    float leftX{0.0f};
    float leftY{0.0f};
    float rightX{0.0f};
    float rightY{0.0f};
    float leftTrigger{0.0f};
    float rightTrigger{0.0f};
    unsigned int buttons{0};  // bit N == the guest's button ordinal N is held
    bool active{false};
};

// What the port last pushed into the guest, in the guest's own numbering.
// Only values that were actually handed to the guest are recorded, so this
// is the guest's own state as far as the port can see it.
struct GuestPushedInput {
    float axes[6]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    unsigned int buttons{0};  // bit N == the guest's button ordinal N is held
    unsigned long long pushes{0};
};

// True when the virtual gamepad script has a state for `frame`.
bool virtualPadForFrame(unsigned long long frame, VirtualPadState* out);

// True when the control file is present on the card.
bool virtualPadControlFilePresent();

// How many script states the last parse produced. Zero with the file present
// means the file was found and nothing in it parsed, which is a different
// fault from "no file".
int virtualPadStepCount();

// Reads the guest's own gamepad state back and appends one line to the
// guest-view log. Cheap and rate limited: it is meant to be called every
// frame and to say something roughly once every few seconds.
//
// `setJoystickExport` is the runtime address of the guest's
// SetJoystickValueJNI export. The bridge already resolves it, and passing it
// in keeps the Java_* name literal in one place (the bridge) instead of
// spreading it over the tree.
//
// The line reports BOTH sides of the door: what the port pushed, and what
// the guest is holding. A difference between the two is the finding.
void probeGuestInputDoors(unsigned long long frame,
                          const GuestPushedInput* pushed,
                          uintptr_t setJoystickExport);

}  // namespace rr3
