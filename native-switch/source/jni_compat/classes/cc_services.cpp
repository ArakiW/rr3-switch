#include <new>

#include "../../eap_port/platform_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "../../eap_port/trace_adapter.h"
#include "../../../include/rr3/gfx_backend.hpp"
#include "cloudcell_defer.h"
#include "cloudcell_natives.h"

/*
 * The Cloudcell service facades RR3 2.7 builds during boot.
 *
 * Every class here is a service that does not exist on this console: Google
 * sign-in, Facebook, Play billing, AdMob, GCM push, an in-app WebView, a rate-
 * the-app dialog. Registering them is not about making those features work - it
 * is about being able to say "no" in the shape the engine expects.
 *
 * With the class missing, FindClass returned NULL, the engine logged
 * "Could not create new Java object instance!" and carried on - but every
 * manager that had asked for the object was then left waiting for a callback
 * that could not be delivered, because the callback is a method on the object
 * that was never built. The failure is silent and permanent.
 *
 * So the rule for every method below is: answer, and answer with the value the
 * real Java produces on a device where the service is unavailable. That value
 * was read out of the decompiled 2.7 sources under
 * /tmp/rr3-jadx-2.7/sources/com/firemonkeys/cloudcellapi/ - each entry says
 * which branch it is imitating. Inventing a "nicer" answer (a successful login,
 * a displayed ad) would put the engine into a state that has follow-up steps
 * this port cannot perform.
 *
 * Everything asynchronous in Java goes through cloudcell_defer(); see
 * cloudcell_defer.h for why answering inline is not an option.
 */

/* java/lang/String is registered by string.cpp, so an empty String[] is a legal
 * object here. Facebook and Google Plus differ on which of null / new String[0]
 * their failure branches pass, and the native sides were written against that
 * difference, so the two are not interchangeable. */
static jobjectArray cc_empty_string_array(JNIEnv *env)
{
    return env->NewObjectArray(0, (jclass)&String::clazz, NULL);
}

/* ------------------------------------------------------------------------ */
/* AndroidAccountManager                                                     */
/* ------------------------------------------------------------------------ */

class CCAccountManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    jlong login_cb = 0;   /* Constructor(long j, long j2): j  */
    jlong user_ptr = 0;   /*                               j2 */
};

/* Java: public static native void LoginCompleteCallback(boolean, long, long) */
using AamLoginCompleteFn = void (*)(JNIEnv *, jclass, unsigned char, jlong, jlong);

static void aam_ctor(JNIEnv *, jobject self, jclass) { new (self) CCAccountManager(); }

static void aam_constructor(JNIEnv *, jobject self, jlong login_cb, jlong user)
{
    CCAccountManager *am = (CCAccountManager *)self;
    am->login_cb = login_cb;
    am->user_ptr = user;
}

static void aam_destructor(JNIEnv *, jobject self)
{
    CCAccountManager *am = (CCAccountManager *)self;
    am->login_cb = 0;
    am->user_ptr = 0;
}

/* IsLoggedIn() reads AccountManager.getAccountsByType(GOOGLE_ACCOUNT_TYPE);
 * with no Google account on the device that array is empty. */
static jboolean aam_is_logged_in(JNIEnv *, jobject) { return JNI_FALSE; }

/* AndroidAccountManager.java:80 - the exception path of Login() calls
 * LoginCompleteCallback(false, ...) itself. Without it CC_AuthenticatorManager
 * (a Cloudcell::Notifier<IAccountManagerListener>) stays pending forever.
 *
 * MEASURED 2026-10-19, archive 20261019-210729-exec1. The guest's
 * LoginCompleteCallback is a trampoline:
 *     tst  w2, #0xff   ; the boolean
 *     mov  x1, x4      ; the user handle
 *     cset w0, ne      ; w0 = (boolean != 0)
 *     br   x3          ; tail-call the guest's own continuation
 * so `cb` is a guest continuation pointer and the boolean is the third
 * argument.
 *
 * With the honest failure the guest stops complaining "Invalid DeviceId!" --
 * so the callback IS reached and IS understood -- and then spins on
 * "Sub Sync Queued: 10765" until the run dies at frame 4. It retries the queue
 * because the login FAILED, not because the callback was missing.
 *
 * ccloginok=1 reports success instead. The user authorised a simulated
 * Cloudcell backend and EA's service is gone, so a local success is the only
 * answer the guest accepts. Off by default: this one is a simulation, and the
 * difference is visible in the log.
 */
static void aam_login(JNIEnv *env, jobject self)
{
    CCAccountManager *am = (CCAccountManager *)self;
    jlong cb = am->login_cb, user = am->user_ptr;

    cloudcell_defer([env, self, cb, user]() {
        static AamLoginCompleteFn fn = cloudcell_native<AamLoginCompleteFn>(
            "Java_com_firemonkeys_cloudcellapi_AndroidAccountManager_LoginCompleteCallback");
        if (fn)
            fn(env, (jclass)&CCAccountManager::clazz,
               rr3::gfxCloudcellLoginOk() ? JNI_TRUE : JNI_FALSE, cb, user);
    });
}

static void aam_on_resume(JNIEnv *, jobject) {}

/*
 * setUserIdentifier and logout, added 2026-10-19.
 *
 * Both are requested by the guest and neither was registered. The method names
 * are the guest's own spellings, checked against the image rather than
 * assumed: "login", "logout" and "setUserIdentifier" appear there as standalone
 * literals and "SetUserIdentifier" does not appear at all.
 *
 * setUserIdentifier stores the handle as handed over. It is not decoded to a
 * char* because nothing reads it back: aam_login passes user_ptr straight to
 * LoginCompleteCallback as the opaque long the native side expects, and
 * decoding it would mean allocating a string the guest never asked for.
 */
static void aam_set_user_identifier(JNIEnv *, jobject self, jobject identifier)
{
    ((CCAccountManager *)self)->user_ptr =
        reinterpret_cast<jlong>(identifier);
}

static void aam_logout(JNIEnv *, jobject self)
{
    CCAccountManager *am = (CCAccountManager *)self;
    am->login_cb = 0;
    am->user_ptr = 0;
}

static const ManagedMethod aam_methods[] = {
    ManagedMethod::RegisterNonVirtual<&aam_ctor>(CCAccountManager::clazz, "<init>", "()V"),
    ManagedMethod::Register<&aam_constructor>(CCAccountManager::clazz, "Constructor", "(JJ)V"),
    ManagedMethod::Register<&aam_destructor>(CCAccountManager::clazz, "Destructor", "()V"),
    ManagedMethod::Register<&aam_is_logged_in>(CCAccountManager::clazz, "IsLoggedIn", "()Z"),
    ManagedMethod::Register<&aam_login>(CCAccountManager::clazz, "login", "()V"),
    ManagedMethod::Register<&aam_logout>(CCAccountManager::clazz, "logout", "()V"),
    ManagedMethod::Register<&aam_set_user_identifier>(CCAccountManager::clazz,
                                                      "setUserIdentifier",
                                                      "(Ljava/lang/String;)V"),
    ManagedMethod::Register<&aam_on_resume>(CCAccountManager::clazz, "onResume", "()V"),
    {NULL},
};

Class CCAccountManager::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/AndroidAccountManager",
    .classname = "AndroidAccountManager",
    .managed_methods = aam_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCAccountManager),
};
static const int aam_registered = ClassRegistry::register_class(CCAccountManager::clazz);

/* ------------------------------------------------------------------------ */
/* UserInterfaceManager                                                 */
/* ------------------------------------------------------------------------ */

class CCUserInterfaceManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* WebBrowserCreate's trailing JJJJJ, in Java argument order
     * (UserInterfaceManager.java:219). WebBrowserOpenUrl carries no
     * callback pointers of its own, so without keeping these the stub has no
     * way to answer the page load at all. */
    jlong wb_should_start = 0;
    jlong wb_start        = 0;
    jlong wb_finish       = 0;
    jlong wb_fail         = 0;
    jlong wb_user         = 0;
};

/* Java: public native void WebBrowserLoadFailCallback(long, long) */
using UimWebFailFn = void (*)(JNIEnv *, jobject, jlong, jlong);

static void uim_ctor(JNIEnv *, jobject self, jclass) { new (self) CCUserInterfaceManager(); }

/* Java returns widthPixels / GetScreenScale(); this port's surface is the
 * kWidth/kHeight pair in src/main.cpp. */
static jint uim_screen_width(JNIEnv *, jobject) { return 640; }
static jint uim_screen_height(JNIEnv *, jobject) { return 480; }
/* Java returns 2.0 only when DisplayMetrics.density > 1.0; a 640x480 panel is
 * density 1, so the engine must not be told to double its UI coordinates. */
static jfloat uim_screen_scale(JNIEnv *, jobject) { return 1.0f; }

/* The Java dialog's button handler is empty and there is no callback native on
 * this class for it, so nothing is waiting on the answer. */
static void uim_show_dialog_box(JNIEnv *, jobject, jstring, jstring, jstring) {}

/*
 * Every widget factory answers NULL. Returning a fake non-NULL handle is not an
 * option: iface_GetObjectClass (jni/jni.cpp:251) makes a virtual call through
 * whatever pointer it is given, so a bogus one is a crash, while NULL is
 * null-checked and merely logged.
 */
static jobject uim_window_create(JNIEnv *, jobject, jint, jint, jint, jint) { return NULL; }
static void uim_window_show(JNIEnv *, jobject, jobject) {}
static void uim_window_hide(JNIEnv *, jobject, jobject) {}
static jobject uim_image_create(JNIEnv *, jobject, jobject, jint, jint, jint, jint,
                                jstring, jint, jint) { return NULL; }
static jobject uim_image_patch_create(JNIEnv *, jobject, jobject, jint, jint, jint,
                                      jint, jstring, jint) { return NULL; }
static void uim_image_show(JNIEnv *, jobject, jobject) {}
static void uim_image_hide(JNIEnv *, jobject, jobject) {}
static jobject uim_label_create(JNIEnv *, jobject, jobject, jobject, jstring, jstring,
                                jint, jfloat, jfloat, jfloat, jint, jint, jint, jint)
{ return NULL; }
static void uim_label_delete(JNIEnv *, jobject, jobject) {}
/* ClickableCallback only fires on a real tap, so there is nothing to answer. */
static void uim_clickable_create(JNIEnv *, jobject, jobject, jint) {}

static jobject uim_web_browser_create(JNIEnv *, jobject self, jobject /*webView*/,
                                      jobject /*layout*/, jint, jint, jint, jint,
                                      jlong should_start, jlong start, jlong finish,
                                      jlong fail, jlong user)
{
    CCUserInterfaceManager *uim = (CCUserInterfaceManager *)self;
    uim->wb_should_start = should_start;
    uim->wb_start        = start;
    uim->wb_finish       = finish;
    uim->wb_fail         = fail;
    uim->wb_user         = user;
    return NULL;
}

/*
 * CC_WebBrowserManager_Class opens the server-message page, the news feed, the
 * support page and the Twitter sign-in, and then waits on one of the four load
 * callbacks. Fail, not finish: "finish" means the page rendered, after which
 * the manager waits for the user to close a WebView that does not exist.
 * WebBrowserLoadFailCallback is onReceivedError - the real device's no-network
 * path, which routes into the engine's own /cc_errormessage.html handling.
 */
static void uim_web_browser_open_url(JNIEnv *env, jobject self, jobject /*webView*/,
                                     jstring, jstring, jboolean, jstring)
{
    CCUserInterfaceManager *uim = (CCUserInterfaceManager *)self;
    jlong fail = uim->wb_fail, user = uim->wb_user;

    cloudcell_defer([env, self, fail, user]() {
        static UimWebFailFn fn = cloudcell_native<UimWebFailFn>(
            "Java_com_firemonkeys_cloudcellapi_UserInterfaceManager_"
            "WebBrowserLoadFailCallback");
        if (fn)
            fn(env, self, fail, user);
    });
}

static const ManagedMethod uim_methods[] = {
    ManagedMethod::RegisterNonVirtual<&uim_ctor>(CCUserInterfaceManager::clazz, "<init>", "()V"),
    ManagedMethod::Register<&uim_screen_width>(CCUserInterfaceManager::clazz, "GetScreenWidth", "()I"),
    ManagedMethod::Register<&uim_screen_height>(CCUserInterfaceManager::clazz, "GetScreenHeight", "()I"),
    ManagedMethod::Register<&uim_screen_scale>(CCUserInterfaceManager::clazz, "GetScreenScale", "()F"),
    ManagedMethod::Register<&uim_show_dialog_box>(
        CCUserInterfaceManager::clazz, "ShowDialogBox",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::Register<&uim_window_create>(
        CCUserInterfaceManager::clazz, "WindowCreate", "(IIII)Landroid/widget/RelativeLayout;"),
    ManagedMethod::Register<&uim_window_show>(
        CCUserInterfaceManager::clazz, "WindowShow", "(Landroid/widget/RelativeLayout;)V"),
    ManagedMethod::Register<&uim_window_hide>(
        CCUserInterfaceManager::clazz, "WindowHide", "(Landroid/widget/RelativeLayout;)V"),
    ManagedMethod::Register<&uim_image_create>(
        CCUserInterfaceManager::clazz, "ImageCreate",
        "(Landroid/widget/RelativeLayout;IIIILjava/lang/String;II)Landroid/widget/ImageView;"),
    ManagedMethod::Register<&uim_image_patch_create>(
        CCUserInterfaceManager::clazz, "ImagePatchCreate",
        "(Landroid/widget/RelativeLayout;IIIILjava/lang/String;I)Landroid/widget/ImageView;"),
    ManagedMethod::Register<&uim_image_show>(
        CCUserInterfaceManager::clazz, "ImageShow", "(Landroid/widget/ImageView;)V"),
    ManagedMethod::Register<&uim_image_hide>(
        CCUserInterfaceManager::clazz, "ImageHide", "(Landroid/widget/ImageView;)V"),
    ManagedMethod::Register<&uim_label_create>(
        CCUserInterfaceManager::clazz, "LabelCreate",
        "(Landroid/widget/TextView;Landroid/widget/RelativeLayout;Ljava/lang/String;"
        "Ljava/lang/String;IFFFIIII)Landroid/widget/TextView;"),
    ManagedMethod::Register<&uim_label_delete>(
        CCUserInterfaceManager::clazz, "LabelDelete", "(Landroid/widget/TextView;)V"),
    ManagedMethod::Register<&uim_clickable_create>(
        CCUserInterfaceManager::clazz, "ClickableCreate", "(Landroid/widget/ImageView;I)V"),
    ManagedMethod::Register<&uim_web_browser_create>(
        CCUserInterfaceManager::clazz, "WebBrowserCreate",
        "(Landroid/webkit/WebView;Landroid/widget/RelativeLayout;IIIIJJJJJ)"
        "Landroid/webkit/WebView;"),
    ManagedMethod::Register<&uim_web_browser_open_url>(
        CCUserInterfaceManager::clazz, "WebBrowserOpenUrl",
        "(Landroid/webkit/WebView;Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V"),
    {NULL},
};

Class CCUserInterfaceManager::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/UserInterfaceManager",
    .classname = "UserInterfaceManager",
    .managed_methods = uim_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCUserInterfaceManager),
};
static const int uim_registered = ClassRegistry::register_class(CCUserInterfaceManager::clazz);

/* ------------------------------------------------------------------------ */
/* FacebookWorker                                                   */
/* ------------------------------------------------------------------------ */

class CCFacebookWorker : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

using FbLoginFn   = void (*)(JNIEnv *, jobject, jstring, jstring, jstring, jstring,
                             jstring, jlong, jlong);
using FbPairFn    = void (*)(JNIEnv *, jobject, jlong, jlong);
using FbBoolFn    = void (*)(JNIEnv *, jobject, unsigned char, jlong, jlong);
using FbFriendsFn = void (*)(JNIEnv *, jobject, unsigned char, jobjectArray,
                             jobjectArray, jlong, jlong);

static void fb_ctor(JNIEnv *, jobject, jclass) {}
static void fb_constructor(JNIEnv *, jobject, jstring /*appId*/) {}

/* m_pSession.isOpened() on a session that is never opened. */
static jboolean fb_session_valid(JNIEnv *, jobject) { return JNI_FALSE; }
/* Java answers Constants.STR_EMPTY when the token is null. */
static jstring fb_access_token(JNIEnv *env, jobject) { return env->NewStringUTF(""); }

/*
 * SessionStatusCallback.call() (FacebookWorker.java:78) is the
 * authoritative failure path and it passes literally "0" as the user id with
 * four empty name strings. The native side tests against that "0"; an empty id
 * is a different case that this build never reaches.
 */
static void fb_login(JNIEnv *env, jobject self, jobjectArray, jboolean,
                     jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static FbLoginFn fn = cloudcell_native<FbLoginFn>(
            "Java_com_firemonkeys_cloudcellapi_FacebookWorker_LoginCallback");
        if (!fn)
            return;
        jstring zero  = env->NewStringUTF("0");
        jstring empty = env->NewStringUTF("");
        fn(env, self, zero, empty, empty, empty, empty, cb, user);
    });
}

static void fb_logout(JNIEnv *env, jobject self, jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static FbPairFn fn = cloudcell_native<FbPairFn>(
            "Java_com_firemonkeys_cloudcellapi_FacebookWorker_LogoutCallback");
        if (fn)
            fn(env, self, cb, user);
    });
}

static void fb_defer_bool(JNIEnv *env, jobject self, const char *symbol,
                          jlong cb, jlong user)
{
    cloudcell_defer([env, self, symbol, cb, user]() {
        FbBoolFn fn = cloudcell_native<FbBoolFn>(symbol);
        if (fn)
            fn(env, self, JNI_FALSE, cb, user);
    });
}

static void fb_permission_check(JNIEnv *env, jobject self, jobjectArray, jlong cb, jlong user)
{
    fb_defer_bool(env, self,
                  "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
                  "PermissionCheckCallback", cb, user);
}

static void fb_permission_grant(JNIEnv *env, jobject self, jobjectArray, jlong cb, jlong user)
{
    fb_defer_bool(env, self,
                  "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
                  "PermissionGrantCallback", cb, user);
}

/* FacebookWorker.java:265 passes null, null on failure - unlike Google
 * Plus, whose equivalent path passes new String[0]. Both native sides were
 * written against their own Java, so the two must not be made uniform. */
static void fb_load_friend_vector(JNIEnv *env, jobject self, jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static FbFriendsFn fn = cloudcell_native<FbFriendsFn>(
            "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
            "LoadFriendVectorCallback");
        if (fn)
            fn(env, self, JNI_FALSE, NULL, NULL, cb, user);
    });
}

static void fb_feed_post(JNIEnv *env, jobject self, jstring, jstring, jstring, jstring,
                         jstring, jstring, jboolean, jlong cb, jlong user)
{
    fb_defer_bool(env, self,
                  "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
                  "FeedPostCallback", cb, user);
}

static void fb_photo_post(JNIEnv *env, jobject self, jstring, jbyteArray, jlong cb, jlong user)
{
    fb_defer_bool(env, self,
                  "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
                  "PhotoPostCallback", cb, user);
}

static void fb_friend_invite(JNIEnv *env, jobject self, jstring, jstring, jlong cb, jlong user)
{
    fb_defer_bool(env, self,
                  "Java_com_firemonkeys_cloudcellapi_FacebookWorker_"
                  "FriendInviteCallback", cb, user);
}

/* Only reachable once an avatar image has already been downloaded, which cannot
 * happen offline. NULL is answered rather than a zeroed AvatarInfo because the
 * inner class is not registered; if the path is ever reached the log will say
 * so through the usual NULL-object diagnostics instead of faulting. */
static jobject fb_decode_avatar(JNIEnv *, jobject, jbyteArray, jint) { return NULL; }

static const ManagedMethod fb_methods[] = {
    ManagedMethod::RegisterNonVirtual<&fb_ctor>(CCFacebookWorker::clazz, "<init>", "()V"),
    ManagedMethod::Register<&fb_constructor>(
        CCFacebookWorker::clazz, "Constructor", "(Ljava/lang/String;)V"),
    ManagedMethod::Register<&fb_session_valid>(CCFacebookWorker::clazz, "GetSessionValid", "()Z"),
    ManagedMethod::Register<&fb_access_token>(
        CCFacebookWorker::clazz, "GetAccessToken", "()Ljava/lang/String;"),
    ManagedMethod::Register<&fb_login>(
        CCFacebookWorker::clazz, "Login", "([Ljava/lang/String;ZJJ)V"),
    ManagedMethod::Register<&fb_logout>(CCFacebookWorker::clazz, "Logout", "(JJ)V"),
    ManagedMethod::Register<&fb_permission_check>(
        CCFacebookWorker::clazz, "PermissionCheck", "([Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&fb_permission_grant>(
        CCFacebookWorker::clazz, "PermissionGrant", "([Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&fb_load_friend_vector>(
        CCFacebookWorker::clazz, "LoadFriendVector", "(JJ)V"),
    ManagedMethod::Register<&fb_feed_post>(
        CCFacebookWorker::clazz, "FeedPost",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;Ljava/lang/String;ZJJ)V"),
    ManagedMethod::Register<&fb_photo_post>(
        CCFacebookWorker::clazz, "PhotoPost", "(Ljava/lang/String;[BJJ)V"),
    ManagedMethod::Register<&fb_friend_invite>(
        CCFacebookWorker::clazz, "FriendInvite",
        "(Ljava/lang/String;Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&fb_decode_avatar>(
        CCFacebookWorker::clazz, "DecodeAvatar",
        "([BI)Lcom/firemonkeys/cloudcellapi/FacebookWorker$AvatarInfo;"),
    {NULL},
};

Class CCFacebookWorker::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/FacebookWorker",
    .classname = "FacebookWorker",
    .managed_methods = fb_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCFacebookWorker),
};
static const int fb_registered = ClassRegistry::register_class(CCFacebookWorker::clazz);

/* ------------------------------------------------------------------------ */
/* GooglePlayWorker                                                 */
/* ------------------------------------------------------------------------ */

class CCGooglePlusWorker : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

using GpLoginFn   = void (*)(JNIEnv *, jobject, jstring, jstring, jlong, jlong);
using GpPairFn    = void (*)(JNIEnv *, jobject, jlong, jlong);
using GpBoolFn    = void (*)(JNIEnv *, jobject, unsigned char, jlong, jlong);
using GpProfileFn = void (*)(JNIEnv *, jobject, unsigned char, jobjectArray, jlong, jlong);
using GpFriendsFn = void (*)(JNIEnv *, jobject, unsigned char, jobjectArray,
                             jobjectArray, jlong, jlong);

static void gp_ctor(JNIEnv *, jobject, jclass) {}
static void gp_constructor(JNIEnv *, jobject, jstring /*clientId*/, jboolean /*gamesApi*/) {}

/* m_pGoogleApiClient.isConnected() on a client that never connects. */
static jboolean gp_session_valid(JNIEnv *, jobject) { return JNI_FALSE; }
/* Polled every frame: answering TRUE would make the engine re-read a session
 * that never changes, forever. */
static jboolean gp_session_changed(JNIEnv *, jobject) { return JNI_FALSE; }
static jstring gp_person_id(JNIEnv *env, jobject) { return env->NewStringUTF(""); }
static jstring gp_person_name(JNIEnv *env, jobject) { return env->NewStringUTF(""); }

/* onConnectionFailed (GooglePlayWorker.java:468) answers
 * LoginCallback(STR_EMPTY, STR_EMPTY). An empty person id is this class's
 * "not signed in" - Facebook uses "0" for the same state, which is why the two
 * logins do not share a helper. */
static void gp_login(JNIEnv *env, jobject self, jlong cb, jlong user, jboolean)
{
    cloudcell_defer([env, self, cb, user]() {
        static GpLoginFn fn = cloudcell_native<GpLoginFn>(
            "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_NativeLoginCallback");
        if (!fn)
            return;
        jstring empty = env->NewStringUTF("");
        fn(env, self, empty, empty, cb, user);
    });
}

static void gp_logout(JNIEnv *env, jobject self, jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static GpPairFn fn = cloudcell_native<GpPairFn>(
            "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_LogoutCallback");
        if (fn)
            fn(env, self, cb, user);
    });
}

/* OnFriendsLoaded (line 277) initialises both arrays to new String[0] and
 * passes those on failure. */
static void gp_load_friend_vector(JNIEnv *env, jobject self, jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static GpFriendsFn fn = cloudcell_native<GpFriendsFn>(
            "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_"
            "LoadFriendVectorCallback");
        if (fn)
            fn(env, self, JNI_FALSE, cc_empty_string_array(env),
               cc_empty_string_array(env), cb, user);
    });
}

/* LoadProfile's own failure branch passes a null array, not an empty one. */
static void gp_load_profile(JNIEnv *env, jobject self, jstring, jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static GpProfileFn fn = cloudcell_native<GpProfileFn>(
            "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_"
            "LoadProfileCallback");
        if (fn)
            fn(env, self, JNI_FALSE, NULL, cb, user);
    });
}

static void gp_share(JNIEnv *env, jobject self, jstring, jstring, jstring, jstring,
                     jstring, jstring, jbyteArray, jstring, jstring, jstring,
                     jlong cb, jlong user)
{
    cloudcell_defer([env, self, cb, user]() {
        static GpBoolFn fn = cloudcell_native<GpBoolFn>(
            "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_ShareCallback");
        if (fn)
            fn(env, self, JNI_FALSE, cb, user);
    });
}

/* The achievement calls return early when the client is not connected and emit
 * no callback at all, so a no-op is the faithful behaviour, not a shortcut. */
static void gp_show_achievements(JNIEnv *, jobject) {}
static void gp_reset_achievements(JNIEnv *, jobject) {}
static void gp_unlock_achievement(JNIEnv *, jobject, jstring) {}

static const ManagedMethod gp_methods[] = {
    ManagedMethod::RegisterNonVirtual<&gp_ctor>(CCGooglePlusWorker::clazz, "<init>", "()V"),
    ManagedMethod::Register<&gp_constructor>(
        CCGooglePlusWorker::clazz, "Constructor", "(Ljava/lang/String;Z)V"),
    ManagedMethod::Register<&gp_session_valid>(
        CCGooglePlusWorker::clazz, "GetSessionValid", "()Z"),
    ManagedMethod::Register<&gp_session_changed>(
        CCGooglePlusWorker::clazz, "GetSessionChanged", "()Z"),
    ManagedMethod::Register<&gp_person_id>(
        CCGooglePlusWorker::clazz, "GetPersonId", "()Ljava/lang/String;"),
    ManagedMethod::Register<&gp_person_name>(
        CCGooglePlusWorker::clazz, "GetPersonName", "()Ljava/lang/String;"),
    ManagedMethod::Register<&gp_login>(CCGooglePlusWorker::clazz, "Login", "(JJZ)V"),
    ManagedMethod::Register<&gp_logout>(CCGooglePlusWorker::clazz, "Logout", "(JJ)V"),
    ManagedMethod::Register<&gp_load_friend_vector>(
        CCGooglePlusWorker::clazz, "LoadFriendVector", "(JJ)V"),
    ManagedMethod::Register<&gp_load_profile>(
        CCGooglePlusWorker::clazz, "LoadProfile", "(Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&gp_share>(
        CCGooglePlusWorker::clazz, "Share",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;Ljava/lang/String;[BLjava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&gp_show_achievements>(
        CCGooglePlusWorker::clazz, "ShowAchievements", "()V"),
    ManagedMethod::Register<&gp_reset_achievements>(
        CCGooglePlusWorker::clazz, "ResetAchievements", "()V"),
    ManagedMethod::Register<&gp_unlock_achievement>(
        CCGooglePlusWorker::clazz, "UnlockAchievement", "(Ljava/lang/String;)V"),
    {NULL},
};

Class CCGooglePlusWorker::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/GooglePlayWorker",
    .classname = "GooglePlayWorker",
    .managed_methods = gp_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCGooglePlusWorker),
};
static const int gp_registered = ClassRegistry::register_class(CCGooglePlusWorker::clazz);

/* ------------------------------------------------------------------------ */
/* CC_GoogleStoreServiceV3_Class                                             */
/* ------------------------------------------------------------------------ */

class CCGoogleStore : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* Constructor(String publicKey, long j .. long j8), field order taken
     * verbatim from CC_GoogleStoreServiceV3_Class.java:327. */
    jlong cb_initialize            = 0;
    jlong cb_product_details_ok    = 0;
    jlong cb_product_details_error = 0;
    jlong cb_purchase_ok           = 0;
    jlong cb_purchase_error        = 0;
    jlong cb_restore               = 0;
    jlong cb_refresh_purchases     = 0;
    jlong user_ptr                 = 0;
};

/* Every callback on this class is `private static native`, so the second
 * parameter is the class, not the instance. */
using GsInitializeFn      = void (*)(JNIEnv *, jclass, unsigned char, unsigned char,
                                     jlong, jlong);
using GsDetailsErrorFn    = void (*)(JNIEnv *, jclass, jlong, jstring, jlong, jlong);
using GsPurchaseErrorFn   = void (*)(JNIEnv *, jclass, jstring, jlong, jstring,
                                     jlong, jlong);
using GsRestoreFn         = void (*)(JNIEnv *, jclass, jlong, jstring, jint, jint,
                                     jlong, jlong);
using GsRefreshFn         = void (*)(JNIEnv *, jclass, unsigned char, jlong, jlong);

/* Consts.ResponseCode (Consts.java:82). Only the three the stub answers with
 * are named here. */
static const jlong kStoreResultOk               = 0;
static const jlong kStoreResultUserCanceled     = 1;
static const jlong kStoreResultBillingUnavail   = 3;

static void gs_ctor(JNIEnv *, jobject self, jclass) { new (self) CCGoogleStore(); }

static void gs_constructor(JNIEnv *, jobject self, jstring /*publicKey*/,
                           jlong initialize, jlong details_ok, jlong details_error,
                           jlong purchase_ok, jlong purchase_error, jlong restore,
                           jlong refresh, jlong user)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    st->cb_initialize            = initialize;
    st->cb_product_details_ok    = details_ok;
    st->cb_product_details_error = details_error;
    st->cb_purchase_ok           = purchase_ok;
    st->cb_purchase_error        = purchase_error;
    st->cb_restore               = restore;
    st->cb_refresh_purchases     = refresh;
    st->user_ptr                 = user;
}

static void gs_destructor(JNIEnv *, jobject self) { new (self) CCGoogleStore(); }

/*
 * (false, false) is the exact tuple onIabSetupFinished produces when
 * queryIntentServices finds no Play Store (Initialize(), line 382 -
 * RESULT_BILLING_UNAVAILABLE); both booleans are iabResult.isSuccess(). It is a
 * definitive answer, so the store manager stops waiting and marks itself
 * unavailable instead of gating the front-end on "store ready".
 */
static void gs_initialize(JNIEnv *env, jobject self)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    jlong cb = st->cb_initialize, user = st->user_ptr;

    cloudcell_defer([env, cb, user]() {
        static GsInitializeFn fn = cloudcell_native<GsInitializeFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_"
            "InitializeCallback");
        if (fn)
            fn(env, (jclass)&CCGoogleStore::clazz, JNI_FALSE, JNI_FALSE, cb, user);
    });
}

/* No callback exists in the Java for this one. */
static void gs_set_consumable_product_list(JNIEnv *, jobject, jobjectArray) {}

/*
 * Error 3 rather than an empty success: succeeding would require building a
 * SkuDetails[] whose elements ProductDetailsSucceedCallback then reads
 * getPriceAmountMicros / getPriceCurrencyCode / getSku / getPrice / getTitle /
 * getType off, which is a lot of surface for a store that does not exist.
 * "Billing unavailable" is simply true here.
 */
static void gs_get_product_details(JNIEnv *env, jobject self, jobjectArray)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    jlong cb = st->cb_product_details_error, user = st->user_ptr;

    cloudcell_defer([env, cb, user]() {
        static GsDetailsErrorFn fn = cloudcell_native<GsDetailsErrorFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_"
            "ProductDetailsErrorCallback");
        if (fn)
            fn(env, (jclass)&CCGoogleStore::clazz, kStoreResultBillingUnavail,
               env->NewStringUTF("Billing service unavailable on device."), cb, user);
    });
}

/* USER_CANCELED, not a hard failure: onPurchaseFinished routes both into
 * PurchaseErrorCallback, but a front-end typically raises an error dialog for a
 * hard failure and stays silent on a cancel, and this port has no dialog. */
static void gs_purchase(JNIEnv *env, jobject self, jstring sku, jlong, jlong)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    jlong cb = st->cb_purchase_error, user = st->user_ptr;

    cloudcell_defer([env, sku, cb, user]() {
        static GsPurchaseErrorFn fn = cloudcell_native<GsPurchaseErrorFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_"
            "PurchaseErrorCallback");
        if (fn)
            fn(env, (jclass)&CCGoogleStore::clazz, sku, kStoreResultUserCanceled,
               env->NewStringUTF("User canceled."), cb, user);
    });
}

/* A clean restore of zero items. onQueryRestorePurchasesFinished passes the
 * response code, the number of newly restored packs and the inventory size; a
 * successful empty restore avoids the error path entirely. If a later run shows
 * the game re-prompting, RESULT_ERROR (6) is the alternative. */
static void gs_restore_purchase(JNIEnv *env, jobject self)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    jlong cb = st->cb_restore, user = st->user_ptr;

    cloudcell_defer([env, cb, user]() {
        static GsRestoreFn fn = cloudcell_native<GsRestoreFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_"
            "RestoreCallback");
        if (fn)
            fn(env, (jclass)&CCGoogleStore::clazz, kStoreResultOk,
               env->NewStringUTF("RESULT_OK"), 0, 0, cb, user);
    });
}

/* Java sets its boolean only when getPurchases returned RESULT_OK; a
 * RemoteException leaves it false. */
static void gs_refresh_store_purchases(JNIEnv *env, jobject self)
{
    CCGoogleStore *st = (CCGoogleStore *)self;
    jlong cb = st->cb_refresh_purchases, user = st->user_ptr;

    cloudcell_defer([env, cb, user]() {
        static GsRefreshFn fn = cloudcell_native<GsRefreshFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_"
            "RefreshStorePurchasesCallback");
        if (fn)
            fn(env, (jclass)&CCGoogleStore::clazz, JNI_FALSE, cb, user);
    });
}

static const ManagedMethod gs_methods[] = {
    ManagedMethod::RegisterNonVirtual<&gs_ctor>(CCGoogleStore::clazz, "<init>", "()V"),
    ManagedMethod::Register<&gs_constructor>(
        CCGoogleStore::clazz, "Constructor", "(Ljava/lang/String;JJJJJJJJ)V"),
    ManagedMethod::Register<&gs_destructor>(CCGoogleStore::clazz, "Destructor", "()V"),
    ManagedMethod::Register<&gs_initialize>(CCGoogleStore::clazz, "Initialize", "()V"),
    ManagedMethod::Register<&gs_set_consumable_product_list>(
        CCGoogleStore::clazz, "setConsumableProductList", "([Ljava/lang/String;)V"),
    ManagedMethod::Register<&gs_get_product_details>(
        CCGoogleStore::clazz, "getProductDetails", "([Ljava/lang/String;)V"),
    ManagedMethod::Register<&gs_purchase>(
        CCGoogleStore::clazz, "Purchase", "(Ljava/lang/String;JJ)V"),
    ManagedMethod::Register<&gs_restore_purchase>(
        CCGoogleStore::clazz, "RestorePurchase", "()V"),
    ManagedMethod::Register<&gs_refresh_store_purchases>(
        CCGoogleStore::clazz, "RefreshStorePurchases", "()V"),
    {NULL},
};

Class CCGoogleStore::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/CC_GoogleStoreServiceV3_Class",
    .classname = "CC_GoogleStoreServiceV3_Class",
    .managed_methods = gs_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCGoogleStore),
};
static const int gs_registered = ClassRegistry::register_class(CCGoogleStore::clazz);

/* ------------------------------------------------------------------------ */
/* CC_GoogleAdManager_Class                                                  */
/* ------------------------------------------------------------------------ */

class CCGoogleAdManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

using AdInterstitialFn = void (*)(JNIEnv *, jobject, jlong);
using AdBannerFn       = void (*)(JNIEnv *, jobject, jlong, jstring);

static void ad_ctor(JNIEnv *, jobject, jclass) {}
static void ad_constructor(JNIEnv *, jobject) {}

/*
 * "Failed" is not a shortcut, it is the branch the real Java takes on a device
 * where ads cannot run: AreAdsSupported() is Build.VERSION.SDK_INT >= 16, and
 * when it is false DisplayInterstitial calls OnInterstitialFailed(j) and
 * returns (line 172), DisplayBanner calls OnBannerFailed(j, str) and returns
 * (line 107). Any other answer - displayed, clicked, dismissed - implies an
 * overlay that does not exist and leaves the ad manager waiting for a close
 * event that cannot arrive.
 */
static void ad_display_interstitial(JNIEnv *env, jobject self, jstring, jobject, jlong cb)
{
    cloudcell_defer([env, self, cb]() {
        static AdInterstitialFn fn = cloudcell_native<AdInterstitialFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_"
            "OnInterstitialFailed");
        if (fn)
            fn(env, self, cb);
    });
}

/* DisplayBanner(String str, String str2, Bundle, long j, boolean z): str is the
 * banner key that indexes m_adViews and is echoed back by every OnBanner*
 * callback; str2 is the AdMob ad-unit id. The callback takes the first. */
static void ad_display_banner(JNIEnv *env, jobject self, jstring key, jstring,
                              jobject, jlong cb, jboolean)
{
    cloudcell_defer([env, self, key, cb]() {
        static AdBannerFn fn = cloudcell_native<AdBannerFn>(
            "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_OnBannerFailed");
        if (fn)
            fn(env, self, cb, key);
    });
}

static void ad_remove_banner(JNIEnv *, jobject, jstring) {}
static jboolean ad_ads_supported(JNIEnv *, jobject) { return JNI_FALSE; }
static void ad_lifecycle(JNIEnv *, jobject) {}

static const ManagedMethod ad_methods[] = {
    ManagedMethod::RegisterNonVirtual<&ad_ctor>(CCGoogleAdManager::clazz, "<init>", "()V"),
    ManagedMethod::Register<&ad_constructor>(CCGoogleAdManager::clazz, "Constructor", "()V"),
    ManagedMethod::Register<&ad_display_interstitial>(
        CCGoogleAdManager::clazz, "DisplayInterstitial",
        "(Ljava/lang/String;Landroid/os/Bundle;J)V"),
    ManagedMethod::Register<&ad_display_banner>(
        CCGoogleAdManager::clazz, "DisplayBanner",
        "(Ljava/lang/String;Ljava/lang/String;Landroid/os/Bundle;JZ)V"),
    ManagedMethod::Register<&ad_remove_banner>(
        CCGoogleAdManager::clazz, "RemoveBanner", "(Ljava/lang/String;)V"),
    ManagedMethod::Register<&ad_ads_supported>(
        CCGoogleAdManager::clazz, "AreAdsSupported", "()Z"),
    ManagedMethod::Register<&ad_lifecycle>(CCGoogleAdManager::clazz, "onPause", "()V"),
    ManagedMethod::Register<&ad_lifecycle>(CCGoogleAdManager::clazz, "onResume", "()V"),
    ManagedMethod::Register<&ad_lifecycle>(CCGoogleAdManager::clazz, "onDestroy", "()V"),
    {NULL},
};

Class CCGoogleAdManager::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/CC_GoogleAdManager_Class",
    .classname = "CC_GoogleAdManager_Class",
    .managed_methods = ad_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCGoogleAdManager),
};
static const int ad_registered = ClassRegistry::register_class(CCGoogleAdManager::clazz);

/* ------------------------------------------------------------------------ */
/* CC_GCM_Helper_Class                                                       */
/* ------------------------------------------------------------------------ */

class CCGcmHelper : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

static void gcm_ctor(JNIEnv *, jobject, jclass) {}
static void gcm_set_sender_id(JNIEnv *, jobject, jstring) {}

/*
 * The only method in this file that deliberately never answers.
 *
 * RegisterApplicationForPushNotifications wraps GCMRegistrar.checkDevice /
 * checkManifest / register in a try, and the catch logs "GCM Couldn't register
 * application for PN" and returns without calling RegisterCallback
 * (CC_GCM_Helper_Class.java:27-29). On a device with no Play Services that is
 * exactly what happens, and CC_PushNotificationManager_Class is written to
 * tolerate it. Firing RegisterCallback here would invent a code path the engine
 * has never seen; the escape hatch, if a later run shows the push manager
 * blocking, is RegisterCallback("") with an empty registration id.
 */
static void gcm_register(JNIEnv *, jobject) {}

static const ManagedMethod gcm_methods[] = {
    ManagedMethod::RegisterNonVirtual<&gcm_ctor>(CCGcmHelper::clazz, "<init>", "()V"),
    ManagedMethod::Register<&gcm_set_sender_id>(
        CCGcmHelper::clazz, "setSenderID", "(Ljava/lang/String;)V"),
    ManagedMethod::Register<&gcm_register>(
        CCGcmHelper::clazz, "RegisterApplicationForPushNotifications", "()V"),
    {NULL},
};

Class CCGcmHelper::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/CC_GCM_Helper_Class",
    .classname = "CC_GCM_Helper_Class",
    .managed_methods = gcm_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCGcmHelper),
};
static const int gcm_registered = ClassRegistry::register_class(CCGcmHelper::clazz);

/* ------------------------------------------------------------------------ */
/* AppPromptManager                                                 */
/* ------------------------------------------------------------------------ */

class CCAppPromptManager : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

using PromptFn = void (*)(JNIEnv *, jobject);

static void prompt_ctor(JNIEnv *, jobject, jclass) {}

/*
 * DisplayRateAppDialog's four strings are message / positive / neutral /
 * negative labels, and the Java has no exit from it other than one of the three
 * button callbacks - the native side is waiting on exactly one of them.
 *
 * OnDontRate rather than OnRateApp (which also runs OpenStorePage() on a real
 * device) or OnRemindLater (which would re-prompt on every boot of a console
 * that cannot show the dialog at all). If OnDontRate turns out to write
 * something undesirable into the save, OnRemindLater is the drop-in swap.
 */
static void prompt_display_rate_app(JNIEnv *env, jobject self, jstring, jstring,
                                    jstring, jstring)
{
    cloudcell_defer([env, self]() {
        static PromptFn fn = cloudcell_native<PromptFn>(
            "Java_com_firemonkeys_cloudcellapi_AppPromptManager_OnDontRate");
        if (fn)
            fn(env, self);
    });
}

static void prompt_dismiss_rate_app(JNIEnv *, jobject) {}
static void prompt_open_store_page(JNIEnv *, jobject) {}

static const ManagedMethod prompt_methods[] = {
    ManagedMethod::RegisterNonVirtual<&prompt_ctor>(CCAppPromptManager::clazz, "<init>", "()V"),
    ManagedMethod::Register<&prompt_display_rate_app>(
        CCAppPromptManager::clazz, "DisplayRateAppDialog",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::Register<&prompt_dismiss_rate_app>(
        CCAppPromptManager::clazz, "DismissRateAppDialog", "()V"),
    ManagedMethod::Register<&prompt_open_store_page>(
        CCAppPromptManager::clazz, "OpenStorePage", "()V"),
    {NULL},
};

Class CCAppPromptManager::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/AppPromptManager",
    .classname = "AppPromptManager",
    .managed_methods = prompt_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCAppPromptManager),
};
static const int prompt_registered = ClassRegistry::register_class(CCAppPromptManager::clazz);

/* ------------------------------------------------------------------------ */
/* Consts                                                                    */
/* ------------------------------------------------------------------------ */

class CCConsts : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

/* The only member of Consts the native layer touches is the static
 * enableLog()V (Consts.java:115), which flips a Java-side logging flag. */
static void consts_enable_log(JNIEnv *, jclass) {}

static const ManagedMethod consts_methods[] = {
    ManagedMethod::RegisterStatic<&consts_enable_log>(CCConsts::clazz, "enableLog", "()V"),
    {NULL},
};

Class CCConsts::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/Consts",
    .classname = "Consts",
    .managed_methods = consts_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    /* Never instantiated - Consts is a bag of statics. */
    .instance_size = 0,
};
static const int consts_registered = ClassRegistry::register_class(CCConsts::clazz);

/*
 * com/popcap/ea2/EASquared is deliberately NOT registered.
 *
 * EASquaredFactoryAndroid::MeetsSystemRequirements (0x962938) is its only user:
 * FindClass, then GetStaticMethodID("MeetsSystemRequirements", "()Z"), and on
 * either failure it logs and returns false. A false return makes
 * ThirdPartyAdvertisingManager::CreateEASquared build EASquaredNullImpl, a real
 * class in this binary - which is precisely the offline, no-ads behaviour this
 * port wants. So "Unable to find class for EASquared" in the log is the desired
 * outcome, already achieved.
 *
 * Registering it would only silence the log line, and would have to answer
 * JNI_FALSE anyway; answering true would drag in com/popcap/ea2/Ultra,
 * UltraDelegate, AdColonyAdProvider and BrandConnectAdProvider, none of which
 * exist here. Do not "fix" this.
 */
