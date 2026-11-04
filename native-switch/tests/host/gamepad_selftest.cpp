// THE GAMEPAD LAYER, EXECUTED.
//
// Everything here is pure, so all of it runs on the host.  The point of
// running it is that a decoder nobody executes is a decoder nobody knows
// works: the user plugs in a controller, the game reads the inputs as
// garbage, and there is nothing on screen that says why.
//
// THE THREE THINGS THAT ARE PROVED
// --------------------------------
//   1. THE REPORT PARSERS decode the bytes those controllers actually send.
//      The Sony layouts are the ones in the Linux kernel's
//      drivers/hid/hid-playstation.c, which is checked against real
//      hardware every day; the Xbox layout is the classic 0x20 report.
//   2. THE MAPPING FILE does what it says, including the lines it cannot
//      understand -- because a line that is silently dropped is a line
//      that never fires.
//   3. THE CARD'S FILE and the built-in defaults produce the SAME guest
//      state.  The defaults are the port's measured behaviour, so the
//      shipped file must not change it.
//
// The card's file is passed on the command line so that the third check
// reads the file that actually ships.

#include "rr3/gamepad.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace gp = rr3::gamepad;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A FLOAT COMPARISON WITH A TOLERANCE.
//
// The decoders divide by 127, so the value a byte at one end of the range
// produces is 128/127, not 1.  A test that insists on exactly 1.0 would be
// testing the arithmetic of the test rather than the decoder.
static bool near(float actual, float expected, float tolerance = 0.01f) {
    const float difference = actual - expected;
    return (difference < 0.0f ? -difference : difference) <= tolerance;
}

static void checkNear(float actual, float expected, const char* what) {
    check(near(actual, expected), what);
}

// ---------------------------------------------------------------------------
// 1. THE NAME TABLES
// ---------------------------------------------------------------------------
static void testNames() {
    check(gp::inputName(gp::kInA) != nullptr && std::strcmp(gp::inputName(gp::kInA), "A") == 0,
          "the input A is named A");
    check(gp::inputName(gp::kInRTrigger) != nullptr &&
              std::strcmp(gp::inputName(gp::kInRTrigger), "RTRIGGER") == 0,
          "the right trigger is named RTRIGGER");
    check(gp::inputName(-1) != nullptr && std::strcmp(gp::inputName(-1), "?") == 0,
          "an unknown input has a name");
    check(gp::guestButtonName(gp::kGuestDpadCenter) != nullptr &&
              std::strcmp(gp::guestButtonName(gp::kGuestDpadCenter), "BTN_DPAD_CENTER") == 0,
          "the guest's centre button is named");
    check(gp::guestAxisName(gp::kGuestRT) != nullptr &&
              std::strcmp(gp::guestAxisName(gp::kGuestRT), "AXIS_RT") == 0,
          "the guest's right trigger axis is named");

    for (int i = 0; i < gp::kInputCount; ++i) {
        check(gp::inputFromName(gp::inputName(i)) == i,
              "every input name round-trips");
    }
    for (int i = 0; i < gp::kGuestButtonCount; ++i) {
        check(gp::guestFromName(gp::guestButtonName(i)) == i,
              "every guest button name round-trips");
    }
    for (int i = 0; i < gp::kGuestAxisCount; ++i) {
        check(gp::guestFromName(gp::guestAxisName(i)) == gp::kGuestButtonCount + i,
              "every guest axis name round-trips");
    }
    // A USER WHO TYPES THE WRONG CASE SHOULD STILL GET A WORKING MAPPING.
    check(gp::inputFromName("rtrigger") == gp::kInRTrigger,
          "an input name is matched case-insensitively");
    check(gp::guestFromName("btn_a") == gp::kGuestA,
          "a guest control name is matched case-insensitively");
    check(gp::inputFromName("NOT_AN_INPUT") == -1, "an unknown input is -1");
    check(gp::guestFromName("NOT_A_CONTROL") == -1, "an unknown control is -1");
}

// ---------------------------------------------------------------------------
// 2. THE SWITCH'S CONTROLLER -> THE CANONICAL INPUTS
// ---------------------------------------------------------------------------
//
// THE MEASURED BEHAVIOUR, which the defaults have to reproduce:
//
//   ZR drove BTN_A, BTN_R2 and the right trigger axis;
//   ZL drove BTN_L2 and the left trigger axis;
//   the left stick click drove BTN_START and the right one BTN_DPAD_CENTER;
//   the sticks went across as (x, -y).
static void testCanonical() {
    gp::NpadState n;
    n.zr = true;
    n.rt = 1.0f;
    const gp::State s = gp::canonicalFromNpad(n);
    check((s.buttons & (1u << gp::kInR2)) != 0, "ZR is the canonical R2");
    check((s.buttons & (1u << gp::kInRTrigger)) == 0,
          "a trigger is an axis, not a button, in the canonical layer");
    check(s.triggers[1] == 1.0f, "ZR carries the trigger VALUE");

    n = gp::NpadState{};
    n.zl = true;
    n.lt = 1.0f;
    const gp::State l = gp::canonicalFromNpad(n);
    check((l.buttons & (1u << gp::kInL2)) != 0, "ZL is the canonical L2");
    check(l.triggers[0] == 1.0f, "ZL carries the trigger VALUE");

    n = gp::NpadState{};
    n.stickL = true;
    check((gp::canonicalFromNpad(n).buttons & (1u << gp::kInL3)) != 0,
          "the left stick click is the canonical L3");
    n = gp::NpadState{};
    n.stickR = true;
    check((gp::canonicalFromNpad(n).buttons & (1u << gp::kInR3)) != 0,
          "the right stick click is the canonical R3");
    n = gp::NpadState{};
    n.plus = true;
    check((gp::canonicalFromNpad(n).buttons & (1u << gp::kInStart)) != 0,
          "PLUS is the canonical START");

    // THE Y AXIS IS NEGATED: the port's convention is that Y is UP-POSITIVE.
    n = gp::NpadState{};
    n.ly = 0.5f;
    check(gp::canonicalFromNpad(n).sticks[1] == -0.5f, "the left stick Y is negated");
    n = gp::NpadState{};
    n.ry = 0.5f;
    check(gp::canonicalFromNpad(n).sticks[3] == -0.5f, "the right stick Y is negated");
    n = gp::NpadState{};
    n.lx = 0.25f;
    check(gp::canonicalFromNpad(n).sticks[0] == 0.25f, "the left stick X is not negated");
}

// ---------------------------------------------------------------------------
// 3. THE DEFAULTS REPRODUCE THE MEASURED BEHAVIOUR
// ---------------------------------------------------------------------------
static void testDefaults() {
    gp::Mapping mapping;
    gp::mappingSetDefaults(&mapping);
    check(mapping.autopilot == gp::kAutopilotAuto, "the default is autopilot");
    check(mapping.deadzone > 0.0f && mapping.deadzone < 1.0f, "the default deadzone");

    gp::NpadState n;
    n.zr = true;
    n.rt = 1.0f;
    gp::GuestState guest;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) != 0, "ZR drives BTN_A");
    check((guest.buttons & (1u << gp::kGuestR2)) != 0, "ZR drives BTN_R2");
    check(guest.axes[gp::kGuestRT] == 1.0f, "ZR drives AXIS_RT");

    n = gp::NpadState{};
    n.zl = true;
    n.lt = 1.0f;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestL2)) != 0, "ZL drives BTN_L2");
    check(guest.axes[gp::kGuestLT] == 1.0f, "ZL drives AXIS_LT");

    n = gp::NpadState{};
    n.stickL = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestStart)) != 0,
          "the left stick click drives BTN_START");
    n = gp::NpadState{};
    n.stickR = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestDpadCenter)) != 0,
          "the right stick click drives BTN_DPAD_CENTER");

    // PLUS IS THE PORT'S OWN EXIT CHORD, SO IT IS NOT BOUND BY DEFAULT.
    n = gp::NpadState{};
    n.plus = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check(guest.buttons == 0u, "PLUS drives nothing by default");

    n = gp::NpadState{};
    n.minus = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestSelect)) != 0, "MINUS drives BTN_SELECT");

    // THE STICKS.
    n = gp::NpadState{};
    n.lx = 0.5f;
    n.ly = -0.25f;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check(guest.axes[gp::kGuestLX] == 0.5f, "the left stick X crosses as AXIS_LX");
    check(guest.axes[gp::kGuestLY] == 0.25f, "the left stick Y crosses as AXIS_LY");
}

// ---------------------------------------------------------------------------
// 4. THE MAPPING FILE
// ---------------------------------------------------------------------------
static void testParser() {
    gp::Mapping mapping;
    int errorLine = 0;

    check(gp::parseMappingText("# only a comment\n", &mapping, &errorLine),
          "a file with only a comment parses");
    check(mapping.autopilot == gp::kAutopilotAuto,
          "a file with no settings keeps the default autopilot");

    check(gp::parseMappingText("autopilot = 0\n", &mapping, &errorLine),
          "autopilot = 0 parses");
    check(mapping.autopilot == gp::kAutopilotManual, "autopilot = 0 is manual");
    check(gp::parseMappingText("autopilot = assist\n", &mapping, &errorLine),
          "autopilot = assist parses");
    check(mapping.autopilot == gp::kAutopilotAssist, "autopilot = assist is assist");
    check(gp::parseMappingText("autopilot = 1\n", &mapping, &errorLine),
          "autopilot = 1 parses");
    check(mapping.autopilot == gp::kAutopilotAuto, "autopilot = 1 is auto");
    check(!gp::parseMappingText("autopilot = maybe\n", &mapping, &errorLine),
          "an unknown autopilot value is refused");
    check(errorLine == 1, "the refused line is reported");

    check(gp::parseMappingText("deadzone = 0.25\n", &mapping, &errorLine),
          "a deadzone parses");
    check(mapping.deadzone > 0.24f && mapping.deadzone < 0.26f, "the deadzone is read");

    // ONE INPUT, SEVERAL TARGETS.
    check(gp::parseMappingText("R2 = BTN_A | BTN_R2\n", &mapping, &errorLine),
          "a target list parses");
    gp::NpadState n;
    n.zr = true;
    n.rt = 1.0f;
    gp::GuestState guest;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) != 0, "the first target fires");
    check((guest.buttons & (1u << gp::kGuestR2)) != 0, "the second target fires");

    // AN AXIS INPUT DRIVING A BUTTON, WITH A THRESHOLD.
    //
    // R2 IS REMOVED FIRST, because the DEFAULT mapping binds R2 to BTN_A as
    // well: with both in place the guest cannot tell which of them pressed
    // the button, and the threshold would look like it fired when it did
    // not.  That is the whole reason the file is authoritative for the
    // inputs it names -- a line can take a binding away.
    check(gp::parseMappingText("R2 = none\nRTRIGGER = BTN_A | AXIS_RT thresh=0.2\n",
                               &mapping, &errorLine),
          "a thresholded trigger parses");
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) != 0,
          "the trigger drives the throttle button");
    checkNear(guest.axes[gp::kGuestRT], 1.0f, "the trigger drives the axis too");

    // A TRIGGER THAT IS BARELY PULLED MUST NOT COUNT, AT A HIGH THRESHOLD.
    gp::NpadState barely;
    barely.zr = true;
    barely.rt = 0.1f;
    gp::applyMapping(mapping, gp::canonicalFromNpad(barely), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) == 0,
          "a trigger below the threshold does not fire the button");
    checkNear(guest.axes[gp::kGuestRT], 0.1f,
              "but the axis still carries the value");

    // NONE REMOVES AN INPUT.
    check(gp::parseMappingText("R2 = none\n", &mapping, &errorLine),
          "none parses");
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) == 0, "none removes the binding");
    check((guest.buttons & (1u << gp::kGuestR2)) == 0, "none removes both targets");

    // SCALE AND INVERT, ON AN AXIS.
    check(gp::parseMappingText("LSTICK_X = AXIS_LX scale=-1\n", &mapping, &errorLine),
          "a negative scale parses");
    gp::NpadState stick;
    stick.lx = 0.5f;
    gp::applyMapping(mapping, gp::canonicalFromNpad(stick), &guest);
    check(guest.axes[gp::kGuestLX] == -0.5f, "the scale is applied");
    check(gp::parseMappingText("LSTICK_X = AXIS_LX invert=1\n", &mapping, &errorLine),
          "invert parses");
    gp::applyMapping(mapping, gp::canonicalFromNpad(stick), &guest);
    check(guest.axes[gp::kGuestLX] == -0.5f, "the inversion is applied");

    // A BAD FILE LEAVES THE CALLER'S MAPPING ALONE.
    gp::Mapping kept;
    gp::mappingSetDefaults(&kept);
    const int before = kept.bindingCount;
    check(!gp::parseMappingText("A = NOT_A_CONTROL\n", &kept, &errorLine),
          "an unknown target is refused");
    check(errorLine == 1, "the line of the unknown target is reported");
    check(kept.bindingCount == before, "a refused file leaves the mapping alone");
    check(!gp::parseMappingText("this line has no equals sign\n", &kept, &errorLine),
          "a line with no '=' is refused");
    check(!gp::parseMappingText("A =\n", &kept, &errorLine),
          "a line with no target is refused");
    check(!gp::parseMappingText("NOT_AN_INPUT = BTN_A\n", &kept, &errorLine),
          "an unknown input is refused");
    check(!gp::parseMappingText("A = none | BTN_A\n", &kept, &errorLine),
          "none may not be mixed with a real target");

    // A FILE IS AUTHORITATIVE FOR THE INPUTS IT MENTIONS, AND THE DEFAULTS
    // SURVIVE FOR THE ONES IT DOES NOT.
    //
    // The state here presses ONLY the A button, so that the R2 binding does
    // not also reach BTN_A and blur what the check is about.
    check(gp::parseMappingText("A = BTN_B\n", &mapping, &errorLine),
          "a single binding parses");
    gp::NpadState onlyA;
    onlyA.a = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(onlyA), &guest);
    check((guest.buttons & (1u << gp::kGuestA)) == 0,
          "the mentioned input lost its default");
    check((guest.buttons & (1u << gp::kGuestB)) != 0, "the new target fires");
    n = gp::NpadState{};
    n.stickL = true;
    gp::applyMapping(mapping, gp::canonicalFromNpad(n), &guest);
    check((guest.buttons & (1u << gp::kGuestStart)) != 0,
          "an unmentioned input keeps its default");
}

// ---------------------------------------------------------------------------
// 5. WHO IS HOLDING WHAT
// ---------------------------------------------------------------------------
static void testWhoIsHoldingWhat() {
    gp::Mapping mapping;
    gp::mappingSetDefaults(&mapping);
    gp::NpadState n;
    n.zr = true;
    n.rt = 1.0f;
    const gp::State s = gp::canonicalFromNpad(n);
    check(gp::mappingGuestButtonActive(mapping, s, gp::kGuestA),
          "the throttle is being held");
    check(!gp::mappingGuestButtonActive(mapping, s, gp::kGuestX),
          "the brake is not being held");
    check(gp::mappingGuestAxisMagnitude(mapping, s, gp::kGuestLX) == 0.0f,
          "nobody is steering");

    gp::NpadState stick;
    stick.lx = 0.4f;
    const gp::State steering = gp::canonicalFromNpad(stick);
    check(gp::mappingGuestAxisMagnitude(mapping, steering, gp::kGuestLX) > 0.0f,
          "the stick is steering");
    check(gp::mappingGuestAxisMagnitude(mapping, s, gp::kGuestRT) > 0.0f,
          "the trigger is on the right trigger axis");
}

// ---------------------------------------------------------------------------
// 6. THE SONY REPORTS
// ---------------------------------------------------------------------------
//
// The layouts are the Linux kernel's (drivers/hid/hid-playstation.c):
//
//   struct dualshock4_input_report_common { u8 x, y; u8 rx, ry;
//                                           u8 buttons[3]; u8 z, rz; };
//   struct dualsense_input_report       { u8 x, y; u8 rx, ry;
//                                           u8 z, rz; u8 seq; u8 buttons[4]; };
//
// and the button bits are the kernel's:
//
//   buttons[0]  0..1 the hat, 4 square, 5 cross, 6 circle, 7 triangle
//   buttons[1]  0 L1, 1 R1, 2 L2, 3 R2, 4 create, 5 options, 6 L3, 7 R3
//   buttons[2]  0 PS, 1 touchpad, 2 mic mute
static void testSony() {
    // THE HAT, ALL NINE POSITIONS.
    struct HatCase {
        uint8_t value;
        uint32_t expect;
    };
    const HatCase kHatCases[] = {
        {0, 1u << gp::kInDpadUp},
        {1, (1u << gp::kInDpadUp) | (1u << gp::kInDpadRight)},
        {2, 1u << gp::kInDpadRight},
        {3, (1u << gp::kInDpadDown) | (1u << gp::kInDpadRight)},
        {4, 1u << gp::kInDpadDown},
        {5, (1u << gp::kInDpadDown) | (1u << gp::kInDpadLeft)},
        {6, 1u << gp::kInDpadLeft},
        {7, (1u << gp::kInDpadUp) | (1u << gp::kInDpadLeft)},
        {8, 0u},
    };
    for (const HatCase& entry : kHatCases) {
        uint32_t bits = 0;
        gp::applyHat(entry.value, &bits);
        check(bits == entry.expect, "the hat direction is decoded");
    }

    // A DUALSHOCK 4 USB REPORT: id 0x01, then the common part.
    uint8_t ds4[16];
    std::memset(ds4, 0, sizeof(ds4));
    ds4[0] = 0x01;          // report id
    ds4[1] = 128;           // x
    ds4[2] = 0;             // y  (up)
    ds4[3] = 128;           // rx
    ds4[4] = 255;           // ry (down)
    ds4[5] = 0x20 | 0x01;   // buttons[0]: cross + hat north
    ds4[6] = 0x09;         // buttons[1]: L1 + R2
    ds4[7] = 0x01;         // buttons[2]: PS
    ds4[8] = 255;          // z
    ds4[9] = 128;          // rz
    size_t offset = 0;
    check(gp::sonyCommonOffset(ds4[0], 64, &offset) && offset == 1,
          "the DS4 USB report's common part starts at 1");
    gp::State state;
    check(gp::parseDualShock4(ds4 + offset, 64 - offset, &state),
          "the DS4 report parses");
    check((state.buttons & (1u << gp::kInA)) != 0, "the DS4 cross is the A button");
    check((state.buttons & (1u << gp::kInL1)) != 0, "the DS4 L1 is L1");
    check((state.buttons & (1u << gp::kInR2)) != 0, "the DS4 R2 is R2");
    check((state.buttons & (1u << gp::kInHome)) != 0, "the DS4 PS button is HOME");
    check((state.buttons & (1u << gp::kInDpadUp)) != 0, "the DS4 hat is up");
    check(state.sticks[0] == 0.0f, "the DS4 stick X is centred");
    checkNear(state.sticks[1], 1.0f, "the DS4 stick Y is UP-positive");
    checkNear(state.sticks[3], -1.0f, "the DS4 right stick Y is UP-positive");
    checkNear(state.triggers[0], 1.0f, "the DS4 L2 is the left trigger");
    checkNear(state.triggers[1], 0.5f, "the DS4 R2 is the right trigger");

    // A DUALSENSE USB REPORT: id 0x01, then the common part.
    uint8_t dualsense[16];
    std::memset(dualsense, 0, sizeof(dualsense));
    dualsense[0] = 0x01;       // report id
    dualsense[1] = 128;        // x
    dualsense[2] = 255;        // y (down)
    dualsense[3] = 128;        // rx
    dualsense[4] = 0;         // ry (up)
    dualsense[5] = 200;        // z
    dualsense[6] = 100;        // rz
    dualsense[7] = 0;          // seq
    dualsense[8] = 0x40 | 0x02; // buttons[0]: circle + hat east
    dualsense[9] = 0x20;       // buttons[1]: options
    dualsense[10] = 0x00;      // buttons[2]
    check(gp::sonyCommonOffset(dualsense[0], 64, &offset) && offset == 1,
          "the DualSense report's common part starts at 1");
    check(gp::parseDualSense(dualsense + offset, 64 - offset, &state),
          "the DualSense report parses");
    check((state.buttons & (1u << gp::kInB)) != 0, "the DualSense circle is the B button");
    check((state.buttons & (1u << gp::kInStart)) != 0, "the DualSense options is START");
    check((state.buttons & (1u << gp::kInDpadRight)) != 0, "the DualSense hat is right");
    checkNear(state.sticks[1], -1.0f, "the DualSense stick Y is UP-positive");
    checkNear(state.sticks[3], 1.0f, "the DualSense right stick Y is UP-positive");
    checkNear(state.triggers[0], 0.784f, "the DualSense L2 is the left trigger");
    checkNear(state.triggers[1], 0.392f, "the DualSense R2 is the right trigger");

    // A BAD LENGTH IS REFUSED RATHER THAN READ PAST.
    check(!gp::parseDualShock4(ds4 + 1, 3, &state), "a short DS4 report is refused");
    check(!gp::parseDualSense(dualsense + 1, 3, &state), "a short DualSense report is refused");
}

// ---------------------------------------------------------------------------
// 7. THE XBOX REPORT
// ---------------------------------------------------------------------------
//
// The 0x20 report, 20 bytes: the 360 pad, the One pad, the Series pad and
// both Elite pads all speak it.
//
//   byte 0  the d-pad and the meta buttons
//   byte 1  the left trigger,  byte 2 the right trigger
//   byte 3  LX, 4 LY, 5 RX, 6 RY
//   byte 7  A B X Y LB RB GUIDE MENU
static void testXbox() {
    uint8_t report[20];
    std::memset(report, 0, sizeof(report));
    report[0] = 0x01 | 0x10 | 0x40;  // up, start, left stick
    report[1] = 0;                   // left trigger
    report[2] = 255;                 // right trigger
    report[3] = 255;                 // LX (right)
    report[4] = 0;                   // LY (up)
    report[5] = 128;                 // RX (centred)
    report[6] = 255;                 // RY (down)
    report[7] = 0x01 | 0x20;         // A and RB (bit 5 -- bit 4 is LB)

    gp::State state;
    check(gp::parseXboxReport(report, 20, &state), "the Xbox report parses");
    check((state.buttons & (1u << gp::kInA)) != 0, "the Xbox A is the A button");
    check((state.buttons & (1u << gp::kInR1)) != 0, "the Xbox RB is R1");
    check((state.buttons & (1u << gp::kInDpadUp)) != 0, "the Xbox d-pad is up");
    check((state.buttons & (1u << gp::kInStart)) != 0, "the Xbox start is START");
    check((state.buttons & (1u << gp::kInL3)) != 0, "the Xbox L3 is L3");
    checkNear(state.triggers[1], 1.0f, "the Xbox right trigger is 1.0");
    checkNear(state.sticks[0], 1.0f, "the Xbox stick X is right-positive");
    checkNear(state.sticks[1], 1.0f, "the Xbox stick Y is UP-positive");
    checkNear(state.sticks[3], -1.0f, "the Xbox right stick Y is UP-positive");
    check(!gp::parseXboxReport(report, 4, &state), "a short Xbox report is refused");
}

// ---------------------------------------------------------------------------
// 8. THE CARD'S FILE AND THE DEFAULTS AGREE
// ---------------------------------------------------------------------------
//
// The file that ships is read here, because a file that says one thing and
// the code does another is a file that lies.
static void testCardFile(const char* path) {
    if (path == nullptr) {
        std::printf("SKIP: no card file was given on the command line\n");
        return;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        check(false, "the card file could be opened");
        return;
    }
    char text[16384];
    const size_t read = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[read] = '\0';

    gp::Mapping fromFile;
    int errorLine = 0;
    check(gp::parseMappingText(text, &fromFile, &errorLine),
          "the card file parses");
    if (errorLine != 0) {
        std::printf("FAIL: the card file has a problem on line %d\n", errorLine);
    }

    gp::Mapping defaults;
    gp::mappingSetDefaults(&defaults);

    // THE EFFECT HAS TO BE THE SAME, FOR EVERY INPUT.
    //
    // The check is on the GUEST STATE, not on the binding list: two
    // mappings that differ in bookkeeping but agree on what the guest
    // receives are the same mapping as far as the game is concerned.
    const uint32_t kButtonCases[] = {
        1u << gp::kInA, 1u << gp::kInB, 1u << gp::kInX, 1u << gp::kInY,
        1u << gp::kInL1, 1u << gp::kInR1, 1u << gp::kInL2, 1u << gp::kInR2,
        1u << gp::kInSelect, 1u << gp::kInStart, 1u << gp::kInHome,
        1u << gp::kInDpadUp, 1u << gp::kInDpadDown, 1u << gp::kInDpadLeft,
        1u << gp::kInDpadRight, 1u << gp::kInL3, 1u << gp::kInR3,
    };
    for (uint32_t mask : kButtonCases) {
        gp::State state;
        state.buttons = mask;
        gp::GuestState a;
        gp::GuestState b;
        gp::applyMapping(defaults, state, &a);
        gp::applyMapping(fromFile, state, &b);
        check(a.buttons == b.buttons, "the card file binds the buttons like the defaults");
    }

    // AND THE AXES, AT SEVERAL MAGNITUDES.
    const float kAxisValues[] = {-1.0f, -0.5f, -0.1f, 0.0f, 0.1f, 0.5f, 1.0f};
    for (float value : kAxisValues) {
        gp::State state;
        state.sticks[0] = value;
        state.sticks[1] = value;
        state.sticks[2] = value;
        state.sticks[3] = value;
        state.triggers[0] = value < 0.0f ? 0.0f : value;
        state.triggers[1] = value < 0.0f ? 0.0f : value;
        gp::GuestState a;
        gp::GuestState b;
        gp::applyMapping(defaults, state, &a);
        gp::applyMapping(fromFile, state, &b);
        bool same = true;
        for (int i = 0; i < gp::kGuestAxisCount; ++i) {
            if (a.axes[i] != b.axes[i]) {
                same = false;
            }
        }
        check(same, "the card file maps the axes like the defaults");
    }

    // THE MODE THE CARD SHIPS IS THE ONE THE PORT HAS BEEN MEASURED WITH.
    //
    // This assertion TRACKS THE SHIPPING DECISION, so it has to move when
    // that decision moves.  It was kAutopilotAuto until 2026-11-04, when
    // the shipped default became kAutopilotManual: the card's own file said
    // "autopilot = 0" while this assertion still demanded "autopilot = 1",
    // and the gate caught the disagreement.
    //
    // MEASURED 2026-11-04 (q149): with the port's own closed loop out of the
    // way, the player's input reaches the car end to end, so MANUAL is the
    // mode the port has been measured with.  Set the card file to
    // "autopilot = 1" for the attract mode, and this assertion has to
    // follow it back.
    check(fromFile.autopilot == gp::kAutopilotManual,
          "the card file ships the measured autopilot mode");
}

int main(int argc, char** argv) {
    testNames();
    testCanonical();
    testDefaults();
    testParser();
    testWhoIsHoldingWhat();
    testSony();
    testXbox();
    testCardFile(argc > 1 ? argv[1] : nullptr);

    if (g_failures == 0) {
        std::printf("PASS: %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
