#include "../jni.h"
#include "../jni_internals.h"`n#include "../../eap_port/trace_adapter.h"

static Class clazz = {
    .classpath = "com/firemint/realracing/LocalNotificationsCenter",
    .classname = "LocalNotificationsCenter",
    .managed_methods = {NULL},
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(Object),
};
static void cancel_all(JNIEnv *, jclass) {}
static const ManagedMethod methods[] = {
    ManagedMethod::RegisterStatic<&cancel_all>(clazz, "CancelAllNotifications", "()V"),
    {NULL},
};
struct Hook { Hook() { clazz.managed_methods = methods; } } hook;
static const int registered = ClassRegistry::register_class(clazz);
