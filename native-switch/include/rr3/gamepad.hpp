#pragma once
//
// THE GAMEPAD LAYER: ONE VOCABULARY FOR EVERY CONTROLLER, AND A MAPPING.
//
// WHY THIS EXISTS
// ---------------
// The guest is an Android game: its ControllerManager has its own button
// ordinals (BTN_A..BTN_DPAD_CENTER) and its own axis ordinals.  Every
// controller family has its own report layout and its own idea of which
// physical button is "A":
//
//   * the Switch Pro Controller and a DualShock 4 that the console presents
//     as a Pro Controller both arrive as NPAD, and the port already reads
//     them (source/guest_input.cpp);
//   * a DualSense, an Xbox 360 pad, an Xbox One pad and an Xbox Elite pad do
//     NOT come as NPAD.  Their reports are raw HID, and their layouts differ
//     from each other.
//
// So there are two jobs, and they are separate:
//
//   1. NORMALISE every controller into ONE set of canonical inputs, so the
//      rest of the port never asks "which controller is this?";
//   2. MAP the canonical inputs onto the guest's controls, with a file on the
//      card, so the user can decide which physical button is the throttle,
//      which is the brake, and which is confirm.
//
// The mapping is not decoration.  It is what makes a controller USABLE: the
// guest's accelerator is BTN_A (MEASURED 2026-10-24, archive
// 20261024-134104-eden-thr-btnA), while on an Xbox pad the physical A is the
// bottom face button and on a DS4 it is the cross.  A user who wants the
// right trigger to be the throttle -- the way every console racing game works
// -- has to be able to say so.
//
// THE VOCABULARY
// ---------------
// Seventeen buttons and six axes, named the way a person would name them:
//
//     A B X Y L1 R1 L2 R2 SELECT START HOME
//     DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT L3 R3
//     LSTICK_X LSTICK_Y RSTICK_X RSTICK_Y LTRIGGER RTRIGGER
//
// The sticks are -1..+1 and the triggers are 0..1.
//
// THE AXIS CONVENTION, WHICH IS NOT THE ONE YOU WOULD GUESS
// -----------------------------------------------------------
// X is RIGHT-POSITIVE and Y is UP-POSITIVE.
//
// Y being up-positive is the PORT's own long-standing convention -- the port
// has always handed the guest "-norm(left.y)" (source/guest_input.cpp) --
// and a normalisation layer that flipped the sign would change what the
// guest receives.
//
// It is NOT Android's convention: the guest passes Android's AXIS_Y straight
// through to AXIS_LTHUMB_Y (ControllerManager.java:488) and Android's AXIS_Y
// is down-positive.  So this axis has always been inverted relative to what
// the guest's own Android source would have delivered.  Nothing in the
// driving uses it (the steering is X), and libnx does not document the sign
// either way (services/hid.h's HidAnalogStickState says only "X" and "Y"),
// so the port keeps the convention it has been running rather than the one it
// would have to guess.  The mapping file can flip it:
// "LSTICK_Y = AXIS_LY scale=-1".
//
// WHAT AN INPUT MAY DRIVE
// ------------------------
// An input may drive a guest BUTTON or a guest AXIS, and the two kinds cross:
// the right trigger (an axis) may drive BTN_A (a button), which is exactly
// what "the trigger is the throttle" means.  One input may drive SEVERAL
// guest controls at once, written with the "|" the guest's own joystick
// profile uses.
//
// EVERYTHING HERE IS PURE.  The parsers take a report and produce a state;
// the mapping parser takes text and produces a mapping.  Nothing here touches
// the guest, the HID service or the card, so all of it can be executed on the
// host and pinned by a test (tests/host/gamepad_selftest.cpp).
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace gamepad {

// The guest's own ordinals (ControllerManager.ControllerButtons, Java enum
// order).  These are the numbers the guest's Java code expects.
enum GuestButton : int {
    kGuestA = 0,
    kGuestB = 1,
    kGuestX = 2,
    kGuestY = 3,
    kGuestL1 = 4,
    kGuestR1 = 5,
    kGuestL2 = 6,
    kGuestR2 = 7,
    kGuestDpadLeft = 8,
    kGuestDpadRight = 9,
    kGuestDpadUp = 10,
    kGuestDpadDown = 11,
    kGuestSelect = 12,
    kGuestStart = 13,
    kGuestDpadCenter = 14,
    kGuestButtonCount = 15,
};

enum GuestAxis : int {
    kGuestLX = 0,
    kGuestLY = 1,
    kGuestRX = 2,
    kGuestRY = 3,
    kGuestLT = 4,
    kGuestRT = 5,
    kGuestAxisCount = 6,
};

// The canonical inputs.  The first sixteen are BUTTONS (a bit each); the
// rest are AXES.
enum Input : int {
    kInA = 0,
    kInB = 1,
    kInX = 2,
    kInY = 3,
    kInL1 = 4,
    kInR1 = 5,
    kInL2 = 6,
    kInR2 = 7,
    kInSelect = 8,
    kInStart = 9,
    kInHome = 10,
    kInDpadUp = 11,
    kInDpadDown = 12,
    kInDpadLeft = 13,
    kInDpadRight = 14,
    kInL3 = 15,
    kInR3 = 16,
    kInputButtonCount = 17,
    kInLStickX = 17,
    kInLStickY = 18,
    kInRStickX = 19,
    kInRStickY = 20,
    kInLTrigger = 21,
    kInRTrigger = 22,
    kInputCount = 23,
};

// One controller's state, normalised.
struct State {
    uint32_t buttons{};            // bits 0..kInputButtonCount-1
    float sticks[4]{};             // LX, LY, RX, RY  (-1..+1)
    float triggers[2]{};           // LT, RT            (0..1)
};

inline const char* inputName(int input) {
    static const char* kNames[kInputCount] = {
        "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
        "SELECT", "START", "HOME",
        "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
        "L3", "R3",
        "LSTICK_X", "LSTICK_Y", "RSTICK_X", "RSTICK_Y",
        "LTRIGGER", "RTRIGGER",
    };
    if (input < 0 || input >= kInputCount) {
        return "?";
    }
    return kNames[input];
}

// The canonical name of a GUEST control, as the mapping file spells it.
inline const char* guestButtonName(int ordinal) {
    static const char* kNames[kGuestButtonCount] = {
        "BTN_A", "BTN_B", "BTN_X", "BTN_Y", "BTN_L1", "BTN_R1", "BTN_L2", "BTN_R2",
        "BTN_DPAD_LEFT", "BTN_DPAD_RIGHT", "BTN_DPAD_UP", "BTN_DPAD_DOWN",
        "BTN_SELECT", "BTN_START", "BTN_DPAD_CENTER",
    };
    if (ordinal < 0 || ordinal >= kGuestButtonCount) {
        return "?";
    }
    return kNames[ordinal];
}

inline const char* guestAxisName(int ordinal) {
    static const char* kNames[kGuestAxisCount] = {
        "AXIS_LX", "AXIS_LY", "AXIS_RX", "AXIS_RY", "AXIS_LT", "AXIS_RT",
    };
    if (ordinal < 0 || ordinal >= kGuestAxisCount) {
        return "?";
    }
    return kNames[ordinal];
}

// The name comparison, case-insensitively.
//
// A user who types "rtrigger" instead of "RTRIGGER" should get a mapping
// that WORKS, not a file that silently does nothing: a line that does
// nothing is the worst kind of bug, because the game still runs and the
// controller is simply dead.
inline bool equalsIgnoreCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        const char ca = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 32) : *a;
        const char cb = (*b >= 'a' && *b <= 'z') ? static_cast<char>(*b - 32) : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// The guest control an AXIS ordinal names.
//
// The button ordinals and the axis ordinals live in ONE numbering, so an
// axis target is the button count plus the axis ordinal.  The arithmetic is
// done here, once, because doing it inline means adding two different
// enumeration types, which the compiler rightly complains about.
inline int guestAxisTarget(int axis) {
    return kGuestButtonCount + axis;
}

// The name of a canonical input, or -1.
inline int inputFromName(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int i = 0; i < kInputCount; ++i) {
        const char* candidate = inputName(i);
        if (equalsIgnoreCase(candidate, name)) {
            return i;
        }
    }
    return -1;
}

// The name of a guest control, or -1.
inline int guestFromName(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int i = 0; i < kGuestButtonCount; ++i) {
        if (equalsIgnoreCase(guestButtonName(i), name)) {
            return i;
        }
    }
    for (int i = 0; i < kGuestAxisCount; ++i) {
        if (equalsIgnoreCase(guestAxisName(i), name)) {
            return kGuestButtonCount + i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// THE REPORT PARSERS
// ---------------------------------------------------------------------------
//
// SONY: the DualShock 4 and the DualSense share the first five bytes of
// their report, which is why one parser serves both.
//
//   byte 0   the buttons, LSB first:
//              0 L1   1 R1   2 L2   3 R2   4 SHARE   5 OPTIONS
//              6 PS    7 TOUCH 8 R3    9 L3    10 UP    11 DOWN
//             12 LEFT 13 RIGHT 14 CIRCLE 15 CROSS
//   byte 1   the left stick X, 0..255, 128 is the centre
//   byte 2   the left stick Y, 0..255, 0 is UP (the Android convention is
//             the opposite, so this is NEGATED here)
//   byte 3   the left trigger, 0..255
//   byte 4   the right trigger, 0..255
//
// The DS4's USB report is 0x05 and 78 bytes, its Bluetooth report is 0x01 and
// 78 bytes.  The DualSense's USB report is 0x01 and 64 bytes, its Bluetooth
// report is 0x01 and 78 bytes.  The first five bytes are the same, so the
// parser only needs five bytes -- but it is given the whole report so that
// a caller can check the report id itself.
//
// XBOX: the 360 pad, the One pad, the Series pad and both Elite pads share
// the 0x20 report, 20 bytes:
//
//   byte 0   the dpad and the meta buttons:
//              0 UP   1 DOWN  2 LEFT  3 RIGHT  4 START  5 BACK
//              6 LEFT STICK  7 RIGHT STICK  8 GUIDE
//   byte 1   the left trigger, 0..255
//   byte 2   the right trigger, 0..255
//   byte 3   the left stick X, 0..255, 0 is LEFT
//   byte 4   the left stick Y, 0..255, 0 is UP
//   byte 5   the right stick X
//   byte 6   the right stick Y
//   byte 7   the face buttons: 0 A  1 B  2 X  3 Y  4 LB  5 RB  6 GUIDE  7 MENU
//
// (The Elite's paddles live in a separate report, 0x21; they are not mapped
// here, because the guest has no paddle concept and the Switch has no
// paddle either -- the mapping file can still be extended for them.)

inline float byteToUnit(uint8_t value) {
    // 0..255 -> -1..+1, with 128 at the centre.
    return (static_cast<float>(value) - 128.0f) / 127.0f;
}

inline float byteToPositive(uint8_t value) {
    return static_cast<float>(value) / 255.0f;
}

// The D-PAD HAT -> THE GUEST'S FOUR D-PAD BUTTONS.
//
// The hat is a DIRECTION, not four buttons, and the guest's d-pad is four
// buttons, so a diagonal press is two of them.
//
// The table is the Linux kernel's (drivers/hid/hid-playstation.c):
//   0 N   1 NE   2 E   3 SE   4 S   5 SW   6 W   7 NW   8 centre
inline void applyHat(uint8_t hat, uint32_t* bits) {
    static const int8_t kHatX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
    static const int8_t kHatY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};
    if (hat > 8) {
        hat = 8;
    }
    if (kHatX[hat] < 0) {
        *bits |= (1u << kInDpadLeft);
    } else if (kHatX[hat] > 0) {
        *bits |= (1u << kInDpadRight);
    }
    if (kHatY[hat] < 0) {
        *bits |= (1u << kInDpadUp);
    } else if (kHatY[hat] > 0) {
        *bits |= (1u << kInDpadDown);
    }
}

// The SONY BUTTON MASKS (the Linux driver's, again):
//   buttons[0]  bits 0..1 the hat, bit 4 square, 5 cross, 6 circle, 7 triangle
//   buttons[1]  0 L1, 1 R1, 2 L2, 3 R2, 4 create/share, 5 options, 6 L3, 7 R3
//   buttons[2]  0 PS/home, 1 touchpad, 2 mic mute
inline uint32_t sonyButtons(const uint8_t* buttons) {
    uint32_t bits = 0;
    applyHat(static_cast<uint8_t>(buttons[0] & 0x03u), &bits);
    if (buttons[0] & 0x10u) bits |= (1u << kInX);      // square
    if (buttons[0] & 0x20u) bits |= (1u << kInA);      // cross
    if (buttons[0] & 0x40u) bits |= (1u << kInB);      // circle
    if (buttons[0] & 0x80u) bits |= (1u << kInY);      // triangle
    if (buttons[1] & 0x01u) bits |= (1u << kInL1);
    if (buttons[1] & 0x02u) bits |= (1u << kInR1);
    if (buttons[1] & 0x04u) bits |= (1u << kInL2);
    if (buttons[1] & 0x08u) bits |= (1u << kInR2);
    if (buttons[1] & 0x10u) bits |= (1u << kInSelect);  // create / share
    if (buttons[1] & 0x20u) bits |= (1u << kInStart);   // options
    if (buttons[1] & 0x40u) bits |= (1u << kInL3);
    if (buttons[1] & 0x80u) bits |= (1u << kInR3);
    if (buttons[2] & 0x01u) bits |= (1u << kInHome);     // the PS button
    return bits;
}

// THE SONY REPORTS.  There are TWO layouts, because the DualShock 4 and the
// DualSense do not agree, and the difference is in where the buttons sit:
//
//   struct dualshock4_input_report_common { u8 x, y; u8 rx, ry;
//                                           u8 buttons[3]; u8 z, rz; };
//   struct dualsense_input_report       { u8 x, y; u8 rx, ry;
//                                           u8 z, rz; u8 seq; u8 buttons[3]; };
//
// (the Linux kernel, drivers/hid/hid-playstation.c -- the layout that is
// checked against real hardware every day)
//
// The report id and the transport header are NOT part of the common part:
//
//   DS4        USB  report id 0x01, 64 bytes, common at offset 1
//   DS4        BT   report id 0x11, 78 bytes, common at offset 3
//   DS4        BT   report id 0x01, 10 bytes (minimal), common at offset 1
//   DualSense      report id 0x01,        common at offset 1
//
// So the DS4 puts the BUTTONS before the triggers and the DualSense puts
// them after.  The two parsers below take the COMMON part, so the caller
// has to strip the header first.

inline bool parseDualShock4(const uint8_t* common, size_t length, State* out) {
    if (common == nullptr || out == nullptr || length < 9) {
        return false;
    }
    out->buttons = sonyButtons(common + 4);
    out->sticks[0] = byteToUnit(common[0]);
    // SONY'S Y IS DOWN-POSITIVE AND THE GUEST'S IS UP-POSITIVE.
    out->sticks[1] = -byteToUnit(common[1]);
    out->sticks[2] = byteToUnit(common[2]);
    out->sticks[3] = -byteToUnit(common[3]);
    out->triggers[0] = byteToPositive(common[7]);   // z
    out->triggers[1] = byteToPositive(common[8]);   // rz
    return true;
}

inline bool parseDualSense(const uint8_t* common, size_t length, State* out) {
    if (common == nullptr || out == nullptr || length < 10) {
        return false;
    }
    out->buttons = sonyButtons(common + 7);
    out->sticks[0] = byteToUnit(common[0]);
    out->sticks[1] = -byteToUnit(common[1]);
    out->sticks[2] = byteToUnit(common[2]);
    out->sticks[3] = -byteToUnit(common[3]);
    out->triggers[0] = byteToPositive(common[4]);   // z
    out->triggers[1] = byteToPositive(common[5]);   // rz
    return true;
}

// The report id and where the common part starts, for the SONY reports.
inline bool sonyCommonOffset(uint8_t reportId, size_t reportLength,
                             size_t* offset) {
    if (offset == nullptr) {
        return false;
    }
    if (reportId == 0x01u) {
        // The DS4's USB report, the DS4's minimal Bluetooth report, and the
        // DualSense's report all start their common part at offset 1.
        *offset = 1;
        return true;
    }
    if (reportId == 0x11u) {
        // The DS4's full Bluetooth report: id, then two reserved bytes.
        *offset = 3;
        return true;
    }
    (void)reportLength;
    return false;
}

// XBOX: the 360 pad, the One pad, the Series pad and both Elite pads share
// the 0x20 report, 20 bytes:
//
//   byte 0   the d-pad and the meta buttons:
//              0 UP   1 DOWN  2 LEFT  3 RIGHT  4 START  5 BACK
//              6 LEFT STICK  7 RIGHT STICK  8 GUIDE
//   byte 1   the left trigger, 0..255
//   byte 2   the right trigger, 0..255
//   byte 3   the left stick X, 0..255, 0 is LEFT
//   byte 4   the left stick Y, 0..255, 0 is UP
//   byte 5   the right stick X
//   byte 6   the right stick Y
//   byte 7   the face buttons: 0 A  1 B  2 X  3 Y  4 LB  5 RB  6 GUIDE  7 MENU
//
// (The Elite's paddles live in a separate report, 0x21; the guest has no
// paddle concept and the Switch has no paddle either.)
inline bool parseXboxReport(const uint8_t* report, size_t length, State* out) {
    if (report == nullptr || out == nullptr || length < 8) {
        return false;
    }
    const uint8_t meta = report[0];
    const uint8_t face = report[7];
    uint32_t bits = 0;
    if (meta & 0x01u) bits |= (1u << kInDpadUp);
    if (meta & 0x02u) bits |= (1u << kInDpadDown);
    if (meta & 0x04u) bits |= (1u << kInDpadLeft);
    if (meta & 0x08u) bits |= (1u << kInDpadRight);
    if (meta & 0x10u) bits |= (1u << kInStart);
    if (meta & 0x20u) bits |= (1u << kInSelect);
    if (meta & 0x40u) bits |= (1u << kInL3);
    if (meta & 0x80u) bits |= (1u << kInR3);
    if (face & 0x01u) bits |= (1u << kInA);
    if (face & 0x02u) bits |= (1u << kInB);
    if (face & 0x04u) bits |= (1u << kInX);
    if (face & 0x08u) bits |= (1u << kInY);
    if (face & 0x10u) bits |= (1u << kInL1);
    if (face & 0x20u) bits |= (1u << kInR1);
    out->buttons = bits;
    out->triggers[0] = byteToPositive(report[1]);
    out->triggers[1] = byteToPositive(report[2]);
    // THE XBOX STICKS ARE LEFT-POSITIVE AND UP-POSITIVE, so X is not negated
    // and Y is -- the same as the Sony layout.
    out->sticks[0] = byteToUnit(report[3]);
    out->sticks[1] = -byteToUnit(report[4]);
    out->sticks[2] = byteToUnit(report[5]);
    out->sticks[3] = -byteToUnit(report[6]);
    return true;
}

// ---------------------------------------------------------------------------
// THE SWITCH'S PHYSICAL CONTROLLER
// ---------------------------------------------------------------------------
//
// The console's controller, described without libnx's types, so that the
// translation into the canonical inputs is a PURE function and can be
// executed -- and pinned -- on the host.
//
// The sticks are already normalised to -1..+1 and the triggers to 0..1.
//
// THE TRIGGER VALUES ARE THE CALLER'S JOB.  lt and rt are the VALUE, not
// the button: a controller with an analogue trigger reports the pressure,
// and a controller without one reports 1.0 while the button is down and 0.0
// otherwise.  The translation does NOT guess.  A trigger that is down but
// reads 0.0 is a caller bug, and hiding it would be exactly the kind of
// silent fallback that turns a broken throttle into "the car is a bit
// slow".
struct NpadState {
    bool a{false}, b{false}, x{false}, y{false};
    bool l{false}, r{false}, zl{false}, zr{false};
    bool minus{false}, plus{false};
    bool left{false}, right{false}, up{false}, down{false};
    bool stickL{false}, stickR{false};
    bool home{false};
    float lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};
    float lt{0.0f}, rt{0.0f};
};

// THE TRANSLATION.
//
// It reproduces EXACTLY what source/guest_input.cpp pushed to the guest
// before this layer existed:
//
//   * ZR drove BTN_A, BTN_R2 and the right trigger axis;
//   * ZL drove BTN_L2 and the left trigger axis;
//   * the left stick click drove BTN_START and the right stick click drove
//     BTN_DPAD_CENTER;
//   * the sticks went across as (x, -y), because the port's convention is
//     that Y is UP-POSITIVE.
//
// A trigger is BOTH a button and an axis: ZL sets the canonical L2 button
// and the canonical LTRIGGER axis, so a file can bind either one.
inline State canonicalFromNpad(const NpadState& n) {
    State state;
    if (n.a) {
        state.buttons |= (1u << kInA);
    }
    if (n.b) {
        state.buttons |= (1u << kInB);
    }
    if (n.x) {
        state.buttons |= (1u << kInX);
    }
    if (n.y) {
        state.buttons |= (1u << kInY);
    }
    if (n.l) {
        state.buttons |= (1u << kInL1);
    }
    if (n.r) {
        state.buttons |= (1u << kInR1);
    }
    if (n.zl) {
        state.buttons |= (1u << kInL2);
    }
    if (n.zr) {
        state.buttons |= (1u << kInR2);
    }
    if (n.minus) {
        state.buttons |= (1u << kInSelect);
    }
    if (n.plus) {
        state.buttons |= (1u << kInStart);
    }
    if (n.home) {
        state.buttons |= (1u << kInHome);
    }
    if (n.up) {
        state.buttons |= (1u << kInDpadUp);
    }
    if (n.down) {
        state.buttons |= (1u << kInDpadDown);
    }
    if (n.left) {
        state.buttons |= (1u << kInDpadLeft);
    }
    if (n.right) {
        state.buttons |= (1u << kInDpadRight);
    }
    if (n.stickL) {
        state.buttons |= (1u << kInL3);
    }
    if (n.stickR) {
        state.buttons |= (1u << kInR3);
    }
    state.sticks[0] = n.lx;
    state.sticks[1] = -n.ly;
    state.sticks[2] = n.rx;
    state.sticks[3] = -n.ry;
    state.triggers[0] = n.lt;
    state.triggers[1] = n.rt;
    return state;
}

// ---------------------------------------------------------------------------
// THE MAPPING
// ---------------------------------------------------------------------------
//
// One canonical input can drive one guest control, and the mapping says
// which.  A canonical input may also drive a guest BUTTON (for example the
// right trigger, which is the accelerator on the console) or a guest AXIS.
//
// The mapping file is TEXT, one binding per line:
//
//     <canonical input> = <guest control> [scale=<f>] [invert=<0|1>]
//
// with these keys at the top:
//
//     autopilot = 0|1      whether the port's own closed loop may drive
//     deadzone = <0..1>    the stick deadzone
//     invert = <input>     a shorthand for "invert this axis"
//
// THE DEFAULTS ARE THE MEASURED ONES.  The guest's accelerator is BTN_A
// (archive 20261024-134104-eden-thr-btnA) and the guest's own gamepad
// profile binds the accelerator to "button 0 (A) | axis 5 (RT) | button 7
// (R2)", so the right trigger drives BTN_A as well as the right trigger axis
// -- which is what a console racing game does, and what the port measured to
// work.

// WHO IS DRIVING.
//
// The port has its own closed loop (source/race_state_probe.cpp): it reads the
// guest's own race state and computes the throttle, the brake and the
// steering.  That loop is what makes the car drive without a human, and it
// is also exactly what would fight a human who is driving.
//
// The mapping file says which of the two owns the car.
enum AutopilotMode : int {
    kAutopilotManual = 0,   // the port does not drive at all
    kAutopilotAuto = 1,     // the port drives; the human's input is ignored
    kAutopilotAssist = 2,   // the human drives, and the port fills in only
                             // the controls the human is leaving alone
};

struct Binding {
    int input{-1};        // the canonical input, or -1
    int guest{-1};        // the guest control (a button ordinal, or
                          // kGuestButtonCount + an axis ordinal)
    float scale{1.0f};
    bool invert{false};
    // The value at which an AXIS input counts as pressing a guest BUTTON.
    // A trigger is the case that matters: with the default 0.5 an analogue
    // throttle would not bite until it was half way down, which is wrong for
    // a throttle, so a trigger binding wants a low threshold.
    float threshold{0.5f};
    // Whether this binding came from the built-in defaults.  An explicit
    // line in the mapping file REPLACES every binding for its input, so a
    // file is authoritative for the inputs it mentions and the defaults
    // survive for the inputs it does not.  This flag is what lets the port
    // say "the file changed 3 of the 22 defaults" instead of guessing.
    bool fromDefault{false};
};

struct Mapping {
    int autopilot{kAutopilotAuto};
    float deadzone{0.12f};
    // ONE list, because a binding may cross the kinds: the right trigger
    // (an axis) may drive the guest's BTN_A (a button), which is exactly
    // what "the trigger is the throttle" means.
    Binding bindings[kInputCount * 4]{};
    int bindingCount{0};
};

// The value a canonical axis carries, or 0 for a button.
inline float inputValue(const State& state, int input) {
    switch (input) {
    case kInLStickX: return state.sticks[0];
    case kInLStickY: return state.sticks[1];
    case kInRStickX: return state.sticks[2];
    case kInRStickY: return state.sticks[3];
    case kInLTrigger: return state.triggers[0];
    case kInRTrigger: return state.triggers[1];
    default: return 0.0f;
    }
}

// Whether a canonical input counts as "pressed".  A button is pressed when
// it is down; an axis counts as pressed when it is past the middle, so an
// analogue trigger can drive a digital guest button.
inline bool inputActive(const State& state, int input) {
    if (input < kInputButtonCount) {
        return (state.buttons & (1u << input)) != 0;
    }
    const float value = inputValue(state, input);
    return value > 0.5f || value < -0.5f;
}

// The same question with the binding's own threshold.
//
// The DEFAULT threshold (0.5) is right for a stick and wrong for a trigger:
// a throttle that does not bite until the trigger is half way down feels
// broken, so a trigger binding wants a low one ("thresh=0.2" in the file).
// A negative value counts as pressing too, because an axis that was
// inverted ("invert=1") pushes the other way.
inline bool inputActiveWithThreshold(const State& state, int input,
                                     float threshold) {
    if (input < kInputButtonCount) {
        return (state.buttons & (1u << input)) != 0;
    }
    const float value = inputValue(state, input);
    if (threshold < 0.0f) {
        threshold = -threshold;
    }
    return value >= threshold || value <= -threshold;
}

// THE BUILT-IN DEFAULTS.
//
// THEY ARE THE PORT'S MEASURED BEHAVIOUR, EXACTLY.
//
// Every line here is what source/guest_input.cpp pushed to the guest before
// this layer existed, so a user with no mapping file gets exactly the
// behaviour that was measured to work -- and the card's mapping file
// reproduces the same thing in text.
//
// THE MEASUREMENTS BEHIND THEM:
//   * the guest's accelerator is BTN_A (archive 20261024-134104-eden-thr-btnA:
//     with only BTN_R2 held the guest draws its own "hold the brake to
//     reverse" message, i.e. the car is NOT moving; with BTN_A held it does);
//   * the port mapped ZR onto BTN_A as well as onto BTN_R2 and the right
//     trigger axis, and ZL onto BTN_L2 and the left trigger axis;
//   * the port mapped the left stick click onto BTN_START and the right
//     stick click onto BTN_DPAD_CENTER.
//
// The canonical START (the console's PLUS) is deliberately NOT bound to
// BTN_START: PLUS is the port's own frame-loop exit chord, and a guest
// menu opening on it would be a surprise.  A mapping file can bind it.
inline void mappingSetDefaults(Mapping* mapping) {
    if (mapping == nullptr) {
        return;
    }
    *mapping = Mapping{};
    struct Default {
        int input;
        int guest;
    };
    static const Default kDefaults[] = {
        {kInA, kGuestA}, {kInB, kGuestB}, {kInX, kGuestX}, {kInY, kGuestY},
        {kInL1, kGuestL1}, {kInR1, kGuestR1}, {kInL2, kGuestL2},
        {kInSelect, kGuestSelect},
        {kInDpadUp, kGuestDpadUp}, {kInDpadDown, kGuestDpadDown},
        {kInDpadLeft, kGuestDpadLeft}, {kInDpadRight, kGuestDpadRight},
        // THE RIGHT TRIGGER IS ALSO THE ACCELERATOR.  MEASURED 2026-10-24:
        // with only BTN_R2 held the guest draws its own "hold the brake to
        // reverse" message -- the car is NOT moving; with BTN_A held it does.
        // The guest's own profile also binds the accelerator to
        // "button 0 (A) | axis 5 (RT) | button 7 (R2)", so BTN_R2 is left
        // in place as well, exactly as the port pushed it.
        {kInR2, kGuestA},
        {kInR2, kGuestR2},
        // THE STICK CLICKS, exactly as the port pushed them.
        {kInL3, kGuestStart},
        {kInR3, kGuestDpadCenter},
        {kInLStickX, guestAxisTarget(kGuestLX)},
        {kInLStickY, guestAxisTarget(kGuestLY)},
        {kInRStickX, guestAxisTarget(kGuestRX)},
        {kInRStickY, guestAxisTarget(kGuestRY)},
        {kInLTrigger, guestAxisTarget(kGuestLT)},
        {kInRTrigger, guestAxisTarget(kGuestRT)},
    };
    for (const Default& entry : kDefaults) {
        Binding& binding = mapping->bindings[mapping->bindingCount++];
        binding.input = entry.input;
        binding.guest = entry.guest;
        binding.fromDefault = true;
    }
}

// The mapping applied to a controller's state: the guest's button mask (a
// bit per guest ordinal) and the guest's six axis values.
struct GuestState {
    uint32_t buttons{};             // bits 0..kGuestButtonCount-1
    float axes[kGuestAxisCount]{};   // LX, LY, RX, RY in -1..+1, LT, RT 0..1
};

inline float applyDeadzone(float value, float deadzone) {
    if (value > -deadzone && value < deadzone) {
        return 0.0f;
    }
    return value;
}

inline void applyMapping(const Mapping& mapping, const State& state,
                         GuestState* out) {
    if (out == nullptr) {
        return;
    }
    *out = GuestState{};
    for (int i = 0; i < mapping.bindingCount; ++i) {
        const Binding& binding = mapping.bindings[i];
        if (binding.input < 0 || binding.guest < 0) {
            continue;
        }
        if (binding.guest < kGuestButtonCount) {
            // A canonical input driving a guest BUTTON.
            if (inputActiveWithThreshold(state, binding.input,
                                         binding.threshold)) {
                out->buttons |= (1u << binding.guest);
            }
            continue;
        }
        // A canonical input driving a guest AXIS.
        float value = inputValue(state, binding.input);
        if (binding.input < kInputButtonCount) {
            // A button driving an axis is a full press.
            value = (state.buttons & (1u << binding.input)) != 0 ? 1.0f : 0.0f;
        }
        if (binding.invert) {
            value = -value;
        }
        value *= binding.scale;
        const int axis = binding.guest - kGuestButtonCount;
        if (axis < 0 || axis >= kGuestAxisCount) {
            continue;
        }
        if (axis == kGuestLT || axis == kGuestRT) {
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
        } else {
            value = applyDeadzone(value, mapping.deadzone);
            if (value > 1.0f) value = 1.0f;
            if (value < -1.0f) value = -1.0f;
        }
        out->axes[axis] = value;
    }
}

// ---------------------------------------------------------------------------
// WHO IS ACTUALLY HOLDING WHAT
// ---------------------------------------------------------------------------
//
// The port's closed loop and a human cannot both own the throttle.  These
// two questions are how the port finds out whether a human is touching a
// control:
//
//   * "is the control that drives the guest's BTN_A being held?"  -> the
//     human is on the throttle;
//   * "how far has the control that drives the guest's AXIS_LX been
//     moved?" -> the human is steering.
//
// Both are answered from the MAPPING, not from a fixed list, so a user who
// remaps the throttle to the right trigger gets the right answer for free.

// Is the guest button at `guestOrdinal` being driven right now?
//
// Any input bound to that guest button counts, so with the defaults the
// answer is "the human is holding A or the right shoulder button".
inline bool mappingGuestButtonActive(const Mapping& mapping, const State& state,
                                     int guestOrdinal) {
    for (int i = 0; i < mapping.bindingCount; ++i) {
        const Binding& binding = mapping.bindings[i];
        if (binding.input < 0 || binding.guest != guestOrdinal) {
            continue;
        }
        if (inputActiveWithThreshold(state, binding.input, binding.threshold)) {
            return true;
        }
    }
    return false;
}

// How far the guest axis at `guestAxis` is being moved, 0..1.
//
// The deadzone is applied, so a stick that is resting in its deadzone reads
// as zero and the closed loop is free to steer.
inline float mappingGuestAxisMagnitude(const Mapping& mapping,
                                       const State& state, int guestAxis) {
    float best = 0.0f;
    for (int i = 0; i < mapping.bindingCount; ++i) {
        const Binding& binding = mapping.bindings[i];
        if (binding.input < 0) {
            continue;
        }
        if (binding.guest != kGuestButtonCount + guestAxis) {
            continue;
        }
        float value = inputValue(state, binding.input);
        if (binding.input < kInputButtonCount) {
            value = (state.buttons & (1u << binding.input)) != 0 ? 1.0f : 0.0f;
        }
        if (binding.invert) {
            value = -value;
        }
        value *= binding.scale;
        value = applyDeadzone(value, mapping.deadzone);
        if (value < 0.0f) {
            value = -value;
        }
        if (value > best) {
            best = value;
        }
    }
    return best;
}

// The guest axis an input drives, or -1.  Used to answer "is this input the
// steering?" without hard-coding the guest's axis ordinals.
inline int mappingInputGuestAxis(const Mapping& mapping, int input) {
    for (int i = 0; i < mapping.bindingCount; ++i) {
        const Binding& binding = mapping.bindings[i];
        if (binding.input == input && binding.guest >= kGuestButtonCount) {
            return binding.guest - kGuestButtonCount;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// THE MAPPING FILE'S PARSER
// ---------------------------------------------------------------------------
//
// Pure: it takes the text and fills a Mapping.  A line it cannot understand
// is a line that never fires, and nothing on screen says so -- so the parser
// reports the first bad line and the caller logs it.
//
// The format:
//
//     # a comment
//     autopilot = 0
//     deadzone = 0.15
//     A = BTN_A
//     RTRIGGER = BTN_A invert=1
//     LSTICK_Y = AXIS_LY scale=-1

inline bool parseMappingText(const char* text, Mapping* out, int* errorLine) {
    if (errorLine != nullptr) {
        *errorLine = 0;
    }
    if (out == nullptr) {
        return false;
    }
    // THE WORK HAPPENS ON A COPY.
    //
    // A file with a bad line must not leave the caller with half a mapping:
    // the port would drive the car with a throttle that had been unbound and
    // a brake that had not.  So the caller's mapping is only replaced when
    // the whole file parsed.
    Mapping work;
    mappingSetDefaults(&work);
    if (text == nullptr) {
        *out = work;
        return true;
    }
    int lineNumber = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        ++lineNumber;
        const char* end = std::strchr(cursor, '\n');
        const size_t length = (end != nullptr)
                                  ? static_cast<size_t>(end - cursor)
                                  : std::strlen(cursor);
        char line[256];
        const size_t copy = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
        std::memcpy(line, cursor, copy);
        line[copy] = '\0';
        cursor = (end != nullptr) ? end + 1 : cursor + length;

        // THE COMMENT, THEN THE BLANK LINE.
        char* hash = std::strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char* begin = line;
        while (*begin == ' ' || *begin == '\t' || *begin == '\r') {
            ++begin;
        }
        char* finish = begin + std::strlen(begin);
        while (finish > begin &&
               (finish[-1] == ' ' || finish[-1] == '\t' || finish[-1] == '\r')) {
            --finish;
        }
        *finish = '\0';
        if (*begin == '\0') {
            continue;
        }
        // THE KEY AND THE VALUE.
        char* equals = std::strchr(begin, '=');
        if (equals == nullptr) {
            if (errorLine != nullptr) {
                *errorLine = lineNumber;
            }
            return false;
        }
        *equals = '\0';
        char* key = begin;
        char* value = equals + 1;
        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        char* keyEnd = key + std::strlen(key);
        while (keyEnd > key && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) {
            --keyEnd;
        }
        *keyEnd = '\0';

        if (equalsIgnoreCase(key, "autopilot")) {
            if (equalsIgnoreCase(value, "0") ||
                equalsIgnoreCase(value, "manual") ||
                equalsIgnoreCase(value, "off")) {
                work.autopilot = kAutopilotManual;
            } else if (equalsIgnoreCase(value, "1") ||
                       equalsIgnoreCase(value, "auto") ||
                       equalsIgnoreCase(value, "on")) {
                work.autopilot = kAutopilotAuto;
            } else if (equalsIgnoreCase(value, "assist")) {
                work.autopilot = kAutopilotAssist;
            } else {
                if (errorLine != nullptr) {
                    *errorLine = lineNumber;
                }
                return false;
            }
            continue;
        }
        if (equalsIgnoreCase(key, "deadzone")) {
            work.deadzone = static_cast<float>(std::atof(value));
            if (work.deadzone < 0.0f) {
                work.deadzone = 0.0f;
            }
            if (work.deadzone > 0.9f) {
                work.deadzone = 0.9f;
            }
            continue;
        }

        const int input = inputFromName(key);
        if (input < 0) {
            if (errorLine != nullptr) {
                *errorLine = lineNumber;
            }
            return false;
        }
        // THE TARGETS AND THE OPTIONS.
        //
        // The value is a LIST: the guest controls this input drives,
        // separated by "|" (or spaces), plus the options "scale=", "invert="
        // and "thresh=".  A token with an "=" in it is an option; a token
        // without one is a guest control.
        //
        // The "|" is the guest's own separator: the guest's own joystick
        // profile writes "button 0 (A) | axis 5 (RT) | button 7 (R2)".
        int targets[kInputCount * 2];
        int targetCount = 0;
        float scale = 1.0f;
        bool invert = false;
        float threshold = 0.5f;
        bool none = false;
        char* token = value;
        while (*token != '\0') {
            while (*token == ' ' || *token == '\t' || *token == '|' ||
                   *token == ',') {
                ++token;
            }
            if (*token == '\0') {
                break;
            }
            char* stop = token;
            while (*stop != '\0' && *stop != ' ' && *stop != '\t' &&
                   *stop != '|' && *stop != ',') {
                ++stop;
            }
            const bool last = (*stop == '\0');
            *stop = '\0';
            if (std::strncmp(token, "scale=", 6) == 0) {
                scale = static_cast<float>(std::atof(token + 6));
            } else if (std::strncmp(token, "invert=", 7) == 0) {
                invert = (token[7] != '0');
            } else if (std::strncmp(token, "thresh=", 7) == 0) {
                threshold = static_cast<float>(std::atof(token + 7));
            } else if (equalsIgnoreCase(token, "none")) {
                none = true;
            } else {
                const int guest = guestFromName(token);
                if (guest < 0) {
                    if (errorLine != nullptr) {
                        *errorLine = lineNumber;
                    }
                    return false;
                }
                if (targetCount >=
                    static_cast<int>(sizeof(targets) / sizeof(targets[0]))) {
                    if (errorLine != nullptr) {
                        *errorLine = lineNumber;
                    }
                    return false;
                }
                targets[targetCount++] = guest;
            }
            if (last) {
                break;
            }
            token = stop + 1;
        }
        if (none) {
            // "none" REMOVES the input: it drives nothing at all.  It may
            // not be mixed with real targets, because "drives nothing AND
            // drives BTN_A" is not a thing.
            if (targetCount > 0) {
                if (errorLine != nullptr) {
                    *errorLine = lineNumber;
                }
                return false;
            }
        } else if (targetCount == 0) {
            // A line with no target is a line that does NOTHING, which is
            // never what the user meant.
            if (errorLine != nullptr) {
                *errorLine = lineNumber;
            }
            return false;
        }
        // THE INPUT'S BINDINGS ARE REPLACED, NOT ADDED TO.
        //
        // A file is authoritative for the inputs it mentions, and the
        // built-in defaults survive for the inputs it does not mention.
        // "|" is how one input drives several guest controls at once.
        int kept = 0;
        for (int i = 0; i < work.bindingCount; ++i) {
            if (work.bindings[i].input == input) {
                continue;
            }
            work.bindings[kept++] = work.bindings[i];
        }
        work.bindingCount = kept;
        for (int t = 0; t < targetCount; ++t) {
            if (work.bindingCount >=
                static_cast<int>(sizeof(work.bindings) / sizeof(work.bindings[0]))) {
                if (errorLine != nullptr) {
                    *errorLine = lineNumber;
                }
                return false;
            }
            Binding& binding = work.bindings[work.bindingCount++];
            binding.input = input;
            binding.guest = targets[t];
            binding.scale = scale;
            binding.invert = invert;
            binding.threshold = threshold;
            binding.fromDefault = false;
        }
    }
    *out = work;
    return true;
}

}  // namespace gamepad
}  // namespace rr3
