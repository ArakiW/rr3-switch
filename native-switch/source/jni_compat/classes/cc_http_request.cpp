#include <stdio.h>
#include <new>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include <switch.h>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/so_util_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../../../include/rr3/jni_bridge.hpp"
#include "../jni.h"
#include "../jni_internals.h"
#include "cc_cloudcell_auth.h"
#include "cc_http_headers.h"
#include "cloudcell_defer.h"
#include "cloudcell_natives.h"
#include "rr3/guest_filesystem.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/rr3_md5.hpp"

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

/* The cc::HttpRequestManager census lives in namespace ccprobe, further
 * down this file.  It is declared here because the JNI hooks below are
 * the port's only reliable "the guest made a request" signal, and the
 * census needs the worker's address to tie a record to a request. */
namespace ccprobe {
void startMgr2Probe(void);
void noteMgr2Request(uintptr_t worker, const char *url, size_t bodyLen,
                     const uint8_t *body);
void noteMgr2TimeRequest(uintptr_t worker, const char *url, size_t bodyLen,
                         const uint8_t *body);
} // namespace ccprobe

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
     * The request URL.
     *
     * The guest builds the URL at runtime (snprintf("%04u-%s%s.cloudcell.com",
     * envId, role, stage) at guest raw 0x1cf0e60) and the path is appended by
     * the caller, so the two requests do NOT share a path: the guest's own log
     * in the measured run prints
     *     CC: [1] POST request [https://connect-dev-ss.cloudcell.com:443/time]
     * while the authenticate and sync requests go to CC_Sync.php.
     *
     * The URL used to be the ONLY discriminator, because the port could not
     * read the request body. It can now (see http_init), so the URL is the
     * fallback and the body's shape is the primary.
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

/* The member id the port compares the response against when the request
 * carries none. The guest compares it at 0x1d01dc4 against a read-only
 * constant of 1 at guest raw 0x600f30 (through the GOT slot 0x1f6e270), so 1
 * is the value that satisfies that comparison. */
static const uint32_t kDefaultMemberId = 1u;

/*
 * The device id this port issues.
 *
 * The guest's account manager keeps a server-issued device id, and a device
 * id of 0 is the one value it treats as "no device id at all": the guest
 * then logs "CC: Invalid DeviceId! Have we connected to cloudcell yet?"
 * and the sync queue never drains.
 *
 * The port is standing in for the server here, so the id is the port's to
 * issue. It is a fixed, non-zero value because there is one guest and one
 * account; a value that is not 0 and is not the member id, so that a log
 * line can tell the two apart.
 */
static const uint32_t kLocalDeviceId = 1001u;

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

/*
 * THE ASSET-PATH REQUEST -- the pack-hash endpoint.
 *
 * MEASURED, archive 20261021-165010-eden-netanswer2: the port received 60
 * requests whose URL is EMPTY ("cc-http-init ... url=(none)") and whose body
 * is a BinaryBlob box of mode 1 (zlib). The guest's own log says what they
 * are: every one of them is
 *
 *   CC: [8] POST request [https://connect-dev-ss.cloudcell.com:443//assetman/
 *         assetManDownloadPackHash.php]
 *
 * and every one of them ends in the guest logging
 *
 *   CC: ASSET DOWNLOAD ERROR! ... [MSG: Download failed - incomplete
 *      Download.] , url: https://.../assetManDownloadPackHash.php, httpCode : 0
 *   ******** CC_AssetManager_Class::AssetDownloadError() : Download attempt
 *          1 of 4 from Cloudcell has failed!  Asset (/MenuBannerData.xml)
 *
 * So the path the request is ABOUT is inside the body, and the port has to
 * read it out of there. The URL is not the discriminator: the port never
 * sees it.
 *
 * THE LAYOUT, measured from the payload heads in that archive's
 * "cc-req-class" lines (those are the UNBOXED payload, in hex words):
 *
 *   bodyLen=205  payloadLen=241  head=[00000013 6e654d2f 6e614275 4472656e
 *                                     2e617461 206c6d78 ...]
 *   bodyLen=184  payloadLen=280  head=[00000061 7373612f 5f737465 38343032
 *                                     33353178 76652f36 73746e65 70616d2f ...]
 *   bodyLen=328  payloadLen=624  head=[0000002c 7373612f 5f737465 78303834
 *                                     2f303233 6d6d6f63 732f6e6f ...]
 *   bodyLen=1487 payloadLen=2569 head=[0000005f 7373612f 5f737465 78303639
 *                                     2f303436 6e657665 6d2f7374 ...]
 *   bodyLen=134  payloadLen=145  head=[0000001c 6865762f 656c6369 61632f73
 *                                     70657272 73726961 7065722e 73726961 ...]
 *
 *   word 0   the path's length in bytes
 *   word 1+  the path, FOUR BYTES AT A TIME WITH EACH GROUP REVERSED
 *
 * THE RULE, checked against the guest's own asset names (the
 * AssetDownloadError lines name them, and they are the only ground truth
 * there is):
 *
 *   bodyLen=205  len=19  payload[4..22] = "/MenuBannerData.xml"
 *   bodyLen=184  len=97  payload[4.. ]  = "/assets_2048x1536/events/map/..."
 *   bodyLen=328  len=44  payload[4.. ]  = "/assets_480x320/common/sprites..."
 *   bodyLen=1487 len=95  payload[4.. ]  = "/assets_960x640/events/map/f..."
 *   bodyLen=134  len=28  payload[4.. ]  = "/vehicles/carrepairs.repairs"
 *
 * i.e. THE PATH IS PLAIN. The first four characters of the path region are
 * "/ass" for four of the five cases and "/veh" for the fifth, and the
 * length word is followed by exactly the path.
 *
 * ONE TRAP, and it is the one this decoder was first written wrong on: the
 * port's own log prints the payload head as little-endian u32 WORDS
 * ("%08x" of a word built from four bytes), so the WORD "6e654d2f" is the
 * BYTES 2f 4d 65 6e = "/Men". Reading the word's hex digits as ASCII
 * instead of un-swapping the bytes produces a path that looks scrambled and
 * is not: it suggests a four-byte-group reversal that the format does not
 * have. The decoder below reads the bytes.
 *
 * WHAT THIS DOES NOT DO. It does not answer the request. The port does not
 * know what the pack-hash endpoint's response looks like, and an invented
 * body would be a false success: the guest would take it for a server answer
 * and the run would say something it did not do. So the port LOGS the
 * request -- the body, the parsed path, and the fact that no answer shape is
 * known -- and the request still fails, honestly.
 */

/* Decode the asset path out of a request body.
 *
 * Returns true when a path was found and written to `path` (NUL terminated).
 * `payload` receives the unboxed payload so the caller can log it.
 */
static bool decodeAssetRequestPath(const uint8_t *body, size_t bodyLen,
                                    std::vector<uint8_t> &payload,
                                    char *path, size_t pathSize)
{
    if (path == nullptr || pathSize == 0) {
        return false;
    }
    path[0] = '\0';
    if (!cccloud::unboxBlob(body, bodyLen, payload) || payload.size() < 8u) {
        return false;
    }
    uint32_t length = 0;
    memcpy(&length, payload.data(), sizeof(length));
    if (length == 0u || length > 4096u) {
        return false;
    }
    /* The path region starts at payload[4], after the length word, and the
     * path is plain. */
    const size_t region = 4u;
    if (payload.size() < region + length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        const char c = static_cast<char>(payload[region + i]);
        if (c < 0x20 || c > 0x7e) {
            path[0] = '\0';
            return false;
        }
        path[i] = c;
    }
    path[length] = '\0';
    /* A path the guest would have asked for starts with '/'. */
    if (path[0] != '/') {
        path[0] = '\0';
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * THE ASSET-SERVER ENDPOINT -- assetManDownloadPackHash.php
 * ------------------------------------------------------------------ */

/*
 * WHAT THE GUEST WANTS BACK, AND WHY THE ANSWER IS NOT BOXED.
 *
 * The request is a Cloudcell BinaryBlob box, and cccloud::unboxBlob() takes
 * it apart; the PAYLOAD is the interesting part. It is a sequence of
 * triples, one per pending asset, in the guest's map order:
 *
 *     [u32 pathLen][path bytes]
 *     [u32 md5Len ][md5 hex, 32 characters]
 *     [u32 id     ]
 *
 * The guest's own reader of the answer is cc::AssetManager::
 * ProcessAssetServerData (guest raw 0x1cdbf08), and it reads the answer as a
 * sequence of blocks:
 *
 *     [u32 A][u32 B][B bytes]
 *
 * where A is the asset's id (the same u32 the request carried) and B is the
 * byte count. The first word of the block is compared against the pending
 * asset's id at 0x1cdc060, and the bytes are written to "<path>.ast" (the
 * ".ast" suffix is at guest raw 0x601e81, used at 0x1cdbbb0, 0x1cdc42c and
 * 0x1cdca10). After the write the guest MD5s what it received
 * (0x1cb60f4 is MD5 -- the 0x67452301/0xefcdab89/0x98badcfe/0x10325476
 * initial state) and compares it word by word with the manifest md5
 * (0x1cdcb90..0x1cdcbe4).
 *
 * BOXED OR RAW -- the answer is RAW, and the reason is the port's own
 * delivery contract:
 *
 *   1. The port hands the guest the authenticate and sync answers THROUGH
 *      dataCallback, and it hands them over BOXED (buildAuthResponse boxes
 *      twice: zlib inside XOR). The guest's sync reader unboxes them ITSELF
 *      (0x1d01380 and 0x1d0138c are two UnboxData calls on the body the
 *      callback delivered). So the Cloudcell HTTP transport passes the body
 *      through VERBATIM: it does not unbox for the consumer.
 *   2. The asset consumer does not unbox either. Neither
 *      CallbackAssetServerData (0x1cd96fc) nor ProcessAssetServerData
 *      (0x1cdbf08) calls UnboxData (0x1cbd8f0) -- the complete list of
 *      UnboxData call sites in the image has none in that range.
 *
 * A transport that unboxed and a consumer that unboxes cannot both be true,
 * and the measurement says the transport does not. So the body the asset
 * callback receives is the RAW HTTP body, and the raw body has to BE the
 * block sequence. The answer this port sends is therefore the block
 * sequence, unboxed.
 *
 * WHAT IS SERVED. The bytes are the bytes of a REAL local file: the port
 * never invents a body. A path with no local file is NOT answered -- it is
 * reported, and the guest takes its own error path for it.
 */

/* One asset the guest asked for. */
struct AssetRequest {
    char     path[256];
    char     md5[64];
    uint32_t id;
};

/* A response that has been built and is waiting to be delivered. */
struct PendingAssetResponse {
    const CCHttpRequest*     request = nullptr;
    uint64_t                 callback = 0;
    std::vector<uint8_t>     body;
};

static uint32_t readU32le(const uint8_t *data, size_t size, size_t off)
{
    if (data == nullptr || off + 4u > size) {
        return 0u;
    }
    return (uint32_t)data[off] |
           ((uint32_t)data[off + 1u] << 8) |
           ((uint32_t)data[off + 2u] << 16) |
           ((uint32_t)data[off + 3u] << 24);
}

static void appendU32le(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back((uint8_t)(value & 0xffu));
    out.push_back((uint8_t)((value >> 8) & 0xffu));
    out.push_back((uint8_t)((value >> 16) & 0xffu));
    out.push_back((uint8_t)((value >> 24) & 0xffu));
}

/*
 * Parse the asset request payload into (path, md5, id) triples.
 *
 * The payload is walked from the front. A FOUR BYTE HEADER is tolerated:
 * if the first word is not a path length the walk is retried one word in,
 * so a payload that starts with a count is still read.
 */
static bool parseAssetRequests(const std::vector<uint8_t> &payload,
                               std::vector<AssetRequest> &out,
                               size_t &baseOut)
{
    for (size_t base = 0; base <= 4u; base += 4u) {
        std::vector<AssetRequest> parsed;
        size_t off = base;
        while (off + 4u <= payload.size()) {
            const uint32_t pathLen = readU32le(payload.data(), payload.size(), off);
            if (pathLen == 0u || pathLen >= sizeof(AssetRequest::path)) {
                break;
            }
            if (off + 4u + pathLen > payload.size()) {
                break;
            }
            char path[sizeof(AssetRequest::path)];
            bool printable = true;
            for (uint32_t i = 0; i < pathLen; ++i) {
                const char c = (char)payload[off + 4u + i];
                if (c < 0x20 || c > 0x7e) {
                    printable = false;
                    break;
                }
                path[i] = c;
            }
            path[pathLen] = '\0';
            if (!printable) {
                break;
            }
            off += 4u + pathLen;

            if (off + 4u > payload.size()) {
                break;
            }
            const uint32_t md5Len = readU32le(payload.data(), payload.size(), off);
            off += 4u;
            if (md5Len == 0u || md5Len >= sizeof(AssetRequest::md5)) {
                break;
            }
            if (off + md5Len > payload.size()) {
                break;
            }
            char md5[sizeof(AssetRequest::md5)];
            bool md5Printable = true;
            for (uint32_t i = 0; i < md5Len; ++i) {
                const char c = (char)payload[off + i];
                if (c < 0x20 || c > 0x7e) {
                    md5Printable = false;
                    break;
                }
                md5[i] = c;
            }
            md5[md5Len] = '\0';
            off += md5Len;
            if (!md5Printable) {
                break;
            }

            if (off + 4u > payload.size()) {
                break;
            }
            const uint32_t id = readU32le(payload.data(), payload.size(), off);
            off += 4u;

            AssetRequest entry;
            memcpy(entry.path, path, pathLen + 1u);
            memcpy(entry.md5, md5, md5Len + 1u);
            entry.id = id;
            parsed.push_back(entry);
        }
        if (!parsed.empty()) {
            out.swap(parsed);
            baseOut = base;
            return true;
        }
    }
    return false;
}

/*
 * The local files that can back a requested path, in the order they are
 * tried.
 *
 * 1. The guest's OWN depot on the card, under the same relative name: the
 *    game's installed copy of the asset. ".depot" + the server path.
 * 2. The volume tree with the same relative name: "files/.depot" + path.
 * 3. The gui_assets tree, for the server's "/assets_<res>/" spelling: the
 *    volumes ship "files/.depot/gui_assets/<res>/...".
 *
 * AND THE REVERSE ALIAS, FOR THE GUEST'S OWN SPELLING.
 *
 * MEASURED 2026-10-22, archive 20261022-031343-eden-gfsalias-r1 and
 * 20261022-032739-eden-gfsalias2-r1: the last 12 "didn't match its MD5"
 * failures were ONE asset -- /gui_assets/960x640/common/sprites.android.atlas
 * -- and here is why.
 *
 * The guest asks for it under the "gui_assets/<res>/" spelling, but the ONLY
 * manifest that names that asset names it under the other spelling:
 * "/assets_960x640/common/sprites.android.atlas" in asset_list_updates.txt.
 * The source files/apk/res/asset_list_gui_assets_core.txt (365,312 bytes, 3,292
 * entries, md5 8e5323fd000fbfb95d0340dc8e9f9158) has NO entry for the bare
 * "gui_assets/<res>/common/sprites.android.atlas" at all -- only sub-paths
 * like gui_assets/960x640/formula_e/common/... So the guest applies the game's
 * own spelling rule (gui_assets/<res>/X is assets_<res>/X) and checks the
 * bytes it received for the "gui_assets" request against the digest published
 * for the "assets_960x640" entry. ONE digest, TWO spellings.
 *
 * So the two spellings have to reach the SAME file, and this is the file:
 *
 *   .depot/assets_960x640/common/sprites.android.atlas   24,689 B  md5 43213527...
 *   files/.depot/gui_assets/960x640/common/sprites.android.atlas
 *                                                      39,199 B  md5 c7256882...
 *
 * Two different revisions. The card's copy is the one the engine renders from
 * -- with the card's copy in the manifest the guest resolved 1,190 sprite
 * references and 265 gui/processed references, against 606 and 58 with the
 * volume's (archive 20261022-015912-eden-md5alias-r3 vs
 * 20261022-014615-eden-md5fix-v3-r1) -- so the card's copy is the one the
 * digest has to describe.
 *
 * Before this change the candidate list for "/gui_assets/<res>/X" held only
 * the card's gui_assets tree (which does not ship that atlas) and the
 * volume's gui_assets tree (which ships a different revision), so the guest's
 * requested digest matched NEITHER and the endpoint answered with the volume's
 * bytes -- hence "didn't match its MD5". Adding the card's assets_<res> copy
 * as a candidate lets the digest tie-breaker below find it.
 *
 * ORDER MATTERS for the fallback (the first candidate that exists wins when no
 * digest matches): the card's assets_<res> copy goes LAST, because it is the
 * stale revision -- see the note at its insertion point.
 *
 * THE LIST MUST STAY IN LOCKSTEP with the manifest-rewrite resolver in
 * native-switch/source/guest_filesystem.cpp (assetCandidates() there). The
 * digest written into the list is the digest of the bytes this endpoint
 * sends; two copies of the list in two files can only ever agree by accident.
 */
static bool isResolutionSegment(const char *text)
{
    if (text == nullptr) {
        return false;
    }
    const char *cursor = text;
    int digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0 || *cursor != 'x') {
        return false;
    }
    ++cursor;
    digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    return *cursor == '\0' || *cursor == '/';
}

static unsigned assetCandidates(const char *path, char out[4][384])
{
    unsigned count = 0;
    if (path == nullptr) {
        return 0u;
    }
    __builtin_snprintf(out[count], sizeof(out[count]), ".depot%s", path);
    ++count;
    __builtin_snprintf(out[count], sizeof(out[count]), "files/.depot%s", path);
    ++count;
    if (strncmp(path, "/assets_", 8) == 0) {
        const char *rest = strchr(path + 8, '/');
        if (rest != nullptr) {
            char resolution[32];
            const size_t n = (size_t)(rest - (path + 8));
            if (n < sizeof(resolution)) {
                memcpy(resolution, path + 8, n);
                resolution[n] = '\0';
                __builtin_snprintf(out[count], sizeof(out[count]),
                                   "files/.depot/gui_assets/%s%s", resolution, rest);
                ++count;
            }
        }
    }
    /* THE REVERSE ALIAS GOES LAST, NOT SECOND.
     *
     * The card's "assets_<res>" tree is the game's INSTALLED revision and the
     * volume's "gui_assets/<res>" tree is the shipped data-pack revision. For
     * 960x640 they are DIFFERENT files:
     *
     *   .depot/assets_960x640/common/sprites.android.atlas        24,689 B
     *       md5 4321352770b573a6b71d57547b2b3885   -- 165 image entries,
     *       and MISSING ten menu sprites (gradient.png, common_info_button.png,
     *       common_info_button_down.png, fb_logo.png, apple_sign_in_icon.png,
     *       ...). Serving this one cost 20 "Failed to load image from atlas"
     *       errors over 10 distinct sprites (archive
     *       20261022-031343-eden-gfsalias-r1).
     *   files/.depot/gui_assets/960x640/common/sprites.android.atlas
     *                                                            39,199 B
     *       md5 c72568828875f4e2a6b11f5c944e27e0   -- 248 image entries, the
     *       sprite-complete one, and byte-identical to the card's
     *       .depot/gui_assets/960x640/common/sprites_0.rgb.pvr.z.
     *
     * So the card's assets_<res> copy is a STALE revision and is a LAST-RESORT
     * fallback only. Putting it second made it win the endpoint's "first
     * candidate that exists" fallback, which is what the fallback is FOR when
     * no digest matches -- and it put a digest in the manifests that the
     * runtime read path (which serves the volume) could never produce.
     *
     * The endpoint's md5 tie-breaker makes the ORDER irrelevant when a digest
     * matches; it is the fallback that the order decides, so the stale copy
     * goes last.
     */
    if (strncmp(path, "/gui_assets/", 12) == 0) {
        const char *rest = strchr(path + 12, '/');
        if (rest != nullptr) {
            char resolution[32];
            const size_t n = (size_t)(rest - (path + 12));
            if (n < sizeof(resolution)) {
                memcpy(resolution, path + 12, n);
                resolution[n] = '\0';
                if (isResolutionSegment(resolution)) {
                    __builtin_snprintf(out[count], sizeof(out[count]),
                                       ".depot/assets_%s%s", resolution, rest);
                    ++count;
                }
            }
        }
    }
    return count;
}

/* Read a local file through the port's own filesystem and MD5 it. */
static bool readLocalAsset(const char *guestPath, std::vector<uint8_t> &out,
                           char digest[33], unsigned long long &sizeOut)
{
    out.clear();
    sizeOut = 0u;
    digest[0] = '\0';
    std::FILE *file = rr3::guestFopen(guestPath, "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize((size_t)size);
    const size_t got = (size > 0) ? std::fread(out.data(), 1, (size_t)size, file)
                                  : 0u;
    std::fclose(file);
    if (got != (size_t)size) {
        out.clear();
        return false;
    }
    sizeOut = (unsigned long long)size;
    rr3::Md5 md5;
    md5.begin();
    if (size > 0) {
        md5.update(out.data(), out.size());
    }
    md5.finish(digest);
    return true;
}

/* The response bodies waiting to be delivered, and the lock that guards
 * them. The body is built in init() (where the request is read) and
 * delivered in post() (where the guest's own Java thread would have
 * answered), so it has to survive the gap between the two calls. */
static std::mutex g_assetResponsesMutex;
static std::deque<PendingAssetResponse> g_assetResponses;

static void queueAssetResponse(const CCHttpRequest *request, uint64_t callback,
                               std::vector<uint8_t> &body)
{
    PendingAssetResponse pending;
    pending.request  = request;
    pending.callback = callback;
    pending.body     = body;
    std::lock_guard<std::mutex> lock(g_assetResponsesMutex);
    g_assetResponses.push_back(pending);
}

static bool takeAssetResponse(const CCHttpRequest *request, uint64_t callback,
                              std::vector<uint8_t> &body)
{
    std::lock_guard<std::mutex> lock(g_assetResponsesMutex);
    for (auto it = g_assetResponses.begin(); it != g_assetResponses.end(); ++it) {
        if (it->request == request && it->callback == callback) {
            body = it->body;
            g_assetResponses.erase(it);
            return true;
        }
    }
    return false;
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

    /* Keep the body.
     *
     * MEASURED, archive 20261021-134632-eden-getinfobool:
     *
     *     cc-http-init: body=1716 url=https://connect-dev-ss.cloudcell.com:443/CC_Sync.php
     *     cc-http-init: body=0    url=https://connect-dev-ss.cloudcell.com:443/time
     *
     * so the body IS readable, and the /time request genuinely carries no
     * body at all. The record the guest hands over is one of the port's own
     * FakeByteArray records: the guest's own JNI glue creates it with slot
     * 176 (NewByteArray, guest raw 0x1ce37a4) and fills it with slot 208
     * (SetByteArrayRegion, guest raw 0x1ce37e8), and both of those slots are
     * this port's own jniNewByteArray / jniSetByteArrayRegion.
     *
     * The length is therefore read from the record's own layout - length at
     * +0x04, data pointer at +0x10 - rather than through a JNIEnv table.
     * The JNIEnv route is kept as a fallback so the port still works if the
     * handle ever comes from somewhere else.
     */
    req->bodyLen = 0;
    if (body != nullptr) {
        int32_t      viewLen  = 0;
        const uint8_t *viewPtr = nullptr;
        if (rr3::rr3ByteArrayView((uintptr_t)body, &viewLen, &viewPtr)) {
            size_t take = ((size_t)viewLen < kMaxBodyBytes) ? (size_t)viewLen
                                                             : kMaxBodyBytes;
            if (take > 0u && viewPtr != nullptr) {
                memcpy(req->body, viewPtr, take);
                req->bodyLen = take;
            }
        } else {
            /* The fallback: the same two JNI calls the port used before. */
            jsize len = env->GetArrayLength(body);
            if (len > 0) {
                size_t take = ((size_t)len < kMaxBodyBytes) ? (size_t)len
                                                            : kMaxBodyBytes;
                env->GetByteArrayRegion(body, 0, (jsize)take,
                                        (jbyte *)req->body);
                req->bodyLen = take;
            }
        }
    }

    /* THE ONE-LINE HANDLE PROBE.
     *
     * The first four bytes of the handle decide whether the body is a port
     * byte-array record at all: 'ARRB' (0x42525241) means it is one, and
     * the length is then at +0x04. Anything else means the handle is not a
     * record and the length of 0 is the port's own answer, not the guest's
     * fault. */
    {
        uint32_t magic = 0;
        const bool isRecord =
            rr3::rr3ByteArrayMagic((uintptr_t)body, &magic);
        if (authLogBudget()) {
            trace("cc-body-probe: handle=%p magic=%08x %s len=%zu",
                  (void *)body, magic,
                  isRecord ? "(ARRB record)" : "(not a record)",
                  (unsigned)req->bodyLen);
        }
        {
            char details[192];
            __builtin_snprintf(details, sizeof(details),
                               "handle=%p magic=%08x %s bodyLen=%u",
                               (void *)body, magic,
                               isRecord ? "ARRB-record" : "not-a-record",
                               (unsigned)req->bodyLen);
            rr3::guestViewWideLog("cc-body-probe",
                                  (unsigned long long)(uintptr_t)body,
                                  (unsigned long long)magic, details);
        }
    }

    /* One line per request, so "the guest never calls this class" is a
     * MEASURED statement and not an inference from the absence of the
     * http.post deferral line. The URL is on the same line because it is
     * what identifies the request, and the first sixteen body bytes are on
     * it because the body's own head is what says what the request is. */
    if (authLogBudget()) {
        char head[80];
        const size_t headLen = (req->bodyLen < 16u) ? req->bodyLen : 16u;
        for (size_t i = 0; i < headLen; ++i) {
            __builtin_snprintf(head + (i * 2), 3u, "%02x", req->body[i]);
        }
        head[headLen * 2u] = '\0';
        trace("cc-http-init: body=%zu head=%s url=%s", (unsigned)req->bodyLen,
              head, req->urlLen > 0u ? req->url : "(none)");
    }
    {
        char details[256];
        char head[80];
        const size_t headLen = (req->bodyLen < 16u) ? req->bodyLen : 16u;
        for (size_t i = 0; i < headLen; ++i) {
            __builtin_snprintf(head + (i * 2), 3u, "%02x", req->body[i]);
        }
        head[headLen * 2u] = '\0';
        __builtin_snprintf(details, sizeof(details),
                           "bodyLen=%u head=%s url=%s",
                           (unsigned)req->bodyLen, head,
                           req->urlLen > 0u ? req->url : "(none)");
        rr3::guestViewWideLog("cc-http-init", (unsigned long long)req->bodyLen,
                              0, details);
    }

    /*
     * THE ASSET-PATH REQUEST (the pack-hash endpoint).
     *
     * The port received 60 of these in archive 20261021-165010-eden-netanswer2
     * and answered none, so the guest logged 60 "AssetDownloadError" lines
     * with no way to say what had been asked for. The path is inside the
     * body -- the URL the port sees is EMPTY -- so it is decoded here.
     *
     * The request is now ANSWERED, with the real bytes of real local files:
     * see the block comment on the asset endpoint above for the answer's
     * shape and for why the answer is not boxed.
     *
     * The cap is 64 lines, and the measured run makes 60, so a normal run is
     * traced in full. The counter is a COUNTER, not a cap on the truth: the
     * "n=" field says how many arrived.
     */
    {
        static unsigned s_assetReqLines = 0;
        static unsigned s_assetReqTotal = 0;
        ++s_assetReqTotal;
        std::vector<uint8_t> payload;
        char path[512];
        const bool assetShaped =
            decodeAssetRequestPath(req->body, req->bodyLen, payload, path,
                                   sizeof(path));
        if (assetShaped) {
            /* The full request: the triples, one per pending asset. */
            std::vector<AssetRequest> entries;
            size_t base = 0;
            const bool parsed = parseAssetRequests(payload, entries, base);

            std::vector<uint8_t> body;
            unsigned served = 0;
            unsigned missing = 0;
            for (size_t i = 0; parsed && i < entries.size(); ++i) {
                const AssetRequest &entry = entries[i];

                /* Which local file backs this path. The md5 the guest
                 * asked for is the tie breaker: a candidate whose md5
                 * matches is the file the guest wants, whatever else is
                 * lying around under a similar name. */
                char candidates[4][384];
                const unsigned candidateCount =
                    assetCandidates(entry.path, candidates);
                const char *chosen = nullptr;
                char chosenMd5[33] = {0};
                unsigned long long chosenSize = 0;
                std::vector<uint8_t> chosenBytes;
                for (unsigned c = 0; c < candidateCount; ++c) {
                    std::vector<uint8_t> bytes;
                    char digest[33];
                    unsigned long long size = 0;
                    if (!readLocalAsset(candidates[c], bytes, digest, size)) {
                        continue;
                    }
                    /* The FIRST candidate that exists is the fallback; a
                     * candidate whose md5 matches the one the guest asked
                     * for REPLACES it. Both are the same move: the bytes
                     * travel with the choice.
                     *
                     * ONE MOVE, NOT TWO. The first version of this ran the
                     * move twice -- once for "first candidate that exists"
                     * and once for "the md5 matches" -- and the second move
                     * took the bytes OUT of the buffer that was about to be
                     * appended: the block went out with size 0 and the guest
                     * answered "not found on server or download failed."
                     * (measured, archive 20261021-223727-eden-assetdl3: the
                     * assets whose md5 MATCHED were the ones that failed,
                     * and the ones whose md5 differed -- the bytes kept by
                     * the first branch -- arrived.) */
                    const bool isMatch = (strcasecmp(digest, entry.md5) == 0);
                    if (chosen == nullptr || isMatch) {
                        chosen = candidates[c];
                        chosenBytes = std::move(bytes);
                        chosenSize = size;
                        memcpy(chosenMd5, digest, sizeof(chosenMd5));
                    }
                    if (isMatch) {
                        break;
                    }
                }

                if (chosen == nullptr) {
                    /* NO LOCAL FILE. Nothing is invented for it: the
                     * guest takes its own error path for this asset. */
                    ++missing;
                    if (s_assetReqLines < 64u) {
                        ++s_assetReqLines;
                        char details[640];
                        __builtin_snprintf(
                            details, sizeof(details),
                            "n=%u id=%u path=%s md5=%s candidates=%u"
                            " answered=0 reason=no-known-answer-shape"
                            " detail=no-local-file",
                            s_assetReqTotal, entry.id, entry.path, entry.md5,
                            candidateCount);
                        rr3::guestViewWideLog("cc-asset-req",
                                              (unsigned long long)entry.id,
                                              (unsigned long long)s_assetReqTotal,
                                              details);
                    }
                    continue;
                }

                /* The block the guest's reader expects:
                 *     [u32 id][u32 size][the bytes] */
                appendU32le(body, entry.id);
                appendU32le(body, (uint32_t)chosenBytes.size());
                body.insert(body.end(), chosenBytes.begin(), chosenBytes.end());
                ++served;

                if (s_assetReqLines < 64u) {
                    ++s_assetReqLines;
                    const bool md5Matches =
                        (strcasecmp(chosenMd5, entry.md5) == 0);
                    char details[1200];
                    __builtin_snprintf(
                        details, sizeof(details),
                        "n=%u id=%u path=%.250s want_md5=%.40s src=%.380s"
                        " size=%llu src_md5=%.32s answered=1 md5_match=%d",
                        s_assetReqTotal, entry.id, entry.path, entry.md5,
                        chosen, chosenSize, chosenMd5, md5Matches ? 1 : 0);
                    rr3::guestViewWideLog("cc-asset-req",
                                          (unsigned long long)entry.id,
                                          (unsigned long long)s_assetReqTotal,
                                          details);
                    if (authLogBudget()) {
                        trace("cc-asset-req: id=%u path=%s -> %s (%llu B)"
                              " md5 %s (want %s)",
                              entry.id, entry.path, chosen, chosenSize,
                              chosenMd5, entry.md5);
                    }
                }
            }

            if (served > 0u) {
                queueAssetResponse(req, (uint64_t)req->callback, body);
                if (s_assetReqLines < 64u) {
                    ++s_assetReqLines;
                    char details[320];
                    __builtin_snprintf(
                        details, sizeof(details),
                        "n=%u assets=%u served=%u missing=%u bytes=%u"
                        " answered=1 reason=real-local-bytes",
                        s_assetReqTotal, (unsigned)entries.size(), served,
                        missing, (unsigned)body.size());
                    rr3::guestViewWideLog("cc-asset-req",
                                          (unsigned long long)body.size(),
                                          (unsigned long long)s_assetReqTotal,
                                          details);
                }
            } else if (parsed) {
                if (s_assetReqLines < 64u) {
                    ++s_assetReqLines;
                    char details[320];
                    __builtin_snprintf(
                        details, sizeof(details),
                        "n=%u assets=%u served=0 missing=%u"
                        " answered=0 reason=no-known-answer-shape"
                        " detail=no-local-file",
                        s_assetReqTotal, (unsigned)entries.size(), missing);
                    rr3::guestViewWideLog("cc-asset-req", 0,
                                          (unsigned long long)s_assetReqTotal,
                                          details);
                }
            } else if (s_assetReqLines < 64u) {
                ++s_assetReqLines;
                char details[1024];
                __builtin_snprintf(
                    details, sizeof(details),
                    "n=%u bodyLen=%u payloadLen=%u url=%.200s path=%.500s"
                    " answered=0 reason=no-known-answer-shape"
                    " detail=payload-not-asset-shaped",
                    s_assetReqTotal, (unsigned)req->bodyLen,
                    (unsigned)payload.size(),
                    req->urlLen > 0u ? req->url : "(none)", path);
                rr3::guestViewWideLog("cc-asset-req",
                                      (unsigned long long)req->bodyLen,
                                      (unsigned long long)s_assetReqTotal, details);
            }
        }
    }

    /* Start the cc::HttpRequestManager census, and tell it which
     * request this is.  The census runs on its own host thread at 1 Hz;
     * all it needs from here is the worker's address (the jlong the
     * guest handed init()) and the identity of the request the port is
     * serving, so the record it measures can be tied to a URL. */
    ccprobe::startMgr2Probe();
    ccprobe::noteMgr2Request((uintptr_t)req->callback, req->url, req->bodyLen,
                              (const uint8_t *)req->body);
}

static void http_add_header(JNIEnv *, jobject, jstring, jstring) {}

/*
 * THE AUTHENTICATE RESPONSE, AND THE URL FALLBACK FOR IT.
 *
 * The body-based match is the PRIMARY discriminator: cccloud::parseRequest()
 * unboxes the body and reads its shape, and it is the only thing that can
 * tell the authenticate request from the ordinary sync request, because the
 * two share the same path (CC_Sync.php) and the same first four words.
 *
 * The body is READABLE.  MEASURED, archive 20261021-134632-eden-getinfobool:
 *
 *     cc-http-init: body=1716 url=https://connect-dev-ss.cloudcell.com:443/CC_Sync.php
 *
 * What used to go wrong is not that the body was unreadable but that it was
 * read as if it were the PAYLOAD while the bytes on the wire are a BinaryBlob
 * BOX.  The first word of the wire bytes is the box's checksum, not the
 * request's tag, so every parse failed, the port fell through to the URL
 * fallback, and the port answered the ordinary SYNC request with the
 * authenticate response.  The guest unpacked that with its sync reader, read
 * the member id out of the authenticate state word, and then ran off the end
 * of the blob: 2980 "UnpackData" assertions at BinaryBlob.cpp line 375.
 *
 * So the URL is the LAST resort, and the log says WHICH discriminator was
 * used, because "matched" and "matched-by-url" are different claims and
 * this project keeps confusing them:
 *
 *   cc-auth-req: matched          the body had the authenticate shape
 *   cc-req-class: kind=sync       the body had the sync shape
 *   cc-auth-req: matched-by-url   the request carried no body to read
 *
 * The fallback is ONE-SHOT: the authenticate response is delivered at most
 * once per process, so a later CC_Sync.php request is not answered with a
 * second authenticate response.
 */
static bool g_authDelivered = false;

/* The guest's own path string for the sync/authenticate endpoint. */
static const char kAuthUrlMarker[] = "CC_Sync.php";

/* Build the authenticate response and hand it to the guest the way
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
        rr3::guestViewWideLog("cc-auth-fail", 0, 0,
                               "natives missing (data/complete export)");
        return false;
    }

    const uint32_t memberId = info.hasMemberId ? info.memberId : kDefaultMemberId;

    std::vector<uint8_t> body;
    if (!cccloud::buildAuthResponse(memberId, body)) {
        warning("HttpRequest: could not build the authenticate response\n");
        rr3::guestViewWideLog("cc-auth-fail", memberId, 0,
                               "could not build the authenticate response");
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
        rr3::guestViewWideLog("cc-auth-fail", memberId,
                               (unsigned long long)body.size(),
                               "could not allocate the response byte array");
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
    g_authDelivered = true;
    return true;
}

/*
 * Deliver the SYNC response.
 *
 * The guest's own log says which request this answers: "CC: Sending Sync
 * with SubSyncs:" (archive 20261021-134632-eden-getinfobool) is printed
 * before the request goes out, and "CC: [2] Sync response received" when
 * the answer comes back.
 *
 * The delivery is the Java one: the body first, then the completion, the
 * same order deliverAuthSuccess uses and the same order the guest's own
 * HttpThread.run() uses.
 */
static bool deliverSyncResponse(JNIEnv *env, CCHttpRequest *req,
                                 uint32_t memberId, uint32_t deviceId)
{
    static CCHttpDataFn data_callback = cloudcell_native<CCHttpDataFn>(
        kCCHttpDataCallback);
    static CCHttpCompleteFn complete_callback = cloudcell_native<CCHttpCompleteFn>(
        kCCHttpCompleteCallback);

    if (data_callback == nullptr || complete_callback == nullptr) {
        warning("HttpRequest: success natives missing (data=%p complete=%p); "
                "sync request %p stays pending\n",
                (void *)data_callback, (void *)complete_callback, (void *)req);
        rr3::guestViewWideLog("cc-sync-fail", memberId, deviceId,
                               "natives missing (data/complete export)");
        return false;
    }

    std::vector<uint8_t> body;
    if (!cccloud::buildSyncResponse(memberId, deviceId, body)) {
        warning("HttpRequest: could not build the sync response\n");
        rr3::guestViewWideLog("cc-sync-fail", memberId, deviceId,
                               "could not build the sync response");
        return false;
    }

    if (authLogBudget()) {
        trace("cc-sync-resp: produced member=%u device=%u body=%zu",
              memberId, deviceId, (unsigned)body.size());
    }
    rr3::guestViewWideLog("cc-sync-resp", memberId, (unsigned long long)body.size(),
                           "sync response built for the guest");

    jbyteArray array = env->NewByteArray((jsize)body.size());
    if (array == nullptr) {
        warning("HttpRequest: could not allocate the response byte array\n");
        rr3::guestViewWideLog("cc-sync-fail", memberId,
                               (unsigned long long)body.size(),
                               "could not allocate the response byte array");
        return false;
    }
    env->SetByteArrayRegion(array, 0, (jsize)body.size(),
                             (const jbyte *)body.data());

    data_callback(env, (jobject)req, req->callback, array, (jint)body.size());
    complete_callback(env, (jobject)req, req->callback, 200);

    if (authLogBudget()) {
        trace("cc-sync-sent: member=%u device=%u bytes=%zu status=200",
              memberId, deviceId, (unsigned)body.size());
    }
    rr3::guestViewWideLog("cc-sync-sent", memberId, (unsigned long long)body.size(),
                           "sync response delivered to the guest");
    return true;
}

/*
 * THE ASSET-SERVER RESPONSE.
 *
 * The body was built in init() (that is where the request is read) and is
 * delivered here, on the port's deferred thread, the way the guest's own
 * HttpThread would have delivered it: the data first, then the completion.
 *
 * The body is the RAW block sequence -- [u32 id][u32 size][bytes] per asset
 * -- and it is NOT boxed. The reason is in the block comment on the asset
 * endpoint above: the Cloudcell transport hands the body to the consumer
 * verbatim (the sync answer proves it -- the port delivers a double-boxed
 * body and the guest's sync reader unboxes it itself), and the asset
 * consumer never unboxes, so the raw body has to be the block sequence.
 */
static bool deliverAssetResponse(JNIEnv *env, CCHttpRequest *req)
{
    static CCHttpDataFn data_callback = cloudcell_native<CCHttpDataFn>(
        kCCHttpDataCallback);
    static CCHttpCompleteFn complete_callback = cloudcell_native<CCHttpCompleteFn>(
        kCCHttpCompleteCallback);

    std::vector<uint8_t> body;
    if (!takeAssetResponse(req, (uint64_t)req->callback, body)) {
        return false;
    }
    /* AN EMPTY BODY IS STILL AN ANSWER.
     *
     * The port omits the block of an asset it has no file for -- nothing is
     * invented for it -- so a request whose assets are ALL missing produces an
     * empty block sequence. The old code refused to deliver an empty body,
     * which meant the request was NEVER answered: the guest's HTTP worker
     * kept it pending, the completion never came, and the guest logged
     *
     *   CC: ASSET DOWNLOAD ERROR! [...] [MSG: Download failed - incomplete
     *   Download.] ... httpCode : 0
     *
     * httpCode 0 is the transport-level "no answer at all", not an HTTP
     * error the guest can handle. Measured in archive
     * 20261022-032739-eden-gfsalias2-r1: a request with two assets, both
     * missing, was logged as "assets=2 served=0 missing=2 answered=0
     * reason=no-known-answer-shape detail=no-local-file" and then never
     * completed. The five assets that are genuinely absent from all seven
     * volumes (the lexus_is_f350_catalunya weekly-event pair at 2048x1536 and
     * 480x320, menu-carselect_porsche.banim, particle_test.pvr,
     * detailmap_road_surface_norm_b.pvr) are all in this shape.
     *
     * Delivering the empty body completes the request with HTTP 200 and a
     * body the guest's reader walks from end to end finding nothing -- which
     * is the same "incomplete download" the guest reports, but on a request
     * that is FINISHED. It cannot affect the assets that do resolve: the
     * body is only empty when nothing at all was served.
     */
    if (data_callback == nullptr || complete_callback == nullptr) {
        warning("HttpRequest: success natives missing (data=%p complete=%p); "
                "asset request %p stays pending\n",
                (void *)data_callback, (void *)complete_callback, (void *)req);
        rr3::guestViewWideLog("cc-asset-deliver", 0,
                               (unsigned long long)body.size(),
                               "FAILED (natives missing)");
        return false;
    }

    jbyteArray array = env->NewByteArray((jsize)body.size());
    if (array == nullptr) {
        warning("HttpRequest: could not allocate the asset response array\n");
        rr3::guestViewWideLog("cc-asset-deliver", 0,
                               (unsigned long long)body.size(),
                               "FAILED (no byte array)");
        return false;
    }
    env->SetByteArrayRegion(array, 0, (jsize)body.size(),
                             (const jbyte *)body.data());

    if (body.empty()) {
        /* Say it plainly in the log: this request WAS answered, with nothing
         * in it. "the guest never got an answer" and "the guest got an empty
         * answer" are different facts, and only the second one lets the guest
         * finish the request. */
        rr3::guestViewWideLog("cc-asset-deliver", 2, 0,
                               "empty answer delivered (every asset in this "
                               "request had no local file); status=200");
    }

    data_callback(env, (jobject)req, req->callback, array, (jint)body.size());
    complete_callback(env, (jobject)req, req->callback, 200);

    if (authLogBudget()) {
        trace("cc-asset-sent: bytes=%zu status=200", (unsigned)body.size());
    }
    rr3::guestViewWideLog("cc-asset-deliver", 1, (unsigned long long)body.size(),
                           "asset response delivered to the guest");
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
 * The cc::HttpRequestManager resolver.  Defined with the stage-2 census
 * further down this file; declared here because the first probe
 * (describeActiveRequests) needs the SAME manager and must not keep
 * asking the .bss word that is not one.
 */
static bool mgr2Resolve(uintptr_t bias, uintptr_t *ownerOut, uintptr_t *mgrOut,
                        uint64_t *vptrOut);
static bool mgr2ResolveViaRecord(uintptr_t rec, uintptr_t bias,
                                  uintptr_t *mgrOut, uint64_t *vptrOut);

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
static void describeActiveRequests(uintptr_t worker, unsigned long long frames)
{
    /* The 4-SAMPLE CAP IS REMOVED (2026-10-21).  It made the probe
     * DEAD from about frame 960 onward: in a 16200-frame run that left
     * 94% of the run unobserved, and "the record never changed" was
     * indistinguishable from "the probe stopped looking".  The cost is
     * one sample per 240-frame tick - about 67 samples over a whole
     * run - and every field read lives on the record's own page, whose
     * svcQueryMemory query is already cached, so the frame budget is
     * untouched.  The bound that remains is a safety net, not a policy. */
    static unsigned s_samples = 0u;
    if (s_samples >= 400u) {
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
     * THE MANAGER IS NOT THE .bss WORD AT 0x1f97818.  That word holds
     * a pointer whose vptr is 0x1e4b948 (a different class), so
     * everything the earlier version of this probe said about
     * "mgr=" / "vecn=" / "invec=" was about the wrong object.
     *
     * The real manager is reached the way the guest's own code reaches
     * it:  cc::HttpRequestManager's factory (guest raw 0x1cbe23c) does
     * new(0x60), constructs it (0x1ce2644) and stores it at owner+0x218;
     * the owner is published in .bss at 0x1fc6e98 (GOT slot 0x1f6d5d0) by
     * 0x1cbe028.  So:
     *
     *   owner = *(bias + 0x1fc6e98)
     *   mgr   = *(owner + 0x218)
     *
     * and the manager's vptr MUST be bias + 0x1f64858.  The vptr is
     * checked before its +0x38/+0x40 are read, so a failed chain can
     * never be reported as an empty vector.
     *
     * The vector at this+0x38 (begin) / this+0x40 (end) holds 8-byte
     * POINTERS to records, not the records themselves: the guest's
     * QueueRequest (0x1ce1a84) does "str x20, [x9], #8" to append.
     */
    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    uint64_t mgr = 0, begin = 0, end = 0;
    uint64_t mgrvptr = 0;
    uint64_t vecn = 0;
    bool found = false;
    uintptr_t owner = 0;
    uintptr_t mgrAddr = 0;
    uint64_t mgrVptr = 0;
    /* PRIMARY: the record's own back-pointer (record+0xc0). */
    uintptr_t mgrRec = 0;
    uint64_t mgrRecVptr = 0;
    if (mgr2ResolveViaRecord(static_cast<uintptr_t>(rec), bias, &mgrRec,
                             &mgrRecVptr) && mgrRec != 0) {
        mgrAddr = mgrRec;
        mgrVptr = mgrRecVptr;
    } else if (mgr2Resolve(bias, &owner, &mgrAddr, &mgrVptr) &&
               mgrAddr != 0) {
        /* the independent owner route, as a fallback */
    }
    if (mgrAddr != 0) {
        mgr = mgrAddr;
        mgrvptr = mgrVptr;
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
                       /* "gf=", not "frames=": the Eden harness's instrument
                        * census reads the LAST /frames=(\d+)/ in the guest log
                        * as the guest's frame total, and a probe line that
                        * says "frames=" would overwrite it. */
                       "gf=%llu worker=0x%llx rec=0x%llx token=0x%llx "
                       "c8=%llu c9=%llu d4=%llu handler100=0x%llx "
                       "body130=0x%llx parser160=0x%llx status=0x%llx | "
                       "mgr=0x%llx mgrvptr=0x%llx vecn=%llu invec=%d",
                       frames, (unsigned long long)worker, (unsigned long long)rec,
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

/* ------------------------------------------------------------------ *
 * STAGE 2: THE cc::HttpRequestManager CENSUS   (tag: cc-probe-mgr2)
 * ------------------------------------------------------------------ *
 *
 * WHY A SECOND STAGE EXISTS.  The first probe (cc-probe-req) asked the
 * .bss word at 0x1f97818 for "the HTTP manager".  That word does not
 * hold one.  Archive 20261021-105547-eden-srvbody2 shows it holding
 * 0x2103b514a0 with vptr 0x21093ce948; with that run's bias
 * (0x2107583000) the vptr's ELF vaddr is 0x1e4b948 - a different
 * class.  Every "mgr=" / "vecn=" / "invec=" that probe printed was
 * therefore about the WRONG object, and none of it is evidence.
 *
 * WHERE THE MANAGER REALLY IS, read out of the guest image (raw
 * addresses are guest ELF vaddrs; add the image load bias for the
 * run-time address):
 *
 *   cc::HttpRequestManager::HttpRequestManager   0x1ce2644
 *       adrp x8, #0x1f7e000 ; ldr x8, [x8, #0x150]   ; the vtable base
 *       add  x10, x8, #0x10                        ; = the vptr, 0x1f64858
 *       str  q1, [x0]                              ; this+0 = the vptr
 *       stur q0, [x0, #0x38] ; stur q0, [x0, #0x48]  ; the vector: begin
 *                                                  ; at +0x38, end at +0x40
 *   the factory that owns it                      0x1cbe23c
 *       mov w0, #0x60 ; bl operator new            ; 0x60 bytes
 *       bl  0x1ce2644                             ; construct it
 *       str x24, [x19, #0x218]                    ; owner+0x218 = manager
 *   the factory that owns the owner               0x1cbe028
 *       adrp x22, #0x1f6d000 ; ldr x22, [x22, #0x5d0]  ; the .bss slot
 *       str x21, [x22]                            ; .bss 0x1fc6e98 = owner
 *
 * SO AT RUN TIME:
 *
 *   owner = *(bias + 0x1fc6e98)
 *   mgr   = *(owner + 0x218)
 *
 * and *(mgr) MUST be bias + 0x1f64858.  That vptr comparison is this
 * stage's POSITIVE CONTROL: when it fails, nothing below is about the
 * HTTP manager, and the log says so instead of pretending.
 *
 * THE VTABLE, from the image's own relocation table:
 *
 *   0x1f64848  offset-to-top 0                 (the vtable base)
 *   0x1f64850  typeinfo
 *   0x1f64858  vfunc0    = 0x1ce2680   <- what an instance holds
 *   ...
 *   0x1f648d0  vfunc15   = 0x1ce29b4   = cc::HttpRequestManager::Poll
 *   0x1f648d8  vfunc16   = 0x1ce2c1c
 *
 * TWO CORRECTIONS to the earlier notes, both from that table: an
 * instance's vptr is the vtable base PLUS 0x10 (0x1f64858), not the
 * vtable base; and Poll is vfunc15 at slot 0x1f648d0, NOT 0x1f648c0 -
 * that slot holds 0x1ce2e4c (vfunc14).
 *
 * THE POLL GATE, from cc::HttpRequestManager::Poll (0x1ce29b4):
 *
 *   0x1ce2a88  ldrb w8, [x21, #0xc8]   ; the response-complete flag
 *   0x1ce2a8c  cbz  w8, skip           ; c8 == 0  -> not delivered
 *   0x1ce2a90  ldrb w8, [x21, #0xc9]   ; the "already handled" flag
 *   0x1ce2a94  cbnz w8, skip           ; c9 != 0  -> already handled
 *   0x1ce2a98  ldr  x8, [x21, #0x100]  ; the handler delegate
 *   0x1ce2a9c  strb w24, [x21, #0xc9]  ; c9 = 1  (BEFORE the call)
 *
 * c9 is set BEFORE the handler is called, and it is set even when the
 * handler delegate is NULL.  So c9 == 1 means "Poll CLAIMED this
 * record", not "the handler ran" - and Poll never claims it twice.
 * After the handler runs, Poll REMOVES the record from the vector.
 * So the four states the vector membership distinguishes are:
 *
 *   not in the vector      -> never queued, or already handled and removed
 *   in, c8 == 0           -> the guest never marked the response complete
 *   in, c8 == 1, c9 == 0  -> complete, handler installed, Poll has not run
 *   in, c8 == 1, c9 == 1  -> Poll claimed it (and may or may not have run
 *                            the handler: +0x100 tells us)
 */

/* The vptr a cc::HttpRequestManager instance carries. */
static const uintptr_t kMgr2VptrElf     = 0x1f64858ull;
/* .bss: the owner object.  owner+0x218 is the manager. */
static const uintptr_t kMgr2OwnerSlot   = 0x1fc6e98ull;
static const uintptr_t kMgr2OwnerMgrOff = 0x218ull;
/* The owner's size, and the ServerTimeManager at owner+0x10. */
static const uintptr_t kMgr2OwnerSize   = 0x4d0ull;
static const uintptr_t kMgr2OwnerStmOff = 0x10ull;
/* The OLD global the first probe used.  Kept, and reported, so the two
 * readings can be told apart in the same log. */
static const uintptr_t kMgr2WrongSlot   = 0x1f97818ull;
/* .bss, from the image's program headers: the third PT_LOAD is
 * vaddr 0x1f80880 filesz 0x146a0 memsz 0x47219, so .bss is
 * 0x1f80880+0x146a0 .. 0x1f80880+0x47219 = 0x1f94f20 .. 0x1fc7a99. */
static const uintptr_t kMgr2BssBegin    = 0x1f94f20ull;
static const uintptr_t kMgr2BssEnd      = 0x1fc7a99ull;

static const unsigned kMgr2MaxVecRecords = 32u;  /* records logged per sample  */
static const unsigned kMgr2MaxLines      = 1200u;/* log lines for the whole run */
static const unsigned kMgr2ScanEvery     = 10u;  /* one .bss scan per N samples  */
static const unsigned kMgr2MaxTicks      = 420u; /* ~7 min at 1 Hz               */

/* The record the port is serving, and the identity the PORT has for the
 * request it is serving.  Written on the guest's own thread, read by the
 * probe thread; both sides use relaxed atomics so the words are not torn.
 *
 * TWO SLOTS, because "the last init()" and "the /time request" are not
 * the same request: the guest creates an HttpRequest for the
 * authenticate exchange and another for /time, and the census has to be
 * able to say WHICH record it is measuring.  The /time one is the slot
 * that matters for the menu blocker; the other is kept so the log can
 * show both. */
static uintptr_t g_mgr2Record = 0;
static uintptr_t g_mgr2Worker = 0;
static char     g_mgr2Url[192] = {0};
static uint32_t g_mgr2Body0 = 0xffffffffu;
static uintptr_t g_mgr2TimeRecord = 0;
static char     g_mgr2TimeUrl[192] = {0};
static uint32_t g_mgr2TimeBody0 = 0xffffffffu;

/*
 * THE MANAGER, THE WAY THE RECORD ITSELF NAMES IT - THE PRIMARY HANDLE.
 *
 * The record carries a DIRECT back-pointer to its own manager:
 *
 *   0x1ce13d8  str x23, [x19, #0xc0]
 *
 * written in exactly one place in the HTTP module, inside QueueRequest,
 * where x23 is the manager's `this`.  So:
 *
 *   mgr = *(record + 0xc0)
 *
 * and *(mgr) MUST be bias + 0x1f64858.  One hop instead of two, immune
 * to the owner object being replaced, and it names the same object Poll
 * walks.  The owner route stays as an INDEPENDENT cross-check and the
 * log reports both, so a disagreement between them is visible.
 */
static bool mgr2ResolveViaRecord(uintptr_t rec, uintptr_t bias,
                                  uintptr_t *mgrOut, uint64_t *vptrOut)
{
    if (rec == 0 || bias == 0) {
        return false;
    }
    uint64_t mgr = 0;
    if (!readU64(rec + 0xc0u, &mgr) || mgr == 0) {
        return false;
    }
    uint64_t vptr = 0;
    if (!readU64(static_cast<uintptr_t>(mgr), &vptr)) {
        return false;
    }
    if (mgrOut != nullptr) { *mgrOut = static_cast<uintptr_t>(mgr); }
    if (vptrOut != nullptr) { *vptrOut  = vptr; }
    return vptr == static_cast<uint64_t>(bias + kMgr2VptrElf);
}

/*
 * THE RECORD'S PATH, with the reader this layout wants.
 *
 * The port's existing readGuestString() assumes a libc++ std::string
 * (flags / length / data) and mis-reads this field: the record's path
 * is a (length, pointer) pair with the inline bytes beside it.
 *
 *   +0x08  16 bytes of INLINE data (used when the path is short)
 *   +0x18  u64  the length
 *   +0x20  ptr  the live data (used when the path is longer than 16)
 *
 * The path in this guest is 45 bytes, so the POINTER is the live data
 * and the length is the length.  readGuestString() read that pointer as
 * a std::string and therefore printed the URL with its FIRST CHARACTER
 * CLIPPED: the same bytes, read through the wrong shape.  This reader
 * prints the path as it is.
 */
static bool mgr2ReadPath(uintptr_t rec, char *out, size_t cap)
{
    if (out == nullptr || cap < 2u) {
        return false;
    }
    out[0] = '\0';
    uint64_t size = 0;
    uint64_t ptr = 0;
    if (!readU64(rec + 0x18u, &size) || !readU64(rec + 0x20u, &ptr)) {
        return false;
    }
    if (size == 0 || size > 1024u) {
        return false;
    }
    if (size <= 16u) {
        const size_t take = static_cast<size_t>(size);
        for (size_t i = 0; i < take; ++i) {
            uint64_t word = 0;
            if (!readU64(rec + 0x08u + (i & ~7ull), &word)) {
                return false;
            }
            out[i] = static_cast<char>((word >> ((i & 7u) * 8u)) & 0xffu);
        }
        out[take] = '\0';
        return true;
    }
    if (ptr == 0 || !guestReadable(static_cast<uintptr_t>(ptr))) {
        return false;
    }
    size_t take = static_cast<size_t>(size);
    if (take > cap - 1u) {
        take = cap - 1u;
    }
    if (!copyGuestBytes(out, static_cast<uintptr_t>(ptr), take)) {
        return false;
    }
    out[take] = '\0';
    return true;
}

/* Read the record's fields, as the guest's own code reads them. */
struct Mgr2Record {
    uint64_t ptr;
    uint64_t token;
    uint64_t c8;
    uint64_t c9;
    uint64_t d0;        /* +0xd0: the HEADER-EVENT witness (not Poll) */
    uint64_t d4;
    uint64_t handler100;
    uint64_t body130;
    uint64_t parser160;
    uint64_t status170;
    uint64_t mgrPtr;    /* +0xc0: the manager, by the record's own back-pointer */
    uint64_t pathLen;   /* +0x18 */
    uint64_t pathPtr;   /* +0x20 */
};

static void mgr2ReadRecord(uintptr_t rec, Mgr2Record *out)
{
    memset(out, 0, sizeof(*out));
    out->ptr = rec;
    if (rec == 0) {
        return;
    }
    uint64_t word = 0;
    if (readU64(rec + 0x00u, &word)) { out->token     = word; }
    if (readU64(rec + 0xc8u, &word)) { out->c8        = word & 0xffu; out->c9 = (word >> 8) & 0xffu; }
    if (readU64(rec + 0xd0u, &word)) { out->d0        = word & 0xffffffffu; }
    if (readU64(rec + 0xd4u, &word)) { out->d4        = word & 0xffffffffu; }
    if (readU64(rec + 0x100u, &word)) { out->handler100 = word; }
    if (readU64(rec + 0x130u, &word)) { out->body130   = word; }
    if (readU64(rec + 0x160u, &word)) { out->parser160 = word; }
    if (readU64(rec + 0x170u, &word)) { out->status170 = word & 0xffffffffu; }
    if (readU64(rec + 0xc0u, &word)) { out->mgrPtr    = word; }
    if (readU64(rec + 0x18u, &word)) { out->pathLen   = word; }
    if (readU64(rec + 0x20u, &word)) { out->pathPtr   = word; }
}

/*
 * THE MANAGER, the way the guest's own code reaches it.
 *
 * owner = *(bias + 0x1fc6e98);  mgr = *(owner + 0x218)
 *
 * Returns true only when the whole chain resolved AND the object's vptr
 * is the HttpRequestManager's.  The vptr check is the positive control:
 * without it a wrong chain would produce a confident, wrong answer.
 */
static bool mgr2Resolve(uintptr_t bias, uintptr_t *ownerOut, uintptr_t *mgrOut,
                        uint64_t *vptrOut)
{
    if (bias == 0) {
        return false;
    }
    uint64_t owner = 0;
    if (!readU64(bias + kMgr2OwnerSlot, &owner) || owner == 0) {
        return false;
    }
    uint64_t mgr = 0;
    if (!readU64(static_cast<uintptr_t>(owner) + kMgr2OwnerMgrOff, &mgr) || mgr == 0) {
        return false;
    }
    uint64_t vptr = 0;
    if (!readU64(static_cast<uintptr_t>(mgr), &vptr)) {
        return false;
    }
    if (ownerOut != nullptr) { *ownerOut = static_cast<uintptr_t>(owner); }
    if (mgrOut   != nullptr) { *mgrOut   = static_cast<uintptr_t>(mgr); }
    if (vptrOut  != nullptr) { *vptrOut  = vptr; }
    return vptr == static_cast<uint64_t>(bias + kMgr2VptrElf);
}

/*
 * THE .bss SCAN - "how many HttpRequestManager instances are there?".
 *
 * It looks for FOUR things in one pass, because a single pass that
 * finds nothing is worthless without a control that must be found:
 *
 *   T0  bias + 0x1f64858   the manager's vptr: a STATIC instance
 *                           (an object whose first word IS the vptr)
 *   T1  the owner address  THE POSITIVE CONTROL.  The owner is stored
 *                           in the .bss at 0x1fc6e98, so this scan MUST
 *                           find it there.  If T1 is not found, the scan
 *                           is broken and T0's zero is meaningless.
 *   T2  the manager address who points at the manager (the owner chain)
 *   T3  0x1f64858          the raw, unrelocated vaddr, in case a word
 *                           was written without the image's relocation
 *
 * It does NOT look for the manager's vptr in the heap, because the
 * manager is NOT in the heap-by-scan: it is heap-ALLOCATED by
 * operator new (0x1cbe23c) and reachable only through owner+0x218.
 * A .bss scan therefore answers "are there static instances?", and the
 * owner chain answers "where is the live one?".
 */
struct Mgr2Scan {
    unsigned t0;        /* vptr hits            */
    unsigned t1;        /* owner hits (control) */
    unsigned t2;        /* manager-address hits */
    unsigned t3;        /* raw vaddr hits       */
    unsigned k0;        /* addresses stored for t0 */
    unsigned k1;
    unsigned k2;
    unsigned k3;
    uintptr_t t0At[4];
    uintptr_t t1At[4];
    uintptr_t t2At[4];
    uintptr_t t3At[4];
    uint64_t words;     /* words actually read  */
};

static void mgr2ScanBss(uintptr_t bias, uint64_t wantVptr, uint64_t wantOwner,
                        uint64_t wantMgr, Mgr2Scan *out)
{
    memset(out, 0, sizeof(*out));
    const uintptr_t begin = bias + kMgr2BssBegin;
    const uintptr_t end   = bias + kMgr2BssEnd;
    /* Every read goes through readU64, which enforces BOTH the
     * svcQueryMemory readability check and the "is this inside the
     * guest's 64-bit window" check.  A bare memcpy here would be the
     * fault the project already paid for once. */
    for (uintptr_t a = begin; a + 8u <= end; a += 8u) {
        uint64_t word = 0;
        if (!readU64(a, &word)) {
            continue;
        }
        ++out->words;
        /* The COUNT and the stored addresses are separate: the count is
         * the number of hits (the answer to "how many instances?"), the
         * stored list is only the first few of them. */
        if (word == wantVptr) {
            ++out->t0;
            if (out->k0 < 4u) { out->t0At[out->k0++] = a; }
        }
        if (word == wantOwner) {
            ++out->t1;
            if (out->k1 < 4u) { out->t1At[out->k1++] = a; }
        }
        if (word == wantMgr) {
            ++out->t2;
            if (out->k2 < 4u) { out->t2At[out->k2++] = a; }
        }
        if (word == kMgr2VptrElf) {
            ++out->t3;
            if (out->k3 < 4u) { out->t3At[out->k3++] = a; }
        }
    }
}

/*
 * THE IDENTITY DUMP - is the record we are measuring really the /time
 * request?
 *
 * The record DOES carry its path, at +0x18 (the length) and +0x20 (the
 * live data pointer), with 16 bytes of inline data at +0x08 for the
 * short case.  This dump prints the first 0x40 bytes as hex, prints the
 * path through the reader that layout wants (mgr2ReadPath), and ALSO
 * tries to read every word in the dump that looks like a pointer as a
 * guest std::string - the port's old reader, which reads the same
 * bytes through the wrong shape and therefore clips the path's first
 * character.  Both are printed so the two readings can be compared.
 *
 * The second, independent instrument is the TOKEN: the guest's
 * cc::HttpRequestManager::QueueRequest (0x1ce1a84) stores the request
 * token in record+0x00, and the ServerTimeManager stores the SAME token
 * in its own +0x70 (0x1cef258).  The third is the port's own view: the
 * URL and body[0] it is serving, logged on the sample line.
 */
static void mgr2DumpRecord(uintptr_t rec, uintptr_t owner, uint64_t stmToken)
{
    char hex[0x41 * 2 + 1];
    size_t used = 0;
    for (unsigned i = 0; i < 0x40u; i += 8u) {
        uint64_t word = 0;
        readU64(rec + i, &word);
        for (unsigned b = 0; b < 8u; ++b) {
            used += (size_t)__builtin_snprintf(hex + used, sizeof(hex) - used,
                                               "%02x",
                                               (unsigned)((word >> (b * 8u)) & 0xffu));
        }
    }
    hex[used] = '\0';

    uint64_t pathLen = 0;
    readU64(rec + 0x18u, &pathLen);
    uint64_t pathPtr = 0;
    readU64(rec + 0x20u, &pathPtr);

    /* Every word in the dump that looks like a pointer, read as a string. */
    char strings[256];
    size_t sUsed = 0;
    strings[0] = '\0';
    for (unsigned i = 0; i < 0x40u; i += 8u) {
        uint64_t word = 0;
        if (!readU64(rec + i, &word) || word == 0) {
            continue;
        }
        const uintptr_t candidate = static_cast<uintptr_t>(word);
        if (!guestReadable(candidate)) {
            continue;
        }
        char text[96];
        if (!readGuestString(candidate, text, sizeof(text))) {
            continue;
        }
        /* Only printable, non-empty text is worth a log line. */
        bool printable = (text[0] != '\0');
        for (const char *p = text; *p != '\0' && printable; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20u || c > 0x7eu) {
                printable = false;
            }
        }
        if (!printable) {
            continue;
        }
        sUsed += (size_t)__builtin_snprintf(strings + sUsed, sizeof(strings) - sUsed,
                                            "%s+0x%02x=\"%s\"",
                                            sUsed == 0 ? "" : " ", i, text);
        if (sUsed >= sizeof(strings) - 1u) {
            break;
        }
    }

    uint64_t recToken = 0;
    readU64(rec + 0x00u, &recToken);

    /* The path, read with the reader this layout wants. */
    char path[160];
    const bool havePath = mgr2ReadPath(rec, path, sizeof(path));
    if (!havePath) {
        path[0] = '\0';
    }

    char line[1400];
    __builtin_snprintf(line, sizeof(line),
                       "rec-dump rec=0x%llx owner=0x%llx recToken=0x%llx "
                       "stmToken=0x%llx pathLen=%llu pathPtr=0x%llx "
                       "path=%s hex=%s strs=%s",
                       (unsigned long long)rec, (unsigned long long)owner,
                       (unsigned long long)recToken,
                       (unsigned long long)stmToken,
                       (unsigned long long)pathLen,
                       (unsigned long long)pathPtr,
                       havePath ? path : "(unreadable)", hex,
                       sUsed != 0 ? strings : "(none)");
    rr3::guestViewWideLog("cc-probe-mgr2", (unsigned long long)rec, 0, line);
}

/* The one sample.  Returns true when a line was written.
 *
 * `src` says which carrier took the sample ("frame" = the guest's own
 * 240-frame tick, "host" = the 1 Hz host thread) and `frames` is the
 * guest's frame counter, so the time series is anchored to the guest's
 * own progress and not only to wall-clock time. */
static bool mgr2Sample(const char *src, unsigned long long sample,
                        unsigned long long frames, uint64_t dtMs, bool fullScan)
{
    static unsigned s_lines = 0;
    if (s_lines >= kMgr2MaxLines) {
        return false;
    }
    const uintptr_t bias = rr3::resolveMainImageLoadBias();
    if (bias == 0) {
        return false;
    }

    const uintptr_t recordAny  = __atomic_load_n(&g_mgr2Record, __ATOMIC_RELAXED);
    const uintptr_t recordTime = __atomic_load_n(&g_mgr2TimeRecord, __ATOMIC_RELAXED);
    const uintptr_t worker = __atomic_load_n(&g_mgr2Worker, __ATOMIC_RELAXED);

    /* THE RECORD UNDER MEASUREMENT: the /time one when the port has
     * served it, otherwise the last request the guest created. */
    const uintptr_t record = (recordTime != 0) ? recordTime : recordAny;
    const char *url = (recordTime != 0) ? g_mgr2TimeUrl : g_mgr2Url;
    const uint32_t body0 = (recordTime != 0) ? g_mgr2TimeBody0 : g_mgr2Body0;

    /* THE MANAGER, TWICE, AND THE TWO MUST AGREE.
     *
     *  - via the record's own back-pointer (record+0xc0): the primary
     *    handle, one hop, and the object Poll walks.
     *  - via the owner (owner = *(bias+0x1fc6e98); mgr = *(owner+0x218)):
     *    the independent route, two hops.
     */
    uintptr_t mgrRec = 0;
    uint64_t mgrRecVptr = 0;
    const bool recOk = mgr2ResolveViaRecord(record, bias, &mgrRec, &mgrRecVptr);
    uintptr_t owner = 0;
    uintptr_t mgrOwner = 0;
    uint64_t mgrOwnerVptr = 0;
    const bool ownerOk = mgr2Resolve(bias, &owner, &mgrOwner, &mgrOwnerVptr);
    const bool agree = (recOk && ownerOk && mgrRec == mgrOwner);

    const uintptr_t mgr = recOk ? mgrRec : mgrOwner;
    const uint64_t vptr = recOk ? mgrRecVptr : mgrOwnerVptr;
    const bool resolved = recOk || ownerOk;

    /* The OLD global, for the side-by-side.  It is NOT the manager. */
    uint64_t oldPtr = 0;
    uint64_t oldVptr = 0;
    if (readU64(bias + kMgr2WrongSlot, &oldPtr) && oldPtr != 0) {
        readU64(static_cast<uintptr_t>(oldPtr), &oldVptr);
    }

    /* THE ServerTimeManager, at the address the hunt found (it is the
     * object whose +0x78 reads back as an epoch second, and whose +0x88
     * holds the server time the port sent).  Its +0x70 is the request
     * token, and the guest's QueueRequest (0x1ce1a84) stores the SAME
     * token in record+0x00 - so "record.token == ServerTimeManager.token"
     * is an identity instrument that does not use the port's own pointer
     * at all.
     *
     * NOT owner+0x10: the owner's +0x10 holds a POINTER to the guest's
     * time provider (the Cloudcell constructor writes a self-pointer
     * there and the engine replaces it later), not the ServerTimeManager.
     * The vptr is logged so the reader can confirm the class. */
    uint64_t stmToken = 0;
    uint64_t stmTime = 0;
    uint64_t stmVptr = 0;
    const uintptr_t stmAddr = g_managerAddr;
    if (stmAddr != 0) {
        readU64(stmAddr, &stmVptr);
        readU64(stmAddr + 0x70u, &stmToken);
        readU64(stmAddr + 0x88u, &stmTime);
    }
    double stmTimeD = 0.0;
    memcpy(&stmTimeD, &stmTime, sizeof(stmTimeD));

    /* The manager's active-request vector. */
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t count = 0;
    uint64_t mgr58 = 0;
    if (resolved) {
        readU64(mgr + 0x38u, &begin);
        readU64(mgr + 0x40u, &end);
        if (end > begin && (end - begin) % 8u == 0u) {
            count = (end - begin) / 8u;
        }
        /* The flag that gates the onHeaders / onBody / onComplete paths. */
        readU64(mgr + 0x58u, &mgr58);
    }
    /* The worker's own slots: the body-append callback at +0x60 and the
     * +0x58 flag.  The body-append path (guest raw 0x1ce2cc4) reads the
     * record from its FOURTH ARGUMENT, not from worker+0x08, so these
     * are the only view the port has of that path. */
    uint64_t wk58 = 0;
    uint64_t wk60 = 0;
    if (worker != 0) {
        readU64(worker + 0x58u, &wk58);
        readU64(worker + 0x60u, &wk60);
    }

    /* IS THE RECORD IN THE VECTOR?  THE HEADLINE NUMBER. */
    int inVector = -1;   /* -1 = unknown (the manager did not resolve) */
    uint64_t hitIndex = 0;
    if (resolved) {
        /* A resolved manager with an EMPTY vector is a DEFINITIVE "not
         * in it": there is no record to be in.  Only an unresolved
         * manager leaves the answer unknown. */
        inVector = 0;
        const uint64_t cap = (count < kMgr2MaxVecRecords) ? count : kMgr2MaxVecRecords;
        for (uint64_t i = 0; i < cap; ++i) {
            uint64_t element = 0;
            if (readU64(static_cast<uintptr_t>(begin) + i * 8u, &element) &&
                element == static_cast<uint64_t>(record)) {
                inVector = 1;
                hitIndex = i;
                break;
            }
        }
    }

    /* The record's own fields, and the vector's records. */
    Mgr2Record own;
    mgr2ReadRecord(record, &own);

    char vecText[640];
    size_t vUsed = 0;
    vecText[0] = '\0';
    /* The INDEPENDENT membership check: the /time request's record is
     * the one in the vector whose token (record+0x00) equals the
     * ServerTimeManager's token (its +0x70).  The guest writes both,
     * from different code, so this does not depend on the port's own
     * pointer at all. */
    int timeTokIndex = -1;
    for (uint64_t i = 0; i < count && i < kMgr2MaxVecRecords; ++i) {
        uint64_t element = 0;
        if (!readU64(static_cast<uintptr_t>(begin) + i * 8u, &element)) {
            continue;
        }
        Mgr2Record r;
        mgr2ReadRecord(static_cast<uintptr_t>(element), &r);
        if (timeTokIndex < 0 && stmToken != 0 && r.token == stmToken) {
            timeTokIndex = static_cast<int>(i);
        }
        vUsed += (size_t)__builtin_snprintf(
            vecText + vUsed, sizeof(vecText) - vUsed,
            "%s[%llu]rec=0x%llx tok=0x%llx c8=%llu c9=%llu d0=%llu d4=%llu "
            "h100=0x%llx b130=0x%llx p160=0x%llx st170=0x%llx",
            vUsed == 0 ? "" : " ", (unsigned long long)i,
            (unsigned long long)r.ptr, (unsigned long long)r.token,
            (unsigned long long)r.c8, (unsigned long long)r.c9,
            (unsigned long long)r.d0,
            (unsigned long long)r.d4, (unsigned long long)r.handler100,
            (unsigned long long)r.body130, (unsigned long long)r.parser160,
            (unsigned long long)r.status170);
        if (vUsed >= sizeof(vecText) - 1u) {
            break;
        }
    }

    /* The .bss scan, on some samples only.  A target that could not be
     * resolved is replaced by a value that cannot occur, because the
     * .bss is mostly ZERO and a zero target would "match" every zero
     * word in it. */
    Mgr2Scan scan;
    memset(&scan, 0, sizeof(scan));
    if (fullScan) {
        const uint64_t noMatch = ~0ull;
        mgr2ScanBss(bias, static_cast<uint64_t>(bias + kMgr2VptrElf),
                    (owner != 0) ? static_cast<uint64_t>(owner) : noMatch,
                    (mgr != 0) ? static_cast<uint64_t>(mgr) : noMatch, &scan);
    }

    /* The record's own PATH, read with the reader this layout wants. */
    char path[160];
    const bool havePath = mgr2ReadPath(record, path, sizeof(path));
    if (!havePath) {
        path[0] = '\0';
    }

    char line[1600];
    __builtin_snprintf(
        line, sizeof(line),
        /* THE FIELD IS NOT CALLED "frames".  The Eden harness's instrument
         * census takes the LAST match of /frames=(\d+)/ in the guest log as
         * the guest's frame total (tools/Test-OnEden.ps1:269), and a probe
         * that emits "frames=" after the frame loop has stopped REPLACES
         * that total with its own zero - which reads exactly like "the
         * guest wedged on frame one", a misreading this project has made
         * before.  "gf=" cannot match that regex. */
        "src=%s sample=%llu gf=%llu dt_ms=%llu worker=0x%llx "
        "owner=0x%llx mgr=0x%llx mgrvptr=0x%llx mgrvptrElf=0x%llx "
        "recOk=%d ownerOk=%d agree=%d "
        "vec=0x%llx/0x%llx/%llu rec=0x%llx invec=%d hit=%llu "
        "timeTokIdx=%d "
        "tok=0x%llx c8=%llu c9=%llu d0=%llu d4=%llu h100=0x%llx b130=0x%llx "
        "p160=0x%llx st170=0x%llx mgr58=0x%llx wk58=0x%llx wk60=0x%llx "
        "recMgr=0x%llx pathLen=%llu pathPtr=0x%llx path=%s "
        "stm=0x%llx stmVptrElf=0x%llx stmTok=0x%llx stmTime=%.6f "
        "recAny=0x%llx recTime=0x%llx "
        "old=0x%llx oldVptr=0x%llx oldVptrElf=0x%llx "
        "bssT0=%u bssT1=%u bssT2=%u bssT3=%u bssWords=%llu url=%s body0=0x%x",
        src != nullptr ? src : "-", sample, frames, (unsigned long long)dtMs,
        (unsigned long long)worker,
        (unsigned long long)owner, (unsigned long long)mgr,
        (unsigned long long)vptr,
        (unsigned long long)(vptr >= bias ? vptr - bias : 0ull),
        recOk ? 1 : 0, ownerOk ? 1 : 0, agree ? 1 : 0,
        (unsigned long long)begin, (unsigned long long)end,
        (unsigned long long)count, (unsigned long long)record, inVector,
        (unsigned long long)hitIndex, timeTokIndex,
        (unsigned long long)own.token,
        (unsigned long long)own.c8, (unsigned long long)own.c9,
        (unsigned long long)own.d0,
        (unsigned long long)own.d4, (unsigned long long)own.handler100,
        (unsigned long long)own.body130, (unsigned long long)own.parser160,
        (unsigned long long)own.status170, (unsigned long long)mgr58,
        (unsigned long long)wk58, (unsigned long long)wk60,
        (unsigned long long)own.mgrPtr, (unsigned long long)own.pathLen,
        (unsigned long long)own.pathPtr, path,
        (unsigned long long)stmAddr,
        (unsigned long long)(stmVptr >= bias ? stmVptr - bias : 0ull),
        (unsigned long long)stmToken,
        stmTimeD, (unsigned long long)recordAny, (unsigned long long)recordTime,
        (unsigned long long)oldPtr, (unsigned long long)oldVptr,
        (unsigned long long)(oldVptr >= bias ? oldVptr - bias : 0ull),
        scan.t0, scan.t1, scan.t2, scan.t3,
        (unsigned long long)scan.words, url, body0);
    rr3::guestViewWideLog("cc-probe-mgr2", (unsigned long long)record,
                          (unsigned long long)mgr, line);
    ++s_lines;

    /* The vector's records, one line each. */
    if (vUsed != 0) {
        char vline[1024];
        __builtin_snprintf(vline, sizeof(vline), "vec mgr=0x%llx n=%llu %s",
                           (unsigned long long)mgr, (unsigned long long)count,
                           vecText);
        rr3::guestViewWideLog("cc-probe-mgr2", (unsigned long long)mgr,
                              (unsigned long long)count, vline);
        ++s_lines;
    }

    /* The owner chain: who points at the manager. */
    if (fullScan) {
        char chain[512];
        size_t cUsed = 0;
        chain[0] = '\0';
        cUsed += (size_t)__builtin_snprintf(
            chain + cUsed, sizeof(chain) - cUsed,
            "owner-chain mgr=0x%llx owner=0x%llx atOwner+0x%llx",
            (unsigned long long)mgr, (unsigned long long)owner,
            (unsigned long long)kMgr2OwnerMgrOff);
        /* k1/k2/k3 are the numbers of STORED addresses; t1/t2/t3 are
         * the total hit counts, which can be much larger. */
        for (unsigned i = 0; i < scan.k2; ++i) {
            cUsed += (size_t)__builtin_snprintf(
                chain + cUsed, sizeof(chain) - cUsed,
                " mgrAtBss+0x%llx", (unsigned long long)(scan.t2At[i] - bias));
            if (cUsed >= sizeof(chain) - 1u) { break; }
        }
        for (unsigned i = 0; i < scan.k1; ++i) {
            cUsed += (size_t)__builtin_snprintf(
                chain + cUsed, sizeof(chain) - cUsed,
                " ownerAtBss+0x%llx", (unsigned long long)(scan.t1At[i] - bias));
            if (cUsed >= sizeof(chain) - 1u) { break; }
        }
        for (unsigned i = 0; i < scan.k0; ++i) {
            cUsed += (size_t)__builtin_snprintf(
                chain + cUsed, sizeof(chain) - cUsed,
                " vptrAtBss+0x%llx", (unsigned long long)(scan.t0At[i] - bias));
            if (cUsed >= sizeof(chain) - 1u) { break; }
        }
        rr3::guestViewWideLog("cc-probe-mgr2", (unsigned long long)mgr,
                              (unsigned long long)scan.t1, chain);
        ++s_lines;
    }

    /* The identity dump, once per distinct record. */
    static uintptr_t s_dumped = 0;
    if (record != 0 && record != s_dumped) {
        s_dumped = record;
        mgr2DumpRecord(record, owner, stmToken);
        ++s_lines;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * THE CARRIER: a raw libnx thread that samples once a second.
 *
 * It is a threadCreate thread and NOT a guest pthread, because guest
 * pthreads never leave svcSleepThread on Eden (guest_view_probe.cpp's
 * note).  It is never joined, on the census reporter's reasoning
 * (jni_bridge.cpp:3429): the process exit reclaims it, and there is no
 * teardown path this file owns to stop it on.
 *
 * The SLEEP is load-bearing: an instrument in this project that spun
 * without one produced no output at all, having starved everything
 * else.  The sleep is 250 ms and one line is emitted every fourth tick,
 * so the nominal cadence is 1 Hz; the REAL elapsed time is on every
 * line as dt_ms, because a starved carrier must show a LONGER gap
 * rather than a burst of catch-up samples.
 * ------------------------------------------------------------------ */
static volatile int g_mgr2Started = 0;
static volatile int g_mgr2Running = 0;
static Thread        g_mgr2Thread;

static void mgr2Worker(void *opaque)
{
    (void)opaque;
    const int64_t kSleepNs = 250000000;   /* 250 ms */
    const uint64_t tickFreq = armGetSystemTickFreq();
    uint64_t lastTick = armGetSystemTick();
    unsigned long long tick = 0;
    unsigned long long sample = 0;
    while (__atomic_load_n(&g_mgr2Running, __ATOMIC_RELAXED)) {
        svcSleepThread(kSleepNs);
        if (!__atomic_load_n(&g_mgr2Running, __ATOMIC_RELAXED)) {
            break;
        }
        ++tick;
        if ((tick % 4ull) != 0ull) {
            continue;
        }
        const uint64_t now = armGetSystemTick();
        const uint64_t dtMs = (tickFreq != 0)
            ? ((now - lastTick) * 1000ull / tickFreq) : 0ull;
        lastTick = now;
        ++sample;
        mgr2Sample("host", sample, 0ull, dtMs,
                   sample == 1ull || (sample % kMgr2ScanEvery) == 0ull);
        if (sample >= kMgr2MaxTicks) {
            break;
        }
    }
    __atomic_store_n(&g_mgr2Running, 0, __ATOMIC_RELAXED);
}

/* Start the carrier, exactly once.  Safe to call from the guest's own
 * thread: it is only a comparison and, once, a threadCreate. */
void startMgr2Probe(void)
{
    if (__atomic_exchange_n(&g_mgr2Started, 1, __ATOMIC_RELAXED) != 0) {
        return;
    }
    __atomic_store_n(&g_mgr2Running, 1, __ATOMIC_RELAXED);
    /* 0x3B, the preemptive priority libnx documents for cores 0..2 and
     * the one the other diagnostic threads use (grep
     * kDiagnosticThreadPriority).  0x2C is the main thread's priority
     * and is NOT time-sliced. */
    if (R_FAILED(threadCreate(&g_mgr2Thread, mgr2Worker, nullptr, nullptr,
                              0x4000, 0x3B, -2 /* default core */))) {
        __atomic_store_n(&g_mgr2Running, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_mgr2Started, 0, __ATOMIC_RELAXED);
        rr3::guestViewWideLog("cc-probe-mgr2", 0, 0, "threadCreate failed");
        return;
    }
    threadStart(&g_mgr2Thread);
    rr3::guestViewWideLog("cc-probe-mgr2", 0, 0, "carrier started (250 ms tick, 1 line/s)");
}

/* Note the request the port is serving, so the record can be tied to
 * it.  Called on the guest's own thread. */
void noteMgr2Request(uintptr_t worker, const char *url, size_t bodyLen,
                     const uint8_t *body)
{
    if (url != nullptr) {
        size_t take = strlen(url);
        if (take >= sizeof(g_mgr2Url)) {
            take = sizeof(g_mgr2Url) - 1u;
        }
        memcpy(g_mgr2Url, url, take);
        g_mgr2Url[take] = '\0';
    }
    g_mgr2Body0 = (bodyLen > 0 && body != nullptr)
        ? static_cast<uint32_t>(body[0]) : 0xffffffffu;
    __atomic_store_n(&g_mgr2Worker, worker, __ATOMIC_RELAXED);
    uint64_t rec = 0;
    if (worker != 0 && readU64(worker + 0x08u, &rec)) {
        __atomic_store_n(&g_mgr2Record, static_cast<uintptr_t>(rec), __ATOMIC_RELAXED);
    }
}

/* The same, for the /time request specifically.  This is the record the
 * menu blocker is about. */
void noteMgr2TimeRequest(uintptr_t worker, const char *url, size_t bodyLen,
                         const uint8_t *body)
{
    if (url != nullptr) {
        size_t take = strlen(url);
        if (take >= sizeof(g_mgr2TimeUrl)) {
            take = sizeof(g_mgr2TimeUrl) - 1u;
        }
        memcpy(g_mgr2TimeUrl, url, take);
        g_mgr2TimeUrl[take] = '\0';
    }
    g_mgr2TimeBody0 = (bodyLen > 0 && body != nullptr)
        ? static_cast<uint32_t>(body[0]) : 0xffffffffu;
    uint64_t rec = 0;
    if (worker != 0 && readU64(worker + 0x08u, &rec)) {
        __atomic_store_n(&g_mgr2TimeRecord, static_cast<uintptr_t>(rec),
                         __ATOMIC_RELAXED);
    }
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
 * `frames` is the guest's own frame counter, so every line is anchored
 * to the guest's progress and not only to wall-clock time.  The 4-sample
 * cap that used to sit on this call site is GONE: it made the probe dead
 * from about frame 960 onward, so 94% of a 16200-frame run went
 * unobserved and "the record never changed" was indistinguishable from
 * "the probe stopped looking".
 *
 * Returns true when a sample was taken.
 */
extern "C" bool rr3ServerTimeManagerProbe(const char *phase,
                                           unsigned long long frames)
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
    ccprobe::describeActiveRequests(ccprobe::g_workerAddr, frames);
    return took;
}

/*
 * THE FRAME-ANCHORED MANAGER SAMPLE.
 *
 * Called from the guest's own 240-frame tick, so the time series is
 * anchored to the guest's frame counter: a line exists for every tick of
 * the run, and the frame number on it says exactly which part of the run
 * it describes.  It never runs the .bss scan - that is the host thread's
 * job - so the cost on the guest's hot path is a handful of reads on
 * pages the probe already touches.
 */
extern "C" void rr3HttpManagerFrameTick(unsigned long long frames)
{
    ccprobe::mgr2Sample("frame", frames, frames, 0ull, false);
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
 *
 * The array is built with the port's own env, and the port's env IS the
 * guest's env: the managed methods are dispatched with the JNIEnv the guest
 * passed (jni_bridge.cpp's jniCallVoidMethodV hands
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
    /* This IS the /time request, so its worker (and therefore its
     * ActiveRequest_Struct) is the record the manager census must
     * measure.  Named separately from the generic "last init()" slot,
     * because the guest creates an HttpRequest for other exchanges too. */
    ccprobe::noteMgr2TimeRequest((uintptr_t)req->callback, req->url,
                                  req->bodyLen, (const uint8_t *)req->body);
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
         * authenticate and sync requests go to CC_Sync.php.
         *
         * MEASURED: the /time request carries NO body ("cc-http-init:
         * body=0"), so there is nothing in it to classify. */
        if (strstr(req->url, "/time") != nullptr) {
            if (deliverServerTime(env, req)) {
                return;
            }
        } else if (authLogBudget()) {
            trace("cc-http-req: not the /time request url=%s", req->url);
        }

        /* THE ASSET-SERVER REQUEST (assetManDownloadPackHash.php).
         *
         * The URL the port sees for this one is EMPTY, so the body is the
         * only discriminator, and the body was read in init(): if a
         * response was built there, this is that request.
         *
         * The answer is the raw block sequence, not a Cloudcell box -- see
         * the block comment on the asset endpoint for the evidence. */
        if (deliverAssetResponse(env, req)) {
            return;
        }

        /* The request the guest posted.
         *
         * The body is a BinaryBlob BOX, so it is unboxed before its shape
         * means anything. The port used to parse the wire bytes as if they
         * were the payload, so the first word it read was the box's
         * checksum instead of the tag, EVERY parse failed, and the port
         * fell through to matching the URL - which is how the port ended up
         * answering the ordinary SYNC request with the authenticate
         * response. The guest then unpacked that as a sync response, read
         * the member id out of the authenticate state word, and ran off the
         * end of the blob: 2980 "UnpackData" assertions at BinaryBlob.cpp
         * line 375 (the guest's pointer-returning reader, 0x1cbdecc). */
        cccloud::AuthRequest info;
        const cccloud::RequestKind kind =
            cccloud::parseRequest(req->body, req->bodyLen, info);

        {
            const char *kindName =
                (kind == cccloud::RequestKind::Authenticate) ? "authenticate"
                : (kind == cccloud::RequestKind::Sync)       ? "sync"
                                                            : "unknown";
            /* The payload's first words, so a run says what the request
             * actually said instead of leaving it to be re-derived. */
            char words[192];
            words[0] = '\0';
            std::vector<uint8_t> payload;
            if (cccloud::unboxBlob(req->body, req->bodyLen, payload) &&
                payload.size() >= 32u) {
                for (size_t i = 0; i < 8u; ++i) {
                    const uint32_t w = (uint32_t)payload[i * 4u] |
                                       ((uint32_t)payload[i * 4u + 1u] << 8) |
                                       ((uint32_t)payload[i * 4u + 2u] << 16) |
                                       ((uint32_t)payload[i * 4u + 3u] << 24);
                    __builtin_snprintf(words + (i * 9), 10u, "%08x ", w);
                }
            } else if (!payload.empty() && payload.size() >= 16u) {
                for (size_t i = 0; i < 4u; ++i) {
                    const uint32_t w = (uint32_t)payload[i * 4u] |
                                       ((uint32_t)payload[i * 4u + 1u] << 8) |
                                       ((uint32_t)payload[i * 4u + 2u] << 16) |
                                       ((uint32_t)payload[i * 4u + 3u] << 24);
                    __builtin_snprintf(words + (i * 9), 10u, "%08x ", w);
                }
            }
            char details[512];
            __builtin_snprintf(details, sizeof(details),
                               "kind=%s bodyLen=%u boxed=%d boxMode=%u "
                               "payloadLen=%u env1=%u env2=%u client=%u "
                               "tailWords=%u member=%u head=[%s]",
                               kindName, (unsigned)req->bodyLen,
                               info.boxed ? 1 : 0, (unsigned)info.boxMode,
                               (unsigned)info.payloadSize, (unsigned)info.env1,
                               (unsigned)info.env2, (unsigned)info.clientId,
                               (unsigned)info.tailWords,
                               info.hasMemberId ? info.memberId : 0u, words);
            rr3::guestViewWideLog("cc-req-class", (unsigned)kind,
                                  (unsigned long long)info.payloadSize, details);
            if (authLogBudget()) {
                trace("cc-req-class: %s", details);
            }
        }

        if (kind == cccloud::RequestKind::Authenticate) {
            /* THE BODY MATCH - the primary discriminator. */
            if (authLogBudget()) {
                trace("cc-auth-req: matched env=%u,%u x=%u headers=%zu body=%zu "
                      "consumed=%zu member=%u",
                      info.env1, info.env2, info.clientId,
                      (unsigned)info.headerCount, (unsigned)info.bodySize,
                      (unsigned)info.consumed,
                      info.hasMemberId ? info.memberId : kDefaultMemberId);
            }
            rr3::guestViewWideLog("cc-auth-req", 1, (unsigned long long)req->bodyLen,
                                   "matched (body shape)");
            const bool ok = deliverAuthSuccess(env, req, info);
            rr3::guestViewWideLog("cc-auth-deliver", ok ? 1 : 0,
                                  (unsigned long long)req->bodyLen,
                                  ok ? "delivered (by=body)"
                                     : "FAILED (by=body)");
            if (ok) {
                return;
            }
        } else if (kind == cccloud::RequestKind::Sync) {
            /* THE SYNC REQUEST. Not the authenticate request, so it must
             * not be answered with the authenticate response: the guest's
             * two readers read different fields and the guest unpacks the
             * answer with the reader that matches the request it sent. */
            const uint32_t memberId = info.hasMemberId ? info.memberId
                                                       : kDefaultMemberId;
            const bool ok = deliverSyncResponse(env, req, memberId,
                                                kLocalDeviceId);
            rr3::guestViewWideLog("cc-sync-deliver", ok ? 1 : 0,
                                  (unsigned long long)req->bodyLen,
                                  ok ? "delivered (by=body, kind=sync)"
                                     : "FAILED (by=body, kind=sync)");
            if (ok) {
                return;
            }
        } else if (req->bodyLen == 0u) {
            /* THE URL FALLBACK - the LAST resort.
             *
             * The body could not be read at all (there was none), so the
             * body match cannot fire and the URL is all that is left. The
             * guest's own path string for this endpoint is CC_Sync.php
             * (guest raw 0x3e3551) and the /time request goes to .../time.
             *
             * ONE-SHOT: only the first such request is answered, so a later
             * request is not answered with a second one.
             *
             * The log says WHICH discriminator was used. "matched-by-url" is
             * NOT "matched": the body was never read. */
            const bool urlSaysAuth =
                (strstr(req->url, kAuthUrlMarker) != nullptr);
            if (urlSaysAuth && !g_authDelivered) {
                if (authLogBudget()) {
                    trace("cc-auth-req: matched-by-url body=%zu url=%s "
                          "(the request carried no body to read)",
                          (unsigned)req->bodyLen, req->url);
                }
                rr3::guestViewWideLog("cc-auth-req", 2,
                                       (unsigned long long)req->bodyLen,
                                       "matched-by-url (no body to read)");
                cccloud::AuthRequest byUrl;   /* default: no member id */
                const bool ok = deliverAuthSuccess(env, req, byUrl);
                rr3::guestViewWideLog("cc-auth-deliver", ok ? 1 : 0,
                                      (unsigned long long)req->bodyLen,
                                      ok ? "delivered (by=url)"
                                         : "FAILED (by=url)");
                if (ok) {
                    return;
                }
            } else if (authLogBudget()) {
                trace("cc-auth-req: no body and no url match body=%zu url=%s",
                      (unsigned)req->bodyLen, req->url);
            }
        } else if (authLogBudget()) {
            /* The body WAS readable and it is neither request shape. The
             * port does not know what to answer, and it says so instead of
             * guessing. */
            trace("cc-req-class: readable body that is neither shape "
                  "bodyLen=%zu url=%s", (unsigned)req->bodyLen, req->url);
            rr3::guestViewWideLog("cc-req-class", 3,
                                  (unsigned long long)req->bodyLen,
                                  "readable body, unknown shape: not answered");
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
    /*
     * The guest's OWN signature for init, read out of its disassembly at
     * guest raw 0x1ce35ac:
     *
     *     (Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BIJZDZZI)V
     *
     * which is the Java one (HttpRequest.java: init(String userAgent,
     * String method, String url, byte[] body, int readCapacity, long
     * callback, boolean, double serverTime, boolean, boolean, int
     * timeoutSeconds)). The port used to register the SEVEN-argument
     * spelling, which is not the signature the guest binds: the guest
     * resolves "init" with the eleven-argument one, so the port's spelling
     * only ever mattered as the fallback in the guest-argument decoder.
     * It is corrected here so the fallback is right too.
     *
     * The managed method still takes the first seven arguments, which are
     * the ones the port needs: the body (index 3) and the callback
     * (index 5). The four the port does not use - the server time, two
     * flags and the timeout - are read past.
     */
    ManagedMethod::Register<&http_init>(
        CCHttpRequest::clazz, "init",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BIJZDZZI)V"),
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
