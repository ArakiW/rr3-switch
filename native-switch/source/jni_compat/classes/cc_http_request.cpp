#include <stdio.h>
#include <new>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/so_util_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "cloudcell_defer.h"
#include "cloudcell_natives.h"

/*
 * com.firemonkeys.cloudcellapi.HttpRequest - the load-bar blocker.
 *
 * Measured before writing this: over a 3.89 M-line run the engine created
 * exactly two of these (emulator.log:11598 and :11988) and neither ever
 * retired. CC_HttpRequestManager_Class::QueueRequest (0x93b0a4) appends an
 * ActiveRequest_Struct and calls BeginPostUnlocked (0x93a844), which builds a
 * CC_AndroidHttpRequestWorker_Class (0x957c50); that constructor is what calls
 * init(), addHeader() and post() on the Java object. The only thing that ever
 * removes a request from the active vector is the completion byte at req+0x48,
 * written by the manager's CompletionCallback (0x93a95a), which is only reached
 * from this class's completeCallback/errorCallback natives. With no Java class
 * the lookups returned NULL, post() was a no-op, and the sync/licence requests
 * stayed pending forever while the asset manager sat on the load bar.
 *
 * The stub answers "the transfer failed" rather than "the transfer finished".
 * errorCallback(ptr, 0) is literally what HttpThread.run() emits when
 * openConnection()/getInputStream() throws - responseCode is still 0 at that
 * point (HttpThread.java:143) - i.e. the no-network path the engine already
 * knows how to unwind. completeCallback would instead hand a zero-length body
 * to CC_SyncManager_Class::HttpPostCallback and friends, which then parse it.
 */
class CCHttpRequest : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* The CC_AndroidHttpRequestWorker_Class* the engine passes to init() as the
     * "long" of (…[BIJZ)V; every callback on this class takes it back. */
    jlong    callback = 0;
    jboolean closed   = JNI_FALSE;
    jboolean pending  = JNI_FALSE;
};

/* Java: public native void errorCallback(long j, int i) - an instance method,
 * so the second parameter is the object. Verified against the disassembly of
 * Java_com_firemonkeys_cloudcellapi_HttpRequest_errorCallback at
 * 0x957bd0, which ignores r1 entirely and does:
 *     mov r0, r2          ; low half of the jlong = the worker
 *     ldr r3, [r0]        ; worker vtable
 *     ldr r3, [r3, #20]   ; +0x14 = OnJNICompletion(bool, int)
 *     mov r1, #0          ; success = false
 * The real object is passed anyway rather than NULL, because that is what the
 * JVM would do and it costs nothing. */
using CCHttpErrorFn = void (*)(JNIEnv *, jobject, jlong, jint);

static void http_ctor(JNIEnv *, jobject self, jclass)
{
    /* NewObject callocs and never runs a constructor (jni/jni.cpp:216), so the
     * vptr is zero and any GetObjectClass on the result would dereference it. */
    new (self) CCHttpRequest();
}

static void http_init(JNIEnv *, jobject self, jstring /*url*/, jstring /*method*/,
                      jstring /*userAgent*/, jbyteArray /*body*/,
                      jint /*readCapacity*/, jlong callback,
                      jboolean /*failOnErrorStatus*/)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->callback = callback;
    req->closed   = JNI_FALSE;
    req->pending  = JNI_FALSE;
}

static void http_add_header(JNIEnv *, jobject, jstring, jstring) {}

static void http_post(JNIEnv *env, jobject self)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->pending = JNI_TRUE;

    cloudcell_defer([env, req]() {
        if (!req->pending || req->closed)
            return;
        req->pending = JNI_FALSE;

        static CCHttpErrorFn error_callback = cloudcell_native<CCHttpErrorFn>(
            "Java_com_firemonkeys_cloudcellapi_HttpRequest_errorCallback");
        if (!error_callback) {
            warning("HttpRequest: errorCallback export missing; "
                    "request %p stays pending\n", (void *)req);
            return;
        }
        error_callback(env, (jobject)req, req->callback, 0);
    });
}

/* CC_AndroidHttpRequestWorker_Class::OnJNICompletion (0x957948) asserts via
 * cc_android_assert_log if isClosed() answers true on entry, then stores the
 * status and calls close() itself before running OnCompletion. So the object
 * must still read "open" when the deferred callback fires, and closing it is
 * the engine's job, not ours. */
static jboolean http_is_closed(JNIEnv *, jobject self)
{
    return ((CCHttpRequest *)self)->closed;
}

static void http_close(JNIEnv *, jobject self)
{
    ((CCHttpRequest *)self)->closed = JNI_TRUE;
}

static void http_shutdown(JNIEnv *, jobject self)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->closed  = JNI_TRUE;
    req->pending = JNI_FALSE;
}

static const ManagedMethod http_methods[] = {
    ManagedMethod::RegisterNonVirtual<&http_ctor>(CCHttpRequest::clazz, "<init>", "()V"),
    ManagedMethod::Register<&http_init>(
        CCHttpRequest::clazz, "init",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BIJZ)V"),
    ManagedMethod::Register<&http_add_header>(
        CCHttpRequest::clazz, "addHeader",
        "(Ljava/lang/String;Ljava/lang/String;)V"),
    ManagedMethod::Register<&http_post>(CCHttpRequest::clazz, "post", "()V"),
    ManagedMethod::Register<&http_close>(CCHttpRequest::clazz, "close", "()V"),
    ManagedMethod::Register<&http_is_closed>(CCHttpRequest::clazz, "isClosed", "()Z"),
    ManagedMethod::Register<&http_shutdown>(CCHttpRequest::clazz, "shutdown", "()V"),
    {NULL},
};

Class CCHttpRequest::clazz = {
    .classpath = "com/firemonkeys/cloudcellapi/HttpRequest",
    .classname = "HttpRequest",
    .managed_methods = http_methods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(CCHttpRequest),
};

static const int http_registered = ClassRegistry::register_class(CCHttpRequest::clazz);
