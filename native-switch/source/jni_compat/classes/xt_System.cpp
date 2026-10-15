// Stub xt_System for Switch
#include <stdio.h>
#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "xt_System.h"

static jstring system_getenv(JNIEnv *env, jclass, jstring) {
    return env->NewStringUTF("");
}
static jlong system_currentTimeMillis(JNIEnv *, jclass) {
    return 0;
}
static void system_gc(JNIEnv *, jclass) {}
static void system_exit(JNIEnv *, jclass, jint) {}

const ManagedMethod xtSystemClassMethods[] = {
    ManagedMethod::RegisterStatic<&system_getenv>(xtSystem::clazz, "getenv", "(Ljava/lang/String;)Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&system_currentTimeMillis>(xtSystem::clazz, "currentTimeMillis", "()J"),
    ManagedMethod::RegisterStatic<&system_gc>(xtSystem::clazz, "gc", "()V"),
    ManagedMethod::RegisterStatic<&system_exit>(xtSystem::clazz, "exit", "(I)V"),
    {NULL},
};

Class xtSystem::clazz = {
    .classpath = "xtSystem",
    .classname = "xtSystem",
    .managed_methods = xtSystemClassMethods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(xtSystem),
};
static const int registered = ClassRegistry::register_class(xtSystem::clazz);