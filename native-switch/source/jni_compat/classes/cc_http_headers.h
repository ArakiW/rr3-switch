#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The HTTP response headers the port hands to the guest's native
 * headerCallback (Java_com_firemonkeys_cloudcellapi_HttpRequest_headerCallback,
 * guest raw 0x1ce4348).
 *
 * WHY HEADERS AND NOT A BODY
 * ==========================
 * The guest's /time answer is read by the ServerTimeManager, and the parser is
 * guest raw 0x1cef8a4 (ServerTimeManager.cpp). Measured:
 *
 *   0x1cef8c8  ldr x8, [x0, #0x70]      ; the request token
 *   0x1cef8cc  cmp x8, x2               ; must match the token
 *   0x1cef8d0  b.ne <return>            ; otherwise the answer is dropped
 *   0x1cef8f8  adrp x8, #0x460000       ; "X-Request-Start" (guest .rodata
 *   0x1cef904  add  x8, x8, #0xbf2      ; 0x460bf2, the ONLY reference to it)
 *   ...                                ; build the key as a tagged string
 *   0x1cef92c  bl  0xa0b240             ; the guest's hash-map find
 *   0x1cef978  str x9, [sp, #0x20]
 *   0x1cef97c  bl  0xbcb038             ; typed extraction of the value
 *   0x1cef9d0  bl  0x1e18b80             ; strtod()
 *   0x1cef9d4  str d0, [x19, #0x88]     ; this->serverTime (a double)
 *
 * The value is a STRING that strtod parses, so the wire form of the server
 * time is ASCII decimal seconds ("1793000000.125000"), not a raw double and
 * not a BinaryBlob.
 *
 * The map it is looked up in is the RESPONSE HEADER map, and that is built by
 * the guest's own native headerCallback: guest raw 0x1ce4348 binds the JNI
 * methods "keySet", "iterator", "hasNext", "next", "get", "size" and walks
 * the java.util.Map the Java layer handed it (HttpThread.java:
 * headerCallback(ptr, contentLength, headers) with headers a
 * Map<String, List<String>>), storing each (name, value) pair in the guest's
 * own map at worker+0x180.
 *
 * So the port has to answer the /time request with a HEADER named
 * "X-Request-Start", and the value is delivered to the guest as a Java
 * Map<String, List<String>>.
 *
 * The three collections the guest walks are tiny and live here rather than in
 * a class registry: the guest's native headerCallback only ever calls them
 * through the JNIEnv, and the port answers those calls by NAME plus the
 * receiver (see jni_bridge.cpp's jniCallObjectMethodV /
 * jniCallBooleanMethodV / jniCallIntMethodV hooks).
 */
namespace ccheaders {

/* One response header. */
struct Entry {
    const char* name;
    const char* value;
};

/*
 * The server time the port answers the /time request with.
 *
 * A plausible epoch-seconds double in ASCII form: 1793000000.125000 is
 * 2026-10-24T00:53:20Z, which is inside the guest's plausible window and
 * far from 0 (the guest treats a non-positive server time as "no answer").
 */
extern const Entry kServerTime;

/*
 * Build the java/util/Map handed to the guest's native headerCallback.
 * Returns 0 when the object could not be allocated, in which case the caller
 * must fall back to the honest failure.
 */
uintptr_t createMap(const Entry* entries, size_t count);

/*
 * The port's object pool, its class table and its string table live inside
 * jni_bridge.cpp's anonymous namespace, so they are reached through these
 * three thin wrappers (defined in jni_bridge.cpp, outside that namespace).
 */
uintptr_t allocObject(uintptr_t classHandle);
uintptr_t classHandle(const char* name);
uintptr_t makeString(const char* value);

/*
 * Receiver-aware JNI hooks. Each returns true when it answered the call, in
 * which case the caller must use the value it wrote and ignore every other
 * path (the port's name-keyed fallbacks answer "keySet", "iterator", "get"
 * and friends with a shared dummy, which is what this replaces for the
 * response-header map only).
 */
bool handleBoolean(uintptr_t object, const char* name, bool* out);
bool handleInt(uintptr_t object, const char* name, int32_t* out);
bool handleObject(uintptr_t object, const char* name, uintptr_t* out);

} // namespace ccheaders
