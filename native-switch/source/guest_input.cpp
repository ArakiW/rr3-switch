#include "rr3/guest_input.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_input_doors.hpp"
#include "rr3/guest_touch.hpp"
#include "rr3/guest_touch_hw.hpp"
#include "rr3/recovery_guard.hpp"

#include <switch.h>

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

} // namespace

bool prepareGuestControllerInput() {
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
    static constexpr uint64_t kFaceMasks[] = {
        HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
        HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR,
    };
    static constexpr int kFaceOrdinals[] = {
        kBtnA, kBtnB, kBtnX, kBtnY, kBtnL1, kBtnR1, kBtnL2, kBtnR2,
    };
    for (int i = 0; i < 8; ++i) {
        emitButton(jniEnvironment, activityObject, deviceId, held,
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
    emitAxis(jniEnvironment, activityObject, deviceId, norm(left.x),
             kAxisLThumbX, &g_lastLx);
    emitAxis(jniEnvironment, activityObject, deviceId, -norm(left.y),
             kAxisLThumbY, &g_lastLy);
    emitAxis(jniEnvironment, activityObject, deviceId, norm(right.x),
             kAxisRThumbX, &g_lastRx);
    emitAxis(jniEnvironment, activityObject, deviceId, -norm(right.y),
             kAxisRThumbY, &g_lastRy);
    emitAxis(jniEnvironment, activityObject, deviceId,
             (held & HidNpadButton_ZL) != 0 ? 1.0f : 0.0f, kAxisLTrigger,
             &g_lastLt);
    emitAxis(jniEnvironment, activityObject, deviceId,
             (held & HidNpadButton_ZR) != 0 ? 1.0f : 0.0f, kAxisRTrigger,
             &g_lastRt);
    }

    ++g_frameCounter;
    // The read-back of the guest's own gamepad state. It is rate limited on
    // the far side, so calling it every frame costs one cheap check.
    probeGuestInputDoors(g_frameCounter, &g_pushed,
                          reinterpret_cast<uintptr_t>(g_setJoystick));
}

} // namespace rr3
