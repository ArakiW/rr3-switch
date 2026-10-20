#include <stdio.h>
#include <new>
#include <string.h>
#include <vector>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/so_util_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "cc_cloudcell_auth.h"
#include "cloudcell_defer.h"
#include "cloudcell_natives.h"
#include "rr3/guest_view_trace.hpp"

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
 * THE FAKE SERVER, added 2026-10-21.
 *
 * The guest's account manager keeps a server-issued device id, it stays 0, and
 * the guest logs "CC: Invalid DeviceId! Have we connected to cloudcell yet?"
 * forever, which keeps the sync queue at 13 items and the main menu
 * unreachable. The device id comes from the Cloudcell LOGIN response body, so
 * this class now answers exactly one request shape - the authenticate request -
 * with a hand-built AUTHENTICATE_STATE_AUTHENTICATE_SUCCESS payload, and
 * leaves every other request failing honestly.
 *
 * The delivery is the Java-side contract, not a guess:
 *
 *   HttpThread.run() (reverse/java/sources/com/firemonkeys/cloudcellapi/
 *   HttpThread.java:92-104, :319-321) is the reference order:
 *       headerCallback(ptr, contentLength, headers)
 *       dataCallback(ptr, bytes, len)      once per read
 *       completeCallback(ptr, httpStatus)   at the end, on success
 *       errorCallback(ptr, httpStatus)      at the end, on failure
 *
 * The guest exports all four natives, and the port already calls
 * errorCallback, so the success pair exists:
 *
 *   Java_com_firemonkeys_cloudcellapi_HttpRequest_dataCallback
 *       guest raw 0x1ce493c, signature (JNIEnv*, jobject, jlong, jbyteArray,
 *       jint). Read from the disassembly: it mallocs a buffer of the given
 *       length, copies the byte array into it through the JNIEnv vtable slot
 *       0x640 (GetByteArrayElements), calls the worker's vtable[4] with
 *       (worker, buffer, length) and frees the buffer.
 *   Java_com_firemonkeys_cloudcellapi_HttpRequest_completeCallback
 *       guest raw 0x1ce4a3c, signature (JNIEnv*, jobject, jlong, jint). Read
 *       from the disassembly: it tail-calls the shared OnJNICompletion with
 *       success = true and the status, which is the counterpart of the
 *       errorCallback's success = false.
 *
 * headerCallback is deliberately NOT called: the guest's own success path
 * would set the response content type, and the assertion at 0x1ce3fd4
 * (AndroidHttpRequestWorker.cpp:147) compares a string the worker derives
 * from that header against the one it expects. Leaving the header alone keeps
 * the inputs of that comparison exactly as they are on the error path the
 * port has been taking all along.
 */

/* The body the guest hands to init(). The engine builds it with the guest's
 * BinaryBlob writer (raw 0x1cbdd28), so it is a flat sequence of
 * little-endian 32-bit words and length-prefixed byte strings. It is small
 * (the authenticate request is a few hundred bytes), so it is kept in a
 * fixed buffer: no heap, no destructor, and no lifetime question for an
 * object the class registry allocated with calloc. */
static const size_t kMaxBodyBytes = 4096;

class CCHttpRequest : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    /* The CC_AndroidHttpRequestWorker_Class* the engine passes to init() as the
     * "long" of (…[BIJZ)V; every callback on this class takes it back. */
    jlong    callback = 0;
    jboolean closed   = JNI_FALSE;
    jboolean pending  = JNI_FALSE;

    uint8_t  body[kMaxBodyBytes];
    size_t   bodyLen  = 0;
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

/* Java: public native void dataCallback(long j, byte[] bArr, int i) and
 * public native void completeCallback(long j, int i). Both are instance
 * methods, so the object is the second parameter. The argument order is the
 * Java one, and the guest's own implementations read it that way (see the
 * file comment). */
using CCHttpDataFn     = void (*)(JNIEnv *, jobject, jlong, jbyteArray, jint);
using CCHttpCompleteFn = void (*)(JNIEnv *, jobject, jlong, jint);

/* The member id the guest compares the response against when the request
 * carries none. The guest compares it at 0x1d01dc4 against a read-only
 * constant of 1 at guest raw 0x600f30 (through the GOT slot 0x1f6e270), so 1
 * is the value that satisfies that comparison. */
static const uint32_t kDefaultMemberId = 1u;

/* Bounded log budget, in the spirit of the other Cloudcell probes (a few
 * dozen lines for a whole run). */
static const unsigned kMaxAuthLogLines = 24;

static bool authLogBudget(void)
{
    static unsigned s_lines = 0;
    if (s_lines >= kMaxAuthLogLines) {
        return false;
    }
    ++s_lines;
    return true;
}

static void http_ctor(JNIEnv *, jobject self, jclass)
{
    /* NewObject callocs and never runs a constructor (jni/jni.cpp:216), so the
     * vptr is zero and any GetObjectClass on the result would dereference it. */
    new (self) CCHttpRequest();
}

static void http_init(JNIEnv *env, jobject self, jstring /*url*/, jstring /*method*/,
                      jstring /*userAgent*/, jbyteArray body,
                      jint /*readCapacity*/, jlong callback,
                      jboolean /*failOnErrorStatus*/)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->callback = callback;
    req->closed   = JNI_FALSE;
    req->pending  = JNI_FALSE;

    /* Keep the body. Without it http_post() cannot tell the authenticate
     * request from the sync request, and the two share the same URL path
     * (CC_Sync.php, guest raw 0x3e3551) and the same first four words. */
    req->bodyLen = 0;
    if (body != nullptr) {
        jsize len = env->GetArrayLength(body);
        if (len > 0) {
            size_t take = ((size_t)len < kMaxBodyBytes) ? (size_t)len : kMaxBodyBytes;
            env->GetByteArrayRegion(body, 0, (jsize)take,
                                    (jbyte *)req->body);
            req->bodyLen = take;
        }
    }

    /* One line per request, so "the guest never calls this class" is a
     * MEASURED statement and not an inference from the absence of the
     * http.post deferral line. */
    if (authLogBudget()) {
        trace("cc-http-init: body=%zu", (unsigned)req->bodyLen);
    }
}

static void http_add_header(JNIEnv *, jobject, jstring, jstring) {}

/*
 * Build the authenticate response and hand it to the guest the way
 * HttpThread.run() would: the body first, then the completion.
 *
 * Returns false when the response could not be built or delivered, in which
 * case the caller falls back to the honest failure.
 */
static bool deliverAuthSuccess(JNIEnv *env, CCHttpRequest *req,
                                const cccloud::AuthRequest &info)
{
    static CCHttpDataFn data_callback = cloudcell_native<CCHttpDataFn>(
        kCCHttpDataCallback);
    static CCHttpCompleteFn complete_callback = cloudcell_native<CCHttpCompleteFn>(
        kCCHttpCompleteCallback);

    if (data_callback == nullptr || complete_callback == nullptr) {
        warning("HttpRequest: success natives missing (data=%p complete=%p); "
                "request %p stays pending\n",
                (void *)data_callback, (void *)complete_callback, (void *)req);
        return false;
    }

    const uint32_t memberId = info.hasMemberId ? info.memberId : kDefaultMemberId;

    std::vector<uint8_t> body;
    if (!cccloud::buildAuthResponse(memberId, body)) {
        warning("HttpRequest: could not build the authenticate response\n");
        return false;
    }

    if (authLogBudget()) {
        trace("cc-auth-resp: produced member=%u body=%zu headers=%zu consumed=%zu",
              memberId, (unsigned)body.size(), (unsigned)info.headerCount,
              (unsigned)info.consumed);
    }
    rr3::guestViewWideLog("cc-auth-resp", memberId, (unsigned long long)body.size(),
                           "authenticate response built for the guest");

    jbyteArray array = env->NewByteArray((jsize)body.size());
    if (array == nullptr) {
        warning("HttpRequest: could not allocate the response byte array\n");
        return false;
    }
    env->SetByteArrayRegion(array, 0, (jsize)body.size(),
                             (const jbyte *)body.data());

    data_callback(env, (jobject)req, req->callback, array, (jint)body.size());
    complete_callback(env, (jobject)req, req->callback, 200);

    if (authLogBudget()) {
        trace("cc-auth-sent: member=%u bytes=%zu status=200",
              memberId, (unsigned)body.size());
    }
    rr3::guestViewWideLog("cc-auth-sent", memberId, (unsigned long long)body.size(),
                           "authenticate response delivered to the guest");
    return true;
}

static void http_post(JNIEnv *env, jobject self)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->pending = JNI_TRUE;

    cloudcell_defer("http.post", [env, req]() {
        if (!req->pending || req->closed)
            return;
        req->pending = JNI_FALSE;

        cccloud::AuthRequest info;
        const bool authShaped =
            cccloud::parseAuthRequest(req->body, req->bodyLen, info);

        if (authShaped) {
            if (authLogBudget()) {
                trace("cc-auth-req: matched env=%u,%u x=%u headers=%zu body=%zu "
                      "consumed=%zu member=%u",
                      info.env1, info.env2, info.clientId,
                      (unsigned)info.headerCount, (unsigned)info.bodySize,
                      (unsigned)info.consumed,
                      info.hasMemberId ? info.memberId : kDefaultMemberId);
            }
            if (deliverAuthSuccess(env, req, info)) {
                return;
            }
        } else if (authLogBudget()) {
            trace("cc-auth-req: not the authenticate request body=%zu",
                  (unsigned)req->bodyLen);
        }

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
