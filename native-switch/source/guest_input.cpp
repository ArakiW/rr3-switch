// The Switch controller -> the guest's ControllerManager.
//
// TWO THINGS THIS FILE DECIDES, and both of them are about the TRIGGER.
//
// 1. WHAT A TRIGGER IS, AND WHAT IT IS WORTH.
//
//    THE GUEST'S CONVENTION. The guest's own Java
//    (reverse/java/sources/com/firemint/realracing/ControllerManager.java)
//    reads an Android trigger as a MotionEvent axis and hands it straight to
//    the guest: SetJoystickValues(id, l, AXIS_LTRIGGER) and
//    SetJoystickValues(id, r, AXIS_RTRIGGER) (ControllerManager.java:491-492),
//    which is SetJoystickValueJNI(id, value, ordinal) with the ordinals 4 and
//    5 of the ControllerAxis enum. Android normalises a one-sided trigger
//    axis to 0..1 with 1.0 = fully pressed, and the guest passes the value
//    through unchanged. So the port sends 0..1 too: 0.0 is released, 1.0 is
//    fully pressed, and anything between is a partial press.
//
//    WHAT libnx ACTUALLY EXPOSES. There is NO analogue trigger in the
//    ordinary Npad state: HidNpadCommonState (hid.h:1072) has no trigger
//    field at all, so on a Pro Controller ZL and ZR are plain digital
//    buttons. The only analogue trigger libnx can deliver is the GameCube
//    one: PadState.gc_triggers[] (runtime/pad.h:25), raw 0x0..0x7FFF
//    (hid.h:1094-1095), read with padGetGcTriggerPos().
//
//    WHY THE PORT HAD NONE. libnx's padUpdate() fills gc_triggers only on
//    the branch where the controller reports HidNpadStyleTag_NpadGc (libnx
//    pad.o, padUpdate: "tbnz w20, #5" on the style set returned by
//    hidGetNpadStyleSet). The Gc style has to be declared to the HID
//    service first, and padConfigureInput() is the only supported way to do
//    that: it ends in hidSetSupportedNpadStyleSet(style_set) (libnx pad.o,
//    padConfigureInput). The port declared HidNpadStyleSet_NpadStandard,
//    which does NOT contain NpadGc (hid.h:245: FullKey|Handheld|JoyDual|
//    JoyLeft|JoyRight), so the analogue trigger could never arrive and the
//    port sent ZL/ZR as 1.0/0.0 -- all or nothing.
//
//    So the trigger is now read as: the analogue value when the controller
//    actually has one, and the digital ZL/ZR bit otherwise. A Pro Controller
//    gets the digital reading (it has no analogue trigger), a GameCube
//    Controller gets the analogue one, and neither loses the button.
//
// 2. THE SCRIPTED PAD, which is how the experiments are driven.
//
//    The script lives on the card at sdmc:/switch/rr3-native-rehost/rr3-pad.txt
//    and its format is:
//
//        [at <frame>] lx=<f> ly=<f> rx=<f> ry=<f> lt=<f> rt=<f> b=<hex>
//
//    EVERY KEY IS OPTIONAL and a key that is absent keeps the value it had
//    before, so a script is a list of states over time rather than a list of
//    complete snapshots. `at <frame>` is the PORT'S frame counter (the same
//    counter the touch script uses), never a wall clock. The axis keys are
//    the guest's own ControllerAxis ordinals, in the guest's own units:
//    lx/ly = AXIS_LTHUMB_X/Y (0/1), rx/ry = AXIS_RTHUMB_X/Y (2/3),
//    lt/rt = AXIS_LTRIGGER/AXIS_RTRIGGER (4/5). The sticks are signed
//    (-1..1); the triggers are 0..1, 1.0 = fully pressed. `b` is a HEX
//    bitmask over the guest's ControllerButtons ORDINALS (bit 0 = BTN_A,
//    bit 2 = BTN_X, bit 6 = BTN_L2, bit 7 = BTN_R2, bit 8 = BTN_DPAD_LEFT),
//    which is the guest's own numbering, not the Switch's.
//
//    THE LAST LINE WHOSE "at" FRAME IS <= THE CURRENT FRAME APPLIES. A
//    script with no line at or before the current frame does not apply, and
//    the physical controller is used instead.
//
//    The format and the parser that implements it are pinned by
//    native-switch/tests/test_guest_pad_script_format.py, so the format
//    documented here is the format that parses.

#include "rr3/guest_input.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_action.hpp"
#include "rr3/guest_input_doors.hpp"
#include "rr3/guest_touch.hpp"
#include "rr3/guest_touch_hw.hpp"
#include "rr3/race_state_probe.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/ui_control_probe.hpp"

#include <switch.h>

#include <cstdio>

namespace rr3 {
namespace {

// ControllerManager.ControllerButtons ordinals (Java enum order).
enum GuestButton : int {
    kBtnA = 0,
    kBtnB = 1,
    kBtnX = 2,
    kBtnY = 3,
    kBtnL1 = 4,
    kBtnR1 = 5,
    kBtnL2 = 6,
    kBtnR2 = 7,
    kBtnDpadLeft = 8,
    kBtnDpadRight = 9,
    kBtnDpadUp = 10,
    kBtnDpadDown = 11,
    kBtnSelect = 12,
    kBtnStart = 13,
    kBtnDpadCenter = 14,
};

enum GuestAxis : int {
    kAxisLThumbX = 0,
    kAxisLThumbY = 1,
    kAxisRThumbX = 2,
    kAxisRThumbY = 3,
    kAxisLTrigger = 4,
    kAxisRTrigger = 5,
};

// The Npad style set the port declares to the HID service.
//
// The GameCube-controller style is the ONLY style that carries an analogue
// trigger, so it has to be declared for the analogue value to arrive at all
// (libnx's padConfigureInput ends in hidSetSupportedNpadStyleSet, and
// padUpdate reads gc_triggers only where the controller reports NpadGc).
// The standard styles are kept because they are what the port's sticks and
// buttons come from.
constexpr u32 kNpadStyleSet =
    HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc;

// The raw analogue trigger is 0x0..0x7FFF (hid.h:1094-1095).
constexpr float kGcTriggerFull = 32767.0f;

// Java: void SetButtonValueJNI(int id, boolean pressed, int ordinal)
using SetButtonFn = void (*)(void* env, void* self, int id, int pressed,
                             int ordinal);
// Java: void SetJoystickValueJNI(int id, float value, int axisOrdinal)
using SetJoystickFn = void (*)(void* env, void* self, int id, float value,
                               int axis);
// Java: void ControllerConnectedJNI(String name, String desc, int id, int flags)
using ControllerConnectedFn = void (*)(void* env, void* self, void* name,
                                       void* desc, int id, int flags);

SetButtonFn g_setButton = nullptr;
SetJoystickFn g_setJoystick = nullptr;
ControllerConnectedFn g_connected = nullptr;
bool g_ready = false;
bool g_connectedIssued = false;
const char* g_progressPath = nullptr;
float g_lastLx = 0, g_lastLy = 0, g_lastRx = 0, g_lastRy = 0;
float g_lastLt = 0, g_lastRt = 0;
bool g_lastFace[8] = {};
bool g_lastDpad[4] = {};
bool g_lastMeta[3] = {};

// What the port last handed to the guest, in the guest's own numbering.
//
// This is the instrument's left-hand side: it says what the guest was TOLD,
// so the read-back of the guest's own storage can be compared against it. Only
// values that were actually pushed are recorded, so "pushed" and "the guest
// holds" mean the same thing when they agree.
GuestPushedInput g_pushed = {};
// The port's own frame counter. The frame loop pumps input once per
// iteration, just before the guest's render call, so this tracks the guest's
// frame number to within one.
unsigned long long g_frameCounter = 0;
// The last button state pushed per guest ordinal, for the scripted pad. The
// physical path keeps its own per-button state in g_lastFace/g_lastDpad/
// g_lastMeta, which are libnx masks; this one is a guest ordinal.
bool g_lastOrdinal[15] = {};

void** jniTable(void* env) {
    return static_cast<void**>(*static_cast<void**>(env));
}

void emitButton(void* env, void* self, int id, uint64_t held, uint64_t mask,
                int ordinal, bool* last) {
    const bool now = (held & mask) != 0;
    if (now == *last) {
        return;
    }
    *last = now;
    if (g_setButton != nullptr) {
        g_setButton(env, self, id, now ? 1 : 0, ordinal);
        if (now) {
            g_pushed.buttons |= 1u << ordinal;
        } else {
            g_pushed.buttons &= ~(1u << ordinal);
        }
        ++g_pushed.pushes;
    }
}

// One button, named by the guest's ControllerButtons ordinal. The scripted
// pad speaks ordinals (that is the guest's own numbering and the bit index
// in the script's mask), while the physical path speaks libnx masks, so the
// two meet here.
void emitOrdinalButton(void* env, void* self, int id, int ordinal, bool pressed,
                       bool* last) {
    if (pressed == *last) {
        return;
    }
    *last = pressed;
    if (g_setButton != nullptr) {
        g_setButton(env, self, id, pressed ? 1 : 0, ordinal);
        if (pressed) {
            g_pushed.buttons |= 1u << ordinal;
        } else {
            g_pushed.buttons &= ~(1u << ordinal);
        }
        ++g_pushed.pushes;
    }
}

void emitAxis(void* env, void* self, int id, float value, int axis,
              float* last) {
    if (value > *last - 0.01f && value < *last + 0.01f) {
        return;
    }
    *last = value;
    if (g_setJoystick != nullptr) {
        g_setJoystick(env, self, id, value, axis);
        g_pushed.axes[axis] = value;
        ++g_pushed.pushes;
    }
}

// The trigger value, in the guest's units (0..1, 1.0 = fully pressed).
//
// `index` is 0 for the left trigger and 1 for the right, which is the
// order of PadState.gc_triggers[].
//
// The analogue value is used when the controller reports one. It is not
// simply "greater than zero": a GameCube Controller reports an analogue
// trigger only while it is pressed, so zero means released, and the
// digital ZL/ZR bit is then the same thing. The digital bit is also what a
// Pro Controller has and all it has, so it stays the fallback -- dropping
// it would take the trigger away from every ordinary controller.
float triggerValue(const PadState* pad, uint64_t held, uint64_t digitalMask,
                   unsigned index) {
    const u32 raw = padGetGcTriggerPos(pad, index);
    if (raw > 0) {
        float value = static_cast<float>(raw) / kGcTriggerFull;
        return value > 1.0f ? 1.0f : value;
    }
    return (held & digitalMask) != 0 ? 1.0f : 0.0f;
}

// The one instrument that says what the HID service actually reports.
//
// It answers a question the source cannot: whether the controller at
// HidNpadIdType_No1 reports the GameCube style (which is what makes
// padUpdate fill gc_triggers at all), and what the analogue trigger
// reads. Without it, "the analogue trigger is zero" and "the analogue
// trigger is not being delivered" are the same observation.
void logTriggerState(unsigned long long frame, const PadState* pad,
                     uint64_t held) {
    static unsigned long long s_lastLogged = 0;
    if (frame < s_lastLogged + 300ULL) {
        return;
    }
    s_lastLogged = frame;
    const u32 controllerStyle = hidGetNpadStyleSet(HidNpadIdType_No1);
    char detail[224];
    std::snprintf(
        detail, sizeof(detail),
        "f=%llu controller_style=0x%08x gc_bit=%u supported_style=0x%08x "
        "gc_l=0x%05x gc_r=0x%05x buttons=0x%016llx",
        frame, controllerStyle,
        (controllerStyle & HidNpadStyleTag_NpadGc) != 0 ? 1u : 0u,
        padGetStyleSet(pad), padGetGcTriggerPos(pad, 0),
        padGetGcTriggerPos(pad, 1),
        static_cast<unsigned long long>(held));
    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log", "pad-trigger", 947, 0,
        detail);
}

} // namespace

bool prepareGuestControllerInput() {
    // Declare the Npad style set the HID service should support.
    //
    // The port declares HidNpadStyleSet_NpadStandard at start-up, which does
    // NOT contain HidNpadStyleTag_NpadGc, and the GameCube style is the only
    // one that carries an analogue trigger. libnx's padConfigureInput() is
    // the only supported way to declare a style set (it ends in
    // hidSetSupportedNpadStyleSet), so it is called again here with the Gc
    // bit added.
    //
    // This runs BEFORE the first padUpdate (the frame loop pumps the pad
    // long after the guest is up) and it re-declares the same Npad id types,
    // so the only thing that changes is the added style bit.
    //
    // It is also the only way the analogue trigger can be CHECKED: the
    // pad-trigger line in the guest-view log records what the controller
    // actually reports, so "the analogue trigger is zero" and "the analogue
    // trigger is not being delivered" stop being the same observation.
    padConfigureInput(1, kNpadStyleSet);
    g_setButton = reinterpret_cast<SetButtonFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI"));
    g_setJoystick = reinterpret_cast<SetJoystickFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI"));
    g_connected = reinterpret_cast<ControllerConnectedFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI"));
    g_ready = g_setButton != nullptr && g_setJoystick != nullptr &&
              g_connected != nullptr;
    g_connectedIssued = false;
    // The touch path is a DIFFERENT set of guest exports
    // (MainActivity.onTouch{Begin,Move,End}JNI), so it is resolved here but
    // its readiness does not gate the controller. It is OFF by default: with
    // no control file on the card, guest_touch.cpp never calls the guest.
    prepareGuestTouchInput();
    // The hardware touchscreen is a THIRD path: the console's own digitizer,
    // read through libnx, turned into the same guest calls. It is ON by
    // default -- a touchscreen that needs a switch flipped is not a
    // touchscreen -- and it stands down on its own while the file control
    // owns the guest's touch path.
    prepareGuestTouchHardware();
    return g_ready;
}

void setGuestInputProgressPath(const char* path) {
    g_progressPath = path;
}

bool guestControllerInputReady() {
    return g_ready;
}

bool connectGuestController(void* jniEnvironment, void* activityObject,
                            int deviceId) {
    if (!g_ready || jniEnvironment == nullptr || g_connectedIssued) {
        return g_connectedIssued;
    }
    void** table = jniTable(jniEnvironment);
    if (table == nullptr || table[167] == nullptr) {
        return false;
    }
    auto newString = reinterpret_cast<void* (*)(void*, const char*)>(table[167]);
    void* name = newString(jniEnvironment, "Nintendo Switch Pro Controller");
    // The description below is NOT the key the guest's profile matcher uses.
    // Measured, not guessed:
    //
    //  * The guest keys a joystick profile on the device DESCRIPTION, and the
    //    sections it looks for are the names in the guest's own
    //    files/.depot/joystick_config.txt.
    //  * The guest logs the description it matched on, and it logs it EMPTY:
    //      "Cannot find description for device:  using default:
    //       [Android Gamepad Extended]"
    //    (seen in every archive under .build/eden-runs, e.g.
    //     20261024-134104-eden-thr-btnA).
    //  * Passing a description that IS a section of that file
    //    ("vendor:3727 product:3") changes nothing: the same empty
    //    description and the same fallback profile are logged
    //    (archive 20261024-142544-eden-desc-axis5).
    //  * The reason is in the guest's own code: ControllerConnectedJNI
    //    (libRealRacing3.so, ELF 0x12aa450) creates the joystick manager
    //    first (0x12aa4dc) and only afterwards hands the name and the
    //    description to the controller (setDeviceInfo, 0x12aa514). The
    //    profile is chosen during the creation, from the manager's own
    //    device description, so the description this port passes arrives
    //    too late to influence it.
    //
    // So the fallback profile is the one that is in force, and it is a
    // complete gamepad profile:
    //
    //   [Android Gamepad Extended]
    //     accel  = button 0 (A) | axis 5 (RT) | button 7 (R2)
    //     brake  = button 2 (X) | axis 4 (LT) | button 6 (L2)
    //     steering = axis 0, steering_left/right = button 8/9
    //
    // and the throttle that drives the car is BUTTON 0 (the A button),
    // measured in 20261024-134104-eden-thr-btnA: with button 0 held the
    // guest's own frames show the car accelerating down the front straight
    // with the exhaust flame lit, and with button 7 (R2) alone the guest
    // draws its own "hold the brake to reverse" message at frame 1991
    // (20261024-135257-eden-thr-btnR2), i.e. the car had stopped.
    //
    // The port therefore keeps its own description: there is no profile for
    // it to select, and the fallback profile already binds every control the
    // port can deliver.
    void* desc = newString(jniEnvironment, "switch-joycon-pair");
    constexpr int kAllButtonFlags =
        1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512;
    g_connected(jniEnvironment, activityObject, name, desc, deviceId,
                kAllButtonFlags);
    g_connectedIssued = true;
    if (g_progressPath != nullptr) {
        writeGuestBootProgress(g_progressPath, "guest-controller-connected", 72,
                               static_cast<unsigned long long>(deviceId));
    }
    return true;
}

void pumpGuestControllerInput(void* jniEnvironment, void* activityObject,
                              GuestPadHandle* padOpaque, int deviceId) {
    // The touch pump runs BEFORE the controller's early-out: the touch
    // exports are a different set of guest entry points and the control file
    // has to work on its own. It is called from the same frame loop that
    // calls the guest's render entry point, which is the same relationship
    // the guest's own GLView.queueEvent() has on Android.
    pumpGuestTouchInput(jniEnvironment, activityObject);
    // The hardware touchscreen, right after it and on the same thread. The
    // file control owns the guest's touch path while its file is on the
    // card, and the hardware path stands down for it -- so the two never
    // inject in the same frame.
    pumpGuestTouchHardware(jniEnvironment, activityObject);
    // The guest's OWN action commands run here, and for the same reason the
    // input is pumped here: this is the frame-loop thread, just before the
    // guest's render call, so the guest's state is consistent and the call
    // is in the same place the port already calls into the guest.
    //
    // It is BEFORE the controller's early-out on purpose. A command is
    // about the GAME, not about the controller, and a command that waits
    // for a controller to be connected is a command that never runs in a
    // run where the port is driving the input itself.
    pumpGuestActions(g_frameCounter);
    if (!g_ready || jniEnvironment == nullptr || padOpaque == nullptr) {
        return;
    }
    auto* pad = static_cast<PadState*>(padOpaque);
    const uint64_t held = padGetButtons(pad);

    // The scripted pad. It exists because a real controller cannot be
    // reproduced inside an emulator, and the experiment needs the steering
    // input to be exactly the same in every run. With no control file on the
    // card this is off and the physical path below is untouched.
    VirtualPadState scripted{};
    const bool scriptedActive = virtualPadForFrame(g_frameCounter, &scripted);

    // THE CLOSED LOOP.
    //
    // The race-state probe reads the guest's own race state out of guest
    // memory (see source/race_state_probe.cpp) and computes the steering the
    // car needs. When it is enabled it is the SINGLE authority for the
    // throttle, the brake and the steering axis: the script and the physical
    // pad are folded into the same state first and then overridden once, so
    // the guest never sees a control change twice in one frame.
    //
    // It is OFF unless the control file
    // sdmc:/switch/rr3-native-rehost/rr3-drive.txt exists and says
    // enable=1, so every other experiment in this tree is unaffected.
    pumpRaceStateProbe(g_frameCounter);

    // THE SEMANTIC UI PROBE.
    //
    // It reads the guest's OWN screen tree, so the port can ask WHERE a
    // button is by the id the guest gives it ("BTN_RACE_NOW") instead of
    // hunting for an accent-coloured blob.  It is pumped HERE, beside the
    // race-state probe, for the same reason: the probe's walk is a graph
    // walk over the guest's objects, and doing that ON DEMAND from the frame
    // loop froze two runs (q23, q24).  The walk runs on its own cadence
    // inside the probe, and the frame loop reads only the cached sample.
    pumpUiControlProbe(g_frameCounter);

    // OPEN A SCREEN BY NAME.
    //
    // The guest's registry holds 59 screens and the guest has its own
    // OpenScreen(mgr, screen, flags) at ELF 0x14a4820.  The port already
    // knows each screen's INSTANCE by name (the probe walks the registry),
    // so the card can name a screen and the port opens it -- CarPurchase,
    // CarCustomisation, Upgrades, Community, MyGarage -- instead of the port
    // hunting for pixels that lead there.
    //
    // This is a CALL INTO the guest, so it is pumped from the input pump,
    // which is the guest's own thread, and it installs the guest's TLS.
    pumpUiScreenOpen(g_frameCounter);

    const bool driving = raceStateDriveEnabled();
    const RaceDriveState& drive = raceDriveState();

    if (driving) {
        scripted.leftX = drive.steer;
        // THE THROTTLE IS RELEASABLE.  The recovery has to reverse the car
        // off a barrier, and the guest's own message is "hold the brake to
        // reverse" -- which only reverses when the throttle is LET GO.
        // Holding both was just braking, so the recovery could never free
        // the car: measured on the guest's own controller state, button 0
        // (the throttle) was down in every single frame of the 2026-10-28
        // race and button 2 (the brake) was never pressed at all.
        if (drive.throttle) {
            scripted.buttons |= (1u << kBtnA);
        } else {
            scripted.buttons &= ~(1u << kBtnA);
        }
        if (drive.brake) {
            scripted.buttons |= (1u << kBtnX);
        } else {
            scripted.buttons &= ~(1u << kBtnX);
        }
    }

    if (scriptedActive) {
        // The script drives the guest. The physical pad is ignored entirely
        // while it is active, so the two can never fight over one button.
        for (int i = 0; i < 15; ++i) {
            const bool pressed = (scripted.buttons >> i) & 1u;
            emitOrdinalButton(jniEnvironment, activityObject, deviceId, i,
                              pressed, &g_lastOrdinal[i]);
        }
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.leftX,
                 kAxisLThumbX, &g_lastLx);
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.leftY,
                 kAxisLThumbY, &g_lastLy);
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.rightX,
                 kAxisRThumbX, &g_lastRx);
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.rightY,
                 kAxisRThumbY, &g_lastRy);
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.leftTrigger,
                 kAxisLTrigger, &g_lastLt);
        emitAxis(jniEnvironment, activityObject, deviceId, scripted.rightTrigger,
                 kAxisRTrigger, &g_lastRt);
    } else {
    // ZR IS THE RIGHT TRIGGER, AND IT IS ALSO THE THROTTLE.
    //
    // The guest's own profile binds the accelerator to "button 0 (A) |
    // axis 5 (RT) | button 7 (R2)", and the port maps the Switch's ZR onto
    // button 7 -- which is what the profile's name says. But MEASURED, it
    // does not accelerate: with only button 7 held the guest draws its own
    // "hold the brake to reverse" message, which is the guest saying the
    // car is NOT moving. (Archive 20261024-135257-eden-thr-btnR2: the
    // port injected 0x80, the guest's own controller held button 7, and
    // the guest's own message said the car was standing still. The same
    // run with button 0 injected has no such message.)
    //
    // So on a real Switch the right trigger would be DEAD, which is no
    // good for a racing game: the trigger is where the throttle belongs.
    // ZR therefore also drives the guest's button 0, the accelerator that
    // is measured to work. The button 7 press is left alone: the profile
    // calls it the accelerator too, and it does no harm.
    static constexpr uint64_t kFaceMasks[] = {
        HidNpadButton_A | HidNpadButton_ZR, HidNpadButton_B, HidNpadButton_X,
        HidNpadButton_Y, HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL,
        HidNpadButton_ZR,
    };
    static constexpr int kFaceOrdinals[] = {
        kBtnA, kBtnB, kBtnX, kBtnY, kBtnL1, kBtnR1, kBtnL2, kBtnR2,
    };
    // The closed loop's overrides are folded into the held mask BEFORE any
    // button is pushed, so the guest sees exactly one state per frame.
    uint64_t effective = held;
    if (driving) {
        effective |= HidNpadButton_A;
        effective &= ~static_cast<uint64_t>(HidNpadButton_X);
        if (drive.brake) {
            effective |= HidNpadButton_X;
        }
    }
    for (int i = 0; i < 8; ++i) {
        emitButton(jniEnvironment, activityObject, deviceId, effective,
                   kFaceMasks[i], kFaceOrdinals[i], &g_lastFace[i]);
    }

    static constexpr uint64_t kDpadMasks[] = {
        HidNpadButton_Left,
        HidNpadButton_Right,
        HidNpadButton_Up,
        HidNpadButton_Down,
    };
    static constexpr int kDpadOrdinals[] = {
        kBtnDpadLeft,
        kBtnDpadRight,
        kBtnDpadUp,
        kBtnDpadDown,
    };
    for (int i = 0; i < 4; ++i) {
        emitButton(jniEnvironment, activityObject, deviceId, held,
                   kDpadMasks[i], kDpadOrdinals[i], &g_lastDpad[i]);
    }

    emitButton(jniEnvironment, activityObject, deviceId, held,
               HidNpadButton_Minus, kBtnSelect, &g_lastMeta[0]);
    // Stick clicks are common "start / menu" on Android pads. PLUS stays with
    // the frame-loop exit chord.
    emitButton(jniEnvironment, activityObject, deviceId, held,
               HidNpadButton_StickL, kBtnStart, &g_lastMeta[1]);
    emitButton(jniEnvironment, activityObject, deviceId, held,
               HidNpadButton_StickR, kBtnDpadCenter, &g_lastMeta[2]);

    const HidAnalogStickState left = padGetStickPos(pad, 0);
    const HidAnalogStickState right = padGetStickPos(pad, 1);
    auto norm = [](int raw) {
        const float v = static_cast<float>(raw) / 32767.0f;
        return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    };
    // The closed loop owns the steering axis when it is on.
    emitAxis(jniEnvironment, activityObject, deviceId,
             driving ? drive.steer : norm(left.x),
             kAxisLThumbX, &g_lastLx);
    emitAxis(jniEnvironment, activityObject, deviceId, -norm(left.y),
             kAxisLThumbY, &g_lastLy);
    emitAxis(jniEnvironment, activityObject, deviceId, norm(right.x),
             kAxisRThumbX, &g_lastRx);
    emitAxis(jniEnvironment, activityObject, deviceId, -norm(right.y),
             kAxisRThumbY, &g_lastRy);
    emitAxis(jniEnvironment, activityObject, deviceId,
             triggerValue(pad, held, HidNpadButton_ZL, 0), kAxisLTrigger,
             &g_lastLt);
    emitAxis(jniEnvironment, activityObject, deviceId,
             triggerValue(pad, held, HidNpadButton_ZR, 1), kAxisRTrigger,
             &g_lastRt);
    logTriggerState(g_frameCounter, pad, held);
    }

    ++g_frameCounter;
    // The read-back of the guest's own gamepad state. It is rate limited on
    // the far side, so calling it every frame costs one cheap check.
    probeGuestInputDoors(g_frameCounter, &g_pushed,
                          reinterpret_cast<uintptr_t>(g_setJoystick));
}

} // namespace rr3
