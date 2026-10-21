#include <stdio.h>
#include <new>
#include <string.h>
#include <time.h>
#include <vector>

#include <switch.h>

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
 * ASCII decimal seconds, because the guest's parser converts the header
 * value with strtod() (guest raw 0x1cef9d0 -> PLT strtod at 0x1e18b80) and
 * stores the result as a double at this+0x88.
 *
 * The value is the CURRENT wall clock, read here at DELIVERY time. The host
 * and the guest share the wall clock, so the answer is the guest's own local
 * time to within the delivery latency; the guest's ServerTimeManager
 * measures the server time against its own clock, so a CONSTANT would show
 * up as a permanent offset. (The constant this replaced, 1793000000.125000,
 * was 460265 s - 5.3 days - ahead of the guest's own clock, which read
 * 1792539735 in archive 20261021-074157-eden-srvtime7.)
 *
 * If the host's clock comes back non-positive the answer falls back to a
 * plausible epoch second: the guest treats a non-positive server time as
 * "no answer" and never logs it.
 */
namespace {

/*
 * The storage for the answer.
 *
 * The header map the port builds keeps the pointer to the Entry AND the
 * characters the value points at, so the storage has to outlive the
 * delivery. It is a process-lifetime slot (the port answers one /time
 * request per run, and the guest copies the value into its own map inside
 * the header callback), which is why a stack buffer is not used here.
 */
struct ServerTimeSlot {
    Entry entry;
    char  text[48];
};

ServerTimeSlot g_serverTimeSlot;

/* Used only when the host's clock is unusable. A plausible epoch second,
 * comfortably positive (the guest treats a non-positive server time as "no
 * answer" and never logs it). */
const char kFallbackServerTime[] = "1793000000.125000";

} // namespace

const Entry* serverTimeEntry(void)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec <= 0 ||
        ts.tv_nsec < 0) {
        __builtin_snprintf(g_serverTimeSlot.text, sizeof(g_serverTimeSlot.text),
                           "%s", kFallbackServerTime);
    } else {
        __builtin_snprintf(g_serverTimeSlot.text, sizeof(g_serverTimeSlot.text),
                           "%lld.%06lld", (long long)ts.tv_sec,
                           (long long)(ts.tv_nsec / 1000));
    }
    g_serverTimeSlot.entry.name  = "X-Request-Start";
    g_serverTimeSlot.entry.value = g_serverTimeSlot.text;
    return &g_serverTimeSlot.entry;
}

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

/* ------------------------------------------------------------------ *
 * The response-header probe
 * ------------------------------------------------------------------ */

/*
 * WHAT THIS PROBES, AND WHY
 * ==========================
 *
 * MEASURED (archive 20261021-063741-eden-srvtime2): the port hands the guest
 * the header "X-Request-Start: 1793000000.125000" through the guest's own
 * native headerCallback (guest raw 0x1ce4348), the guest's header walk runs
 * (the port's cc-hdr trace shows keySet / iterator / hasNext / next / get /
 * size all answered) and the request completes with Succeeded=True - but the
 * guest never logs
 *     "CC: Server time returned; server time: %f server-local time diff: %f"
 * (format string at guest raw 0x3fc1d0).
 *
 * The guest's own code says that line is printed from exactly one place, the
 * ServerTimeManager's response handler at guest raw 0x1cef2f8, and it is
 * reached only when ALL of these hold:
 *
 *   0x1cef360  |now - (this+0x80)| <= 5.0   the parse happened recently
 *   0x1cef37c  (this+0x88) > 0.0            THE SERVER TIME WAS PARSED
 *   0x1cef384  (this+0x70) == the token     the token still matches
 *   0x1cef390  |now - (this+0x78)| <  5.0   the response came back fast
 *
 * and that (this+0x88) is written by the parser at guest raw 0x1cef8a4, which
 * ZEROES it first (0x1cef900) and only fills it when the key "X-Request-Start"
 * is FOUND in the map it is handed:
 *
 *   0x1cef8c8  ldr x8, [x0, #0x70]   ; the token
 *   0x1cef8cc  cmp x8, x2            ; must match, else the answer is dropped
 *   0x1cef92c  bl 0xa0b240           ; the guest's hash-map find
 *   0x1cef9d4  str d0, [x19, #0x88]  ; this->serverTime
 *
 * So the probe has to answer ONE question: did the header land in the guest's
 * OWN response-header map, and does the parser read THAT map?
 *
 * It can answer the first half, because the guest's map is a hash map whose
 * layout the port can read (taken from the guest's own find at guest raw
 * 0xa0b240 and from the guest's own insert at guest raw 0x1ce48b8):
 *
 *   container+0x00  void**  the bucket array
 *   container+0x08  size_t  the bucket count
 *   node+0x00       node*   the next node in the bucket
 *   node+0x08       u64     the key's hash
 *   node+0x10       string  the key
 *   node+0x28       string  the value
 *
 * and a std::string in this guest is
 *
 *   +0x00  byte   flags: bit0 set -> the data is on the heap, otherwise the
 *                       length is (flags >> 1) and the data is inline
 *   +0x01  byte[] the inline data
 *   +0x08  u64    the length (long strings only)
 *   +0x10  void*  the data pointer (long strings only)
 *
 * The container the guest's headerCallback writes to is at
 * (*(worker+0x08)) + 0x178 (guest raw 0x1ce4794 and 0x1ce47a4), and the
 * port knows the worker: it is the jlong the guest handed init(), and the port
 * hands the very same value back to every callback.
 *
 * The port therefore reads the guest's map after the headerCallback and prints
 * the (key, value) pairs it finds. No guest code is called, nothing is
 * written, and every read is bounded.
 */
namespace ccprobe {

/* Bounded, read-only. The guest's addresses are host addresses: the rehosted
 * image is mapped into this process, so a guest pointer is directly readable. */

static void appendText(char *out, size_t cap, size_t *used, const char *text)
{
    if (out == nullptr || text == nullptr || *used + 1u >= cap) {
        return;
    }
    size_t take = strlen(text);
    if (*used + take + 1u > cap) {
        take = cap - *used - 1u;
    }
    memcpy(out + *used, text, take);
    *used += take;
    out[*used] = '\0';
}

/*
 * Every guest read is gated on TWO checks, and both are load-bearing.
 *
 * svcQueryMemory answers for an unmapped address instead of letting the load
 * trap (the rule the census uses: pthread_bridge.cpp's censusReadStackWord).
 * On its own it is NOT enough under Eden: the kernel is asked about the HOST's
 * mapping of the address, and Eden's emulated guest MMU has its own idea of
 * what is mapped. A host address that happens to be readable can still be
 * unmapped as far as the guest is concerned, and reading it takes the whole
 * run down (archive 20261021-072048-eden-srvtime4 died on
 * "HW.Memory <Error> core/memory.cpp:624: Unmapped Read64 @ 0x3A00000000",
 * frames 16200 -> 3960, because the probe followed a pointer into the host's
 * own address space).
 *
 * So a guest pointer is only followed when it is inside the guest's own 64-bit
 * window - the guest's mappings sit either side of the image, and the host's
 * code and heap do not - AND the kernel says the page is readable.
 *
 * The page query is CACHED, one page deep, and that is not an optimisation:
 * the probe runs on the thread the guest called the JNI method from, so every
 * syscall it spends is a syscall the guest's frame loop does not get. The
 * first version of the hunt spent about 1500 of them and cost the run
 * thousands of frames (16200 -> 8280).
 */
static uintptr_t g_checkedPage = 0;
static bool     g_checkedPageOk = false;

static bool guestReadable(uintptr_t addr)
{
    if (addr == 0 || (addr & 7u) != 0u) {
        return false;
    }
    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    if (bias == 0) {
        return false;
    }
    const uintptr_t lo = bias - 0x1000000000ull;
    const uintptr_t hi = bias + 0x1000000000ull;
    if (addr < lo || addr >= hi) {
        return false;
    }
    const uintptr_t page = addr & ~0xfffull;
    if (page != g_checkedPage) {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(page)))) {
            g_checkedPage = page;
            g_checkedPageOk = false;
            return false;
        }
        g_checkedPage = page;
        g_checkedPageOk = (info.perm & Perm_R) != 0;
    }
    return g_checkedPageOk;
}

static bool readU64(uintptr_t addr, uint64_t *out)
{
    if (out == nullptr || !guestReadable(addr)) {
        return false;
    }
    memcpy(out, reinterpret_cast<const void *>(addr), sizeof(uint64_t));
    return true;
}

/*
 * Copy `len` bytes out of guest memory a guarded word at a time. A plain
 * memcpy would cross a page boundary into an unmapped page: the string the
 * guest's own code reads may start on the LAST byte of a page, and the read
 * that follows it then lands on the next one.
 */
static bool copyGuestBytes(char *dst, uintptr_t src, size_t len)
{
    size_t done = 0;
    while (done < len) {
        uint64_t word = 0;
        if (!readU64(src + done, &word)) {
            return false;
        }
        size_t take = len - done;
        if (take > sizeof(word)) {
            take = sizeof(word);
        }
        memcpy(dst + done, &word, take);
        done += take;
    }
    return true;
}

/* Read a guest std::string into a host buffer. Returns false when the string
 * is not readable in the shape the guest's own code reads it. */
static bool readGuestString(uintptr_t addr, char *out, size_t cap)
{
    if (addr == 0 || out == nullptr || cap < 2u) {
        return false;
    }
    uint64_t head = 0;
    if (!readU64(addr, &head)) {
        out[0] = '\0';
        return false;
    }
    const uint8_t flags = static_cast<uint8_t>(head & 0xffu);
    if ((flags & 1u) != 0u) {
        uint64_t len = 0;
        uint64_t data = 0;
        if (!readU64(addr + 8u, &len) || !readU64(addr + 0x10u, &data)) {
            out[0] = '\0';
            return false;
        }
        if (data == 0 || len == 0 || len > 4096u ||
            !guestReadable(static_cast<uintptr_t>(data))) {
            out[0] = '\0';
            return false;
        }
        size_t take = (static_cast<size_t>(len) < cap - 1u)
                          ? static_cast<size_t>(len) : cap - 1u;
        if (!copyGuestBytes(out, static_cast<uintptr_t>(data), take)) {
            out[0] = '\0';
            return false;
        }
        out[take] = '\0';
        return true;
    }
    size_t len = static_cast<size_t>(flags >> 1);
    if (len > cap - 1u) {
        len = cap - 1u;
    }
    /* The inline data starts one byte into the object, so it is copied a
     * guarded byte at a time from the word that was already read. */
    const uint8_t *src = reinterpret_cast<const uint8_t *>(&head);
    size_t done = 0;
    while (done < len) {
        const size_t inWord = (done < 7u) ? (7u - done) : 0u;
        if (inWord != 0u) {
            memcpy(out + done, src + 1u + done, inWord);
            done += inWord;
            continue;
        }
        uint64_t word = 0;
        if (!readU64(addr + 8u + (done - 7u), &word)) {
            out[0] = '\0';
            return false;
        }
        size_t take = len - done;
        if (take > sizeof(word)) {
            take = sizeof(word);
        }
        memcpy(out + done, &word, take);
        done += take;
    }
    out[len] = '\0';
    return true;
}

/*
 * Print the (key, value) pairs of the guest's hash map at `container`.
 * Returns how many pairs were printed.
 */
static unsigned describeMap(uintptr_t container, char *out, size_t cap)
{
    size_t used = 0;
    if (out == nullptr || cap == 0u) {
        return 0u;
    }
    out[0] = '\0';

    uint64_t buckets = 0;
    uint64_t count   = 0;
    if (!readU64(container, &buckets) || !readU64(container + 8u, &count)) {
        appendText(out, cap, &used, "unreadable");
        return 0u;
    }

    char tmp[192];
    __builtin_snprintf(tmp, sizeof(tmp), "buckets=0x%llx count=%llu",
                       (unsigned long long)buckets, (unsigned long long)count);
    appendText(out, cap, &used, tmp);

    if (buckets == 0 || count == 0 || count > 8192u) {
        appendText(out, cap, &used, " -> not a hash map");
        return 0u;
    }

    unsigned pairs = 0u;
    const uint64_t bucketsRead = (count < 16u) ? count : 16u;
    for (uint64_t i = 0; i < bucketsRead && pairs < 8u; ++i) {
        uint64_t node = 0;
        if (!readU64(static_cast<uintptr_t>(buckets) + i * 8u, &node)) {
            continue;
        }
        unsigned guard = 0u;
        while (node != 0 && guard < 8u && pairs < 8u) {
            ++guard;
            char key[64];
            char value[96];
            if (!readGuestString(static_cast<uintptr_t>(node) + 0x10u, key, sizeof(key)) ||
                !readGuestString(static_cast<uintptr_t>(node) + 0x28u, value, sizeof(value))) {
                break;
            }
            __builtin_snprintf(tmp, sizeof(tmp), " [%s=%s]", key, value);
            appendText(out, cap, &used, tmp);
            ++pairs;
            uint64_t next = 0;
            if (!readU64(static_cast<uintptr_t>(node), &next)) {
                break;
            }
            node = next;
        }
    }
    return pairs;
}

/*
 * The ServerTimeManager's vtable pointer, read out of the guest image's
 * .data.rel.ro (the vtable base is 0x1f65b88: [0] = offset-to-top,
 * [1] = the typeinfo, [2] = vfunc0, so the vptr an object carries is the
 * address of the FIRST function slot = 0x1f65b98).
 *
 * THE OLD CONSTANT WAS WRONG BY 0x200 (eight slots): it was 0x1f65bb98,
 * which is the slot of vfunc4 (cc::ServerTimeManager::Update, guest raw
 * 0x1ceedcc), not the vtable pointer. The vtable-selector arm of
 * reportManagerCandidate therefore NEVER matched, and every manager sample
 * in every archive was found by the +0x78 shape fallback instead. The
 * fallback works, so the bug was invisible - but the selector is the cheap
 * path (one read per candidate) and it should work.
 */
static const uintptr_t kManagerVtableElf = 0x1f65b98ull;

/*
 * The ServerTimeManager's address, remembered once the hunt finds it. The
 * hunt is expensive and the manager does not move between the three phases of
 * one exchange, so the later phases read it directly (without this the probe
 * cost the run thousands of frames: 16200 -> 8280).
 */
static uintptr_t g_managerAddr = 0;

/*
 * The cc::AndroidHttpRequestWorker the port is serving. Remembered so the
 * late sample can read the request record (the worker's +0x08) without
 * hunting for it again.
 */
static uintptr_t g_workerAddr = 0;

/*
 * The ServerTimeManager's shape, read out of the guest's own code:
 *
 *   +0x70  the request token - the return value of the HTTP manager's
 *          request-start call, stored by the manager itself at 0x1cef258
 *   +0x78  the local time when the request went out   (0x1cef1d0)
 *   +0x80  the local time when the answer was parsed   (0x1cef8fc)
 *   +0x88  THE SERVER TIME (0x1cef9d4) - a double, epoch seconds
 *
 * So an object whose +0x78 reads back as an epoch second IS the manager, and
 * its +0x88 says whether the answer reached it and its +0x70 is the token the
 * parser compares against.
 */
static void reportManagerCandidate(uintptr_t p, const char *phase)
{
    /* The vtable selector first: one read per candidate, and the page it
     * lands on is usually the page the candidate itself was read from. */
    uint64_t vptr = 0;
    if (!readU64(p, &vptr)) {
        return;
    }
    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    const uintptr_t want = (bias != 0) ? (bias + kManagerVtableElf) : 0;
    if (want == 0 || vptr != want) {
        /* Fall back to the shape test: the manager's +0x78 is an epoch
         * second, which no other object in the request has. */
        uint64_t raw78 = 0;
        if (!readU64(p + 0x78u, &raw78)) {
            return;
        }
        double m78 = 0.0;
        memcpy(&m78, &raw78, sizeof(m78));
        if (!(m78 > 1.0e9 && m78 < 2.0e9)) {
            return;
        }
    }
    uint64_t raw78 = 0;
    uint64_t raw80 = 0;
    uint64_t raw88 = 0;
    uint64_t raw60 = 0;
    uint64_t raw70 = 0;
    uint64_t raw90 = 0;
    uint64_t raw98 = 0;
    uint64_t rawa0 = 0;
    uint64_t rawa8 = 0;
    uint64_t rawe8 = 0;
    uint64_t raw150 = 0;
    uint64_t raw158 = 0;
    readU64(p + 0x70u, &raw70);
    readU64(p + 0x78u, &raw78);
    readU64(p + 0x80u, &raw80);
    readU64(p + 0x88u, &raw88);
    readU64(p + 0x60u, &raw60);
    /* The fields the guest's own handler at raw 0x1cef2f8 reads. That
     * handler's success path clears +0x70 (the token), +0x88 (the server
     * time) and the byte at +0x60, and its failure path leaves them, so
     * they say whether the handler ran. +0x90 is the sample counter the
     * handler bumps, and +0x150/+0x158 are the pending-record list the
     * handler walks (and pops). */
    readU64(p + 0x90u, &raw90);
    readU64(p + 0x98u, &raw98);
    readU64(p + 0xa0u, &rawa0);
    readU64(p + 0xa8u, &rawa8);
    readU64(p + 0xe8u, &rawe8);
    readU64(p + 0x150u, &raw150);
    readU64(p + 0x158u, &raw158);
    double m78 = 0.0;
    double m80 = 0.0;
    double m88 = 0.0;
    double ma0 = 0.0;
    double ma8 = 0.0;
    double me8 = 0.0;
    memcpy(&m78, &raw78, sizeof(m78));
    memcpy(&m80, &raw80, sizeof(m80));
    memcpy(&m88, &raw88, sizeof(m88));
    memcpy(&ma0, &rawa0, sizeof(ma0));
    memcpy(&ma8, &rawa8, sizeof(ma8));
    memcpy(&me8, &rawe8, sizeof(me8));

    /* The first pending record, when the list is not empty: the handler
     * walks it (stride 0x30) and reads the callback object at +0x20. */
    uint64_t rec[5] = { 0, 0, 0, 0, 0 };
    const bool havePending = (raw150 != 0 && raw150 != raw158);
    if (havePending) {
        for (unsigned i = 0; i < 5u; ++i) {
            readU64(static_cast<uintptr_t>(raw150) + i * 8u, &rec[i]);
        }
    }

    char line[512];
    __builtin_snprintf(line, sizeof(line),
                       "%s mgr=0x%llx token=0x%llx m78=%.6f m80=%.6f m88=%.6f "
                       "m60=0x%llx flag60=0x%llx cnt90=0x%llx m98=0x%llx "
                       "ma0=%.6f ma8=%.6f me8=%.6f pend=0x%llx/0x%llx "
                       "rec0=0x%llx rec1=0x%llx rec2=0x%llx rec3=0x%llx "
                       "rec4=0x%llx",
                       phase != nullptr ? phase : "-", (unsigned long long)p,
                       (unsigned long long)raw70, m78, m80, m88,
                       (unsigned long long)raw60,
                       (unsigned long long)(raw60 & 0xffu),
                       (unsigned long long)raw90, (unsigned long long)raw98,
                       ma0, ma8, me8,
                       (unsigned long long)raw150, (unsigned long long)raw158,
                       (unsigned long long)rec[0], (unsigned long long)rec[1],
                       (unsigned long long)rec[2], (unsigned long long)rec[3],
                       (unsigned long long)rec[4]);
    rr3::guestViewWideLog("cc-probe-mgr", (unsigned long long)p, raw88, line);
    g_managerAddr = p;
}

/*
 * THE GATE ON THE HANDLER, read straight out of the guest's request record.
 *
 * The guest's cc::HttpRequestManager holds its active requests in a vector at
 * this+0x38 (begin) / this+0x40 (end); each element is a 0x30-byte
 * cc::ActiveRequest_Struct.  Read out of the guest image:
 *
 *   +0x00  u64   the request token (the ServerTimeManager's this+0x70)
 *   +0x88  u64   the "completion" delegate (void(bool*)) - the worker calls it
 *   +0x90  u64   its holder
 *   +0xc8  u8    THE RESPONSE-COMPLETE FLAG
 *   +0xc9  u8    "the completion delegate was already delivered"
 *   +0xd4  u32   the pending body byte count
 *   +0xd8  ptr   the pending body buffer
 *   +0x100 u64   THE HANDLER DELEGATE  (void(char const*, u32, u64))
 *   +0x130 u64   the body delegate     (void(char const*, u32, u64))
 *   +0x160 u64   THE PARSER DELEGATE  (void(unordered_map&, u64))
 *   +0x170 u32   the HTTP status code
 *   +0x178 ...   the response header map
 *
 * cc::HttpRequestManager::OnRequestComplete (guest raw 0x1ce2da0 - the
 * function that prints "CC: [%lu] %s complete [Succeeded=%s]") sets +0xc8 to
 * 1, and cc::HttpRequestManager::Poll (0x1ce29b4) invokes the +0x100
 * delegate ONLY when +0xc8 != 0 and +0xc9 == 0.  So:
 *
 *   +0xc8 == 0   the guest never marked the response complete
 *   +0xc8 == 1, +0xc9 == 0, +0x100 == 0   the handler delegate was never
 *                                         installed
 *   +0xc8 == 1, +0xc9 == 0, +0x100 != 0   the handler IS installed and
 *                                         waiting, and the only thing left is
 *                                         that Poll has not run
 *
 * The manager pointer comes from the guest's own request record: the
 * cc::AndroidHttpRequestWorker's +0x08 IS the cc::ActiveRequest_Struct it is
 * serving (cc::AndroidHttpRequestWorker::CompletionCallback at guest raw
 * 0x1ce4c60 does "ldr x9,[x0,#8]" and then "str w8,[x9,#0x170]" - the status
 * into the record), and the port already holds the worker pointer: it is the
 * jlong the guest handed init() and hands back to every callback.
 *
 * (The first version of this probe went after the Cloudcell's +0x10 and
 * found the TIME SERVICE there, not the HTTP request manager - the manager
 * comes out of cloudcell->vtable[4](), which a probe must not call. The
 * worker is the right handle and it costs one fewer hop.)
 */
static void describeActiveRequests(uintptr_t worker)
{
    /* Bounded: the record's fields live in one or two pages, but each read
     * that crosses a page boundary costs the guest a syscall, and the probe
     * runs on the thread the guest called JNI from. Four samples is a time
     * series; sixty-seven is a cost. */
    static unsigned s_samples = 0u;
    if (s_samples >= 4u) {
        return;
    }
    ++s_samples;
    if (worker == 0) {
        return;
    }
    uint64_t rec = 0;
    if (!readU64(worker + 0x08u, &rec) || rec == 0) {
        return;
    }
    const uintptr_t r = static_cast<uintptr_t>(rec);
    uint64_t tok = 0, c8 = 0, d4 = 0, d100 = 0, d130 = 0, d160 = 0, status = 0;
    readU64(r + 0x00u, &tok);
    readU64(r + 0xc8u, &c8);
    readU64(r + 0xd4u, &d4);
    readU64(r + 0x100u, &d100);
    readU64(r + 0x130u, &d130);
    readU64(r + 0x160u, &d160);
    readU64(r + 0x170u, &status);

    /*
     * Is the record still in the HTTP request manager's active-request
     * vector?
     *
     * The manager is a global: the guest's cc::HttpRequestManager pointer
     * lives in .bss at 0x1f97818 (GOT 0x1f6dac8 -> that address), and
     * cc::HttpRequestManager::Poll (0x1ce29b4) walks its vector at
     * this+0x38 (begin) / this+0x40 (end) - a vector of POINTERS, 8 bytes
     * per element ("ldr x21,[x8,x9,lsl #3]"), not of 0x30-byte records.
     *
     * A record that is not in the vector is one the poll can never see, and
     * a record that IS in it with +0xc8 set and +0xc9 clear is one the
     * poll has not delivered yet. Those are different blockers and the
     * probe has to tell them apart.
     */
    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    uint64_t mgr = 0, begin = 0, end = 0;
    uint64_t mgrvptr = 0;
    uint64_t vecn = 0;
    bool found = false;
    if (bias != 0 &&
        readU64(bias + 0x1f97818ull, &mgr) && mgr != 0) {
        /* The object's own vptr, so the log says WHICH class the global
         * holds. cc::HttpRequestManager's vtable base is 0x1f64848, so its
         * vfunc0 slot - the address an instance's vptr points at - is
         * 0x1f64858. If the vptr does not match, the global is some other
         * object and its +0x38/+0x40 are not an active-request vector. */
        readU64(static_cast<uintptr_t>(mgr), &mgrvptr);
        if (readU64(static_cast<uintptr_t>(mgr) + 0x38u, &begin) &&
            readU64(static_cast<uintptr_t>(mgr) + 0x40u, &end) &&
            end > begin) {
            vecn = (end - begin) / 8u;
            for (uint64_t i = 0; i < vecn && i < 32u; ++i) {
                uint64_t e = 0;
                if (readU64(static_cast<uintptr_t>(begin) + i * 8u, &e) && e == rec) {
                    found = true;
                    break;
                }
            }
        }
    }

    char line[512];
    __builtin_snprintf(line, sizeof(line),
                       "worker=0x%llx rec=0x%llx token=0x%llx c8=%llu c9=%llu "
                       "d4=%llu handler100=0x%llx body130=0x%llx "
                       "parser160=0x%llx status=0x%llx | mgr=0x%llx "
                       "mgrvptr=0x%llx vecn=%llu invec=%d",
                       (unsigned long long)worker, (unsigned long long)rec,
                       (unsigned long long)tok,
                       (unsigned long long)(c8 & 0xffu),
                       (unsigned long long)((c8 >> 8) & 0xffu),
                       (unsigned long long)(d4 & 0xffffffffu),
                       (unsigned long long)d100, (unsigned long long)d130,
                       (unsigned long long)d160,
                       (unsigned long long)(status & 0xffffffffu),
                       (unsigned long long)mgr, (unsigned long long)mgrvptr,
                       (unsigned long long)vecn, found ? 1 : 0);
    rr3::guestViewWideLog("cc-probe-req", (unsigned long long)rec,
                          (unsigned long long)d100, line);
}

/*
 * Hunt for the manager among the objects the record and the worker point at,
 * one level down. Bounded: at most 96 direct candidates and 256 second-level
 * ones, every read gated on svcQueryMemory.
 */
static void huntServerTime(uintptr_t record, uintptr_t worker, const char *phase)
{
    if (g_managerAddr != 0) {
        reportManagerCandidate(g_managerAddr, phase);
        return;
    }

    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    if (bias == 0) {
        return;
    }

    uintptr_t roots[96];
    unsigned rootCount = 0u;
    for (unsigned i = 0; i < 32u && rootCount < 96u; ++i) {
        uint64_t word = 0;
        if (readU64(record + i * 8u, &word) && word != 0) {
            roots[rootCount++] = static_cast<uintptr_t>(word);
        }
    }
    for (unsigned i = 0; i < 52u && rootCount < 96u; ++i) {
        uint64_t word = 0;
        if (readU64(worker + i * 8u, &word) && word != 0) {
            roots[rootCount++] = static_cast<uintptr_t>(word);
        }
    }

    /* The manager sits among those words (measured: the hunt finds it there),
     * so one pass is enough. The second one is a fallback for a layout that
     * puts it one step further out. */
    for (unsigned i = 0; i < rootCount; ++i) {
        reportManagerCandidate(roots[i], phase);
        if (g_managerAddr != 0u) {
            break;
        }
    }
}

/*
 * Report the worker's own state: its vtable (so the OnHeaders slot can be
 * named), the object that owns the header map, and the header map's contents.
 * `phase` says when this was taken.
 */
static void probeWorker(uintptr_t worker, const char *phase)
{
    char details[768];
    size_t used = 0;
    details[0] = '\0';

    if (worker == 0) {
        rr3::guestViewWideLog("cc-probe", 0, 0, "worker=0");
        return;
    }

    uint64_t vptr = 0;
    uint64_t vt3  = 0;
    uint64_t base = 0;
    if (!readU64(worker, &vptr) || !readU64(worker + 8u, &base)) {
        rr3::guestViewWideLog("cc-probe", (unsigned long long)worker, 0,
                              "worker unreadable");
        return;
    }
    if (vptr != 0) {
        readU64(static_cast<uintptr_t>(vptr) + 0x18u, &vt3);
    }

    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    char tmp[256];
    __builtin_snprintf(tmp, sizeof(tmp),
                       "%s worker=0x%llx vptr=0x%llx vtbl3=0x%llx elf3=0x%llx base=0x%llx",
                       phase != nullptr ? phase : "-",
                       (unsigned long long)worker, (unsigned long long)vptr,
                       (unsigned long long)vt3,
                       (unsigned long long)(vt3 >= bias ? vt3 - bias : 0u),
                       (unsigned long long)base);
    appendText(details, sizeof(details), &used, tmp);

    /* The container the guest's headerCallback writes to, and the two
     * neighbours, so a run says which of them is the real map. */
    const uintptr_t candidates[3] = {
        (base != 0) ? static_cast<uintptr_t>(base) + 0x178u : 0u,
        worker + 0x178u,
        worker + 0x180u,
    };
    static const char *kNames[3] = { "map@base+0x178", "map@worker+0x178",
                                      "map@worker+0x180" };
    for (int i = 0; i < 3; ++i) {
        if (candidates[i] == 0) {
            continue;
        }
        char mapText[384];
        const unsigned pairs = describeMap(candidates[i], mapText, sizeof(mapText));
        __builtin_snprintf(tmp, sizeof(tmp), " %s@0x%llx pairs=%u %s", kNames[i],
                           (unsigned long long)candidates[i], pairs, mapText);
        appendText(details, sizeof(details), &used, tmp);
    }

    rr3::guestViewWideLog("cc-probe", (unsigned long long)worker,
                          (unsigned long long)vt3, details);

    /* Remember the worker: the late sample reads the request record out of
     * it (the worker's +0x08 IS the cc::ActiveRequest_Struct). */
    g_workerAddr = worker;

    /* The objects the record and the worker point at, so a run says which
     * pointer leads to the ServerTimeManager and what its fields say. */
    huntServerTime(worker, (base != 0) ? static_cast<uintptr_t>(base) : worker, phase);
}

} // namespace ccprobe

/*
 * A LATE sample of the ServerTimeManager, for the host's periodic hook.
 *
 * The three ccprobe samples inside the /time exchange all happen inside the
 * guest's own JNI call, so they cannot say whether the guest's handler at
 * raw 0x1cef2f8 runs LATER - on a tick of the guest's own state machine
 * rather than inside the response delivery. This samples the manager once
 * the host is back on its own thread, which is the only way to separate
 * "the handler never ran" from "the handler ran after the exchange".
 *
 * It is cheap: the manager's address is remembered by the hunt, and every
 * field it reads lives in the manager's own page, so the page query is
 * already cached and no new syscall is spent.
 *
 * Returns true when a sample was taken.
 */
extern "C" bool rr3ServerTimeManagerProbe(const char *phase)
{
    bool took = false;
    if (ccprobe::g_managerAddr != 0) {
        ccprobe::reportManagerCandidate(ccprobe::g_managerAddr,
                                        phase != nullptr ? phase : "late");
        took = true;
    }
    /* The gate on the handler: is the request record still there, has the
     * guest marked the response complete, and is the handler delegate
     * installed?  This is the read that says which of those three is
     * missing.  It does not need the manager, so it runs even when the
     * manager hunt has not landed yet. */
    ccprobe::describeActiveRequests(ccprobe::g_workerAddr);
    return took;
}

/* ------------------------------------------------------------------ *
 * The /time response BODY
 * ------------------------------------------------------------------ */

/*
 * WHICH BODY THE PORT SHIPS - a build-time selector, because every
 * candidate is worth one Eden run and a run cannot answer two questions
 * at once.
 *
 *   RR3_SRV_BODY 0  EMPTY - no bytes at all
 *   RR3_SRV_BODY 1  24 zero bytes
 *   RR3_SRV_BODY 2  24 bytes holding the current epoch seconds as a
 *                   little-endian double, the rest zero
 *   RR3_SRV_BODY 3  the server time as ASCII decimal, zero padded
 *
 * WHY THE SELECTOR EXISTS: "is a body delivered at all" and "what the body
 * holds" are two different questions. The port was answering neither, and
 * the guest's own contract (HttpThread.run, reverse/java/sources/com/
 * firemonkeys/cloudcellapi/HttpThread.java:92-104 and :319-321) answers
 * both at once: headerCallback, then dataCallback (the BODY), then
 * completeCallback.
 *
 * WHAT THE GUEST DOES WITH THE BYTES, read out of the image:
 *
 *   Java_com_firemonkeys_cloudcellapi_HttpRequest_dataCallback
 *   (guest raw 0x1ce493c) mallocs the length, copies the array out
 *   through JNIEnv slot 0x640 - index 200, which in THIS guest's JNIEnv
 *   layout is GetByteArrayRegion - calls the worker's vtable[4] with
 *   (worker, buffer, length) and frees the buffer.
 *
 *   That worker slot is cc::HttpRequestManager's body append
 *   (guest raw 0x1ce2cc4), which grows the record's pending body buffer
 *   at record+0xd8 and its byte count at record+0xd4.
 *
 *   cc::HttpRequestManager::Poll (guest raw 0x1ce29b4) then hands the
 *   record's handler delegate (record+0x100) the body, and hands the
 *   BODY delegate (record+0x130) the same bytes; with record+0xd4 == 0
 *   it hands the handler a NULL body and a zero count instead.
 *
 * The guest's own ServerTimeManager handler (guest raw 0x1cef2f8) reads
 * the server time out of this+0x88 and the token out of this+0x70 and
 * never looks at the body - so the CONTENT of the body cannot be what
 * makes the handler run. Whether a body is DELIVERED is still a
 * different thing from that, and this is the change that makes the
 * delivery happen at all.
 */
#ifndef RR3_SRV_BODY
#define RR3_SRV_BODY 1
#endif

/* The body the port ships. Returns how many bytes it wrote. */
static size_t serverTimeBody(uint8_t *out, size_t cap)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec  = 1793000000;
        ts.tv_nsec = 125000000;
    }

#if RR3_SRV_BODY == 0
    (void)out;
    (void)cap;
    return 0u;
#elif RR3_SRV_BODY == 1
    if (cap < 24u) {
        return 0u;
    }
    memset(out, 0, 24u);
    return 24u;
#elif RR3_SRV_BODY == 2
    if (cap < 24u) {
        return 0u;
    }
    memset(out, 0, 24u);
    {
        const double seconds = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        memcpy(out, &seconds, sizeof(seconds));
    }
    return 24u;
#elif RR3_SRV_BODY == 3
    {
        char text[48];
        const int written = __builtin_snprintf(
            text, sizeof(text), "%lld.%06lld\n", (long long)ts.tv_sec,
            (long long)(ts.tv_nsec / 1000));
        if (written <= 0 || (size_t)written > cap) {
            return 0u;
        }
        memcpy(out, text, (size_t)written);
        return (size_t)written;
    }
#else
#error "RR3_SRV_BODY must be 0, 1, 2 or 3"
#endif
}

/*
 * Deliver the response BODY through the guest's own dataCallback.
 *
 * THE ARRAY HAS TO BE A REAL ONE. The guest's dataCallback copies it out
 * through GetByteArrayRegion, so a raw pointer would be read as a byte[]
 * handle and the copy would land on whatever the handle happened to be.
 * This is the same trap as the REQUEST body: the guest's own byte[] is
 * not a port array object, so the port's GetArrayLength answered 0 for
 * every request.
 *
 * The array is therefore built with the port's own env, and the port's
 * env IS the guest's env: the managed methods are dispatched with the
 * JNIEnv the guest passed (jni_bridge.cpp's jniCallVoidMethodV hands
 * `reinterpret_cast<JNIEnv*>(env)` to the dispatcher), and that env's
 * table is the guest's own g_jniTable. Its NewByteArray is slot 176 and
 * its SetByteArrayRegion is slot 208, so the array lands in the guest's
 * OWN byte-array pool - the same pool the guest's GetByteArrayRegion
 * (slot 200) reads back.
 *
 * Returns false when the native is missing or the array could not be
 * built. The caller completes the request either way: the completion is
 * what marks the response complete (the guest's record+0xc8), and the
 * response is still a response without a body.
 */
static bool deliverResponseBody(JNIEnv *env, CCHttpRequest *req,
                                const uint8_t *data, size_t len)
{
    static CCHttpDataFn data_callback = cloudcell_native<CCHttpDataFn>(
        kCCHttpDataCallback);

    if (data_callback == nullptr) {
        warning("HttpRequest: dataCallback export missing; no body delivered\n");
        return false;
    }

    jbyteArray array = env->NewByteArray((jsize)len);
    if (array == nullptr) {
        warning("HttpRequest: could not allocate the response byte array\n");
        return false;
    }
    if (len > 0u) {
        env->SetByteArrayRegion(array, 0, (jsize)len, (const jbyte *)data);
    }

    data_callback(env, (jobject)req, req->callback, array, (jint)len);

    if (authLogBudget()) {
        trace("cc-srvtime-body: delivered %zu bytes to the guest", (unsigned)len);
    }
    {
        char details[128];
        __builtin_snprintf(details, sizeof(details),
                           "body=%u bytes delivered before the completion",
                           (unsigned)len);
        rr3::guestViewWideLog("cc-srvtime-body",
                              static_cast<unsigned long long>(req->callback),
                              static_cast<unsigned long long>(len), details);
    }
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
 * THE DELIVERY ORDER IS THE JAVA ONE (HttpThread.run): headerCallback, then
 * the BODY (dataCallback), then completeCallback. The port used to deliver
 * the first and the last and never the body, which is not a shape the
 * guest's Java class can produce: ReadHttpData calls dataCallback for
 * every chunk it reads, and the guest's own dataCallback is the only
 * thing that grows the request record's pending body buffer
 * (record+0xd8) and its byte count (record+0xd4).
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

    /* The value is the CURRENT wall clock, read here at delivery time - the
     * host and the guest share the wall clock, so the guest's own clock and
     * this answer agree to within the delivery latency. */
    const ccheaders::Entry *server_time = ccheaders::serverTimeEntry();
    if (server_time == nullptr) {
        warning("HttpRequest: could not build the /time answer\n");
        return false;
    }

    const uintptr_t map = ccheaders::createMap(server_time, 1u);
    if (map == 0) {
        warning("HttpRequest: could not build the response header map\n");
        return false;
    }

    if (authLogBudget()) {
        trace("cc-srvtime: answering /time with header %s=%s (map=%p)",
              server_time->name, server_time->value, (void *)map);
    }
    {
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "url=%s header=%s value=%s map=%p", req->url,
                           server_time->name, server_time->value, (void *)map);
        rr3::guestViewWideLog("cc-srvtime",
                              static_cast<unsigned long long>(req->callback),
                              static_cast<unsigned long long>(req->bodyLen),
                              details);
    }

    ccprobe::probeWorker((uintptr_t)req->callback, "before-header");
    header_callback(env, (jobject)req, req->callback, 0, (jobject)map);
    if (authLogBudget()) {
        trace("cc-srvtime: headerCallback returned");
    }
    /* Did the header land in the guest's own map? The guest's ServerTimeManager
     * reads it out of that map and nothing else, so this is the measurement
     * that says whether the answer can reach this+0x88 at all. */
    ccprobe::probeWorker((uintptr_t)req->callback, "after-header");

    /* THE BODY, between the header and the completion - the guest's own
     * HttpThread.run() order. The guest's dataCallback copies it into the
     * request record's pending body buffer (record+0xd8) and grows its byte
     * count (record+0xd4), which is what cc::HttpRequestManager::Poll hands
     * to the response-complete delegate. */
    {
        uint8_t body[64];
        const size_t bodyLen = serverTimeBody(body, sizeof(body));
        deliverResponseBody(env, req, body, bodyLen);
    }

    complete_callback(env, (jobject)req, req->callback, 200);
    ccprobe::probeWorker((uintptr_t)req->callback, "after-complete");

    if (authLogBudget()) {
        trace("cc-srvtime: header delivered, completed with status 200");
    }
    return true;
}

/*
 * The /time answer.
 *
 * THE DELIVERY ORDER IS NOT THE BLOCKER - MEASURED 2026-10-21, archive
 * 20261021-100155-eden-srvinline.
 *
 * The hypothesis was: the guest's cc::HttpRequestManager::QueueRequest
 * (0x1ce1a84) pushes the request record into the manager's vector
 * (this+0x38..+0x40) only AFTER post() returns, and the guest's only poll
 * of the manager is the one inside cc::Cloudcell::Cloudcell (0xcd1014) -
 * a CONSTRUCTOR, so it runs once, at construction, before any answer can
 * exist.  Answering /time INLINE, inside post(), would therefore leave a
 * record that is already complete when the vector push happens.
 *
 * Answering /time inline was implemented and measured.  It changed
 * NOTHING: the record is still not in the manager's vector (cc-probe-req:
 * vecn=0, invec=0), c8 is still 1, c9 is still 0, "CC: Server time
 * returned" is still absent, and the guest still issues exactly one /time
 * request.  The same reading also shows the manager the port can see (the
 * guest's .bss global 0x1f97818) has an EMPTY vector, so the request is
 * registered in a DIFFERENT cc::HttpRequestManager instance - and the
 * poll that would deliver the handler never walks that one.
 *
 * The deferred delivery stays, because that is what the real Java
 * HttpThread does (it answers on its own thread) and because the inline
 * arm bought nothing.
 */
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
