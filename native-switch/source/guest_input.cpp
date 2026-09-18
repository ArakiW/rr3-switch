#include "rr3/guest_input.hpp"

#include "rr3/elf_preflight.hpp"
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
    if (!g_ready || jniEnvironment == nullptr || padOpaque == nullptr) {
        return;
    }
    auto* pad = static_cast<PadState*>(padOpaque);
    const uint64_t held = padGetButtons(pad);

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

} // namespace rr3
