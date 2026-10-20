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
#include "cc_http_headers.h"
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

/* ------------------------------------------------------------------ *
 * The response-header facade (cc_http_headers.h)
 * ------------------------------------------------------------------ */

namespace ccheaders {

/*
 * The server time the port answers the /time request with.
 *
 * ASCII decimal seconds, because the guest's parser converts the header value
 * with strtod() (guest raw 0x1cef9d0 -> PLT strtod at 0x1e18b80) and stores
 * the result as a double at this+0x88. 1793000000.125000 is
 * 2026-10-24T00:53:20Z: a plausible epoch, comfortably positive (the guest
 * treats a non-positive server time as "no answer" and never logs it).
 */
const Entry kServerTime = { "X-Request-Start", "1793000000.125000" };

namespace {

/* "RCHTTP1" - lets the JNI hooks recognise the port's own header objects
 * among the pool's other residents without a class lookup. */
constexpr uint64_t kHeaderMagic = 0x5243485454503031ull;

enum Kind : uint32_t {
    kKindMap = 1,
    kKindSet = 2,
    kKindIterator = 3,
    kKindList = 4,
};

/*
 * One object of the java.util.Map / Set / Iterator / List the guest's native
 * headerCallback walks.
 *
 * All four share one layout on purpose: the guest only ever reaches them
 * through the JNIEnv, and the port answers those calls by name plus the
 * receiver, so one type with a kind tag is enough and the port needs no
 * entry in the class registry for java.util.
 */
struct HeaderObj {
    uint64_t    magic;
    uint32_t    kind;
    uint32_t    index;    /* the iterator's cursor */
    const Entry* entries;
    uint32_t    count;
};

bool nameIs(const char* name, const char* wanted)
{
    if (name == nullptr) {
        return false;
    }
    return strcmp(name, wanted) == 0;
}

const char* classNameFor(Kind kind)
{
    switch (kind) {
    case kKindMap:      return "java/util/Map";
    case kKindSet:      return "java/util/Set";
    case kKindIterator: return "java/util/Iterator";
    case kKindList:     return "java/util/List";
    default:            return nullptr;
    }
}

HeaderObj* asHeader(uintptr_t object)
{
    if (object == 0) {
        return nullptr;
    }
    HeaderObj* obj = reinterpret_cast<HeaderObj*>(object);
    return obj->magic == kHeaderMagic ? obj : nullptr;
}

uintptr_t makeHeader(Kind kind, const Entry* entries, uint32_t count)
{
    const char* className = classNameFor(kind);
    if (className == nullptr) {
        return 0;
    }
    const uintptr_t handle = classHandle(className);
    if (handle == 0) {
        return 0;
    }
    const uintptr_t object = allocObject(handle);
    if (object == 0) {
        return 0;
    }
    new (reinterpret_cast<void*>(object))
        HeaderObj{ kHeaderMagic, static_cast<uint32_t>(kind), 0u, entries, count };
    return object;
}

} // namespace

uintptr_t createMap(const Entry* entries, size_t count)
{
    return makeHeader(kKindMap, entries, static_cast<uint32_t>(count));
}

/*
 * The guest's native headerCallback walks the map it is given:
 *
 *   keySet()          -> a Set
 *   iterator()        -> an Iterator
 *   hasNext()         -> true while the cursor is inside the entries
 *   next()            -> the next header NAME, as a Java string
 *   map.get(name)     -> a List of that header's values
 *   list.size()       -> how many values the list holds
 *   list.get(index)   -> the value, as a Java string
 *
 * Every one of those is answered here, by name and by receiver. The port's
 * own name-keyed fallbacks would answer "keySet", "iterator" and "get" with
 * a shared dummy object (jni_bridge.cpp's fakeObjectHandle), which is what
 * made the header map empty and the server time unreadable.
 */
bool handleObject(uintptr_t object, const char* name, uintptr_t* out)
{
    HeaderObj* obj = asHeader(object);
    if (obj == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    switch (obj->kind) {
    case kKindMap:
        if (nameIs(name, "keySet")) {
            *out = makeHeader(kKindSet, obj->entries, obj->count);
            return *out != 0;
        }
        if (nameIs(name, "get")) {
            /* The port answers with one header, so every key maps to the
             * same single-value list. The guest only asks for the keys the
             * port itself handed out. */
            *out = makeHeader(kKindList, obj->entries, obj->count);
            return *out != 0;
        }
        return false;
    case kKindSet:
        if (nameIs(name, "iterator")) {
            *out = makeHeader(kKindIterator, obj->entries, obj->count);
            return *out != 0;
        }
        return false;
    case kKindIterator:
        if (nameIs(name, "next")) {
            if (obj->index >= obj->count) {
                *out = 0;
                return true;
            }
            *out = makeString(obj->entries[obj->index].name);
            obj->index += 1u;
            return true;
        }
        return false;
    case kKindList:
        if (nameIs(name, "get")) {
            *out = (obj->count > 0u) ? makeString(obj->entries[0].value) : 0u;
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool handleBoolean(uintptr_t object, const char* name, bool* out)
{
    HeaderObj* obj = asHeader(object);
    if (obj == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    if (obj->kind == kKindIterator && nameIs(name, "hasNext")) {
        *out = obj->index < obj->count;
        return true;
    }
    return false;
}

bool handleInt(uintptr_t object, const char* name, int32_t* out)
{
    HeaderObj* obj = asHeader(object);
    if (obj == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    if (obj->kind == kKindList && nameIs(name, "size")) {
        *out = static_cast<int32_t>(obj->count);
        return true;
    }
    return false;
}

} // namespace ccheaders

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

    /*
     * The request URL, which is what tells the /time request apart from the
     * authenticate request.
     *
     * The guest builds the URL at runtime (snprintf("%04u-%s%s.cloudcell.com",
     * envId, role, stage) at guest raw 0x1cf0e60) and the path is appended by
     * the caller, so the two requests do NOT share a path: the guest's own log
     * in the measured run prints
     *     CC: [1] POST request [https://connect-dev-ss.cloudcell.com:443/time]
     * while the authenticate request goes to CC_Sync.php. The URL is the only
     * reliable discriminator, because the port cannot read the request body
     * at all: the guest's byte[] is not a port array object, so
     * GetArrayLength answers 0 for every request (the measured
     * "cc-http-init: body=0").
     */
    char     url[256];
    size_t   urlLen = 0;
};

/* This facade is dispatched onto an object that the port's fake-object pool
 * handed out (jni_bridge.cpp newFakeObject), and that pool gives every
 * instance a FIXED stride - it does not size the slot from
 * Class::instance_size. The inline body above is 4 KB, so a pool that grants
 * less than that would let init() write over the neighbouring slots and their
 * headers. Checked here rather than discovered at runtime. */
static_assert(sizeof(CCHttpRequest) <= kFakeObjectInstanceBytes,
              "CCHttpRequest does not fit in the port's fake-object instance "
              "space; raise kFakeObjectInstanceBytes");

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

/* Java: public native void headerCallback(long j, int i, Map map) - the
 * response HEADERS. The guest's own implementation (guest raw 0x1ce4348)
 * walks the java.util.Map it is given with "keySet", "iterator",
 * "hasNext", "next", "get" and "size", and stores every (name, value) pair
 * in the guest's own map at worker+0x180 - which is the map the
 * ServerTimeManager then reads the server time out of.
 *
 * The signature is the guest's own: (JNIEnv*, jobject, jlong, jint, jobject).
 * The int is the content length and the jobject is the Map. */
using CCHttpHeaderFn   = void (*)(JNIEnv *, jobject, jlong, jint, jobject);

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

/* Java: public void init(String userAgent, String method, String url,
 *                        byte[] body, int readCapacity, long callback,
 *                        boolean, double, boolean, boolean, int timeoutSeconds)
 *
 * The parameter ORDER is the guest's, read out of the decompiled
 * com/firemonkeys/cloudcellapi/HttpRequest.java:
 *
 *     public void init(String str, String str2, String str3, byte[] bArr,
 *                      int i, long j, boolean z, double d, boolean z2,
 *                      boolean z3, int i2) {
 *         this.m_serverTime = d;
 *         this.m_bSSLCheck = z2;
 *         if (i2 > 0) { this.m_TimeoutMilliseconds = i2 * 1000; }
 *         initSSLContext();
 *         initUserAgent(str);
 *         this.m_thread = new HttpThread(this, str2, str3, bArr, i, j, z, z3);
 *     }
 *
 * so str is the user agent, str2 the method and str3 the URL. The four
 * trailing parameters are not modelled here: the port answers the request
 * itself and has no use for the server time, the SSL check or the timeout.
 * The two the port DOES need - the body and the callback - sit at index 3
 * and index 5 of the guest's argument list, which is where the port's
 * array-form dispatcher reads them.
 */
static void http_init(JNIEnv *env, jobject self, jstring /*userAgent*/,
                      jstring /*method*/, jstring url, jbyteArray body,
                      jint /*readCapacity*/, jlong callback,
                      jboolean /*failOnErrorStatus*/)
{
    CCHttpRequest *req = (CCHttpRequest *)self;
    req->callback = callback;
    req->closed   = JNI_FALSE;
    req->pending  = JNI_FALSE;

    /* Keep the URL. It is the only thing that tells the /time request from
     * the authenticate request (see the class comment). */
    req->urlLen = 0;
    req->url[0] = '\0';
    if (url != nullptr) {
        const char *text = env->GetStringUTFChars(url, nullptr);
        if (text != nullptr) {
            size_t take = 0;
            while (take + 1u < sizeof(req->url) && text[take] != '\0') {
                take += 1u;
            }
            memcpy(req->url, text, take);
            req->url[take] = '\0';
            req->urlLen = take;
            env->ReleaseStringUTFChars(url, text);
        }
    }

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
     * http.post deferral line. The URL is on the same line because it is
     * what identifies the request. */
    if (authLogBudget()) {
        trace("cc-http-init: body=%zu url=%s", (unsigned)req->bodyLen,
              req->urlLen > 0u ? req->url : "(none)");
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

/*
 * Answer the /time request.
 *
 * MEASURED: the guest's ServerTimeManager does not read the server time out
 * of the response BODY. Its parser (guest raw 0x1cef8a4) looks the key
 * "X-Request-Start" up in the guest's response-HEADER map (worker+0x180,
 * filled by the guest's own native headerCallback at guest raw 0x1ce4348)
 * and converts the value with strtod (guest raw 0x1cef9d0). The value is
 * therefore ASCII decimal seconds.
 *
 * The delivery order is the Java one (HttpThread.run): headerCallback, then
 * the body, then completeCallback. The /time response has no body the guest
 * reads, so only the header and the completion are delivered - matching the
 * Java contract, where ReadHttpData never calls dataCallback for a body that
 * is empty.
 *
 * No Content-Type is sent: the guest's completion handler compares the
 * response's content type against the one the worker expects
 * (guest raw 0x1ce3f68 -> 0x1ce3fd4) and asserts when they differ. The port
 * sends no content type today and the guest does not assert, so sending none
 * keeps that comparison exactly as it is.
 */
static bool deliverServerTime(JNIEnv *env, CCHttpRequest *req)
{
    static CCHttpHeaderFn header_callback = cloudcell_native<CCHttpHeaderFn>(
        kCCHttpHeaderCallback);
    static CCHttpCompleteFn complete_callback = cloudcell_native<CCHttpCompleteFn>(
        kCCHttpCompleteCallback);

    if (header_callback == nullptr || complete_callback == nullptr) {
        warning("HttpRequest: /time natives missing (header=%p complete=%p); "
                "the server time cannot be delivered\n",
                (void *)header_callback, (void *)complete_callback);
        return false;
    }

    const uintptr_t map = ccheaders::createMap(&ccheaders::kServerTime, 1u);
    if (map == 0) {
        warning("HttpRequest: could not build the response header map\n");
        return false;
    }

    if (authLogBudget()) {
        trace("cc-srvtime: answering /time with header %s=%s (map=%p)",
              ccheaders::kServerTime.name, ccheaders::kServerTime.value,
              (void *)map);
    }
    {
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "url=%s header=%s value=%s map=%p", req->url,
                           ccheaders::kServerTime.name,
                           ccheaders::kServerTime.value, (void *)map);
        rr3::guestViewWideLog("cc-srvtime",
                              static_cast<unsigned long long>(req->callback),
                              static_cast<unsigned long long>(req->bodyLen),
                              details);
    }

    header_callback(env, (jobject)req, req->callback, 0, (jobject)map);
    if (authLogBudget()) {
        trace("cc-srvtime: headerCallback returned");
    }
    complete_callback(env, (jobject)req, req->callback, 200);

    if (authLogBudget()) {
        trace("cc-srvtime: header delivered, completed with status 200");
    }
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

        /* The /time request. The URL is the discriminator: the guest's own
         * log prints "CC: [1] POST request
         * [https://connect-dev-ss.cloudcell.com:443/time]" while the
         * authenticate request goes to CC_Sync.php. The body cannot be used:
         * the guest's byte[] is not a port array object, so the port reads a
         * length of 0 for every request. */
        if (strstr(req->url, "/time") != nullptr) {
            if (deliverServerTime(env, req)) {
                return;
            }
        } else if (authLogBudget()) {
            trace("cc-http-req: not the /time request url=%s", req->url);
        }

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
