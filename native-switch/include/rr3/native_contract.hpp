#pragma once

#include <cstdint>

namespace rr3 {

// Opaque JNI-compatible handles. The rehost supplies its own small function table;
// no Android VM is embedded.
using JniEnv = void*;
using JniObject = void*;

struct NativeExports {
    using LifecycleFn = void (*)(JniEnv, JniObject);
    using ViewChangedFn = void (*)(JniEnv, JniObject, int32_t, int32_t, int32_t, int32_t);
    using ViewRenderFn = void (*)(JniEnv, JniObject, int32_t, int32_t);
    using ControllerConnectedFn = void (*)(JniEnv, JniObject, void*, void*, int32_t, int32_t);
    using ControllerDisconnectedFn = void (*)(JniEnv, JniObject, int32_t);
    using ButtonFn = void (*)(JniEnv, JniObject, int32_t, bool, int32_t);
    using JoystickFn = void (*)(JniEnv, JniObject, int32_t, float, int32_t);

    LifecycleFn onCreate{};
    LifecycleFn onStart{};
    LifecycleFn onResume{};
    LifecycleFn onPause{};
    LifecycleFn onStop{};
    LifecycleFn onDestroy{};
    LifecycleFn onViewCreated{};
    ViewChangedFn onViewChanged{};
    ViewRenderFn onViewRender{};
    ControllerConnectedFn controllerConnected{};
    ControllerDisconnectedFn controllerDisconnected{};
    ButtonFn setButton{};
    JoystickFn setJoystick{};
};

enum class RehostStage : uint8_t {
    PlatformShell,
    ElfMapped,
    ImportsResolved,
    JniReady,
    GraphicsReady,
    GameBooted,
};

} // namespace rr3

