// The Java side of csdk.glucentralservices.eventbus.ServerEventBus.
//
// The reasoning, the measurement and the decompiled-Java citations are in
// csdk_eventbus.h. This file is the code.
//
// WHAT IT DOES, AND WHAT IT DELIBERATELY DOES NOT DO
// ---------------------------------------------------
// The APK's class is (reverse/java/sources/csdk/glucentralservices/eventbus/
// ServerEventBus.java):
//
//   private static final ServerEventBus SHARED = createShared();
//   private final long mEventBus;
//   private final Object mGlobalToken;
//   private final long mMIDHandleEvent = getMIDHandleEvent();
//
//   private static ServerEventBus createShared() {
//       try { System.loadLibrary("glucentralservices"); }
//       catch (UnsatisfiedLinkError unused) { }
//       return new ServerEventBus(shared());
//   }
//
//   private ServerEventBus(long j) {
//       this.mEventBus = j;
//       this.mGlobalToken = tokenFromNativeToken(globalToken(j));
//   }
//
// Every one of those three calls is `private static native`, so the whole
// Java-visible behaviour of the class is: call into the guest, three times.
//
// This file performs exactly those three calls, against the guest's own
// registered natives, and prints what the guest returned.
//
// It does NOT publish anything, and it does NOT invent a config. The
// remaining six natives (subscribe, unsubscribe, publish, publishError and
// their companions) are called BY the Java app in the real game -- the app
// subscribes to the bus and publishes events into it -- and the port has no
// app to do that. Fabricating a subscription or a published payload would be
// exactly the "fake that silently pretends to work" this port must not ship:
// the numbers would look right and the guest's state would be fiction.

#include "csdk_eventbus.h"

#include <switch.h>

#include <stdint.h>
#include <stdio.h>

#include "../../eap_port/trace_adapter.h"

namespace rr3 {

namespace {

// The class the guest binds. The guest's own call site builds this name as
// "csdk/glucentralservices/eventbus/ServerEventBus" (raw 0x44d4a0) and passes
// it to its FindClass helper (raw 0x1643f30) at raw 0x1687e50.
const char kEventBusClass[] = "csdk/glucentralservices/eventbus/ServerEventBus";

// A raw svc, so a line can be emitted from the guest's own thread with the
// guest TLS installed. The bridge's rawLog is static to that file; this is
// the same primitive, used because the evidence has to land in BOTH sinks:
// the Eden log (where the "[jni] ..." lines live) and the guest-view log
// (where the archive's other evidence lives).
void rawLine(const char* text) {
    if (text == nullptr) {
        return;
    }
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    svcOutputDebugString(text, length);
}

}  // namespace

bool csdkEventBusJavaSideRun() {
    const JniNativeBinding* shared = findJniNativeBinding(kEventBusClass, "shared");
    if (shared == nullptr) {
        return false;
    }
    const JniNativeBinding* token = findJniNativeBinding(kEventBusClass, "globalToken");
    const JniNativeBinding* mid = findJniNativeBinding(kEventBusClass, "getMIDHandleEvent");
    if (token == nullptr || mid == nullptr) {
        rawLine("[jni] eventbus: registration incomplete (no globalToken/getMIDHandleEvent)\n");
        return false;
    }

    // The JNI calling convention for a STATIC native is
    //   jlong (*)(JNIEnv*, jclass)
    // and the env and the class are the ones the guest itself passed to
    // RegisterNatives, so the guest sees the same arguments it would see on a
    // real device.
    using LongNoArg = int64_t (*)(void*, void*);
    using LongOneArg = int64_t (*)(void*, void*, int64_t);

    void* const env = reinterpret_cast<void*>(shared->env);
    void* const clazz = reinterpret_cast<void*>(shared->clazz);

    rawLine("[jni] eventbus: java-side begin (shared/globalToken/getMIDHandleEvent)\n");
    trace("jni-eventbus: java-side begin class=%s env=%p clazz=%p",
          kEventBusClass, env, clazz);

    const LongNoArg sharedFn = reinterpret_cast<LongNoArg>(shared->fn);
    const int64_t bus = sharedFn(env, clazz);
    trace("jni-eventbus: shared() -> 0x%llx",
          static_cast<unsigned long long>(bus));

    const LongOneArg tokenFn = reinterpret_cast<LongOneArg>(token->fn);
    const int64_t globalToken = tokenFn(env, clazz, bus);
    trace("jni-eventbus: globalToken(0x%llx) -> 0x%llx",
          static_cast<unsigned long long>(bus),
          static_cast<unsigned long long>(globalToken));

    const LongNoArg midFn = reinterpret_cast<LongNoArg>(mid->fn);
    const int64_t midHandle = midFn(env, clazz);
    trace("jni-eventbus: getMIDHandleEvent() -> 0x%llx",
          static_cast<unsigned long long>(midHandle));

    rawLine("[jni] eventbus: java-side end\n");
    trace("jni-eventbus: java-side end bus=0x%llx token=0x%llx mid=0x%llx",
          static_cast<unsigned long long>(bus),
          static_cast<unsigned long long>(globalToken),
          static_cast<unsigned long long>(midHandle));
    return true;
}

}  // namespace rr3
