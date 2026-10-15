#include "../jni.h"
#include "../jni_internals.h"
#include "lang_ClassLoader.h"
#include "rr3_MainActivity.h"
#include "../../eap_port/trace_adapter.h"

class RR3AssetManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

Class RR3AssetManager::clazz = {
    .classpath = "android/content/res/AssetManager",
    .classname = "RR3AssetManager",
    .managed_methods = {NULL},
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(RR3AssetManager),
};

static const int asset_manager_registered =
    ClassRegistry::register_class(RR3AssetManager::clazz);

static jobject get_class_loader(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    return reinterpret_cast<jobject>(&g_class_loader);
}

static jstring get_package_name(JNIEnv *env, jobject self)
{
    (void)self;
    return env->NewStringUTF("com.ea.games.r3_row");
}

static jobject get_assets(JNIEnv *env, jobject self)
{
    (void)env;
    (void)self;
    static RR3AssetManager assets;
    return reinterpret_cast<jobject>(&assets);
}

/*
 * hideSplash() (MainActivity.java:544) removes the Android splash ImageView
 * from the view hierarchy. There is no such view here, so the method is a
 * no-op - but it is worth having, because the engine calls it exactly twice and
 * only once it believes boot is over. Its absence showed up as
 * "Class RR3MainActivity does not have method hideSplash()V." at
 * emulator.log:12877, right after the second Cloudcell HTTP request was built.
 */
static void hide_splash(JNIEnv *, jobject) {}

/*
 * finishActivity() (MainActivity.java:462) is Android's "close the app". The
 * loader owns the run loop (src/main.cpp), so tearing the process down from
 * inside a JNI call would lose the frame accounting the harness asserts on.
 * Registered so the engine gets a method instead of a NULL id; if a run ever
 * shows this being called, that is a finding worth reading, not noise.
 */
static void finish_activity(JNIEnv *, jobject)
{
    trace("MainActivity.finishActivity called - ignored, the loader owns the run loop");
}

static const ManagedMethod methods[] = {
    ManagedMethod::Register<&hide_splash>(
        RR3MainActivity::clazz, "hideSplash", "()V"),
    ManagedMethod::Register<&finish_activity>(
        RR3MainActivity::clazz, "finishActivity", "()V"),
    ManagedMethod::Register<&get_class_loader>(
        RR3MainActivity::clazz, "getClassLoader", "()Ljava/lang/ClassLoader;"),
    ManagedMethod::Register<&get_package_name>(
        RR3MainActivity::clazz, "getPackageName", "()Ljava/lang/String;"),
    ManagedMethod::Register<&get_assets>(
        RR3MainActivity::clazz, "getAssets",
        "()Landroid/content/res/AssetManager;"),
    {NULL},
};

Class RR3MainActivity::clazz = {
    .classpath = "com/firemint/realracing/MainActivity",
    .classname = "RR3MainActivity",
    .managed_methods = methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(RR3MainActivity),
};

static const int registered =
    ClassRegistry::register_class(RR3MainActivity::clazz);
