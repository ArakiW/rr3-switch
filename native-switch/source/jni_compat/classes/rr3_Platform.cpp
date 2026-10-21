#include <stdio.h>
#include <new>

#include "../../eap_port/platform_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "cloudcell_defer.h"
#include "cloudcell_natives.h"
#include "../../eap_port/trace_adapter.h"

/*
 * com.firemint.realracing.Platform - the class that answers RR3's native
 * alert dialogs, and with them the hard stall on the load bar.
 *
 * ============================ RETRACTION 2026-10-05 ======================
 * The "Measured chain" below is WRONG and must not be used to justify a write
 * to any object at any offset. Two independent problems, both measured:
 *
 *  1. THE ADDRESSES ARE IN THE WRONG LIBRARY. 0x3276b8, 0x323534, 0x31ffe0,
 *     0x31ff1c and 0x325098 all fall inside libfuelmetrics.so
 *     (.text 0xeb000..0x3ca6f4). The highest mapped address in
 *     libRealRacing3.so is 0x1fc7a99 (.bss end), so none of them can be an
 *     address in this module at all.
 *  2. THE DESCRIBED CODE IS NOT THERE. 0x3276b8 disassembles to a
 *     NINE-INSTRUCTION accessor ending in `ret` at 0x3276dc:
 *         mov x20,x1 ; adrp x1,#0x403000 ; mov x19,x0 ; ldr x0,[x0,#0x98]
 *         ldr x1,[x1,#0x750] ; bl 0x312f0c ; str x20,[x19,#0x98] ; ... ; ret
 *     and 0x3276e0 -- described below as `if (am[0x8d]) return;` -- is
 *     `str x1,[x0,#0x38] ; ret`, a setter for +0x38.
 *
 * So the "+0x8d download freeze, cleared only by CancelAllDownloads /
 * ContinueDownloadingVia3G / OnResume" chain does not exist at these addresses.
 *
 * WHY IT SURVIVED: the addresses are plausible, they do sit in a real .text,
 * and the story they tell is exactly the story the symptom wanted. Nothing in
 * the note was ever cross-checked by disassembling it. A comment that reads
 * correctly is not a comment that is correct -- the same failure as a probe
 * whose formula disagrees with the listing next to it.
 *
 * The original text is kept below as the record of what was believed, not as a
 * description of the binary.
 * ========================================================================
 *
 * Measured chain, from the 03:23 run (emulator.log, 122 MB, QEMU_STRACE):
 *
 *   AssetDownloadService::OnUpdate (0x3276b8)
 *     -> GetFirstWifiNetworkCheckFailed (0x325098): isNetworkAvailable(true)
 *        is false and GetQueuedDownloadSize(true) is not zero
 *     -> ShowNoWifiMessage (0x323534): writes CC_AssetManager+0x8d = 1
 *        (0x3235a8 / 0x323610) and calls system_ShowPlatformMessageWithButtons
 *        -> ndPlatformJNI::addAlertMessage -> Platform.showMessage
 *   and OnUpdate's first instruction is `if (am[0x8d]) return;` (0x3276e0).
 *
 * The only writers of that byte back to 0 in the entire binary are
 * CancelAllDownloads (0x31ffe0), ContinueDownloadingVia3G (0x31ff1c) and
 * OnResume - i.e. the dialog's own button handlers. With no Platform class the
 * dialog could never be shown, so no button could ever be pressed, so
 * AssetDownloadService::OnUpdate returned at its first instruction for the
 * remaining 850 000 frames of the run. That is the freeze.
 *
 * showMessage below closes that loop. src/rr3_asset_patch.cpp removes the
 * reason the dialog is raised in the first place; this class is the safety net
 * for every other alert the engine can raise, of which there are many.
 */
class RR3Platform : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

/* The alert answer itself now lives in jni_bridge.cpp, which owns the single
 * resolution of Java_com_firemint_realracing_MainActivity_
 * alertMessageExecuteCallback. Declared here rather than in a header so this
 * change stays inside the two files that own the alert path.
 *
 * A note on the export, because the obvious reading of it is wrong. The DYNSYM
 * entry for that name is st_value=0x12b3c50, st_size=0x28, which is exactly
 * these 9 instructions:
 *     adrp x8,#0x1f6e000 ; ldr x8,[x8,#0x540] ; ldr x8,[x8] ; cbz x8,ret
 *     adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5a8] ; mov x1,x2 ; ldr x0,[x8]
 *     b #0x12b8ed4          (0x12b8ed4: cbz x1,ret ; br x1)
 * `mov x1, x2` is an X-register move, so all 64 bits of the third argument do
 * travel - but they travel into the BRANCH SLOT, and 0x12b8ed4 is a generic
 * "tail-call through a pointer if non-null" helper shared by many functions
 * (the byte after it, 0x12b8ee0, is a JNI MonitorExit on [x0+0x128]). The
 * incoming x0 is clobbered by the first adrp and the incoming x1 is overwritten
 * by `mov x1, x2`, so neither the JNIEnv* nor a thiz is ever read. The third
 * argument is therefore a CODE POINTER, not a message id - which is the real
 * defect here, and it is not fixable from this file. */
namespace rr3 {
void rr3AlertAnswerPositiveCallback(uintptr_t env, int64_t idPositive,
                                    uintptr_t guestX1);
} // namespace rr3

/*
 * Answer with the positive button, always.
 *
 * MainActivity.Message (MainActivity.java:205) maps the three buttons to:
 *   positive -> alertMessageExecuteCallback(idPositive)
 *   neutral  -> alertMessageExecuteCallback(idNeutral)
 *   negative -> Platform.exitApp()
 * so the negative one must never be fired - it quits the game.
 *
 * For the no-Wifi dialog specifically, ShowNoWifiMessage's call at 0x32367c
 * passes a real function pointer only for the positive slot (the negative and
 * neutral callback words are both 0 on the path this port takes, because
 * isMobileDataAvailable() answers false). So there is exactly one button that
 * does anything, and it is the positive one.
 *
 * Deferred, like everything else Cloudcell: on Android the button press arrives
 * from the UI thread through GLView.queueEvent, never inside the call that
 * raised the dialog. Answering inline would run CancelAllDownloads in the
 * middle of ShowNoWifiMessage.
 *
 * The whole tuple is traced because the id -> handler mapping is assigned at
 * runtime by ndPlatformJNI::addAlertMessage (0x8b7994), which hands out
 * incrementing ids from a global counter; the log is the only way to see which
 * dialog was raised and which id it wanted.
 */
/* The three ids are read as jlong, not jint: Platform.java:1423 declares
 * showMessage(..., long j, long j2, long j3), so each id owns a full 8-byte
 * AArch64 variadic slot and a 4-byte read discards half of it. The registered
 * descriptor stays "III)V" on purpose - that is the string the guest itself
 * pairs with "showMessage" in its literal pool (0x3a1cf3 / 0x3a1d00), and the
 * descriptor is a match key, not a width contract. Reading the register 64 bits
 * wide is correct whichever of the two the guest meant: a 32-bit store zeroes
 * the top half, so the two agree on every id small enough to fit. */
static void platform_show_message(JNIEnv *env, jclass clazz, jstring title, jstring body,
                                  jstring positive, jstring negative, jstring neutral,
                                  jlong id_positive, jlong id_negative, jlong id_neutral)
{
    auto text = [](jstring s) -> const char * {
        String *str = (String *)s;
        return (str && str->str) ? str->str : "";
    };

    trace("Platform.showMessage: title='%s' body='%s' pos='%s'/%lld neg='%s'/%lld "
          "neutral='%s'/%lld -> answering positive",
          text(title), text(body), text(positive), static_cast<long long>(id_positive),
          text(negative), static_cast<long long>(id_negative),
          text(neutral), static_cast<long long>(id_neutral));

    // The lookup, the deferral and the answer all live in jni_bridge.cpp now.
    // This class used to carry its own copy of all three, resolved the export
    // by a second dlsym of the same name, and answered with its own NULL thiz -
    // which is how two routes to one export were able to drift apart. clazz is
    // the guest's own x1 here; it is forwarded for the shared path's log line
    // only, not used as thiz (see the note on rr3AlertAnswerPositiveCallback).
    rr3::rr3AlertAnswerPositiveCallback(reinterpret_cast<uintptr_t>(env),
                                        static_cast<int64_t>(id_positive),
                                        reinterpret_cast<uintptr_t>(clazz));
}

/*
 * Network state.
 *
 * WARNING: this managed registration is NOT what answers the guest.  Slot 118
 * (jniCallStaticBooleanMethodV) returns platformBoolForMethod(name) and never
 * consults the ManagedMethod table, so a static boolean is answered by
 * jni_bridge.cpp's name table or not at all.  The answer for
 * "isNetworkAvailable" therefore lives in jni_bridge.cpp, and THIS function
 * is kept in agreement with it rather than being the source of truth.
 *
 * isNetworkAvailable() answers TRUE, and the reason is not "the Switch has
 * Wi-Fi" -- it has none, and the port refuses every non-loopback connect()
 * (symbol_resolver.cpp, destinationIsLoopback).  It answers TRUE because the
 * port IS the network for this guest: the guest's own HTTP requests are
 * answered by this process, at the JNI layer, out of the local data
 * volumes.  Measured, archive 20261021-155728-eden-aliasfix:
 *
 *   CC: [2] POST request [https://connect-dev-ss.cloudcell.com:443/CC_Sync.php]
 *   CC: [2] POST complete [Succeeded=True]
 *
 * so the Cloudcell exchange is served locally and succeeds.  Answering FALSE
 * told the guest "you have no network", and the guest acted on it:
 *
 *   AssetDownloadService::ShowNoWifiMessage
 *   nBytesToDownload = 4629803
 *   AssetDownloadService:: ShowConsentMessage - No connection available
 *   AssetDownloadService::ConsentToDownload
 *   CheckNetworkAvailability::Network interface Network is NOT available
 *   CheckNetworkAvailability::m_downloadConsentState = 1
 *
 * 19 identical cycles in 300 s, with the no-wifi alert delivered on the FRAME
 * path (cloudcell_pump site=alert-inline, then openNetworkSettings).  The
 * consent flag is ALREADY 1 -- the guest asked, the port's alert answer was
 * accepted ("ConsentToDownload"), and the consent state is set -- so consent
 * was never the blocker.  The only thing between the guest and its asset set
 * was this answer.
 *
 * WHAT THE GUEST DOES WITH TRUE, from the guest's own string table:
 *
 *   CC_AssetManager_Class::StartListDownload()
 *   CC_AssetManager_Class::AssetDownloadError() : Downloading from CDN has
 *       failed! Reverting to Cloudcell server. Asset (%s) %s
 *   CC_AssetManager_Class::AssetDownloadError() : Downloading from CDN has
 *       failed! NO Cloudcell Fallback server. Asset (%s) %s
 *   /assetman/assetManDownloadPackHash.php        (the Cloudcell fallback)
 *   firemonkeys.akamaized.net                     (the CDN)
 *   GetAssetsDownloadPath / ProcessAssetServerData
 *
 * So TRUE makes the guest ask for the asset pack.  The CDN is NOT reachable
 * (the connect() policy is frozen), so the reachable endpoint is the
 * Cloudcell one -- and this port answers Cloudcell requests itself, logging
 * every URL it is handed.  A request the port cannot answer is NOT answered:
 * cc_http_request.cpp says "readable body, unknown shape: not answered"
 * rather than guessing.  That is the honest outcome, and it is recorded.
 *
 * isMobileDataAvailable() stays FALSE: the guest uses it to decide whether a
 * cellular connection may be used for a download, and this port has no
 * cellular radio to report.
 *
 * isNetworkSettingsShown() must stay false: Java sets that flag in
 * openNetworkSettings() and clears it when the settings activity returns, and
 * the engine polls it to decide whether it is still waiting for the user.
 */
static jboolean platform_is_network_available(JNIEnv *, jclass, jboolean) { return JNI_TRUE; }
static jboolean platform_is_mobile_data_available(JNIEnv *, jclass) { return JNI_FALSE; }
static jboolean platform_is_network_settings_shown(JNIEnv *, jclass) { return JNI_FALSE; }
static void platform_open_network_settings(JNIEnv *, jclass) {}

/* Locale.getDefault().toString(). The donor ships text_en.txt, so this is the
 * language file that will actually resolve. */
static jstring platform_get_locale(JNIEnv *env, jclass) { return env->NewStringUTF("en_US"); }
/* TelephonyManager.getNetworkOperatorName() with no radio. */
static jstring platform_get_carrier(JNIEnv *env, jclass) { return env->NewStringUTF(""); }

/* Crashlytics is not linked into this port; the engine only ever writes to it. */
static void platform_set_crashlytics_int(JNIEnv *, jclass, jint, jstring) {}
static void platform_set_crashlytics_string(JNIEnv *, jclass, jstring, jstring) {}

/* deleteDir() on a directory that is not there returns true (File.delete on a
 * missing path is a no-op in the recursive walk). The engine uses this to clear
 * stale download staging directories. */
static jboolean platform_delete_directory(JNIEnv *, jclass, jstring) { return JNI_TRUE; }

/* /proc/meminfo MemTotal in MB. The R36S has 1 GB; the engine uses this only to
 * pick texture budgets, and reporting the real figure is better than 0, which
 * some of those comparisons treat as "unknown, assume the smallest tier". */
static jint platform_get_total_memory(JNIEnv *, jclass) { return 1024; }

/*
 * exitApp() is only reached from the negative button of an alert - which this
 * port never presses - and from the engine's own quit path. Traced and left as
 * a no-op: the loader owns the run loop (src/main.cpp) and tearing the process
 * down from inside a JNI call would lose the frame accounting the harness
 * checks. If a run ever shows this being hit, that is a finding, not noise.
 */
static void platform_exit_app(JNIEnv *, jclass)
{
    trace("Platform.exitApp called - ignored, the loader owns the run loop");
}

/*
 * The rest of ndPlatformJNI's surface.
 *
 * These only became visible once the class existed: with no Platform at all
 * every lookup died at FindClass, and the first run after registering it turned
 * 26 further "Class Platform does not have static method ..." lines up in the
 * log - the engine caching its whole method table at init. They are not new
 * failures, they are the same NULL ids the port has always had, now itemised.
 *
 * Device identity is answered consistently with the values
 * jni/classes/cloudcell_compat.cpp already reports through GetInfo, so the two
 * paths cannot disagree about what machine this is.
 */
static jstring platform_get_device_uid(JNIEnv *env, jclass) { return env->NewStringUTF("rr3-r36s-local"); }
static jstring platform_get_model_id(JNIEnv *env, jclass) { return env->NewStringUTF("R36S"); }
static jstring platform_get_model_name(JNIEnv *env, jclass) { return env->NewStringUTF("R36S"); }
static jstring platform_get_app_name(JNIEnv *env, jclass) { return env->NewStringUTF("Real Racing 3"); }
static jstring platform_get_app_version(JNIEnv *env, jclass) { return env->NewStringUTF("2.7.0"); }
/* Build.VERSION.RELEASE. 2.7 shipped for KitKat-era devices; nothing here
 * depends on the exact value beyond ">= the minimum the game supports". */
static jstring platform_get_os_version(JNIEnv *env, jclass) { return env->NewStringUTF("4.4.4"); }
static jint platform_get_api_level(JNIEnv *, jclass) { return 19; }

/*
 * getAppPath / getExternalStorageDir / getInternalStorageDir /
 * getExternalStorageState / extractRes are deliberately NOT registered.
 *
 * They were, for one run, all answering io_game_dir(). It regressed: the engine
 * uses those roots to build its Android search list, so every content lookup
 * moved from "/game/gametext.txt" - which exists - to "/game/.depot/�?,
 * "/game/apk/res/�? and "/game/doc/�?, which do not, and the run died with a
 * SIGSEGV in libRealRacing3.so+0x3c99fc after langs.utf8, gametext.txt and
 * text_.txt all failed to load. With the methods absent the engine keeps its
 * own default root and the port's file layer (src/symtab_io.cpp) resolves
 * everything, which is the arrangement that was already working.
 *
 * If they are ever needed, the answer is not io_game_dir() - it is whatever
 * makes "<answer>/apk/res" land on the extracted tree, which no single value of
 * these three can do at once.
 */

/* Java reads PackageInfo.firstInstallTime, in milliseconds. There is no install
 * record here; 0 is "unknown", which is what a fresh install also looks like. */
static jlong platform_get_app_install_time(JNIEnv *, jclass) { return 0; }

/*
 * Debug.getNativeHeapAllocatedSize()/getNativeHeapFreeSize(), in bytes.
 *
 * Deliberately optimistic: every consumer of these is a low-memory guard, and
 * the failure mode of a wrong answer is one-directional - report too little
 * free and the engine drops to a smaller texture tier or refuses content;
 * report plenty and it behaves as it would on the 1 GB device this is. The
 * numbers are a floor, not a measurement, and are marked as such here rather
 * than dressed up as one.
 */
static jlong platform_get_app_memory_free(JNIEnv *, jclass) { return 512LL * 1024 * 1024; }
static jlong platform_get_app_memory_usage(JNIEnv *, jclass) { return 128LL * 1024 * 1024; }
static void platform_memory_probe(JNIEnv *, jclass) {}

/* The surface this port actually creates - kWidth/kHeight in src/main.cpp -
 * with a density-1 panel and no rotation. */
static jint platform_get_screen_width(JNIEnv *, jclass) { return 640; }
static jint platform_get_screen_height(JNIEnv *, jclass) { return 480; }
static jint platform_get_screen_dpi(JNIEnv *, jclass) { return 160; }
static jint platform_get_screen_rotation(JNIEnv *, jclass) { return 0; }

/* No browser and no photo gallery on this console. saveImage answers false
 * rather than pretending, so the engine reports the failure to the player
 * instead of leaving them looking for a file that was never written. */
static void platform_open_url(JNIEnv *, jclass, jstring) {}
static jboolean platform_save_image(JNIEnv *, jclass, jintArray, jint, jint, jint, jstring)
{ return JNI_FALSE; }

/*
 * loadTextureFromMemory decodes a PNG/JPEG through android.graphics.Bitmap and
 * hands back a com.firemint.realracing.TextureInfo. NULL, because that class
 * is not registered and because this port decodes textures itself
 * (src/dxt_decompress.cpp, src/atc_decompress.cpp, PVRTDecompress). If a run
 * ever shows this being called for real, registering TextureInfo is the fix -
 * not returning a fake object.
 */
static jobject platform_load_texture_from_memory(JNIEnv *, jclass, jbyteArray, jint)
{ return NULL; }

/* Keeps the screen awake on Android; nothing to keep awake here. */
static void platform_toggle_idle_mode(JNIEnv *, jclass, jboolean) {}
/* Literally `return i + 1` in the Java - a liveness check for the JNI bridge,
 * so it has to behave exactly. */
static jint platform_test_func(JNIEnv *, jclass, jint value) { return value + 1; }

/* The engine does build an instance of this otherwise-static class and keeps it
 * as its Platform handle; the run log showed the NewObject failing on a missing
 * <init>. calloc leaves the vptr zero, hence the placement-new. */
static void platform_ctor(JNIEnv *, jobject self, jclass) { new (self) RR3Platform(); }

static const ManagedMethod platform_methods[] = {
    ManagedMethod::RegisterNonVirtual<&platform_ctor>(RR3Platform::clazz, "<init>", "()V"),
    ManagedMethod::RegisterStatic<&platform_show_message>(
        RR3Platform::clazz, "showMessage",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;III)V"),
    ManagedMethod::RegisterStatic<&platform_is_network_available>(
        RR3Platform::clazz, "isNetworkAvailable", "(Z)Z"),
    ManagedMethod::RegisterStatic<&platform_is_mobile_data_available>(
        RR3Platform::clazz, "isMobileDataAvailable", "()Z"),
    ManagedMethod::RegisterStatic<&platform_is_network_settings_shown>(
        RR3Platform::clazz, "isNetworkSettingsShown", "()Z"),
    ManagedMethod::RegisterStatic<&platform_open_network_settings>(
        RR3Platform::clazz, "openNetworkSettings", "()V"),
    ManagedMethod::RegisterStatic<&platform_get_locale>(
        RR3Platform::clazz, "getLocale", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_carrier>(
        RR3Platform::clazz, "getCarrier", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_set_crashlytics_int>(
        RR3Platform::clazz, "setCrashlyticsInt", "(ILjava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&platform_set_crashlytics_string>(
        RR3Platform::clazz, "setCrashlyticsString",
        "(Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&platform_delete_directory>(
        RR3Platform::clazz, "deleteDirectory", "(Ljava/lang/String;)Z"),
    ManagedMethod::RegisterStatic<&platform_get_total_memory>(
        RR3Platform::clazz, "getTotalMemory", "()I"),
    ManagedMethod::RegisterStatic<&platform_exit_app>(
        RR3Platform::clazz, "exitApp", "()V"),
    ManagedMethod::RegisterStatic<&platform_get_device_uid>(
        RR3Platform::clazz, "getDeviceUID", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_model_id>(
        RR3Platform::clazz, "getModelID", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_model_name>(
        RR3Platform::clazz, "getModelName", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_app_name>(
        RR3Platform::clazz, "getAppName", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_app_version>(
        RR3Platform::clazz, "getAppVersion", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_os_version>(
        RR3Platform::clazz, "getOsVersion", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&platform_get_api_level>(
        RR3Platform::clazz, "getApiLevel", "()I"),
    ManagedMethod::RegisterStatic<&platform_get_app_install_time>(
        RR3Platform::clazz, "getAppInstallTime", "()J"),
    ManagedMethod::RegisterStatic<&platform_get_app_memory_free>(
        RR3Platform::clazz, "getAppMemoryFree", "()J"),
    ManagedMethod::RegisterStatic<&platform_get_app_memory_usage>(
        RR3Platform::clazz, "getAppMemoryUsage", "()J"),
    ManagedMethod::RegisterStatic<&platform_memory_probe>(
        RR3Platform::clazz, "memoryProbe", "()V"),
    ManagedMethod::RegisterStatic<&platform_get_screen_width>(
        RR3Platform::clazz, "getScreenWidth", "()I"),
    ManagedMethod::RegisterStatic<&platform_get_screen_height>(
        RR3Platform::clazz, "getScreenHeight", "()I"),
    ManagedMethod::RegisterStatic<&platform_get_screen_dpi>(
        RR3Platform::clazz, "getScreenDPI", "()I"),
    ManagedMethod::RegisterStatic<&platform_get_screen_rotation>(
        RR3Platform::clazz, "getScreenRotation", "()I"),
    ManagedMethod::RegisterStatic<&platform_open_url>(
        RR3Platform::clazz, "openURL", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&platform_save_image>(
        RR3Platform::clazz, "saveImage", "([IIIILjava/lang/String;)Z"),
    ManagedMethod::RegisterStatic<&platform_save_image>(
        RR3Platform::clazz, "saveToImageGallery", "([IIIILjava/lang/String;)Z"),
    ManagedMethod::RegisterStatic<&platform_load_texture_from_memory>(
        RR3Platform::clazz, "loadTextureFromMemory",
        "([BI)Lcom/firemint/realracing/TextureInfo;"),
    ManagedMethod::RegisterStatic<&platform_toggle_idle_mode>(
        RR3Platform::clazz, "toggleIdleMode", "(Z)V"),
    ManagedMethod::RegisterStatic<&platform_test_func>(
        RR3Platform::clazz, "testFunc", "(I)I"),
    {NULL},
};

Class RR3Platform::clazz = {
    .classpath = "com/firemint/realracing/Platform",
    .classname = "Platform",
    .managed_methods = platform_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    /* The Java class is all statics, but the engine still builds one instance
     * as its handle - measured, the first run with this class registered logged
     * "Class Platform does not have method <init>()V." */
    .instance_size = sizeof(RR3Platform),
};

static const int platform_registered = ClassRegistry::register_class(RR3Platform::clazz);
