#pragma once

// The Java side of csdk.glucentralservices.eventbus.ServerEventBus.
//
// WHY THIS FILE EXISTS
// --------------------
// The guest's csdk (Glu Central Services) layer binds its Java side by calling
// RegisterNatives on the APK's own class
// csdk.glucentralservices.eventbus.ServerEventBus, with seven methods whose
// function pointers are R_AARCH64_ABS64 relocations to the guest's OWN
// exports (measured in reverse/native/arm64/libRealRacing3.so):
//
//   .data raw 0x1f8ae88, seven JNINativeMethod entries:
//     getMIDHandleEvent ()J
//     shared            ()J
//     globalToken       (J)J
//     subscribe         (JJJLjava/lang/String;[Ljava/lang/String;
//                          Lcsdk/glucentralservices/eventbus/EventHandlerData;)J
//     unsubscribe       (JJ)V
//     publish           (JJLjava/lang/String;Ljava/lang/String;
//                          Ljava/lang/String;Ljava/lang/String;)V
//     publishError      (JJLjava/lang/String;Ljava/lang/String;
//                          Ljava/lang/String;Ljava/lang/String;
//                          Ljava/lang/String;Ljava/lang/String;
//                          Ljava/lang/String;)V
//
//   the function pointers resolve to the guest's exports
//     Java_csdk_glucentralservices_eventbus_ServerEventBus_getMIDHandleEvent
//     ..._shared  ..._globalToken  ..._subscribe  ..._unsubscribe
//     ..._publish  ..._publishError
//
// so the guest is the PROVIDER: the Java side has to be able to call INTO the
// guest. That is the whole content of the APK's
// reverse/java/sources/csdk/glucentralservices/eventbus/ServerEventBus.java --
// every one of those seven methods is `private static native`.
//
// THERE IS NO JAVA VM TO CALL THEM
// --------------------------------
// MEASURED 2026-10-20 on archive 20261020-134823-eden-dlprobe2:
//
//  * Every "[jni] ..." line in Eden's log is the PORT's own output. The
//    bridge's jniFindClass writes exactly "[jni] FindClass <name>" through
//    rawLog -> svcOutputDebugString, and the run's 27 distinct class names
//    have that spelling. So the FindClass lookups were answered by the port's
//    own class table, not by a VM behind the emulator.
//  * There is no second JNI path: Eden is a Switch emulator and has no JVM,
//    the port never calls JNI_CreateJavaVM, and
//    jni_compat/jni.cpp's jnienv_funcs is reachable only through that file's
//    own fake JavaVM -- while the guest's JNIEnv is the g_jniTable in
//    jni_bridge.cpp.
//
// So the "Java side" of a native-bound class IS this port, and a registration
// that is thrown away is a registration that can never be called. This file
// is the Java side for the one class the guest binds: it performs exactly
// what the APK's own decompiled class does in its static initializer and its
// constructor, by calling the guest's own registered natives. Nothing here
// invents a value: every number the log prints is the guest's own return
// value.

#include <stdint.h>

namespace rr3 {

// One native method the guest bound with RegisterNatives.
struct JniNativeBinding {
    const char* className;  // the class the guest registered
    const char* name;       // the Java method name
    const char* signature;  // the JNI signature
    uintptr_t fn;           // the guest's function pointer
    uintptr_t clazz;        // the class handle the guest passed
    uintptr_t env;          // the JNIEnv the guest used
};

// The lookup. jni_bridge.cpp owns the JNIEnv table and therefore owns the
// table of bindings; this is its accessor.
const JniNativeBinding* findJniNativeBinding(const char* className,
                                              const char* name);

// Runs the Java side of the event bus: the static initializer and the
// constructor of csdk.glucentralservices.eventbus.ServerEventBus, as the
// APK's decompiled source has them:
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
//   private ServerEventBus(long j) {
//       this.mEventBus = j;
//       this.mGlobalToken = tokenFromNativeToken(globalToken(j));
//   }
//
// which is three native calls, in this order:
//   shared()            -> the event bus handle
//   globalToken(bus)    -> the global token
//   getMIDHandleEvent() -> the method id of the Java static handleEvent
//
// The System.loadLibrary is a no-op here: the library is already loaded, and
// the natives are already bound -- that is what RegisterNatives did.
//
// Returns true when the Java side ran, false when the guest has not
// registered the class (nothing to do).
bool csdkEventBusJavaSideRun();

}  // namespace rr3
