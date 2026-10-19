// jni_bridge.cpp 閳?闂冭埖顔?9閿涙碍娓剁亸?JNIEnv 閹绘劒绶甸懓?//
// RR3 is a JNI MainActivity app: the game entry points are the exported
// Java_com_firemint_realracing_MainActivity_* symbols, and they talk to the
// Android runtime through the JNIEnv function table. No Android VM is
// embedded, so this file supplies a minimal host-side JNINativeInterface:
//
//  - The six slots the onCreate constructor is confirmed to use are
//    implemented for real (verified against the decompilation):
//      0x20/8 =  4  GetVersion
//      0xa8/8 = 21  NewGlobalRef
//      0xb8/8 = 23  DeleteLocalRef
//      0xf8/8 = 31  GetObjectClass
//      0x108/8= 33  GetMethodID
//      0x6d8/8=219  GetJavaVM
//  - Byte-array slots so the guest can stage PNG payloads for
//    loadTextureFromMemory (see png_decode.hpp for the decoder):
//      171 GetArrayLength
//      176 NewByteArray
//      184 GetByteArrayElements
//      192 ReleaseByteArrayElements
//      200 GetByteArrayRegion
//      208 SetByteArrayRegion
//  - A small set of common, safely-answerable slots get trivial
//    implementations (exceptions, strings, refs, RegisterNatives).
//  - Every other slot points at a per-index stub that records the slot number
//    and returns 0, so the first unsupported call is identifiable from the
//    telemetry (jniLastSlot) instead of crashing silently.
//
// Everything here must be host-TLS-free: these functions run while the guest
// TLS scope is installed (TPIDR_EL0 points at the guest TLS block), so no
// newlib calls (printf/malloc/...) may be made. svcOutputDebugString is a raw
// svc and is safe.
//
// Diagnostic: every JNI call the guest makes is pushed into the per-thread
// census ring (see pthread_bridge.cpp) so a JNI stub that returns a dummy
// object shows up IN ORDER next to the host imports around it. Without this
// the census could only see libc/pthread imports, and the JNI surface -- the
// most likely source of a rejected Surface/Context/window handle -- was
// invisible in the frozen thread's history.

#include "rr3/jni_bridge.hpp"

#include <switch.h>
#include <GLES2/gl2.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdarg>

#include "rr3/elf_preflight.hpp"
#include "rr3/gfx_backend.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/tls_probe.hpp"
#include "rr3/png_decode.hpp"

// The one-frame-deferred answer queue the Cloudcell facades owe the engine,
// and the by-name guest-export resolver behind it. Both are shared with
// jni_compat/classes/rr3_Platform.cpp, which answers Platform.showMessage
// through the in-process class table; the guest reaches the same dialog
// through the JNIEnv vtable implemented here, so both paths have to enqueue
// into the same queue.
#include "eap_port/trace_adapter.h"
#include "jni_compat/classes/cloudcell_defer.h"
#include "jni_compat/classes/cloudcell_natives.h"

// For ClassRegistry / ManagedMethod, so the call slots can be pointed at the
// stubs in jni_compat/classes/. Nothing from that directory is currently
// reachable from here: jniGetMethodID hands back a JniMethodRecord and every
// call slot stops at "log and return", so a class the guest resolves and a
// method it then calls are both real and both completely unanswered.
//
// Added 2026-10-19, behind ccdisp. See dispatchManagedVoid below.
#include "jni_compat/jni_internals.h"

namespace rr3 {

// Defined in pthread_bridge.cpp. Declared here so the JNI call paths below can
// push themselves into the per-thread census ring.
void censusNoteImportFrom(const void* returnAddress, const char* symbol);

namespace {

constexpr size_t kJniSlotCount = rr3::kJniSlotCount;

// Telemetry for unimplemented slots: the same stubs serve every JNI phase, so
// these are total counters snapped into a JniSlotHistogram at phase boundaries.
// Diffing two snapshots attributes each phase exactly.
uint32_t g_jniUnhandledCalls = 0;
uint32_t g_jniLastSlot = 0xffffffffu;
uint32_t g_jniMethodBindings = 0;
uint32_t g_jniSlotHits[kJniSlotCount]{};

JniSlotHistogram snapshotJniHistogram() {
    JniSlotHistogram histogram{};
    for (size_t slot = 0; slot < kJniSlotCount; ++slot) {
        histogram.slotHits[slot] = g_jniSlotHits[slot];
    }
    histogram.unhandledCalls = g_jniUnhandledCalls;
    histogram.lastSlot = g_jniLastSlot;
    histogram.methodBindings = g_jniMethodBindings;
    return histogram;
}

// Returns the per-phase histogram: caller-provided phase slice minus the
// before-phase baseline (which is the last phase's snapshot).
JniSlotHistogram diffJniHistogram(const JniSlotHistogram& after,
                                  const JniSlotHistogram& before) {
    JniSlotHistogram delta{};
    for (size_t slot = 0; slot < kJniSlotCount; ++slot) {
        delta.slotHits[slot] = after.slotHits[slot] - before.slotHits[slot];
    }
    delta.unhandledCalls = after.unhandledCalls - before.unhandledCalls;
    delta.lastSlot = after.unhandledCalls > before.unhandledCalls
        ? after.lastSlot : 0xffffffffu;
    delta.methodBindings = after.methodBindings - before.methodBindings;
    return delta;
}

// Opaque sentinel objects. JNI handles are opaque to the guest: it only
// stores them and hands them back, so the address of any stable storage is a
// valid fake handle. The activity object must be non-null 閳?the guest
// JNIObject ctor error-paths on a null jobject.
char g_fakeActivityObject{};
char g_fakeClassObject{};
char g_fakeMethodId{};
char g_fakeFieldId{};
char g_fakeObjectObject{};

// Mirrors com.firemint.realracing.TextureInfo (defined after raw* helpers).
struct FakeTextureInfo {
    uint32_t magic; // 'TEXI'
    int32_t texId;
    int32_t width;
    int32_t height;
    int32_t texWidth;
    int32_t texHeight;
};
constexpr uint32_t kTextureInfoMagic = 0x49584554; // TEXI
constexpr size_t kMaxTextureInfos = 128;
FakeTextureInfo g_textureInfos[kMaxTextureInfos]{};
size_t g_textureInfoCount = 0;

// Defined below with the other raw helpers; declared here so the jbyteArray
// pool can log without depending on definition order.
void rawLog(const char* text);

// ---- per-call file logging: one shared budget ------------------------------
//
// Every appendGuestBootLine in this file is an fopen/fprintf/fclose. It is
// deliberately fsync-free (recovery_guard.cpp), but it is still three SD-card
// syscalls, and Eden's filesystem HLE warns on a partial stat() for OUR OWN two
// log files on the way in: 911 `GetFileTimeStampRaw ... (Partial
// Implementation) called` lines in the archived 20261003-165034 run, 565 of them
// naming rr3-native-rehost.guest-view.log. Calling that per guest object-method
// call is not a diagnostic, it is an I/O amplifier on the hot route.
//
// Bounded in the kCloudcellSlotTraceBudget shape already used by
// cloudcellTraceSlot (guest_view_probe.cpp): ONE global atomic, relaxed, no
// mutex, no per-thread dedup, unsigned wrap so a read of 0 stays 0. A
// suppressed call therefore costs two relaxed atomics and no syscall, no
// snprintf, no allocation, and nothing that could stall the emulator's
// scheduler -- the 187 s of CPU an earlier std::mutex on this path cost.
//
// The budget is shared rather than per-site on purpose: a per-site cap would
// let one noisy dispatcher spend the whole log and starve the other, and a
// single total is the number a run actually needs ("how many per-call writes
// did we drop"). Which site was suppressed is carried on the cap line.
constexpr unsigned kPerCallLogBudget = 64;
std::atomic<unsigned> g_perCallLogBudget{kPerCallLogBudget};
std::atomic<unsigned long long> g_perCallLogSuppressed{0};
std::atomic<unsigned long long> g_perCallLogEmitted{0};

// Emits the running total only when it has at least doubled since the last
// report, so the line count is O(log N) whatever N is. Deliberately not a
// timer. Two callers, both already on an existing path: the gate below reports
// at every power of two from the suppressed site itself, which is why a run
// that wedges or is powered off still leaves the last few doublings in the log
// as phase=per-call-log-cap; this function adds the absolute total at the JNI
// phase boundary guest_view_probe.cpp already snapshots at. A shutdown-only
// report was rejected because the runs this instrumentation exists for are the
// ones that never reach a shutdown hook.
void perCallLogReport(const char* where) {
    const unsigned long long suppressed =
        g_perCallLogSuppressed.load(std::memory_order_relaxed);
    // Benign data race: a lost update costs one duplicated or skipped report
    // line and nothing else, which is not worth a lock on this path.
    static unsigned long long s_lastReported = 0;
    if (suppressed == 0 || suppressed < s_lastReported * 2) {
        return;
    }
    s_lastReported = suppressed;
    char details[224];
    __builtin_snprintf(
        details, sizeof(details),
        "where=%s suppressed_total=%llu emitted_total=%llu shared_budget=%u "
        "(budget exhausted: further per-call file writes dropped; the site that "
        "ran it dry is on phase=per-call-log-cap)",
        where, suppressed,
        (unsigned long long)g_perCallLogEmitted.load(std::memory_order_relaxed),
        kPerCallLogBudget);
    guestViewWideLog("per-call-log-suppressed", suppressed, kPerCallLogBudget,
                     details);
}

// True when the caller may spend a file write. False means the line was
// suppressed AND counted, which is the number a run needs to show the cap
// engaged rather than merely showing a shorter log.
bool perCallLogAllow(const char* site) {
    if (g_perCallLogBudget.fetch_sub(1, std::memory_order_relaxed) == 0) {
        const unsigned long long n =
            g_perCallLogSuppressed.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            // Power of two: one report per doubling, and the first one says
            // which site ran the budget dry.
            char details[192];
            __builtin_snprintf(details, sizeof(details),
                               "site=%s suppressed_total=%llu emitted_total=%llu "
                               "shared_budget=%u (budget exhausted)",
                               site, n,
                               (unsigned long long)g_perCallLogEmitted.load(
                                   std::memory_order_relaxed),
                               kPerCallLogBudget);
            guestViewWideLog("per-call-log-cap", n, kPerCallLogBudget, details);
        }
        return false;
    }
    g_perCallLogEmitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- fake jbyteArray pool --------------------------------------------------
// Guest code creates byte[] via NewByteArray, fills it, then hands the handle
// to loadTextureFromMemory. Handles are opaque FakeByteArray records whose
// data pointer lands in a static bump pool (recycled when full).

struct FakeByteArray {
    uint32_t magic;   // 'ARRB'
    int32_t length;
    int32_t capacity;
    uint8_t* data;    // points into g_byteArrayPool
};

constexpr uint32_t kByteArrayMagic = 0x42525241;  // ARRB
constexpr size_t kMaxByteArrays = 32;
constexpr size_t kByteArrayPoolSize = 4u * 1024u * 1024u;
FakeByteArray g_byteArrays[kMaxByteArrays]{};
size_t g_byteArrayCount = 0;
uint8_t g_byteArrayPool[kByteArrayPoolSize]{};
size_t g_byteArrayPoolUsed = 0;

FakeByteArray* asByteArray(uintptr_t handle) {
    if (handle == 0) {
        return nullptr;
    }
    FakeByteArray* array = reinterpret_cast<FakeByteArray*>(handle);
    // Reject handles that are not one of our records (guest may pass a raw
    // pointer or a stale reference).
    bool known = false;
    for (size_t index = 0; index < kMaxByteArrays; ++index) {
        if (handle == reinterpret_cast<uintptr_t>(&g_byteArrays[index])) {
            known = true;
            break;
        }
    }
    if (!known || array->magic != kByteArrayMagic) {
        return nullptr;
    }
    return array;
}

const uint8_t* byteArrayBytes(uintptr_t handle, int32_t* outLength) {
    FakeByteArray* array = asByteArray(handle);
    if (array == nullptr || array->data == nullptr || array->length <= 0) {
        return nullptr;
    }
    if (outLength != nullptr) {
        *outLength = array->length;
    }
    return array->data;
}

uintptr_t jniNewByteArray(uintptr_t /*env*/, int32_t length) {
    if (length < 0) {
        return 0;
    }
    if (static_cast<size_t>(length) > kByteArrayPoolSize) {
        rawLog("[jni] NewByteArray: length exceeds pool\n");
        return 0;
    }
    // Recycle the whole pool when the bump pointer cannot satisfy the request.
    const size_t needed = (static_cast<size_t>(length) + 7u) & ~size_t{7};
    if (g_byteArrayPoolUsed + needed > kByteArrayPoolSize) {
        g_byteArrayPoolUsed = 0;
        g_byteArrayCount = 0;
        for (size_t index = 0; index < kMaxByteArrays; ++index) {
            g_byteArrays[index].magic = 0;
            g_byteArrays[index].length = 0;
            g_byteArrays[index].capacity = 0;
            g_byteArrays[index].data = nullptr;
        }
    }
    if (g_byteArrayCount >= kMaxByteArrays) {
        g_byteArrayCount = 0;  // overwrite oldest slot
    }
    FakeByteArray& array = g_byteArrays[g_byteArrayCount++];
    array.magic = kByteArrayMagic;
    array.length = length;
    array.capacity = static_cast<int32_t>(needed);
    array.data = g_byteArrayPool + g_byteArrayPoolUsed;
    g_byteArrayPoolUsed += needed;
    // Zero so GetByteArrayElements callers see deterministic contents.
    for (int32_t i = 0; i < length; ++i) {
        array.data[i] = 0;
    }
    if (perCallLogAllow("NewByteArray")) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "NewByteArray",
                            static_cast<unsigned long long>(length),
                            static_cast<unsigned long long>(g_byteArrayCount));
    }
    return reinterpret_cast<uintptr_t>(&array);
}

int32_t jniGetArrayLength(uintptr_t /*env*/, uintptr_t arrayHandle) {
    FakeByteArray* array = asByteArray(arrayHandle);
    return array != nullptr ? array->length : 0;
}

uintptr_t jniGetByteArrayElements(uintptr_t /*env*/, uintptr_t arrayHandle,
                                  uintptr_t isCopy) {
    FakeByteArray* array = asByteArray(arrayHandle);
    if (array == nullptr) {
        return 0;
    }
    if (isCopy != 0) {
        *reinterpret_cast<int32_t*>(isCopy) = 0;  // JNI_FALSE: direct pointer
    }
    return reinterpret_cast<uintptr_t>(array->data);
}

void jniReleaseByteArrayElements(uintptr_t /*env*/, uintptr_t arrayHandle,
                                 uintptr_t /*elems*/, int32_t /*mode*/) {
    // Direct pointer: nothing to copy back or free.
    (void)arrayHandle;
}

void jniGetByteArrayRegion(uintptr_t /*env*/, uintptr_t arrayHandle,
                           int32_t start, int32_t length, uintptr_t buf) {
    FakeByteArray* array = asByteArray(arrayHandle);
    if (array == nullptr || buf == 0 || length <= 0 || start < 0) {
        return;
    }
    if (static_cast<int64_t>(start) + length > array->length) {
        return;
    }
    uint8_t* destination = reinterpret_cast<uint8_t*>(buf);
    for (int32_t i = 0; i < length; ++i) {
        destination[i] = array->data[start + i];
    }
}

void jniSetByteArrayRegion(uintptr_t /*env*/, uintptr_t arrayHandle,
                           int32_t start, int32_t length, uintptr_t buf) {
    FakeByteArray* array = asByteArray(arrayHandle);
    if (array == nullptr || buf == 0 || length <= 0 || start < 0) {
        return;
    }
    if (static_cast<int64_t>(start) + length > array->length) {
        return;
    }
    const uint8_t* source = reinterpret_cast<const uint8_t*>(buf);
    for (int32_t i = 0; i < length; ++i) {
        array->data[start + i] = source[i];
    }
}

struct JniFieldRecord {
    uint32_t magic;
    // The class the ID was resolved against. FindClass used to hand back one
    // shared singleton for every class, so a name-only key made `ascent` on
    // Font alias `ascent` on any other class and `width` alias the TextureInfo
    // field of the same name. Part D: the class is part of the identity.
    uintptr_t clazz;
    char name[40];
};
constexpr uint32_t kJniFieldMagic = 0x52523346; // RR3F
constexpr size_t kMaxJniFields = 64;
JniFieldRecord g_jniFields[kMaxJniFields]{};
size_t g_jniFieldCount = 0;

struct JniStringHandle {
    uint32_t magic;
    char value[128];
};

constexpr uint32_t kJniStringMagic = 0x52523353; // RR3S
constexpr size_t kMaxJniStrings = 64;
JniStringHandle g_fakeStringObject{kJniStringMagic, {0}};
JniStringHandle g_jniStrings[kMaxJniStrings]{};

struct JniMethodRecord {
    uint32_t magic;
    uintptr_t clazz;
    bool isStatic;
    char name[64];
    char signature[128];
};

constexpr uint32_t kJniMethodMagic = 0x5252334d; // RR3M
constexpr size_t kMaxJniMethods = 256;
JniMethodRecord g_jniMethods[kMaxJniMethods]{};
size_t g_jniMethodCount = 0;

// Short, allocation-free description of what a JNI call handed back, so a stub
// returning a dummy where the engine wanted a real Surface/Context/window
// handle is visible in the log instead of inferred. The shapes that matter are
// null, a zero/small integer, the three shared fakes, and a real heap pointer.
bool isNamedObject(uintptr_t handle);
// Defined with the class table further down; declared here so the shape
// reporter can recognise a per-name class handle as a jclass.
bool isClassHandle(uintptr_t handle);
const char* jniShape(uintptr_t value, bool isString) {
    if (value == 0) {
        return "null";
    }
    if (value == reinterpret_cast<uintptr_t>(&g_fakeObjectObject)) {
        return "jobject-fake";
    }
    if (value == reinterpret_cast<uintptr_t>(&g_fakeClassObject) ||
        isClassHandle(value)) {
        return "jclass-fake";
    }
    if (value == reinterpret_cast<uintptr_t>(&g_fakeStringObject)) {
        return "jstring-fake";
    }
    if (isNamedObject(value)) {
        return "jobject-named";
    }
    if (value < 0x10000) {
        return "small-int";
    }
    return isString ? "jstring-heap" : "jobject-heap";
}

char g_emptyUtfString[1] = {0};

// MainActivity.m_gameState. Java onCreate/onStart/onResume set 1/2/3; the
// native lifecycle we drive must keep the same values so GLRenderer's
// getGameState()==5 skip and the guest's own state gates see a running app.
// Storage error uses 5; 4/6 appear in pause/stop paths.
int32_t g_gameState = 0;

// ---- stub table: one distinct function per slot so the index is known ----

template <uint32_t Slot>
void* jniSlotStub() {
    g_jniLastSlot = Slot;
    ++g_jniUnhandledCalls;
    ++g_jniSlotHits[Slot];
    return nullptr;
}

using JniFn = void* (*)();

template <size_t... Indices>
std::array<JniFn, sizeof...(Indices)> makeStubTable(std::index_sequence<Indices...>) {
    return {(&jniSlotStub<static_cast<uint32_t>(Indices)>)...};
}

const std::array<JniFn, kJniSlotCount> g_stubTable =
    makeStubTable(std::make_index_sequence<kJniSlotCount>{});

// ---- implemented slots -------------------------------------------------

// Small helpers must not touch host TLS: no stdio/malloc. strlen-equivalent
// is inline so the debug log can carry method names without newlib.

size_t rawStrlen(const char* text) {
    size_t length = 0;
    while (text[length] != '\0' && length < 192) {
        ++length;
    }
    return length;
}

void rawLog(const char* text) {
    if (text != nullptr) {
        svcOutputDebugString(text, rawStrlen(text));
    }
}

bool rawEquals(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    size_t index = 0;
    while (index < 192 && left[index] == right[index]) {
        if (left[index] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

void rawCopy(char* destination, size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    size_t index = 0;
    if (source != nullptr) {
        while (index + 1 < capacity && index < 192 && source[index] != '\0') {
            destination[index] = source[index];
            ++index;
        }
    }
    destination[index] = '\0';
}

uintptr_t rememberJniField(uintptr_t clazz, const char* name) {
    for (size_t index = 0; index < g_jniFieldCount; ++index) {
        if (g_jniFields[index].magic == kJniFieldMagic &&
            g_jniFields[index].clazz == clazz &&
            rawEquals(g_jniFields[index].name, name)) {
            return reinterpret_cast<uintptr_t>(&g_jniFields[index]);
        }
    }
    if (g_jniFieldCount >= kMaxJniFields) {
        return reinterpret_cast<uintptr_t>(&g_fakeFieldId);
    }
    JniFieldRecord& field = g_jniFields[g_jniFieldCount++];
    field.magic = kJniFieldMagic;
    field.clazz = clazz;
    rawCopy(field.name, sizeof(field.name), name);
    return reinterpret_cast<uintptr_t>(&field);
}

const char* jniFieldName(uintptr_t fieldId) {
    for (size_t index = 0; index < g_jniFieldCount; ++index) {
        if (fieldId == reinterpret_cast<uintptr_t>(&g_jniFields[index]) &&
            g_jniFields[index].magic == kJniFieldMagic) {
            return g_jniFields[index].name;
        }
    }
    return nullptr;
}

void uploadPlaceholderTexture(GLuint texture) {
    ::glBindTexture(GL_TEXTURE_2D, texture);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    constexpr int32_t kSize = 64;
    unsigned char pixels[kSize * kSize * 4];
    for (int32_t y = 0; y < kSize; ++y) {
        for (int32_t x = 0; x < kSize; ++x) {
            const bool dark = ((x / 8) + (y / 8)) % 2 == 0;
            unsigned char* p = &pixels[(y * kSize + x) * 4];
            p[0] = static_cast<unsigned char>(dark ? 40 : 220);
            p[1] = static_cast<unsigned char>(dark ? 40 : 80);
            p[2] = static_cast<unsigned char>(dark ? 40 : 200);
            p[3] = 255;
        }
    }
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSize, kSize, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels);
}

uintptr_t makeTextureInfoHandle() {
    if (g_textureInfoCount >= kMaxTextureInfos) {
        g_textureInfoCount = 0;
    }
    FakeTextureInfo& info = g_textureInfos[g_textureInfoCount++];
    info.magic = kTextureInfoMagic;
    GLuint texture = 0;
    ::glGenTextures(1, &texture);
    info.texId = static_cast<int32_t>(texture);
    info.width = 64;
    info.height = 64;
    info.texWidth = 64;
    info.texHeight = 64;
    if (texture != 0) {
        uploadPlaceholderTexture(texture);
    }
    if (perCallLogAllow("load-texture")) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "load-texture", static_cast<unsigned long long>(texture),
                            static_cast<unsigned long long>(g_textureInfoCount));
    }
    return reinterpret_cast<uintptr_t>(&info);
}

// Decode a real PNG from guest memory and upload it as an RGBA texture.
// Falls back to the 64x64 checker placeholder on any failure.
uintptr_t makeTextureInfoFromPng(const uint8_t* bytes, size_t size) {
    PngDecodeSession session{};
    if (!pngDecodeBegin(&session, bytes, size)) {
        rawLog("[jni] loadTextureFromMemory: PNG decode failed -> placeholder\n");
        if (perCallLogAllow("load-texture-badpng")) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "load-texture-badpng",
                                static_cast<unsigned long long>(size), 0);
        }
        return makeTextureInfoHandle();
    }

    GLuint texture = 0;
    ::glGenTextures(1, &texture);
    if (texture == 0) {
        return makeTextureInfoHandle();
    }
    ::glBindTexture(GL_TEXTURE_2D, texture);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                   static_cast<GLsizei>(session.width),
                   static_cast<GLsizei>(session.height), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);

    uint8_t row[rr3::kPngMaxDimension * 4];
    for (uint32_t y = 0; y < session.height; ++y) {
        bool done = false;
        if (!pngDecodeNextRow(&session, row, &done) || done) {
            rawLog("[jni] loadTextureFromMemory: row decode failed -> placeholder\n");
            ::glDeleteTextures(1, &texture);
            return makeTextureInfoHandle();
        }
        ::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(y),
                          static_cast<GLsizei>(session.width), 1, GL_RGBA,
                          GL_UNSIGNED_BYTE, row);
    }

    if (g_textureInfoCount >= kMaxTextureInfos) {
        g_textureInfoCount = 0;
    }
    FakeTextureInfo& info = g_textureInfos[g_textureInfoCount++];
    info.magic = kTextureInfoMagic;
    info.texId = static_cast<int32_t>(texture);
    info.width = static_cast<int32_t>(session.width);
    info.height = static_cast<int32_t>(session.height);
    info.texWidth = static_cast<int32_t>(session.width);
    info.texHeight = static_cast<int32_t>(session.height);
    if (perCallLogAllow("load-texture-png")) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "load-texture-png", static_cast<unsigned long long>(texture),
                            (static_cast<unsigned long long>(session.width) << 16) |
                                session.height);
    }
    return reinterpret_cast<uintptr_t>(&info);
}

// CallStaticObjectMethodV path for loadTextureFromMemory(byte[], int).
// Reads the jbyteArray handle and jint length from the guest va_list; if the
// handle cannot be resolved, falls back to the placeholder with a breadcrumb.
uintptr_t loadTextureFromMemoryArgs(void* args) {
    if (args == nullptr) {
        rawLog("[jni] loadTextureFromMemory: null va_list -> placeholder\n");
        if (perCallLogAllow("load-texture-noargs")) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "load-texture-noargs", 0, 0);
        }
        return makeTextureInfoHandle();
    }
    va_list* vap = reinterpret_cast<va_list*>(args);
    void* handle = va_arg(*vap, void*);
    int length = va_arg(*vap, int);
    int32_t byteLength = 0;
    const uint8_t* bytes = byteArrayBytes(reinterpret_cast<uintptr_t>(handle),
                                          &byteLength);
    if (bytes == nullptr || byteLength <= 0) {
        rawLog("[jni] loadTextureFromMemory: unreadable byte[] -> placeholder\n");
        if (perCallLogAllow("load-texture-badarray")) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "load-texture-badarray",
                                static_cast<unsigned long long>(length),
                                reinterpret_cast<unsigned long long>(handle));
        }
        return makeTextureInfoHandle();
    }
    if (length > 0 && length < byteLength) {
        byteLength = length;  // trust the explicit length when smaller
    }
    return makeTextureInfoFromPng(bytes, static_cast<size_t>(byteLength));
}

JniMethodRecord* findJniMethod(uintptr_t methodId) {
    for (size_t index = 0; index < g_jniMethodCount; ++index) {
        if (methodId == reinterpret_cast<uintptr_t>(&g_jniMethods[index])) {
            return &g_jniMethods[index];
        }
    }
    return nullptr;
}

uintptr_t rememberJniMethod(uintptr_t clazz, const char* name,
                            const char* signature, bool isStatic) {
    for (size_t index = 0; index < g_jniMethodCount; ++index) {
        const JniMethodRecord& method = g_jniMethods[index];
        if (method.clazz == clazz && method.isStatic == isStatic &&
            rawEquals(method.name, name) && rawEquals(method.signature, signature)) {
            return reinterpret_cast<uintptr_t>(&g_jniMethods[index]);
        }
    }
    if (g_jniMethodCount >= kMaxJniMethods) {
        return reinterpret_cast<uintptr_t>(&g_fakeMethodId);
    }
    JniMethodRecord& method = g_jniMethods[g_jniMethodCount++];
    method.magic = kJniMethodMagic;
    method.clazz = clazz;
    method.isStatic = isStatic;
    rawCopy(method.name, sizeof(method.name), name);
    rawCopy(method.signature, sizeof(method.signature), signature);
    return reinterpret_cast<uintptr_t>(&method);
}

const char* jniMethodName(uintptr_t methodId) {
    const JniMethodRecord* method = findJniMethod(methodId);
    return method != nullptr ? method->name : nullptr;
}

bool jniMethodReturnsString(uintptr_t methodId) {
    const JniMethodRecord* method = findJniMethod(methodId);
    if (method == nullptr) {
        return false;
    }
    const char* close = method->signature;
    while (*close != '\0' && *close != ')') {
        ++close;
    }
    return *close == ')' && rawEquals(close + 1, "Ljava/lang/String;");
}

// ---------------------------------------------------------------------------
// Per-class identity, and the Font object behind it.
//
// FindClass used to ignore its name and return the one &g_fakeClassObject for
// every class. That single choice cost three things the guest could not work
// around, and all three are fixed here by giving each distinct class name its
// own stable handle:
//
//   1. NewObjectV could not tell a Font apart from anything else, so the
//      receiver of every object method was the same &g_fakeObjectObject.
//   2. GetMethodID/rememberJniMethod deduped on (clazz, name, sig) with clazz
//      constant, so <init> on Font aliased <init> on any other class.
//   3. GetIntField/GetFloatField/GetObjectField had no way to tell a Font
//      metric from a TextureInfo field of the same name.
//
// The handles are stable (the guest stores them and hands them back), which is
// the same requirement namedObjectFor documents for the getComponent table.
// ---------------------------------------------------------------------------

struct FakeClass {
    uint32_t magic; // 'CLSS'
    char name[96];
};

constexpr uint32_t kFakeClassMagic = 0x53534C43;  // CLSS

// RAISED 32 -> 256 on 2026-10-19, with a measurement behind it.
//
// MEASURED on archive 20261019-100814-ccjni01: the guest issues 16,230
// FindClass calls for 34 DISTINCT class names in a 300 s run, and the first
// 34 in first-seen order end at exactly:
//
//     #31  com/firemonkeys/cloudcellapi/FacebookWorker
//     #32  com/firemonkeys/cloudcellapi/GooglePlayWorker   <-- overflowed the old 32
//     #33  com/firemonkeys/cloudcellapi/HttpRequest       <-- overflowed the old 32
//
// So the old capacity dropped the two classes off the end by a margin of one.
// AndroidAccountManager (#27) and FacebookWorker (#31) still fit and still
// resolve by name; HttpRequest -- the class behind the load-bar blocker -- did
// not.
//
// Overflow used to return &g_fakeClassObject, the same sentinel every other
// unanswered class gets. That is the worst possible failure shape here: the
// guest could not tell two different classes apart, but nothing in the host
// said so. Six of the sixteen void calls in that run logged class=(none) for
// exactly this reason. 256 leaves ~7x headroom over what is actually observed;
// the counter below is what catches the next overflow, because a silent alias
// reads exactly like a class the guest never asked for.
constexpr size_t kMaxFakeClasses = 256;
std::atomic<size_t> g_fakeClassOverflow{0};
FakeClass g_fakeClasses[kMaxFakeClasses]{};
size_t g_fakeClassCount = 0;

// The one class this file answers by name. Matched against the tail of the
// FindClass argument so the slash form the guest logs
// ("com/firemint/realracing/Font") and a bare "Font" both resolve.
const char kFontClassTail[] = "firemint/realracing/Font";

bool nameIsFontClass(const char* name) {
    if (name == nullptr) {
        return false;
    }
    // rawEquals is a bounded walk from the front; the guest may pass either
    // "com/firemint/realracing/Font" or a plain "Font", so match the tail.
    const size_t nameLength = rawStrlen(name);
    const size_t tailLength = rawStrlen(kFontClassTail);
    if (nameLength < tailLength) {
        return rawEquals(name, "Font");
    }
    size_t index = 0;
    while (index < tailLength) {
        if (name[nameLength - tailLength + index] != kFontClassTail[index]) {
            return false;
        }
        ++index;
    }
    return true;
}

// Stable handle per class name. Falls back to the shared sentinel when the
// name is absent or the table is full: aliasing every overflow class onto one
// record is the old behaviour, so a full table degrades to the bug instead of
// inventing an identity that never existed.
uintptr_t classHandleFor(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return reinterpret_cast<uintptr_t>(&g_fakeClassObject);
    }
    for (size_t index = 0; index < g_fakeClassCount; ++index) {
        if (g_fakeClasses[index].magic == kFakeClassMagic &&
            rawEquals(g_fakeClasses[index].name, name)) {
            return reinterpret_cast<uintptr_t>(&g_fakeClasses[index]);
        }
    }
    if (g_fakeClassCount >= kMaxFakeClasses) {
        // Say so. The old form aliased to g_fakeClassObject with no trace, and
        // the only way it was noticed was class=(none) appearing on unrelated
        // call sites six rounds later. A silent alias is indistinguishable from
        // "the guest never asked", which is the whole problem.
        //
        // Rate-limited to the first 4 because this sits on a path the guest
        // takes 16,230 times per run; g_fakeClassOverflow carries the real
        // total and is reported at census time.
        const size_t overflow = ++g_fakeClassOverflow;
        if (overflow <= 4) {
            char details[192];
            __builtin_snprintf(details, sizeof(details),
                               "class table full at %u entries; %s aliased to the "
                               "shared sentinel (overflow #%u)",
                               (unsigned)kMaxFakeClasses, name,
                               (unsigned)overflow);
            guestViewWideLog("jni-fakeclass-overflow", (unsigned long long)overflow,
                             (unsigned long long)g_fakeClassCount, details);
        }
        return reinterpret_cast<uintptr_t>(&g_fakeClassObject);
    }
    FakeClass& record = g_fakeClasses[g_fakeClassCount++];
    record.magic = kFakeClassMagic;
    rawCopy(record.name, sizeof(record.name), name);
    return reinterpret_cast<uintptr_t>(&record);
}

const char* className(uintptr_t handle) {
    for (size_t index = 0; index < g_fakeClassCount; ++index) {
        if (handle == reinterpret_cast<uintptr_t>(&g_fakeClasses[index]) &&
            g_fakeClasses[index].magic == kFakeClassMagic) {
            return g_fakeClasses[index].name;
        }
    }
    return nullptr;
}

bool isClassHandle(uintptr_t handle) {
    return className(handle) != nullptr;
}

// The Font class handle, or 0 when the guest never asked for it. Used so the
// per-slot Font branches key on the class the guest actually named rather than
// on a hardcoded address.
uintptr_t fontClassHandle() {
    if (g_fakeClassCount == 0) {
        return 0;
    }
    for (size_t index = 0; index < g_fakeClassCount; ++index) {
        if (g_fakeClasses[index].magic == kFakeClassMagic &&
            nameIsFontClass(g_fakeClasses[index].name)) {
            return reinterpret_cast<uintptr_t>(&g_fakeClasses[index]);
        }
    }
    return 0;
}

// ---- Font state -----------------------------------------------------------
//
// Synthetic but self-consistent metrics for com.firemint.realracing.Font at
// 48 px. The units are a 2048-unit em, which is the one head field that
// survives the file's obfuscation (r3_symbols.ttf reports unitsPerEm = 2048);
// the hhea ascent/descent/lineGap that a real init() would read are scrambled,
// so the split below is the conventional 0.8 / 0.2 Latin em box and NOT a
// measurement of this font. Rasterisation is a separate, unimplemented step:
// every value here is a plausible constant, and none of them is a pixel.
//
// Derived once, at S = 48 px, scale = 48 / 2048 = 0.0234375 px/unit:
//   ascent   = ceil (1664 u * 0.0234375) = ceil(39.00)  =  39   (0.8125 em)
//   descent  = floor(-384 u * 0.0234375) = floor(-9.00) =  -9   (0.1875 em)
//   height   = ascent + descent            =  39 + (-9)  =  30   (Font.java:85
//              assigns exactly this, so height == ascent+descent by
//              construction, which satisfies "height >= ascent + descent")
//   top      =  40  ( >= ascent 39, one px of font-box overshoot )
//   bottom   =  11  ( >= |descent| 9 )
//   leading  =   6  ( lineGap 256 u, the space between successive line boxes )
//
// Glyph box, from the same getTextBounds/getTextWidths pair Font.java calls:
//   glyphOffY    = 39   (ink top sits on the ascent line: -rect.top)
//   glyphHeight  = 35   (ink bottom 4 px above the baseline: -rect.bottom = 4)
//   glyphWidth   = 24   (0.5 em ink box: rect.right - rect.left)
//   glyphAdvance = 24   (getTextWidths of a 0.5 em symbol glyph)
//   glyphOffX    =  0   (Paint.Align.LEFT, so rect.left = 0)
//   -> ink rect = (left 0, top -39, right 24, bottom -4)
//   -> 35 <= height + leading (36) and <= ascent - descent (48): the ink fits.
//
// Bitmap box, from loadBitmapDynamic's own arithmetic with f=0, f2=0, f3=0 and
// outline mode OUTLINE_NONE (i2=0, so f4 = 1.0):
//   fFloor  = floor((0 + 0) - 1.0)          = -1   -> bmpLeft   = -1
//   fCeil   = ceil (24 + 0 + 1.0)            = 25
//   fFloor2 = floor((-39 + 0) - 1.0)         = -40  -> bmpTop    = 40
//   fCeil2  = ceil ((-4 + 0) + 1.0)          = -3
//   bmpWidth  = 25 - (-1)  = 26   (ink 24 px + 1 px outline each side)
//   bmpHeight = -3 - (-40) = 37   (ink 35 px + 1 px outline top and bottom)
//   bmpPitch  = getRowBytes() = 26, because ALPHA_8 is one byte per pixel
//   bmpData   = new byte[26 * 37] = 962 bytes
//   -> ink occupies bitmap columns 1..24 of 0..25 and rows 1..36 of 0..36, so
//      the whole ink box is inside the bitmap it claims to describe.
//
// These are the OUTLINE_NONE, zero-offset numbers. loadBitmap/loadBitmapDynamic
// take (codePoint, offsetX, offsetY, stroke, outline) and the offset/stroke
// arguments are deliberately NOT read back out of the AArch64 va_list (a
// variadic float is only readable via va_arg(ap, double) and the guest's
// native signature is not in evidence), so the outline modes 1 and 3 will not
// be reflected in what the guest gets back. Documented, not silent.
constexpr int32_t kFontSizeX100 = 4800;  // 48.00 px, getSize()
constexpr float kFontAscent = 39.0f;
constexpr float kFontDescent = -9.0f;
constexpr float kFontHeight = 30.0f;
constexpr float kFontTop = 40.0f;
constexpr float kFontBottom = 11.0f;
constexpr float kFontLeading = 6.0f;
constexpr float kFontGlyphOffX = 0.0f;
constexpr float kFontGlyphOffY = 39.0f;
constexpr float kFontGlyphWidth = 24.0f;
constexpr float kFontGlyphHeight = 35.0f;
constexpr float kFontGlyphAdvance = 24.0f;
constexpr int32_t kFontBmpLeft = -1;
constexpr int32_t kFontBmpTop = 40;
constexpr int32_t kFontBmpWidth = 26;
constexpr int32_t kFontBmpHeight = 37;
constexpr int32_t kFontBmpPitch = 26;
constexpr int32_t kFontBmpBytes = kFontBmpWidth * kFontBmpHeight;  // 962

struct FakeFont {
    uint32_t magic;        // 'FNTO'
    uint32_t initialised;  // 1 once init() has answered true
    uintptr_t bmpData;     // FakeByteArray handle, 0 before the first loadBitmap
};

constexpr uint32_t kFakeFontMagic = 0x4F544E46;  // FNTO
FakeFont g_fakeFont{kFakeFontMagic, 0, 0};

FakeFont* asFontObject(uintptr_t handle) {
    if (handle == 0) {
        return nullptr;
    }
    FakeFont* font = reinterpret_cast<FakeFont*>(handle);
    return (font == &g_fakeFont && font->magic == kFakeFontMagic) ? font : nullptr;
}

// The byte[] behind bmpData. Allocated through the existing NewByteArray slot
// so the guest gets a real array record - magic, length, readable data pointer -
// rather than the 1-byte char sentinel. Every glyph the guest asks for has the
// same box (see the arithmetic above), so the array is allocated once and
// reused; that also keeps NewByteArray's own boot line off the retry path.
// asByteArray revalidates the record, so a pool recycle by the PNG staging path
// is detected and the array re-made rather than handed back as a dangling
// handle.
uintptr_t fontBmpData(FakeFont* font) {
    if (font == nullptr) {
        return 0;
    }
    if (font->bmpData != 0 && asByteArray(font->bmpData) != nullptr) {
        return font->bmpData;
    }
    font->bmpData = jniNewByteArray(0, kFontBmpBytes);
    return font->bmpData;
}

float fontFloatField(const FakeFont* font, const char* name) {
    if (font == nullptr || name == nullptr) {
        return 0.f;
    }
    if (rawEquals(name, "ascent")) return kFontAscent;
    if (rawEquals(name, "descent")) return kFontDescent;
    if (rawEquals(name, "height")) return kFontHeight;
    if (rawEquals(name, "top")) return kFontTop;
    if (rawEquals(name, "bottom")) return kFontBottom;
    if (rawEquals(name, "leading")) return kFontLeading;
    if (rawEquals(name, "glyphOffX")) return kFontGlyphOffX;
    if (rawEquals(name, "glyphOffY")) return kFontGlyphOffY;
    if (rawEquals(name, "glyphWidth")) return kFontGlyphWidth;
    if (rawEquals(name, "glyphHeight")) return kFontGlyphHeight;
    if (rawEquals(name, "glyphAdvance")) return kFontGlyphAdvance;
    return 0.f;
}

int32_t fontIntField(const FakeFont* font, const char* name) {
    if (font == nullptr || name == nullptr) {
        return 0;
    }
    if (rawEquals(name, "bmpLeft")) return kFontBmpLeft;
    if (rawEquals(name, "bmpTop")) return kFontBmpTop;
    if (rawEquals(name, "bmpWidth")) return kFontBmpWidth;
    if (rawEquals(name, "bmpHeight")) return kFontBmpHeight;
    if (rawEquals(name, "bmpPitch")) return kFontBmpPitch;
    return 0;
}

bool fontBoolMethod(const char* name) {
    return rawEquals(name, "init") || rawEquals(name, "loadGlyph") ||
           rawEquals(name, "loadBitmap") || rawEquals(name, "loadBitmapDynamic");
}

// One line per Font lifecycle step, the first time it happens only, so a run
// shows the sequence without putting anything on the per-frame retry path.
bool g_fontLoggedInit = false;
bool g_fontLoggedGlyph = false;
bool g_fontLoggedBitmap = false;
bool g_fontLoggedSummary = false;

void fontNoteInit() {
    if (g_fontLoggedInit) {
        return;
    }
    g_fontLoggedInit = true;
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "init -> true size=%d.%02d ascent=%.0f descent=%.0f "
                       "height=%.0f (unitsPerEm=2048, scale=48/2048)",
                       kFontSizeX100 / 100, kFontSizeX100 % 100, kFontAscent,
                       kFontDescent, kFontHeight);
    rawLog("[jni] Font ");
    rawLog(details);
    rawLog("\n");
}

void fontNoteGlyph() {
    if (g_fontLoggedGlyph) {
        return;
    }
    g_fontLoggedGlyph = true;
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "loadGlyph -> true offX=%.0f offY=%.0f width=%.0f "
                       "height=%.0f advance=%.0f",
                       kFontGlyphOffX, kFontGlyphOffY, kFontGlyphWidth,
                       kFontGlyphHeight, kFontGlyphAdvance);
    rawLog("[jni] Font ");
    rawLog(details);
    rawLog("\n");
}

// The end-of-setup line: every value this object hands the guest, so a run can
// be diffed against the arithmetic in the comment above. Once, via the
// no-fsync guest-view log.
void fontNoteSummary(uintptr_t bmpData) {
    if (g_fontLoggedSummary) {
        return;
    }
    g_fontLoggedSummary = true;
    int32_t bytes = 0;
    const uint8_t* probe = byteArrayBytes(bmpData, &bytes);
    char details[384];
    __builtin_snprintf(
        details, sizeof(details),
        "SYNTHETIC-NO-RASTER size=%.2f ascent=%.0f descent=%.0f height=%.0f "
        "top=%.0f bottom=%.0f leading=%.0f glyphOffX=%.0f glyphOffY=%.0f "
        "glyphWidth=%.0f glyphHeight=%.0f glyphAdvance=%.0f bmpLeft=%d "
        "bmpTop=%d bmpWidth=%d bmpHeight=%d bmpPitch=%d bmpDataBytes=%d "
        "bmpDataReadable=%d",
        kFontSizeX100 / 100.0, kFontAscent, kFontDescent, kFontHeight, kFontTop,
        kFontBottom, kFontLeading, kFontGlyphOffX, kFontGlyphOffY,
        kFontGlyphWidth, kFontGlyphHeight, kFontGlyphAdvance, kFontBmpLeft,
        kFontBmpTop, kFontBmpWidth, kFontBmpHeight, kFontBmpPitch, bytes,
        probe != nullptr ? 1 : 0);
    guestViewWideLog("jni-font-metrics", static_cast<unsigned long long>(kFontBmpBytes),
                     0, details);
}

uintptr_t makeJniString(const char* value) {
    if (value == nullptr) {
        value = "";
    }
    if (rawEquals(g_fakeStringObject.value, value)) {
        return reinterpret_cast<uintptr_t>(&g_fakeStringObject);
    }
    for (size_t index = 0; index < kMaxJniStrings; ++index) {
        if (g_jniStrings[index].magic == kJniStringMagic &&
            rawEquals(g_jniStrings[index].value, value)) {
            return reinterpret_cast<uintptr_t>(&g_jniStrings[index]);
        }
    }
    for (size_t index = 0; index < kMaxJniStrings; ++index) {
        if (g_jniStrings[index].magic != kJniStringMagic) {
            g_jniStrings[index].magic = kJniStringMagic;
            rawCopy(g_jniStrings[index].value,
                    sizeof(g_jniStrings[index].value), value);
            return reinterpret_cast<uintptr_t>(&g_jniStrings[index]);
        }
    }
    return reinterpret_cast<uintptr_t>(&g_fakeStringObject);
}

const char* jniStringValue(uintptr_t string) {
    if (string == reinterpret_cast<uintptr_t>(&g_fakeStringObject)) {
        return g_fakeStringObject.value;
    }
    for (size_t index = 0; index < kMaxJniStrings; ++index) {
        if (string == reinterpret_cast<uintptr_t>(&g_jniStrings[index]) &&
            g_jniStrings[index].magic == kJniStringMagic) {
            return g_jniStrings[index].value;
        }
    }
    return g_emptyUtfString;
}

// ---- distinct, stable named objects (getComponent / loadClass) -----------
//
// getComponent("...") and loadClass("...") both name what they want, and the
// reference port answers loadClass with a *different* Class per class name
// (lang_ClassLoader.cpp:33-35, a pointer into its own class registry). The
// shared g_fakeObjectObject made every one of those requests return the same
// handle, so any guest-side identity test collapsed.
//
// Identity therefore has to be stable, not merely distinct: the guest stores
// these handles and hands them back on later calls, so a request that returned
// a fresh address each time would be strictly worse than the old constant.
// Keyed on the call's method plus what it asked for, so the same request
// always yields the same object and two different requests never alias.
//
// Layout is deliberately opaque (magic + key). Every JNI slot in this file
// dispatches on methodId and never dereferences a jobject, so no field can be
// read back; the key is carried for logging, not for the guest.
struct FakeNamedObject {
    uint32_t magic; // 'CMPO'
    char key[80];
};

constexpr uint32_t kNamedObjectMagic = 0x4F504D43; // CMPO
constexpr size_t kMaxNamedObjects = 64;
FakeNamedObject g_namedObjects[kMaxNamedObjects]{};
size_t g_namedObjectCount = 0;

bool isNamedObject(uintptr_t handle) {
    for (size_t index = 0; index < g_namedObjectCount; ++index) {
        if (handle == reinterpret_cast<uintptr_t>(&g_namedObjects[index])) {
            return true;
        }
    }
    return false;
}

const char* namedObjectKey(uintptr_t handle) {
    for (size_t index = 0; index < g_namedObjectCount; ++index) {
        if (handle == reinterpret_cast<uintptr_t>(&g_namedObjects[index]) &&
            g_namedObjects[index].magic == kNamedObjectMagic) {
            return g_namedObjects[index].key;
        }
    }
    return nullptr;
}

uintptr_t namedObjectFor(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
    }
    for (size_t index = 0; index < g_namedObjectCount; ++index) {
        if (g_namedObjects[index].magic == kNamedObjectMagic &&
            rawEquals(g_namedObjects[index].key, key)) {
            return reinterpret_cast<uintptr_t>(&g_namedObjects[index]);
        }
    }
    if (g_namedObjectCount >= kMaxNamedObjects) {
        // Table full: fall back to the shared sentinel rather than hand out a
        // duplicate, so identity stays consistent with the earlier request.
        return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
    }
    FakeNamedObject& object = g_namedObjects[g_namedObjectCount++];
    object.magic = kNamedObjectMagic;
    rawCopy(object.key, sizeof(object.key), key);
    return reinterpret_cast<uintptr_t>(&object);
}

// First variadic JNI argument, read only when it is one of our own string
// handles.
//
// AArch64 va_list is a struct, so `args` is a pointer to the caller's va_list
// and raw `*args` dereference would read gp_offset, not the argument - the
// same va_list* / va_arg(*vap, ...) shape loadTextureFromMemoryArgs uses.
// jniStringValue matches against known handles and returns "" for anything
// else, so a foreign pointer is never dereferenced; the caller gets nullptr
// and the shared sentinel answer.
const char* firstStringArg(void* args, uintptr_t* outHandle) {
    if (outHandle != nullptr) {
        *outHandle = 0;
    }
    if (args == nullptr) {
        return nullptr;
    }
    va_list* vap = reinterpret_cast<va_list*>(args);
    const uintptr_t handle = reinterpret_cast<uintptr_t>(va_arg(*vap, void*));
    if (outHandle != nullptr) {
        *outHandle = handle;
    }
    if (handle == 0) {
        return nullptr;
    }
    const char* value = jniStringValue(handle);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

// getComponent(...) and loadClass(String) get a distinct object per requested
// name. Returns 0 when the method is not one of those, so the caller keeps its
// existing per-method behaviour. outArg carries the raw first argument so the
// log can show what the guest actually passed.
//
// loadClass takes a String, and the requested class name resolves cleanly.
// getComponent does NOT: the argument observed on device is 0x0 / 0x10 / 0x63
// and two host-range values, i.e. an id or handle rather than a jstring. So the
// key is the resolved name when there is one and the raw argument otherwise.
// Keying on the raw value is what makes identity a faithful function of the
// request without having to guess getComponent's real signature; its one
// limitation is that two components that pass the same argument share an
// object, which is the same thing the old constant did but no worse.
uintptr_t jniNamedObjectForMethod(uintptr_t methodId, void* args,
                                  uintptr_t* outArg) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return 0;
    }
    if (!rawEquals(name, "getComponent") && !rawEquals(name, "GetComponent") &&
        !rawEquals(name, "loadClass")) {
        return 0;
    }
    uintptr_t handle = 0;
    const char* text = firstStringArg(args, &handle);
    if (outArg != nullptr) {
        *outArg = handle;
    }
    // Method name in the key so a loadClass and a getComponent that happen to
    // pass the same argument never alias each other's object.
    char key[96];
    if (text != nullptr) {
        __builtin_snprintf(key, sizeof(key), "%s:%s", name, text);
    }
    else {
        __builtin_snprintf(key, sizeof(key), "%s:arg:%llx", name,
                           (unsigned long long)handle);
    }
    return namedObjectFor(key);
}

// Switch for the per-call entry/exit pair added 2026-10-05 on three bridge
// sites. Each marker is a file write on a path the guest hits in a loop, so it
// is the one piece of this session's instrumentation that can plausibly cost
// the guest measurable time -- and the PC sampler then spends its samples
// inside this port's own name lookup instead of in the guest. Default OFF,
// because the pair did not exist before tonight and the pre-investigation
// behaviour is the correct default. Turn it on when a run needs the names.
#ifndef RR3_JNI_PAIR_LOG
#define RR3_JNI_PAIR_LOG 0
#endif

// A/B switch for the shared dummy handle. 0 = one object for every call (the
// shipped behaviour), 1 = a distinct object per call. See fakeObjectHandle().
#ifndef RR3_FRESH_FAKE_OBJECT
#define RR3_FRESH_FAKE_OBJECT 0
#endif

// The handle handed back for a method that must yield an object but whose real
// result this port does not model.
//
// WHY THIS IS BEING A/B'D. The guest's container destructor at 0xcd2abc walks a
// node chain with `cursor -= 0x30` and deletes each node, exiting only when the
// cursor equals the sentinel it started from. In a 10-minute run the PC sampler
// put it at 0xcd2abc five times and at 0xcd2ad0 three times, and those were the
// only two distinct guest addresses in the whole run: **that loop is the spin**.
//
// This port returns ONE `char g_fakeObjectObject{}` for getComponent,
// getCustomProperties, keySet, iterator and put. In the same run getComponent
// alone was called 16 times and every call returned the same address. A single
// handle inserted into a container once per call is a self-referential node, and
// a walk that never meets its end pointer never terminates.
//
// The comment this replaces argued that "the shared sentinel is the right answer
// (one object, not a per-call fresh one)". That argument is now the prime
// suspect, so it gets tested rather than defended. The default stays 0 until a
// run shows which shape the guest survives.
uintptr_t fakeObjectHandle(const char* name) {
    (void)name;
#if RR3_FRESH_FAKE_OBJECT
    // 0x40 of stride, NOT 1: two pool entries must never sit exactly 0x30
    // apart, or the A/B would manufacture a chain that looks valid.
    struct Slot { char pad[0x40]; };
    static Slot pool[256];
    static std::atomic<unsigned> next{0};
    const unsigned i = next.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<uintptr_t>(&pool[i % 256]);
#else
    return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
#endif
}

const char* platformStringForMethod(const char* name) {
    if (name == nullptr) {
        return "";
    }
    if (rawEquals(name, "getDeviceUID")) return "switch-rr3-native";
    if (rawEquals(name, "GetDeviceUID")) return "switch-rr3-native";
    if (rawEquals(name, "getModelID")) return "Nintendo Switch";
    if (rawEquals(name, "getModelName")) return "Nintendo Switch";
    if (rawEquals(name, "GetDeviceModel")) return "Nintendo Switch";
    if (rawEquals(name, "GetDeviceHardware")) return "Nintendo Switch";
    if (rawEquals(name, "getBuildDevice")) return "nx";
    if (rawEquals(name, "getBuildManufacturer")) return "Nintendo";
    if (rawEquals(name, "getScreenDensityName")) return "mdpi";
    // Platform.getScreenSizeName() maps Configuration.screenLayout size bits
    // to small/normal/large/xlarge. Switch (1280x720) is Android "large".
    if (rawEquals(name, "getScreenSizeName")) return "large";
    if (rawEquals(name, "getSupportedAbis")) return "arm64-v8a";
    if (rawEquals(name, "getCpuName")) return "ARM Cortex-A57";
    if (rawEquals(name, "getPackageName")) return "com.ea.games.r3_row";
    if (rawEquals(name, "GetPackageName")) return "com.ea.games.r3_row";
    if (rawEquals(name, "getAppName")) return "Real Racing 3";
    if (rawEquals(name, "getAppVersion")) return "14.0.1";
    if (rawEquals(name, "GetBuildVersion")) return "14.0.1";
    if (rawEquals(name, "GetDeviceFirmwareVersion")) return "Horizon";
    if (rawEquals(name, "GetDeviceMacAddress")) return "02:00:00:00:00:00";
    if (rawEquals(name, "GetAdvertisingID")) return "";
    // Amazon-only advertising id; offline Switch build has none.
    if (rawEquals(name, "getAmazonAdvertisingId")) return "";
    if (rawEquals(name, "GetLocalIp")) return "0.0.0.0";
    if (rawEquals(name, "getAppPath")) return "sdmc:/switch/rr3-native-rehost";
    if (rawEquals(name, "getOsVersion")) return "Horizon";
    // getExternalStorageState is an int method (mounted閳?) 閳?see
    // platformIntForMethod; do not answer it from the string path.
    if (rawEquals(name, "getExternalStorageDir")) return "sdmc:/switch/rr3-native-rehost";
    if (rawEquals(name, "getLocale")) return "zh_CN";
    if (rawEquals(name, "getCountry")) return "CN";
    if (rawEquals(name, "getMobileDataType")) return "";
    if (rawEquals(name, "GetWifiSSID")) return "";
    if (rawEquals(name, "getCarrier")) return "";
    if (rawEquals(name, "getCarrierName")) return "";
    if (rawEquals(name, "getCloudcellEnvironment")) return "switch";
    if (rawEquals(name, "getStarlightEnvironment")) return "switch";
    // A/B 2026-10-04, RESULT NULL. Archive 20261004-181329-remotecfg against
    // 20261004-175843-opensize: every counter identical.
    //
    //                        baseline   "{}" returned
    //   Corrupted JSON config      1              1
    //   startMusic disabled       2              2
    //   LoadReadOnlyMappedFile    6              6
    //   stat ENOENT            1133           1133
    //   mmap ok                 124            124
    //
    // The hypothesis was that this getter feeds the guest's JSON config, because
    // the csdk.* keys it looks up ARE remote-config keys. It is not: the method
    // is either never called on this path or its result is discarded. Back to
    // "", which is the pre-investigation behaviour.
    //
    // What survives from this: the guest's config schema is fully recovered
    // (93 csdk.* keys, from a csdk\.[A-Za-z0-9_.]+ scan of the image, including
    // a complete disabled.* family that is the honest offline answer), and the
    // failure is definitely NOT an empty file and NOT a JNI string we control.
    // Finding the actual source needs the guest's PC, which Eden will not give.
    if (rawEquals(name, "GetStringValueFromRemoteConfig")) return "";
    // Offline: no EA Tracking/Synergy component. Returning a non-null fake
    // made Java call methods on a sentinel and stall cloudcell init.
    if (rawEquals(name, "getComponent")) return nullptr;
    if (rawEquals(name, "GetEADeviceId")) return "";
    if (rawEquals(name, "GetSynergyId")) return "";
    return "";
}

int32_t platformIntForMethod(const char* name) {
    if (name == nullptr) return 0;
    if (rawEquals(name, "getCpuCount")) return 4;
    if (rawEquals(name, "getCpuSpeed")) return 1020;
    if (rawEquals(name, "getApiLevel")) return 35;
    if (rawEquals(name, "getSystemMediaVolume")) return 80;
    if (rawEquals(name, "getAppInstallTime")) return 0;
    if (rawEquals(name, "getScreenRotation")) return 0;
    // ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE 閳?Switch default layout.
    if (rawEquals(name, "getScreenOrientation")) return 2;
    if (rawEquals(name, "getAppMemoryUsage")) return 256 * 1024 * 1024;
    if (rawEquals(name, "getAppMemoryFree")) return 1536 * 1024 * 1024;
    if (rawEquals(name, "getScreenWidth")) return 1280;
    if (rawEquals(name, "getScreenHeight")) return 720;
    if (rawEquals(name, "getScreenDPI")) return 160;
    // Platform.getScreenSize(): Configuration.screenLayout & 15.
    // 3 == Configuration.SCREENLAYOUT_SIZE_LARGE (matches getScreenSizeName).
    if (rawEquals(name, "getScreenSize")) return 3;
    // Platform.getExternalStorageState(): 2 = Environment.MEDIA_MOUNTED,
    // 1 = mounted_ro, 0 = anything else. Host has mounted storage.
    if (rawEquals(name, "getExternalStorageState")) return 2;
    if (rawEquals(name, "getTotalMemory")) return 2047 * 1024 * 1024;
    if (rawEquals(name, "GetAudioFramesPerBuffer")) return 1024;
    if (rawEquals(name, "GetAudioSampleRate")) return 48000;
    if (rawEquals(name, "getGameState")) return g_gameState;
    return 0;
}

bool platformBoolForMethod(const char* name) {
    if (name == nullptr) return false;
    if (rawEquals(name, "isNotificationPermissionGranted")) return true;
    // Host owns the framebuffer continuously while the guest is running.
    if (rawEquals(name, "IsInForeground")) return true;
    if (rawEquals(name, "isInForeground")) return true;
    // Treat cloudcell local data as available so the post-hideSplash path
    // can finish and request menu assets.
    if (rawEquals(name, "isDataAvailable")) return true;
    // Empty Java collections: HashMap.keySet().iterator() must terminate.
    if (rawEquals(name, "hasNext")) return false;
    if (rawEquals(name, "isEmpty")) return true;
    if (rawEquals(name, "isNetworkAvailable")) return false;
    if (rawEquals(name, "isMobileDataAvailable")) return false;
    if (rawEquals(name, "isNetworkSettingsShown")) return false;
    if (rawEquals(name, "hasCameraPermission")) return false;
    // extractRes is boolean in Platform.java; assets already live on sdmc.
    if (rawEquals(name, "extractRes")) return true;
    return false;
}

uintptr_t jniObjectForMethod(uintptr_t methodId) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
    }
    // Fallback for methods jniNamedObjectForMethod did not claim, and for any
    // call path that has no argument list to read a requested name from. Stable
    // dummy: native C++ stores the handle and calls void methods on it
    // (setThresholdLevel/writeWithTitle). Returning null made the C++ cloudcell
    // path skip work that gates the menu asset load.
    //
    // The name-keyed getComponent/loadClass case is intercepted before we get
    // here on both CallObjectMethodV and CallStaticObjectMethodV; the branch
    // below only fires if a name was missing or unparseable, where the shared
    // sentinel is the right answer (one object, not a per-call fresh one).
    if (rawEquals(name, "getComponent") || rawEquals(name, "GetComponent")) {
        return fakeObjectHandle(name);
    }
    if (rawEquals(name, "createAnalytics") || rawEquals(name, "createGluAds") ||
        rawEquals(name, "createCustomerSupport") ||
        rawEquals(name, "getClassLoader") || rawEquals(name, "loadClass") ||
        rawEquals(name, "getCustomProperties") || rawEquals(name, "keySet") ||
        rawEquals(name, "iterator") || rawEquals(name, "put")) {
        return fakeObjectHandle(name);
    }
    if (jniMethodReturnsString(methodId)) {
        return makeJniString(platformStringForMethod(name));
    }
    // The catch-all: every object-returning method this port does not recognise
    // currently hands the guest the ONE shared sentinel &g_fakeObjectObject.
    //
    // That default is already known to be hazardous, and the same file records
    // why: the getComponent case above was changed to return nullptr because
    // "returning a non-null fake made Java call methods on a sentinel and stall
    // cloudcell init". A shared sentinel is worse than a per-call fresh one,
    // because three different services (createAnalytics, createGluAds,
    // createCustomerSupport) all receive the SAME object and the guest cannot
    // tell them apart.
    //
    // A/B 2026-10-04, RESULT NULL. Archive 20261004-182328-nullobj against
    // 20261004-181653-opensize-clean:
    //
    //                        baseline   null returned
    //   Corrupted JSON config      1              1
    //   startMusic disabled       2              2
    //   LoadReadOnlyMappedFile    6              6
    //   mmap ok                 124            124
    //   glShaderSource           92             92
    //   jni-static-obj           29             29
    //   stat-path              1261           1260
    //
    // So the shared sentinel is not what stalls the guest either, and the
    // getComponent precedent does NOT generalise to the catch-all. Kept at 0,
    // which is the pre-investigation behaviour.
    //
    // That is the third consecutive null result on "something we hand the guest
    // during init is wrong" -- after the zero-byte stubs and after
    // GetStringValueFromRemoteConfig. Taken together they say the guest's
    // "downloading" state and its config parse failure are internal to the
    // guest, not caused by anything the host returns. The remaining
    // discriminators are the guest's own PC and what real hardware draws, and
    // Eden supplies neither.
#ifndef RR3_NULL_UNKNOWN_OBJECTS
#define RR3_NULL_UNKNOWN_OBJECTS 0
#endif
    if (RR3_NULL_UNKNOWN_OBJECTS != 0) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
}

uintptr_t jniGetVersion() {
    return 0x00010006;  // JNI_VERSION_1_6
}

uintptr_t jniReturnZero() {
    return 0;
}

uintptr_t jniNoop() {
    return 0;
}

uintptr_t jniFindClass(uintptr_t /*env*/, const char* name) {
    rawLog("[jni] FindClass ");
    rawLog(name);
    rawLog("\n");
    return classHandleFor(name);
}

uintptr_t jniGetObjectClass(uintptr_t /*env*/, uintptr_t object) {
    // A Font receiver reports the Font class the guest itself named, so a
    // GetObjectClass -> GetFieldID round trip binds to the same class record
    // the FindClass did. Everything else keeps the shared sentinel.
    if (asFontObject(object) != nullptr) {
        const uintptr_t fontClass = fontClassHandle();
        if (fontClass != 0) {
            return fontClass;
        }
    }
    return reinterpret_cast<uintptr_t>(&g_fakeClassObject);
}

uintptr_t jniGetMethodID(uintptr_t /*env*/, uintptr_t clazz,
                         const char* name, const char* signature) {
    rawLog("[jni] GetMethodID ");
    rawLog(name);
    rawLog("\n");
    ++g_jniMethodBindings;
    return rememberJniMethod(clazz, name, signature, false);
}

uintptr_t jniGetStaticMethodID(uintptr_t /*env*/, uintptr_t clazz,
                               const char* name, const char* signature) {
    rawLog("[jni] GetStaticMethodID ");
    rawLog(name);
    rawLog("\n");
    ++g_jniMethodBindings;
    return rememberJniMethod(clazz, name, signature, true);
}

uintptr_t jniGetFieldID(uintptr_t /*env*/, uintptr_t clazz,
                        const char* name, const char* /*signature*/) {
    rawLog("[jni] GetFieldID ");
    rawLog(name);
    rawLog("\n");
    return rememberJniField(clazz, name);
}

int32_t jniGetIntField(uintptr_t /*env*/, uintptr_t object, uintptr_t fieldId) {
    if (object == 0) {
        return 0;
    }
    // Font first: the int metrics are bmpLeft/bmpTop/bmpWidth/bmpHeight/
    // bmpPitch, and the tag keeps this disjoint from the TextureInfo branch.
    FakeFont* font = asFontObject(object);
    if (font != nullptr) {
        return fontIntField(font, jniFieldName(fieldId));
    }
    const FakeTextureInfo* info =
        reinterpret_cast<const FakeTextureInfo*>(object);
    if (info->magic == kTextureInfoMagic) {
        const char* name = jniFieldName(fieldId);
        if (name == nullptr) return 0;
        if (rawEquals(name, "texId")) return info->texId;
        if (rawEquals(name, "width")) return info->width;
        if (rawEquals(name, "height")) return info->height;
        if (rawEquals(name, "texWidth")) return info->texWidth;
        if (rawEquals(name, "texHeight")) return info->texHeight;
    }
    return 0;
}

// GetFloatField (slot 102). This slot had no implementation at all, so all
// eleven of Font's float fields (ascent, descent, height, top, bottom, leading,
// glyphOffX, glyphOffY, glyphWidth, glyphHeight, glyphAdvance) were being read
// through jniSlotStub and answering 0.0f. Font.java declares every one of them
// float, so they cannot be served by GetIntField; the slot has to exist for the
// object to be self-consistent at all.
float jniGetFloatField(uintptr_t /*env*/, uintptr_t object, uintptr_t fieldId) {
    FakeFont* font = asFontObject(object);
    if (font == nullptr) {
        return 0.f;
    }
    return fontFloatField(font, jniFieldName(fieldId));
}

uintptr_t jniNewGlobalRef(uintptr_t /*env*/, uintptr_t object) {
    return object;  // sentinels are immortal; passing them through is safe
}

uintptr_t jniNewLocalRef(uintptr_t /*env*/, uintptr_t object) {
    return object;
}

uintptr_t jniIsSameObject(uintptr_t /*env*/, uintptr_t a, uintptr_t b) {
    return a == b ? 1u : 0u;
}

uintptr_t jniNewStringUTF(uintptr_t /*env*/, const char* bytes) {
    rawLog("[jni] NewStringUTF ");
    rawLog(bytes);
    rawLog("\n");
    return makeJniString(bytes);
}

uintptr_t jniGetStringUTFLength(uintptr_t /*env*/, uintptr_t string) {
    return static_cast<uintptr_t>(rawStrlen(jniStringValue(string)));
}

uintptr_t jniGetStringUTFChars(uintptr_t /*env*/, uintptr_t string,
                               uintptr_t /*isCopy*/) {
    return reinterpret_cast<uintptr_t>(jniStringValue(string));
}

uintptr_t jniNewObjectV(uintptr_t /*env*/, uintptr_t clazz,
                        uintptr_t /*methodId*/, void* /*args*/) {
    // The one class the guest constructs and then calls instance methods on.
    // Everything else keeps the shared sentinel.
    const uintptr_t fontClass = fontClassHandle();
    if (fontClass != 0 && clazz == fontClass) {
        g_fakeFont.initialised = 0;
        g_fakeFont.bmpData = 0;  // re-made on demand, pool may have recycled
        return reinterpret_cast<uintptr_t>(&g_fakeFont);
    }
    return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
}

// Generic entry/exit marker for the other two object-calling bridge sites, so
// the whole triple {CallObjectMethodV, CallStaticObjectMethodV, CallVoidMethodV}
// is covered. Added 2026-10-05 after the run named getCpuName as the last
// static call AND showed it returning: the census reported `enters=1199
// exited=0`, and only these three handlers call censusNoteImportFrom, so the
// LR ring -- which the wedge attribution rests on -- can be stale for any
// handler that does not report. A marker on entry and one on exit is the only
// way to tell "this call hung" from "the call after it never happened".
static constexpr unsigned kJniPairBudget = 4096;
static std::atomic<unsigned> g_jniPairEntryBudget{kJniPairBudget};
static std::atomic<unsigned> g_jniPairExitBudget{kJniPairBudget};

// Pair marks are gated behind RR3_JNI_PAIR_LOG because each is a file write on
// a path the guest hits in a loop, and the PC sampler then spends its samples
// inside this port's own name lookup instead of in the guest. Default OFF: the
// marks did not exist before 2026-10-05 and the pre-investigation behaviour is
// the correct default.
#ifndef RR3_JNI_PAIR_LOG
#define RR3_JNI_PAIR_LOG 0
#endif

// `phase` is compared in FULL. The first version compared only phase[0], and
// "enter" and "exit" both start with 'e' -- so every exit was labelled
// "jni-obj-enter", the exit budget was never selected, and the pair-log totals
// read 62 enters / 0 exits. That looked exactly like a handler that never
// returns, and it was this file's own bug, not a fact about the guest
// (archive 20261005-085156-nopairlog). A label that cannot be wrong is a label
// nobody has to re-derive.
void jniPairMark(const char* phase, const char* name, uintptr_t methodId,
                 uintptr_t value) {
#if !RR3_JNI_PAIR_LOG
    (void)phase; (void)name; (void)methodId; (void)value;
#else
    static constexpr unsigned kJniPairBudget = 4096;
    static std::atomic<unsigned> g_jniPairEnter{kJniPairBudget};
    static std::atomic<unsigned> g_jniPairExit{kJniPairBudget};
    const bool entering = phase[0] == 'e' && phase[1] == 'n';
    std::atomic<unsigned>& budget = entering ? g_jniPairEnter : g_jniPairExit;
    const bool ok = budget.fetch_sub(1, std::memory_order_relaxed) != 0;
    char line[176];
    __builtin_snprintf(line, sizeof(line), "site=%s method=%s mid=0x%llx v=0x%llx",
                       phase, name != nullptr ? name : "<null>",
                       (unsigned long long)methodId, (unsigned long long)value);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        ok ? (entering ? "jni-obj-enter" : "jni-obj-exit")
                           : (entering ? "jni-obj-enter-CAPPED"
                                      : "jni-obj-exit-CAPPED"),
                        0, static_cast<unsigned long long>(methodId), line);
#endif
}

uintptr_t jniCallObjectMethodV(uintptr_t /*env*/, uintptr_t /*object*/,
                               uintptr_t methodId, void* args) {
    const char* name = jniMethodName(methodId);
    const bool returnsString = jniMethodReturnsString(methodId);
    jniPairMark("enter", name, methodId, 0);
    uintptr_t rawArg = 0;
    uintptr_t result = jniNamedObjectForMethod(methodId, args, &rawArg);
    const bool named = result != 0;
    if (!named) {
        result = jniObjectForMethod(methodId);
    }
    ::rr3::censusNoteImportFrom(__builtin_return_address(0), "jni:CallObjectMethod");
    jniPairMark("exit", name, methodId, result);
    // The census call above is the cheaper per-call tracer and it stays: it is a
    // per-thread ring, no SD card, and it is what attributes a wedge to this
    // slot. The file line below only added the name/shape payload, so it is
    // what gets the shared budget -- and the snprintf with it, since formatting
    // the details is itself per-call work on a route the guest hits in a loop.
    if (name != nullptr && perCallLogAllow("jni-obj")) {
        const char* objectKey = namedObjectKey(result);
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "method=%s ret=%s obj=0x%llx arg=0x%llx%s%s", name,
                           jniShape(result, returnsString),
                           (unsigned long long)result, (unsigned long long)rawArg,
                           objectKey != nullptr ? " name=" : "",
                           objectKey != nullptr ? objectKey : "");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "jni-obj", 0, static_cast<unsigned long long>(methodId),
                            details);
    }
    return result;
}

uintptr_t jniCallStaticObjectMethodV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                                     uintptr_t methodId, void* args) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return jniObjectForMethod(methodId);
    }
    // Full name dump: one line per call so the next device run shows the real
    // service surface instead of another GL counter. The return SHAPE is now
    // included: a stub handing back a dummy where the engine wanted a real
    // Surface/Context/window handle is the failure this has to make visible.
    // Bounded by the shared per-call budget above: the first kPerCallLogBudget
    // such calls across this file are written, the rest are counted, and
    // phase=per-call-log-cap says the cap engaged. The comment above used to
    // claim "one line per unique static call" while the code wrote one line per
    // call of any name; the budget is what actually makes it unique-ish.
    const bool returnsString = jniMethodReturnsString(methodId);

    // ENTRY LOG -- own budget, on purpose, and this is not redundancy.
    //
    // The exit log at the bottom of this function is written AFTER the
    // dispatch, so a call that never returns writes nothing, and that is
    // exactly the call whose name we need. Measured 2026-10-05, run
    // 20261005-030220-second-render: the main thread wedged inside a
    // CallStaticObjectMethodV, and the only evidence was
    //     phase=per-call-log-cap site=jni-static-obj suppressed_total=1
    // Exactly one suppressed call -- the one that hung. The instrument deleted
    // the answer at the moment it mattered, which is the second time this
    // project has lost a diagnosis to a log budget (the first was the
    // 200-line guest log cap, raised to 4000 on 2026-10-04; same bug, different
    // counter). The guest wrapper at 0xba83fc has 45 direct callers, so static
    // attribution is hopeless and the runtime name is the only instrument.
    //
    // Hence: the name goes in on the way IN, from a counter that is not shared
    // with the exit log, and exhaustion is announced on a line of its own
    // rather than by dropping the name silently.
    static constexpr unsigned kJniStaticEnterBudget = 4096;
    static std::atomic<unsigned> g_jniStaticEnterBudget{kJniStaticEnterBudget};
    {
        const bool enterOk =
            g_jniStaticEnterBudget.fetch_sub(1, std::memory_order_relaxed) != 0;
        char enter[176];
        __builtin_snprintf(enter, sizeof(enter),
                           "method=%s mid=0x%llx returns=%s budget=%u", name,
                           (unsigned long long)methodId,
                           returnsString ? "string" : "object",
                           kJniStaticEnterBudget);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            enterOk ? "jni-static-enter" : "jni-static-enter-CAPPED",
                            0, static_cast<unsigned long long>(methodId), enter);
    }

    uintptr_t result;
    uintptr_t rawArg = 0;
    if (rawEquals(name, "loadTextureFromMemory")) {
        result = loadTextureFromMemoryArgs(args);
        rawArg = 0xffffffffffffffffull;
    }
    else {
        result = jniNamedObjectForMethod(methodId, args, &rawArg);
        if (result == 0) {
            result = jniObjectForMethod(methodId);
            rawArg = 0xffffffffffffffffull;
        }
    }
    ::rr3::censusNoteImportFrom(__builtin_return_address(0), "jni:CallStaticObjectMethod");

    // EXIT MARKER, own budget again. The entry log above says a call started;
    // this says it finished. Without the pair, "the last method named was X"
    // cannot be distinguished from "the call for X is the one that hung", which
    // is the whole question. Cheap, and it is the difference between naming the
    // culprit and naming its victim.
    {
        static constexpr unsigned kJniStaticExitBudget = 4096;
        static std::atomic<unsigned> g_jniStaticExitBudget{kJniStaticExitBudget};
        const bool exitOk =
            g_jniStaticExitBudget.fetch_sub(1, std::memory_order_relaxed) != 0;
        char exitLine[160];
        __builtin_snprintf(exitLine, sizeof(exitLine),
                           "method=%s mid=0x%llx ret=0x%llx", name,
                           (unsigned long long)methodId,
                           (unsigned long long)result);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            exitOk ? "jni-static-exit" : "jni-static-exit-CAPPED",
                            0, static_cast<unsigned long long>(methodId), exitLine);
    }

    if (!perCallLogAllow("jni-static-obj")) {
        return result;
    }
    const char* objectKey = namedObjectKey(result);
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "method=%s ret=%s obj=0x%llx arg=0x%llx%s%s", name,
                       jniShape(result, returnsString),
                       (unsigned long long)result, (unsigned long long)rawArg,
                       objectKey != nullptr ? " name=" : "",
                       objectKey != nullptr ? objectKey : "");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "jni-static-obj", 0,
                        static_cast<unsigned long long>(methodId), details);
    return result;
}

// Non-V CallStaticObjectMethod (slot 114). Guest C++ often calls the
// non-variadic form through a function pointer; missing it left
// loadTextureFromMemory unanswered even after the V path existed.
uintptr_t jniCallStaticObjectMethod(uintptr_t env, uintptr_t clazz,
                                    uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    uintptr_t result = jniCallStaticObjectMethodV(env, clazz, methodId,
                                                  &arguments);
    va_end(arguments);
    return result;
}

uintptr_t jniCallObjectMethod(uintptr_t env, uintptr_t object,
                              uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    uintptr_t result = jniCallObjectMethodV(env, object, methodId, &arguments);
    va_end(arguments);
    return result;
}

// Non-V CallLongMethod (slot 52). No CallLongMethodV yet; answer 0.
int64_t jniCallLongMethod(uintptr_t /*env*/, uintptr_t /*object*/,
                          uintptr_t /*methodId*/, ...) {
    return 0;
}

// Non-V CallFloatMethod (slot 55). getSize() on a Font reports the em size the
// metrics above were derived at; every other float method still answers 0.f.
float jniCallFloatMethod(uintptr_t /*env*/, uintptr_t object,
                         uintptr_t methodId, ...) {
    FakeFont* font = asFontObject(object);
    if (font != nullptr) {
        const char* name = jniMethodName(methodId);
        if (rawEquals(name, "getSize")) {
            return kFontSizeX100 / 100.0f;
        }
    }
    return 0.f;
}

// Defined further down, next to the static-void gate tracer; declared here so
// the scalar call paths below can use the same name filter and cap.
bool gateScalarNameInteresting(const char* name);
void gateTraceScalar(const char* kind, const char* name, long long result,
                     uintptr_t methodId);

int32_t jniCallBooleanMethodV(uintptr_t /*env*/, uintptr_t object,
                              uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    // Scoped by the receiver, not by the name. init / loadGlyph / loadBitmap /
    // loadBitmapDynamic are ordinary method names that other classes may also
    // declare, and platformBoolForMethod has no receiver to scope on (it is
    // shared with the static path, where there is no receiver at all). So the
    // name table below is left exactly as it was and only a Font receiver gets
    // the new answers. init returning false is what made the guest retry every
    // frame: Font.java bails out at :53 and :65 when the typeface is null, and
    // all three load* methods bail at :93, :114 and :150 on a null
    // defaultPaint, which only init() ever creates.
    FakeFont* font = asFontObject(object);
    if (font != nullptr && fontBoolMethod(name)) {
        if (rawEquals(name, "init")) {
            font->initialised = 1;
            fontNoteInit();
        }
        else if (rawEquals(name, "loadGlyph")) {
            fontNoteGlyph();
        }
        else {
            // loadBitmap / loadBitmapDynamic: the byte[] is made here rather
            // than in GetObjectField so the first bitmap call is the one that
            // logs, and so the summary can report a real length. loadBitmap is
            // called once per glyph, so the line is one-shot like the others.
            const uintptr_t bmpData = fontBmpData(font);
            if (!g_fontLoggedBitmap) {
                g_fontLoggedBitmap = true;
                char details[160];
                __builtin_snprintf(details, sizeof(details),
                                   "loadBitmap -> true left=%d top=%d w=%d h=%d "
                                   "pitch=%d bytes=%d",
                                   kFontBmpLeft, kFontBmpTop, kFontBmpWidth,
                                   kFontBmpHeight, kFontBmpPitch, kFontBmpBytes);
                rawLog("[jni] Font ");
                rawLog(details);
                rawLog("\n");
            }
            fontNoteSummary(bmpData);
        }
        return 1;
    }
    if (name != nullptr && gateScalarNameInteresting(name)) {
        gateTraceScalar("gate-obj-bool", name, platformBoolForMethod(name), methodId);
    }
    return platformBoolForMethod(name) ? 1u : 0u;
}

// Non-V CallBooleanMethod (slot 37). Guest C++ often calls the non-variadic
// form through a function pointer; wrap the V path.
int32_t jniCallBooleanMethod(uintptr_t env, uintptr_t object,
                             uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    int32_t result = jniCallBooleanMethodV(env, object, methodId, &arguments);
    va_end(arguments);
    return result;
}

int32_t jniCallIntMethodV(uintptr_t /*env*/, uintptr_t /*object*/,
                          uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    const int32_t result = platformIntForMethod(name);
    if (name != nullptr && gateScalarNameInteresting(name)) {
        gateTraceScalar("gate-obj-int", name, result, methodId);
    }
    return result;
}

// Non-V CallIntMethod (slot 49). Same va_list wrap as CallObjectMethod.
int32_t jniCallIntMethod(uintptr_t env, uintptr_t object,
                         uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    int32_t result = jniCallIntMethodV(env, object, methodId, &arguments);
    va_end(arguments);
    return result;
}

int32_t jniCallStaticIntMethodV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                                uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    const int32_t result = platformIntForMethod(name);
    if (name != nullptr && gateScalarNameInteresting(name)) {
        gateTraceScalar("gate-static-int", name, result, methodId);
    }
    return result;
}

// Non-V CallStaticIntMethod (slot 129). Wrap the V path.
int32_t jniCallStaticIntMethod(uintptr_t env, uintptr_t clazz,
                               uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    int32_t result = jniCallStaticIntMethodV(env, clazz, methodId, &arguments);
    va_end(arguments);
    return result;
}

// ---- gate instrumentation (observation only; no behaviour change) ----------
//
// The freeze sits immediately after the JNI sequence that ends at getAppPath,
// and the question under test is whether the guest reached a Platform alert /
// download-consent gate that this faked layer can never answer. Three of the
// JNI slots that carry that traffic had no logging at all, so "showMessage was
// not called" was unfalsifiable:
//
//   jniCallStaticVoidMethodV    - Platform.showMessage / exitApp / openURL
//   jniCallStaticBooleanMethodV - isNetworkAvailable / isMobileDataAvailable
//                                 / isNetworkSettingsShown
//   jniCallStaticIntMethodV      - state getters behind the same gate
//
// Each is now logged with the guest return address, so a call that happens can
// be tied to a rr3+offset and compared against the string table offline.
// showMessage additionally gets its whole argument tuple: the five label
// strings and the three callback ids that ndPlatformJNI::addAlertMessage hands
// out from a global counter. The id is what a real UI thread would feed back
// into MainActivity.alertMessageExecuteCallback, so it is the only part that
// could ever unblock the guest.
//
// Bounded: the counters below cap the log so a spin inside these paths cannot
// flood the SD card.
constexpr unsigned kGateMaxLines = 24;
unsigned g_gateStaticVoidLines = 0;
unsigned g_gateStaticScalarLines = 0;
unsigned g_gateShowMessageLines = 0;

// Name filter for the scalar slots. These are called in tight init loops, so
// only the ones that can carry a gate are worth a line each.
bool gateScalarNameInteresting(const char* name) {
    return rawEquals(name, "isNetworkAvailable") ||
           rawEquals(name, "isMobileDataAvailable") ||
           rawEquals(name, "isNetworkSettingsShown") ||
           rawEquals(name, "isDataAvailable") ||
           rawEquals(name, "hasConsent") ||
           rawEquals(name, "getDownloadConsentState") ||
           rawEquals(name, "isLegalUsageEnabled") ||
           rawEquals(name, "getConsentStatus");
}

// One line for a scalar call the engine uses to probe the gate. The answer is
// printed next to the name because the answer IS the interesting part: every
// value here comes from platformBoolForMethod/platformIntForMethod, so a
// "false" that was hard-coded rather than measured shows up directly.
void gateTraceScalar(const char* kind, const char* name, long long result,
                     uintptr_t methodId) {
    if (g_gateStaticScalarLines >= kGateMaxLines) {
        return;
    }
    ++g_gateStaticScalarLines;
    const uintptr_t bias = resolveMainImageLoadBias();
    const uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    char details[192];
    __builtin_snprintf(details, sizeof(details), "method=%s -> %lld ra=rr3+%llx", name,
                       result, (unsigned long long)(ra > bias ? ra - bias : ra));
    guestViewWideLog(kind, static_cast<unsigned long long>(methodId),
                     (unsigned long long)ra, details);
}

// Platform.showMessage(title, body, positive, negative, neutral,
//                      idPositive, idNegative, idNeutral)V
//
// Returns the parsed call so the caller can answer the dialog. The va_arg walk
// happens before the log-budget check and exactly once, for two reasons: the
// enqueue below needs the same three ids, and a second walk over the same
// va_list would run off the end of it; and the answer must not be gated by the
// log budget - the trace is best-effort, the answer is not.
//
// The three ids are 64-bit. Platform.showMessage declares them `long`
// (Platform.java:1423: `..., long j, long j2, long j3`), so each one arrives in
// its own 8-byte AArch64 variadic slot. va_arg(*vap, int) read 4 bytes of an
// 8-byte argument and dropped the high half; the ids seen so far
// (120977816, 131320216) are small enough that the truncation looked correct
// by accident. The slot advance is 8 either way, so widening the read changes
// no offsets - it only stops discarding half the value.
struct ShowMessageCall {
    const char* title = "";
    const char* body = "";
    const char* positive = "";
    const char* negative = "";
    const char* neutral = "";
    int64_t idPositive = 0;
    int64_t idNegative = 0;
    int64_t idNeutral = 0;
    bool parsed = false; // false when args was null: nothing to read, nothing to answer
};

ShowMessageCall gateTraceShowMessage(void* args) {
    ShowMessageCall call{};
    if (args == nullptr) {
        return call;
    }
    va_list* vap = reinterpret_cast<va_list*>(args);
    const uintptr_t h[5] = {
        va_arg(*vap, uintptr_t), va_arg(*vap, uintptr_t), va_arg(*vap, uintptr_t),
        va_arg(*vap, uintptr_t), va_arg(*vap, uintptr_t),
    };
    call.title = jniStringValue(h[0]);
    call.body = jniStringValue(h[1]);
    call.positive = jniStringValue(h[2]);
    call.negative = jniStringValue(h[3]);
    call.neutral = jniStringValue(h[4]);
    call.idPositive = va_arg(*vap, int64_t);
    call.idNegative = va_arg(*vap, int64_t);
    call.idNeutral = va_arg(*vap, int64_t);
    call.parsed = true;

    if (g_gateShowMessageLines < kGateMaxLines) {
        ++g_gateShowMessageLines;
        char details[512];
        // %lld on all four: the ids are int64_t now, and a %d here would
        // truncate the very value this line exists to show.
        __builtin_snprintf(
            details, sizeof(details),
            "title='%s' body='%s' pos='%s' neg='%s' neutral='%s' ids=%lld/%lld/%lld"
            " -> enqueue alertMessageExecuteCallback(%lld)",
            call.title, call.body, call.positive, call.negative, call.neutral,
            static_cast<long long>(call.idPositive),
            static_cast<long long>(call.idNegative),
            static_cast<long long>(call.idNeutral),
            static_cast<long long>(call.idPositive));
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gate-show-message", static_cast<unsigned long long>(call.idPositive),
                            static_cast<unsigned long long>(call.idNegative), details);
    }
    return call;
}

// ---------------------------------------------------------------------------
// Answering the dialog on the path the guest actually uses.
//
// The gate trace above used to be the whole of the handling, which left the
// fatal modal unanswered:
//
//   phase=gate-show-message ordinal=131320216 title='不可恢复的错误'
//     body='加载游戏数据时出错，请重新启动《真实赛车3》。' ids=131320216/0/0
//
// Only the positive button exists, and alertMessageExecuteCallback is the only
// thing that clears the state it is blocking on, so the run had no way out.
// rr3_Platform.cpp:73 already does the right thing - trace, defer, resolve the
// export, answer positive - but the guest never reaches that class table: it
// calls through the JNIEnv vtable implemented in this file. The proof is in
// the log rather than inferred: all five cloudcell_pump: lines of the failing
// run read enqueued_total=0, and platform_show_message is the only enqueuer on
// that dialog's path.
//
// Only the positive id is ever fired. MainActivity.java:205 maps the negative
// button to Platform.exitApp(), so firing it would quit the game - and the
// guest is passing idNegative=0 here, i.e. there is no second button anyway.
//
// Deferred rather than inline, exactly as cloudcell_defer.h requires:
// answering inside showMessage would run the guest's button handler while a
// Cloudcell manager mutex is held and an HttpRequest worker is still
// half-constructed. (A function-local static is the one lock this path can
// touch, and it is taken by whichever host thread drains the queue first -
// never by a guest thread.)
// ---------------------------------------------------------------------------

// Java: public native void alertMessageExecuteCallback(long j) on MainActivity
// (MainActivity.java:156), so the JNI signature is (J)V and the third argument
// arrives as a full 64-bit register. Measured rather than assumed: the export
// at 0x12b3c50 is
//
//     adrp/ldr x8, [..] ; cbz x8, ret
//     adrp/ldr x8, [..] ; mov x1, x2 ; ldr x0, [x8] ; b 0x12b8ed4
//
// `mov x1, x2` is an X-register move - the whole third register, with no w-form
// and no narrowing - and its tail target 0x12b8ed4 is the `cbz x1 ; br x1`
// dispatch thunk, so all 64 bits travel on. Reading the argument as a jint
// dropped the high half; with the ids seen so far (120977816, 131320216) that
// went unnoticed, which is exactly why it has to be fixed rather than watched.
// env/thiz are passed properly anyway.
//
// jlong rather than int64_t to match the JNI-signature spelling used by the
// other shims (rr3_Platform.cpp:188); jni.h:36 defines it as int64_t.
using AlertExecuteFn = void (*)(JNIEnv*, jobject, jlong);

// The real export name, and the single place it is resolved.
//
// The name is the guest's own, not a reconstruction: the exact string
// "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback"
// occupies 0x8bc2..0x8c06 of libRealRacing3.so - it is the name the guest uses
// to register its own natives, and the image carries no other spelling of it.
// The previous spelling here and in rr3_Platform.cpp carried a spurious
// "3", so this resolved NULL on every run and the alert-answering path had
// never executed - the dialog was raised and nothing ever answered it.
//
// One helper, memoised, shared by the deferred callback below and by
// alertCallbackExportSelfTest, so a green self-test is a statement about the
// exact pointer the callback will call rather than about a parallel lookup.
AlertExecuteFn alertExecuteCallback() {
    static AlertExecuteFn fn = cloudcell_native<AlertExecuteFn>(
        "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback");
    return fn;
}

// One-shot startup self-test for the export above.
//
// A wrong name here fails silently by nature: dlsym returns NULL, nothing
// crashes, and the only symptom is a dialog that is raised and never answered -
// the AssetDownloadService freeze documented in rr3_Platform.cpp's header, which
// shows up much later as a stall with no proximate cause. One line at startup
// turns that into a boot-time answer.
//
// Runs from runGuestJniOnLoad, the first guest-facing entry point, after the
// loader has mapped the guest images. Guarded so the whole run costs one
// lookup, and it uses rawLog (a bare svcOutputDebugString) so it stays clear of
// the guest TLS scope the JNI entry points run under.
void alertCallbackExportSelfTest() {
    static bool sDone = false;
    if (sDone) {
        return;
    }
    sDone = true;
    if (alertExecuteCallback() != nullptr) {
        rawLog("alert-callback-selftest: OK resolved "
               "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback "
               "sig=(J)V\n");
    } else {
        rawLog("alert-callback-selftest: FAIL could not resolve "
               "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback "
               "- every alert will stay unanswered\n");
    }
}

// One-shot startup census of the whole Cloudcell callback layer.
//
// alertCallbackExportSelfTest above answers for one symbol. This one answers for
// the layer: every distinct name cloudcell_native<>() is ever asked to resolve,
// counted against the main image. The lookup this replaced resolved against the
// host table and so returned 0 for all of them, silently - a NULL callback skips
// the invocation and reports nothing, so an entire callback layer could be dead
// for the life of the port and the only symptom would be a feature that never
// answers. One number at boot is the difference between that and a fact.
//
// Costs one pass over a fixed list at startup and nothing after. The list lives
// next to the lookup in cloudcell_natives.h so it cannot drift away from the
// call sites. It calls rr3::resolveMainImageExport directly rather than going
// through cloudcell_native<>, so the line measures the symbol table and not some
// wrapper over it. rawLog, as above, to stay clear of the guest TLS scope.
//
// Adds no mutex and no fsync: one bool, one fixed-size buffer, one pass.
void cloudcellNativesResolveSelfTest() {
    static bool sDone = false;
    if (sDone) {
        return;
    }
    sDone = true;
    unsigned count = 0;
    const char *const *names = cloudcellNativeNames(count);
    unsigned resolved = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (rr3::resolveMainImageExport(names[i]) != 0) {
            ++resolved;
        } else {
            // Named individually: a bare "20/25" says the layer is broken but not
            // which callbacks, and the whole failure class here is a name that
            // looks right and is not.
            char line[192];
            __builtin_snprintf(line, sizeof(line),
                               "cloudcell-natives: MISSING %s\n", names[i]);
            rawLog(line);
        }
    }
    char line[96];
    __builtin_snprintf(line, sizeof(line),
                       "cloudcell-natives: %u/%u resolved in the main image\n",
                       resolved, count);
    rawLog(line);
}

// 80 ms: fast enough that the guest's next AssetDownloadService poll sees the
// answer, slow enough that a permanently empty queue costs ~12 wakeups a
// second and nothing else.
constexpr int64_t kAlertPumpIntervalNanos = 80000000;

// 0xEA01: the value libnx's condvarWaitTimeout documents for a wait that expired
// on its own (switch/kernel/condvar.h: "@return Result code (0xEA01 on
// timeout)"). Named so the anomaly test at the wait site reads as the thing it
// is rather than as a magic number, and so a libnx change is one edit here. 0
// means success, i.e. condvarWake actually signalled this thread.
constexpr uint32_t kCondvarWaitTimeoutResult = 0xEA01;

// 0x4000, 16 KB. The working sampler thread is created at 0x8000 (32 KB,
// guest_view_probe.cpp:880) with the same priority, the same core and the
// same Mutex + CondVar + condvarWaitTimeout shape, so the stack is the only
// creation parameter that differs between a thread that demonstrably ticks and
// this one. Deliberately a named constant and deliberately NOT raised: the next
// run is what says whether 16 KB is the cause, and changing it now would
// change the thing being measured. Grep kAlertPumpStackSize for the sites.
constexpr uint32_t kAlertPumpStackSize = 0x4000;

// 0x3B, not the 0x2C this thread used to be created at. libnx's own header
// (switch/kernel/thread.h:37) says 0x3B "is a special priority on cores 0..2
// that enables preemptive multithreading"; 0x2C is only "the usual priority of
// the main thread" and is NOT time-sliced. A long-lived 0x2C thread that is not
// sleeping holds its core and same-or-lower-priority threads wait even while
// the other three cores sit idle. This pump sleeps 80 ms between ticks, so the
// window is narrow, but it is 12 times a second for the life of the process.
// Guest threads are unaffected: libnx's __syscall_thread_create hardcodes
// w5 = 0x3b, so every pthread_create already gets the preemptive priority.
// Deliberately NOT logged: the point of the change is to perturb scheduling as
// little as possible, and a tick that logs 12x/second is itself a scheduler
// cost. Grep kDiagnosticThreadPriority to find the sites.
constexpr int kDiagnosticThreadPriority = 0x3B;

std::atomic<bool> g_alertPumpRunning{false};
std::atomic<bool> g_alertPumpStarted{false};
Thread g_alertPumpThread;

// The 80 ms tick's wait, swapped from svcSleepThread to the pair the detached
// reaper already blocks on (pthread_bridge.cpp:3989, condvarWaitTimeout on
// gThreadsChanged). Same reason, same primitive, so there is nothing new to
// trust: in the 20261004-030501-pump-before run this thread logged
// "alert-pump: started" and never produced a second tick, because
// svcSleepThread on an Eden-created thread does not come back. The condvar
// route is the in-tree precedent, not a new mechanism.
//
// libnx Mutex + CondVar, NOT std::mutex. This thread is host-only -- it is
// created by threadCreate at jni_bridge.cpp:2045 and never runs on a guest call
// path -- so the "no std::mutex on a guest-visible path" rule is not engaged,
// and naming the type here is what keeps that true if this code is moved later.
Mutex g_alertPumpWaitLock{};
CondVar g_alertPumpWait{};

// One-shot, second iteration only, behind the same one-shot pattern the rest of
// this file uses (cloudcellNativesResolveSelfTest's `static bool sDone`). It
// answers one question -- did the wait actually expire and wake this thread a
// second time -- and an interval far from kAlertPumpIntervalNanos means the
// wait returned early or immediately, which is the failure the whole change is
// guarding against.
std::atomic<bool> g_alertPumpWakeReported{false};

// One-shot, FIRST statement of the thread body. file-scope std::atomic plus
// exchange(), deliberately NOT a function-local `static bool` with a non-trivial
// initialiser: that form is guarded by __cxa_guard_acquire, which takes a lock
// on first use, and this flag's whole job is to report that the thread reached
// its first statement at all. A guard would put a lock acquisition in front of
// the first observable statement of a thread that is already suspected of never
// reaching it. g_alertPumpWakeReported above is the same shape for the same
// reason.
std::atomic<bool> g_alertPumpEntryReported{false};

// One-shot, first completed wait, immediately before the drain. NOT a duplicate
// of g_alertPumpWakeReported, which is ticks == 2 and measures the wait: this
// one is ticks == 1, and it exists because reading the body shows the only thing
// between the first wait and the first pump output is cloudcell_pump's own
// std::mutex. Without it, "the first-statement line printed but wake-wakeup did
// not" is still ambiguous between a block before the wait and a block inside
// the drain.
std::atomic<bool> g_alertPumpDrainReported{false};

// One-shot, first iteration, between ++ticks and the first mutexLock. The two
// lines above bracket the wait slice from the outside, but neither sits on its
// near side, so "entered the loop body" and "got as far as the lock" are still
// a single event. This is that boundary, and it is the only line the next run
// needs: thread-body-enter, pre-lock and reached-drain together pin the block
// to one of three single statements. Same file-scope std::atomic shape as the
// other two, for the reason given at g_alertPumpEntryReported.
std::atomic<bool> g_alertPumpPreLockReported{false};

// Two bounded emits of condvarWaitTimeout's Result, which the call at the wait
// site used to discard. Both are file-scope std::atomic<bool> for the reason
// given at g_alertPumpEntryReported, and both are one-shot, so the process can
// print at most two of these lines no matter how long it runs.
//
// Deliberately two flags and not one. 0xEA01 is the expected, normal result, so
// a single first-result-only line would report 0xEA01 forever and never notice
// a later tick degrading -- and "times out correctly and then stops" is exactly
// the shape one first-result line cannot tell apart from health. Two flags make
// the next run able to distinguish it.
std::atomic<bool> g_alertPumpWaitRcReported{false};
std::atomic<bool> g_alertPumpWaitRcAnomalyReported{false};

void alertPumpWorker(void* opaque) {
    // Declared ahead of the one-shot below because that one-shot fills it in.
    // The initialiser is a constant, so it costs nothing and cannot block.
    uint64_t bootMs = 0;
    // ---- FIRST STATEMENT OF THE BODY, BEFORE ANYTHING ELSE ----------------
    //
    // Everything ahead of this line in the entry path is libnx's own thread
    // trampoline (__libnx_initthread and whatever newlib work it does), which is
    // outside this file and outside this log. That is exactly why this line is
    // needed: it is the only thing that can separate the two failures that look
    // identical from the parent's side. "alert-pump: started" is written by the
    // PARENT at startAlertPump, after threadStart returns, and proves nothing
    // about the child. So -- this line absent means the child never ran its
    // first statement (the block is in the trampoline, or the child never got
    // scheduled); this line present with no wake-wakeup means the child ran and
    // blocked in this body.
    //
    // rawLog, not trace/appendGuestBootLine, so it lands in Eden's debug output
    // alongside alert-pump: started and alert-pump: wake-wakeup -- the sink the
    // two of those already use. rawLog is a bare svcOutputDebugString
    // (jni_bridge.cpp:478) with no TLS, no allocation and no file handle, which
    // is what makes it safe on a raw threadCreate thread whose TLS was installed
    // by libnx rather than by us. samplerWorker reaching appendGuestBootLine
    // (fopen/fprintf/fclose) on a raw thread is the proof that this is safe here.
    //
    // boot_ms is the reaper's convention, censusMillis (pthread_bridge.cpp:472):
    // cntpct_el0 * 1000 / armGetSystemTickFreq. cntpct_el0 is zeroed at boot, so
    // this is ms since boot. The guest-view log carries no timestamps at all, so
    // without this the entry cannot be compared against alert-pump: started or
    // against the reaper's 1 s cadence.
    if (!g_alertPumpEntryReported.exchange(true)) {
        const uint64_t entryFreq = armGetSystemTickFreq();
        bootMs = armGetSystemTick() * 1000ULL / (entryFreq != 0 ? entryFreq : 1);
        char enter[192];
        __builtin_snprintf(enter, sizeof(enter),
                           "alert-pump: thread-body-enter handle=0x%x"
                           " boot_ms=%llu site=first-statement",
                           (unsigned)threadGetCurHandle(),
                           (unsigned long long)bootMs);
        rawLog(enter);
    }
    (void)opaque;
    // cntpct_el0 ticks, not nanoseconds: armGetSystemTick is a register read
    // (libnx include/switch/arm/counter.h), scaled by armGetSystemTickFreq below.
    const uint64_t firstTick = armGetSystemTick();
    unsigned long long ticks = 0;
    while (g_alertPumpRunning.load(std::memory_order_relaxed)) {
        ++ticks;
        // One-shot, first iteration, immediately before the first mutexLock.
        // Reading table for the next run (alert-pump: pre-lock):
        //   absent, with thread-body-enter present => the block is between the
        //     entry log and this line -- armGetSystemTick, the relaxed load,
        //     ++ticks, or the log call itself -- or the thread faulted before
        //     reaching it, because a faulted thread reports nothing at all.
        //     That is the shape a 16 KB stack running out would take.
        //   present, reached-drain absent => the block is the wait slice: the
        //     mutexLock, the condvarWaitTimeout or the mutexUnlock below. It
        //     cannot be a contended lock: g_alertPumpWaitLock is named in
        //     exactly two places in this file, the lock and the unlock
        //     enclosing this one, and nothing anywhere signals
        //     g_alertPumpWait, so on tick 1 the mutex is uncontended by
        //     construction and condvarWaitTimeout is the only blocking call
        //     left in the window.
        //   present, reached-drain present => the wait returned and the block
        //     is inside cloudcell_pump.
        //
        // running=1 is free here and closes the one shape that mimics this
        // failure exactly: g_alertPumpRunning false makes the while below
        // never be taken, and the body then falls off its end having armed
        // neither reached-drain nor wake-wakeup. It is stored true at
        // startAlertPump before threadCreate and cleared only on the
        // create-failure path, so it must read 1 -- this line is what makes
        // that measured rather than argued.
        //
        // sp is the frame address at the top of the loop body, so it pins the
        // body's own stack depth: a value within a few hundred bytes of the
        // top of a 0x4000 allocation says the body is shallow and the 16 KB
        // stack is not under pressure at this point, which is the half of the
        // stack hypothesis this placement can answer without touching the
        // stack size. stack is the value actually passed to threadCreate,
        // logged here rather than changed.
        if (ticks == 1 && !g_alertPumpPreLockReported.exchange(true)) {
            char pre[176];
            __builtin_snprintf(pre, sizeof(pre),
                               "alert-pump: pre-lock iters=%llu running=%d"
                               " sp=0x%llx stack=0x%llx site=pre-mutexLock",
                               ticks,
                               (int)g_alertPumpRunning.load(
                                   std::memory_order_relaxed),
                               (unsigned long long)(uintptr_t)
                                   __builtin_frame_address(0),
                               (unsigned long long)kAlertPumpStackSize);
            rawLog(pre);
        }
        // A/B, 2026-10-04. DEFAULT IS SLEEP, NOT CONDVAR-WAIT.
        //
        // NOT a fix, and the reason matters. A first version of this comment
        // claimed the 80 ms timeout simply never expires on Eden. The sampler in
        // guest_view_probe.cpp falsifies that directly: it uses the identical
        // primitive and logged "sampler-wake iters=2 interval_ns=150907708"
        // against a 75 ms target, in a run from BEFORE this change. The real
        // finding is that the wait is UNRELIABLE, not dead: this thread, on
        // byte-identical code, went wait-result 0 -> 1 between two consecutive
        // runs (20261004-075251 vs 20261004-075549). A wait that fires
        // sometimes is worse for diagnosis than one that never fires, because
        // the run that happens to work reads as a fix.
        //
        // The switch stays, and not as a fix. g_alertPumpWait has no signaler
        // anywhere and g_alertPumpWaitLock is named only at the lock and the
        // unlock around this call, so there is no producer, no shared invariant
        // and no contended region. It was a sleep wearing a lock, which is
        // exactly what svcSleepThread says. The sampler's tick got the same
        // treatment, so one grep compares the two.
        //
        // What this thread actually does next is the open question, and it is
        // not a wait: in 20261004-075549 it produced wait-result=1 and then
        // reached-drain=0, which puts the block inside cloudcell_pump's lock
        // on g_queue_lock -- held by the guest's main thread while it spins
        // inside the render call.
        //
        // CORRECTED 2026-10-16: this said "std::lock_guard<std::mutex>", which
        // is wrong. cloudcell_defer.cpp:64 is `static Mutex g_queue_lock{}` --
        // a libnx Mutex, try-locked at cloudcell_defer.cpp:236. Naming it a
        // std::mutex invites the next reader to "port" the fix onto a Mutex
        // that already is one. The measurement underneath is unaffected, and it
        // is also why ccmode=bg cannot work: see
        // docs/compose/spec/unblock-menu-loading.md S4.
#ifndef RR3_ALERT_PUMP_CONDVAR
#define RR3_ALERT_PUMP_CONDVAR 0
#endif
        Result waitRc = kCondvarWaitTimeoutResult;
        if (RR3_ALERT_PUMP_CONDVAR) {
            mutexLock(&g_alertPumpWaitLock);
            waitRc = condvarWaitTimeout(
                &g_alertPumpWait, &g_alertPumpWaitLock,
                static_cast<s64>(kAlertPumpIntervalNanos));
            mutexUnlock(&g_alertPumpWaitLock);
        } else {
            svcSleepThread(static_cast<s64>(kAlertPumpIntervalNanos));
        }
        // Bounded to two lines per process: the first Result this thread ever
        // gets back, then the first Result that is not the expected timeout.
        // Both flags are consumed even when only one line prints, so the
        // combined case (an unexpected Result on the very first wait) stays one
        // line and the reader sees the anomaly on the first-wait-return line
        // rather than getting a near-duplicate.
        const bool firstWaitRc = !g_alertPumpWaitRcReported.exchange(true);
        const bool firstAnomaly =
            RR3_ALERT_PUMP_CONDVAR &&
            waitRc != kCondvarWaitTimeoutResult &&
            !g_alertPumpWaitRcAnomalyReported.exchange(true);
        if (firstWaitRc || firstAnomaly) {
            // censusMillis' formula (pthread_bridge.cpp:472),
            // cntpct_el0*1000/freq, recomputed live rather than reusing the
            // entry stamp `bootMs` holds: the anomaly emit can land on any
            // later tick, and the entry stamp would then report when the thread
            // started rather than when the anomaly happened. armGetSystemTick is
            // a register read, same as the firstTick above.
            const uint64_t rcFreq = armGetSystemTickFreq();
            const uint64_t rcBootMs =
                armGetSystemTick() * 1000ULL / (rcFreq != 0 ? rcFreq : 1);
            char wrc[192];
            __builtin_snprintf(wrc, sizeof(wrc),
                               "alert-pump: wait-result rc=0x%08x iters=%llu"
                               " slice_ns=%lld boot_ms=%llu site=%s",
                               (unsigned)waitRc, ticks,
                               (long long)kAlertPumpIntervalNanos,
                               (unsigned long long)rcBootMs,
                               firstWaitRc ? "first-wait-return"
                                           : "first-non-timeout");
            rawLog(wrc);
        }
        if (ticks == 2 && !g_alertPumpWakeReported.exchange(true)) {
            const uint64_t freq = armGetSystemTickFreq();
            const uint64_t intervalNs =
                (armGetSystemTick() - firstTick) * 1000000000ULL /
                (freq != 0 ? freq : 1);
            char wake[160];
            __builtin_snprintf(wake, sizeof(wake),
                               "alert-pump: wake-wakeup iters=%llu"
                               " interval_ns=%llu target_ns=%lld"
                               " prim=condvarWaitTimeout",
                               ticks, (unsigned long long)intervalNs,
                               (long long)kAlertPumpIntervalNanos);
            rawLog(wake);
        }
        // One-shot, first completed wait, immediately before the drain. Reaching
        // here at all means the wait returned (so the wait slice and the condvar
        // are both fine) and the tick is about to enter cloudcell_pump, whose
        // first act is std::lock_guard<std::mutex> on g_queue_lock
        // (cloudcell_defer.cpp:71) -- the only lock on this thread's tick-1 path
        // that anybody else can hold, and the only newlib-backed state the body
        // touches. So this line splits the remaining question cleanly:
        //   present + no wake-wakeup => the block is inside cloudcell_pump
        //   absent                 => the block is the wait itself, or the
        //                             mutexLock/mutexUnlock pair above it
        // Both are one-shot, and both are rawLog, so together with the
        // first-statement line the next run needs at most three new lines in
        // Eden's log to place the block exactly.
        if (ticks == 1 && !g_alertPumpDrainReported.exchange(true)) {
            char reach[160];
            __builtin_snprintf(reach, sizeof(reach),
                               "alert-pump: reached-drain iters=%llu"
                               " boot_ms=%llu site=pre-cloudcell_pump",
                               ticks, (unsigned long long)bootMs);
            rawLog(reach);
        }
        // always_log=false, and it matters: this tick runs ~12 times a second
        // for the life of the process and the queue is empty on almost all of
        // them, so the always-logging form would bury everything else (the
        // reason cloudcell_defer.h gives for the per-frame site). With false
        // the pump line appears only when there was something to drain, and
        // that line is what makes the answer attributable.
        cloudcell_pump("alert-pump", false);
    }
}

// Start the drain thread, exactly once.
//
// Started from here rather than from the Cloudcell init, because of the shape
// of this particular failure: the guest's view UI had already exited by the
// time the dialog was raised, so a thread tied to the view or frame-loop
// lifetime would already be gone. One that starts with the first alert cannot
// be too late.
//
// A raw libnx thread, matching startGuestPcSampler(). Not the guest pthread
// bridge, which guest_view_probe.cpp:147 records as never leaving
// svcSleepThread on Eden. samplerWorker is the precedent that logging is safe
// from here: it is also a threadCreate thread and it calls appendGuestBootLine
// (guest_view_probe.cpp:424), which works because newlib reaches its thread
// vars via TPIDRRO_EL0 and not the guest block in TPIDR_EL0. The sleep between
// ticks is load-bearing, not laziness: an instrument in this file that spun
// without one produced no output at all, having starved everything else (see
// the note at guest_view_probe.cpp:465).
//
// Adds no std::mutex and no fsync. The only new shared state is the atomic run
// flag; cloudcell_pump's own lock is taken and dropped inside
// cloudcell_defer.cpp:50-68 before any callback runs, which is what makes
// calling it from this second thread safe.
void startAlertPump() {
    bool expected = false;
    if (!g_alertPumpStarted.compare_exchange_strong(expected, true)) {
        return;
    }
    g_alertPumpRunning.store(true, std::memory_order_relaxed);
    if (R_FAILED(threadCreate(&g_alertPumpThread, alertPumpWorker, nullptr,
                              nullptr, kAlertPumpStackSize,
                              kDiagnosticThreadPriority,
                              -2 /* default core */))) {
        g_alertPumpRunning.store(false, std::memory_order_relaxed);
        g_alertPumpStarted.store(false, std::memory_order_relaxed);
        rawLog("alert-pump: threadCreate failed; queued alerts have no drain\n");
        return;
    }
    // Never joined, on the census reporter's reasoning (pthread_bridge.cpp:2361):
    // the handoff must not block on a diagnostic thread. The process exit
    // reclaims it, and there is no teardown path this file owns to stop it on
    // - the only one that exists, stopGuestPcSampler(), is reached when the
    // frame loop ends, which is before the alert that needs this thread.
    threadStart(&g_alertPumpThread);
    rawLog("alert-pump: started\n");
}

// The one place an alert is answered, shared by both routes that reach this
// export: the static-void gate below (jniCallStaticVoidMethodV) and
// rr3_Platform.cpp's platform_show_message. They used to carry a duplicated
// lookup and a duplicated deferred body, which is how the two drifted apart
// last time; the resolution and the answer now live here only.
//
// guestX1 is whatever the guest actually put in x1 on the route that got us
// here, kept purely so the one log line below can report it. It is NOT used as
// thiz, for the reason given at the call site.
void alertAnswerPositiveImpl(uintptr_t env, int64_t idPositive,
                             uintptr_t guestX1) {
    // Consumer before producer: if the thread cannot be created the enqueue
    // below still happens, and this rawLog line is what says nobody will take
    // it. (The view-side pumps would, if the frame loop is still alive.)
    startAlertPump();
    cloudcell_defer([env, idPositive, guestX1]() {
        // Resolved inside the deferred body, not on the guest path: the
        // ManagedMethod tables cannot be used for these exports (libRealRacing3.so
        // has no DT_SONAME, so the DT_SONAME pass in so_util.cpp never runs for
        // it), which is why cloudcell_natives.h resolves by name.
        const AlertExecuteFn fn = alertExecuteCallback();
        if (!fn) {
            // The name is spelled out in the message on purpose: this is the one
            // failure that a reader cannot act on without it, because the whole
            // bug class here is a name that looks right and is not.
            warning("alert-callback: export "
                    "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback "
                    "missing; the dialog stays unanswered\n");
            return;
        }
        // thiz stays NULL deliberately. Measured 20261003 against the DYNSYM
        // entry for this name (st_value=0x12b3c50, st_size=0x28, which is
        // exactly the 9 instructions below): the incoming x1 is never read.
        //   adrp x8,#0x1f6e000 ; ldr x8,[x8,#0x540] ; ldr x8,[x8] ; cbz x8,ret
        //   adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5a8] ; mov x1,x2 ; ldr x0,[x8]
        //   b #0x12b8ed4      (0x12b8ed4: cbz x1,ret ; br x1)
        // x0 is clobbered before use, so not even the JNIEnv* we pass survives;
        // x1 is overwritten by `mov x1, x2`; and the tail target branches to x1,
        // so the third argument is used as a CODE POINTER. thiz is dead - there
        // is no faithful value to supply, and inventing a plausible object here
        // would convert a legible fault into silent state corruption. The real
        // defect is the third argument, and fixing it needs the guest's own
        // callback address, which lives outside this file.
        jobject thiz = nullptr;
        trace("alert-callback: invoking alertMessageExecuteCallback(%lld) "
              "thiz=%p guestx1=0x%llx%s | measured: 0x12b3c50 reads neither "
              "x0 nor x1, and tail-branches x2 to as code via 0x12b8ed4",
              static_cast<long long>(idPositive),
              static_cast<const void*>(thiz),
              static_cast<unsigned long long>(guestX1),
              guestX1 != 0 ? "" : " (no guest object on this path)");
        fn(reinterpret_cast<JNIEnv*>(env), thiz, idPositive);
    });

    // A/B, 2026-10-04. DEFAULT: drain here, on the caller's own thread.
    //
    // MEASURED: the alert-pump thread reaches its wait and then stops before
    // `reached-drain` -- cloudcell_pump's first act is
    // `std::lock_guard<std::mutex> on g_queue_lock` (cloudcell_defer.cpp:71),
    // which the spinning guest main thread is holding. The answer therefore
    // never arrives, the modal box is never dismissed, and the run ends with
    // the guest waiting on it and ~948,000 unmapped accesses behind it.
    //
    // Draining inline is safe for the reason the comment above startAlertPump
    // already gives (lines 2310-2313): cloudcell_pump takes and drops its own
    // lock inside cloudcell_defer.cpp:50-68 BEFORE any callback runs. Calling
    // it from the caller's own thread is no less safe than calling it from the
    // pump thread, and this is the thread that is not being starved.
    //
    // It is not the fix for whatever makes the guest raise the dialog, and the
    // `fn` call above still fails if the export is missing -- the trace line
    // says so. What it changes is whether the run stops at the first dialog or
    // continues past it, and what the guest does next is the only information
    // left that this investigation has not already collected.
#ifndef RR3_ALERT_INLINE_DRAIN
#define RR3_ALERT_INLINE_DRAIN 1
#endif
    if (RR3_ALERT_INLINE_DRAIN) {
        cloudcell_pump("alert-inline", false);
    }
}

void gateAnswerShowMessage(const ShowMessageCall& call, uintptr_t env,
                           uintptr_t guestX1) {
    if (!call.parsed) {
        return;
    }
    alertAnswerPositiveImpl(env, call.idPositive, guestX1);
}

// Which JNI slot entered jniCallVoidMethodV.
//
// Slots 61 (CallVoidMethod) and 62 (CallVoidMethodV) share one handler but pass
// DIFFERENT arg layouts: 62 hands over a `jobjject*` array, while 61 hands over
// a `va_list*` (see the wrapper below). Indexing one as the other yields
// garbage that still looks like a pointer -- the same failure shape as the old
// baseOk probe, which manufactured confident zeros for six rounds. The entry
// site therefore rides on every Cloudcell line instead of being inferred.
//
// thread_local, not a plain static: the guest drives this from several threads
// and a shared flag would stamp one thread's entry site onto another's line.
thread_local const char* t_jniVoidEntry = "62";

// The Cloudcell HttpRequest facade's Java classpath, as the GUEST spells it.
//
// MEASURED 2026-10-19 against reverse/native/arm64/libRealRacing3.so
// (33,110,472 B): the guest's .rodata carries
// "com/firemonkeys/cloudcellapi/HttpRequest" and the string
// "com/firemonkeys/cloudcellapi/CC_HttpRequest_Class" appears ZERO times. The
// same holds for the natives: Java_..._HttpRequest_{error,complete,data,header}
// Callback are all present, Java_..._CC_1HttpRequest_1Class_errorCallback is
// not.
//
// That contradicts cc_http_request.cpp, which registers
// "com/firemonkeys/cloudcellapi/CC_HttpRequest_Class" and resolves
// "..._CC_1HttpRequest_1Class_errorCallback", and whose header claims the
// opposite was "Verified against the disassembly ... at 0x957bd0". 0x957bd0 is
// below this binary's .text (which starts at 0x988000). There is only one
// guest in use -- three byte-identical 33,110,472 B copies plus an unrelated
// 2013 PS Vita build that is not loaded -- so this is not an armhf/arm64 mixup.
//
// Matched by suffix, not equality, because FindClass may hand back a spelling
// that differs in the package prefix while the leaf class is the same.
const char* cloudcellClass(uintptr_t methodId) {
    const JniMethodRecord* method = findJniMethod(methodId);
    if (method == nullptr) {
        return nullptr;
    }
    const char* cls = className(method->clazz);
    if (cls == nullptr) {
        return nullptr;
    }
    // Any Cloudcell class, not just HttpRequest. The first cut matched the
    // HttpRequest leaf only, which put the dispatch block below out of reach of
    // every other Cloudcell call; the second cut compared the package as a
    // PREFIX, which also matched nothing, because the classpath the guest uses
    // starts "com/firemonkeys/" and only then reaches "cloudcellapi/".
    // A live run proved both: cc-dispatch=0 lines while jni-void showed
    // class=com/firemonkeys/cloudcellapi/AndroidAccountManager right beside it.
    //
    // Substring, not prefix and not full equality: the package can be spelled
    // with or without a leading path by different FindClass callers, and the
    // leaf alone would collide across packages.
    static const char kPkg[] = "cloudcellapi/";
    const size_t pkgLen = sizeof(kPkg) - 1;
    for (size_t i = 0; cls[i] != '\0'; ++i) {
        if (cls[i] != kPkg[0]) {
            continue;
        }
        size_t k = 1;
        while (k < pkgLen && cls[i + k] == kPkg[k]) {
            ++k;
        }
        if (k == pkgLen) {
            return cls;
        }
    }
    return nullptr;
}

// True when `cls` names the HttpRequest facade specifically, i.e. the leaf past
// the package is HttpRequest.
bool cloudcellClassIsHttpRequest(const char* cls) {
    static const char kLeaf[] = "cloudcellapi/HttpRequest";
    const size_t leafLen = sizeof(kLeaf) - 1;
    size_t clsLen = 0;
    while (cls[clsLen] != '\0') {
        ++clsLen;
    }
    if (clsLen < leafLen) {
        return false;
    }
    for (size_t i = 0; i < leafLen; ++i) {
        if (cls[clsLen - leafLen + i] != kLeaf[i]) {
            return false;
        }
    }
    return true;
}

// Cloudcell HttpRequest callback state, keyed by the JNI object handle the
// guest passes on each call. A request object's callback arrives on init() and
// is needed again on post(); the guest owns the object's lifetime, so this
// table only ever holds a borrowed handle and never frees anything.
struct CloudcellHttpRequestSlot {
    uintptr_t object;
    uint64_t callback;
};
constexpr size_t kCloudcellHttpRequestSlots = 8;
CloudcellHttpRequestSlot g_ccHttpSlots[kCloudcellHttpRequestSlots]{};

// Resolve a guest classpath against the ManagedMethod registry.
//
// Matched on the tail, for the same reason cloudcellHttpRequestClass does:
// FindClass may hand back a spelling that differs in the package prefix while
// the leaf class is the same. Matching the FULL path would silently miss
// anything registered without its package, and matching only the leaf would
// collide across packages.
const Class* managedClassFor(const char* classpath) {
    if (classpath == nullptr) {
        return nullptr;
    }
    for (const Class* clazz : ClassRegistry::get_class_registry()) {
        if (clazz == nullptr) {
            continue;
        }
        if (rawEquals(clazz->classpath, classpath) ||
            rawEquals(clazz->classname, classpath)) {
            return clazz;
        }
        const char* leaf = clazz->classname;
        size_t leafLen = 0;
        while (leaf[leafLen] != '\0') {
            ++leafLen;
        }
        size_t pathLen = 0;
        while (classpath[pathLen] != '\0') {
            ++pathLen;
        }
        if (pathLen > leafLen) {
            const char* tail = classpath + (pathLen - leafLen);
            if (rawEquals(tail, leaf)) {
                return clazz;
            }
        }
    }
    return nullptr;
}

// Resolve a method inside a Class. Tables are NULL-terminated, not length-
// counted: every one of them ends with a {NULL} entry (cc_services.cpp:105,
// cc_http_request.cpp:140, and so on), so the loop has to stop on a null name
// rather than on a count.
const ManagedMethod* managedMethodFor(const Class* clazz, const char* name) {
    if (clazz == nullptr || clazz->managed_methods == nullptr) {
        return nullptr;
    }
    for (const ManagedMethod* method = clazz->managed_methods; method->name != nullptr;
         ++method) {
        if (rawEquals(method->name, name)) {
            return method;
        }
    }
    return nullptr;
}

void jniCallVoidMethodV(uintptr_t env, uintptr_t object,
                       uintptr_t methodId, void* args) {
    // `env` is the JNIEnv the guest passed.
    const char* name = jniMethodName(methodId);
    jniPairMark("enter", name, methodId, 0);
    if (name == nullptr) {
        return;
    }

    // --- Cloudcell HttpRequest fast-fail, added 2026-10-19 ------------------
    //
    // Why here and not in jni_compat/classes/cc_http_request.cpp: that file's
    // logic is already right (it answers "the transfer failed", which is what
    // HttpThread.java emits when openConnection() throws and responseCode is
    // still 0). It is unreachable. jniGetMethodID returns a JniMethodRecord*,
    // not a ManagedMethod*, nothing dispatches into ClassRegistry, and the
    // [jni] GetMethodID marker only exists in this file's rawLog. So editing
    // names there would have produced code that looks correct and never runs.
    //
    // UNBUDGETED on purpose: HttpRequest methods are a handful of calls per run,
    // and this line is the only thing that can say whether the guest calls
    // post() at all, through which slot, and with what callback. Under the
    // shared 64-line per-call budget it would have been the first thing dropped.
    const char* ccClass = cloudcellClass(methodId);
    if (ccClass != nullptr) {
        const bool isHttpRequest = cloudcellClassIsHttpRequest(ccClass);
        const bool isInit = rawEquals(name, "init");
        const bool isPost = rawEquals(name, "post");
        char detail[256];
        __builtin_snprintf(detail, sizeof(detail),
                           "entry=%s class=%s method=%s static=%d obj=%p args=%p",
                           t_jniVoidEntry, ccClass, name,
                           findJniMethod(methodId)->isStatic ? 1 : 0,
                           (void *)object, args);
        guestViewWideLog("cc-http-void", static_cast<unsigned long long>(methodId),
                         static_cast<unsigned long long>(object), detail);

        if (isHttpRequest && isInit) {
            // Only slot 62 gives an indexable array. Slot 61's args are a
            // va_list* and reading it as one would be undefined; the line above
            // records which entry happened, so a slot-61 run is visible instead
            // of silently mis-parsed.
            if (rawEquals(t_jniVoidEntry, "62") && args != nullptr) {
                const jobject* argv = static_cast<const jobject*>(args);
                // init(String url, String method, String userAgent,
                //      byte[] body, int readCapacity, long callback,
                //      boolean failOnErrorStatus)
                const uint64_t callback =
                    reinterpret_cast<uintptr_t>(argv[7]);
                CloudcellHttpRequestSlot* slot = nullptr;
                for (size_t i = 0; i < kCloudcellHttpRequestSlots; ++i) {
                    if (g_ccHttpSlots[i].object == object ||
                        g_ccHttpSlots[i].object == 0) {
                        slot = &g_ccHttpSlots[i];
                        break;
                    }
                }
                if (slot != nullptr) {
                    slot->object = object;
                    slot->callback = callback;
                    char got[128];
                    __builtin_snprintf(got, sizeof(got),
                                       "init-captured callback=0x%llx slot=%d",
                                       (unsigned long long)callback,
                                       (int)(slot - g_ccHttpSlots));
                    guestViewWideLog("cc-http-callback",
                                     static_cast<unsigned long long>(callback),
                                     static_cast<unsigned long long>(object), got);
                }
            } else {
                guestViewWideLog("cc-http-callback",
                                 static_cast<unsigned long long>(object),
                                 static_cast<unsigned long long>(methodId),
                                 "init seen but args NOT indexed (entry is not 62)");
            }
        } else if (isHttpRequest && isPost) {
            uint64_t callback = 0;
            for (size_t i = 0; i < kCloudcellHttpRequestSlots; ++i) {
                if (g_ccHttpSlots[i].object == object) {
                    callback = g_ccHttpSlots[i].callback;
                    break;
                }
            }
            guestViewWideLog("cc-http-callback",
                             static_cast<unsigned long long>(callback),
                             static_cast<unsigned long long>(object),
                             callback != 0 ? "post has a callback"
                                           : "post has NO callback (init never seen)");
        }

        // --- ManagedMethod dispatch (ccdisp) --------------------------------
        //
        // Every Cloudcell void call the guest makes lands here, and none of
        // them reached jni_compat/classes/ before this. The stubs in there
        // (aam_login, aam_is_logged_in, ...) were written, registered and never
        // executed; cc_services.cpp:116 registers CCAccountManager under the
        // very classpath the guest asks for, so the registration was right and
        // only the wiring was missing.
        //
        // Skipped deliberately:
        //   * takes_class methods. RegisterNonVirtual wants a jclass between
        //     the object and the argument block and there is none to hand it;
        //     calling the four-argument dispatcher with three reads one
        //     register past the va_list (jni_internals.h says so about
        //     AudioTrack.write). That covers <init>.
        //   * any class outside Cloudcell, for now.
        //
        // The name match is reported whether or not it succeeds. A miss here is
        // the interesting case, not a non-event: cc_services registers "Login"
        // and the guest asks for "login", which is invisible in every other
        // readout this port has.
        const int dispatchLevel = ::rr3::gfxCloudcellDispatch();
        if (dispatchLevel > 0) {
            const Class* clazz = managedClassFor(ccClass);
            const ManagedMethod* method =
                clazz != nullptr ? managedMethodFor(clazz, name) : nullptr;
            const char* verdict = method == nullptr
                                      ? (clazz == nullptr ? "no class" : "no method")
                                      : (method->takes_class ? "skipped takes_class"
                                                             : "MATCH");
            char dd[256];
            __builtin_snprintf(dd, sizeof(dd),
                               "level=%d class=%s method=%s -> %s static=%d obj=%p",
                               dispatchLevel, ccClass, name, verdict,
                               method != nullptr ? (method->is_static_method ? 1 : 0) : -1,
                               (void *)object);
            guestViewWideLog("cc-dispatch", static_cast<unsigned long long>(methodId),
                             static_cast<unsigned long long>(object), dd);

            if (dispatchLevel >= 2 && method != nullptr && !method->takes_class) {
                if (rawEquals(t_jniVoidEntry, "62") && args != nullptr) {
                    // Slot 62 hands over an array of 8-byte slots, which is
                    // exactly the jvalue array the array-form dispatcher reads.
                    // This is the form that matches; passing the array to the
                    // va_list-form dispatcher would read one word too few.
                    using Fn = void (*)(JNIEnv*, jobject, jvalue*);
                    reinterpret_cast<Fn>(method->addr_array)(
                        reinterpret_cast<JNIEnv*>(env),
                        reinterpret_cast<jobject>(object),
                        static_cast<jvalue*>(args));
                } else {
                    // Slot 61's args really are a va_list*, so the variadic
                    // form is correct there. Deliberately NOT indexed.
                    using Fn = void (*)(JNIEnv*, jobject, va_list);
                    va_list forwarded;
                    va_copy(forwarded, *static_cast<va_list*>(args));
                    reinterpret_cast<Fn>(method->addr_variadic)(
                        reinterpret_cast<JNIEnv*>(env),
                        reinterpret_cast<jobject>(object), forwarded);
                    va_end(forwarded);
                }
            }
        }
    }

    if (rawEquals(name, "showMessage") || rawEquals(name, "displayDialog") ||
        rawEquals(name, "ShowDialogBox")) {
        // Guest VA = load bias + ELF virtual address, so the difference is the
        // rr3+offset the census already reports for imports.
        const uintptr_t bias = resolveMainImageLoadBias();
        const uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        char details[192];
        __builtin_snprintf(details, sizeof(details), "method=%s (instance) ra=rr3+%llx",
                           name, (unsigned long long)(ra > bias ? ra - bias : ra));
        guestViewWideLog("gate-void", static_cast<unsigned long long>(methodId),
                         (unsigned long long)ra, details);
    }
    // Was a bare `static unsigned s_voidLog < 40` cap. Moved onto the shared
    // per-call budget so the over-cap calls land in the same suppressed counter
    // as every other site, and so the cap is a relaxed atomic rather than a
    // plain function-local static that several guest threads could race on.
    // Effective cap is now kPerCallLogBudget (64) across the file, not 40 here.
    if (perCallLogAllow("jni-void")) {
        // Class name added 2026-10-19: a bare method name cannot be attributed
        // when several classes define the same one, and "did the guest ever call
        // HttpRequest.post" is unanswerable without it.
        const JniMethodRecord* rec = findJniMethod(methodId);
        const char* cls = rec != nullptr ? className(rec->clazz) : nullptr;
        char line[256];
        __builtin_snprintf(line, sizeof(line), "%s class=%s", name,
                           cls != nullptr ? cls : "(none)");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "jni-void", 0,
                            static_cast<unsigned long long>(methodId), line);
    }
    ::rr3::censusNoteImportFrom(__builtin_return_address(0), "jni:CallVoidMethod");
    jniPairMark("exit", name, methodId, 0);
    if (rawEquals(name, "hideSplash") || rawEquals(name, "showSplash") ||
        rawEquals(name, "hideLoadingSplash")) {
        guestViewNoteAndroidLog(name);
    }
    // openURL / showMessage / exitApp / extractRes: intentional no-ops.
    // Host owns the surface; opening URLs, dialogs, or finishing the activity
    // would tear down guest progress.
}

void jniCallStaticVoidMethodV(uintptr_t env, uintptr_t clazz,
                              uintptr_t methodId, void* args) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return;
    }
    if (rawEquals(name, "showMessage")) {
        // clazz is the guest's own x1 on this call, forwarded only so the
        // shared answer path can report what the guest supplied.
        gateAnswerShowMessage(gateTraceShowMessage(args), env, clazz);
    } else if (g_gateStaticVoidLines < kGateMaxLines) {
        // Every other static void is a no-op below, so a name here is a place
        // where the guest asked for something and got silence. Worth a line:
        // the list is the engine's own "call Java" surface.
        ++g_gateStaticVoidLines;
        const uintptr_t bias = resolveMainImageLoadBias();
        const uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        char details[192];
        __builtin_snprintf(details, sizeof(details), "method=%s ra=rr3+%llx", name,
                           (unsigned long long)(ra > bias ? ra - bias : ra));
        guestViewWideLog("gate-static-void", static_cast<unsigned long long>(methodId),
                         (unsigned long long)ra, details);
    }
    if (rawEquals(name, "hideSplash") || rawEquals(name, "showSplash")) {
        guestViewNoteAndroidLog(name);
    }
    // openURL / exitApp / extractRes: intentional no-ops (extractRes is actually
    // boolean; if bound as void it is still safe). showMessage is not one of
    // them any more - it is answered above, deferred, positive button only.
}

// Non-V CallStaticVoidMethod (slot 141). Wrap the V path.
void jniCallStaticVoidMethod(uintptr_t env, uintptr_t clazz,
                             uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    jniCallStaticVoidMethodV(env, clazz, methodId, &arguments);
    va_end(arguments);
}

// Slot 118 CallStaticBooleanMethodV. setGameState(int) returns true when the
// activity instance exists; the host lifecycle already advances g_gameState
// after each callback, so only acknowledge the call here.
uintptr_t jniCallStaticBooleanMethodV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                                      uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return 0;
    }
    if (rawEquals(name, "setGameState")) {
        return 1;
    }
    const uintptr_t result = platformBoolForMethod(name) ? 1u : 0u;
    if (gateScalarNameInteresting(name)) {
        gateTraceScalar("gate-static-bool", name, result, methodId);
    }
    return result;
}

// Non-V CallStaticBooleanMethod (slot 117). Wrap the V path.
uintptr_t jniCallStaticBooleanMethod(uintptr_t env, uintptr_t clazz,
                                     uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    uintptr_t result = jniCallStaticBooleanMethodV(env, clazz, methodId,
                                                   &arguments);
    va_end(arguments);
    return result;
}

// Non-V CallVoidMethod (slot 61). Wrap the V path.
void jniCallVoidMethod(uintptr_t env, uintptr_t object,
                       uintptr_t methodId, ...) {
    va_list arguments;
    va_start(arguments, methodId);
    // Mark the entry site so the shared body knows its `args` is a va_list*, not
    // a jobject array. Restored rather than reset to "62" on the way out, so a
    // nested call on this thread cannot be mislabelled as slot 62.
    const char* previousEntry = t_jniVoidEntry;
    t_jniVoidEntry = "61";
    jniCallVoidMethodV(env, object, methodId, &arguments);
    t_jniVoidEntry = previousEntry;
    va_end(arguments);
}

// GetObjectField (slot 95). bmpData on a Font is a real array record with a
// length the guest can read and bytes it can address; the shared sentinel stays
// the answer for every other object so nothing that stores a handle without a
// null check regresses.
uintptr_t jniGetObjectField(uintptr_t /*env*/, uintptr_t object,
                            uintptr_t fieldId) {
    FakeFont* font = asFontObject(object);
    if (font != nullptr && rawEquals(jniFieldName(fieldId), "bmpData")) {
        return fontBmpData(font);
    }
    return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
}

// GetStaticBooleanField (slot 146) / GetStaticIntField (slot 150).
uintptr_t jniGetStaticBooleanField(uintptr_t /*env*/, uintptr_t /*clazz*/,
                                   uintptr_t /*fieldId*/) {
    return 0;
}

int32_t jniGetStaticIntField(uintptr_t /*env*/, uintptr_t /*clazz*/,
                             uintptr_t /*fieldId*/) {
    return 0;
}

uintptr_t jniPushLocalFrame(uintptr_t /*env*/, int32_t /*capacity*/) {
    return 0; // JNI_OK; sentinel handles are immortal and need no frame.
}

uintptr_t jniPopLocalFrame(uintptr_t /*env*/, uintptr_t result) {
    return result;
}

uintptr_t jniGetObjectRefType(uintptr_t /*env*/, uintptr_t /*object*/) {
    return 2; // JNIGlobalRefType for the stable fake handles.
}

void* g_jniTable[kJniSlotCount]{};
void* g_vmTable[8]{};

void** g_jniVtable = g_jniTable;  // &g_jniVtable is the JNIEnv handle
void** g_vmVtable = g_vmTable;    // &g_vmVtable is the JavaVM handle

uintptr_t jniGetJavaVM(uintptr_t /*env*/, void** outVm) {
    if (outVm != nullptr) {
        *outVm = &g_vmVtable;
    }
    return 0;  // JNI_OK
}

// Fake JavaVM: a JavaVM is a pointer to a variable holding a
// JNIInvokeInterface table pointer. GetEnv (index 6) and AttachCurrentThread
// (index 4) are implemented; the rest fall back to returning 0.
uintptr_t vmGetEnv(uintptr_t /*vm*/, void** outEnv, uintptr_t /*version*/) {
    if (outEnv != nullptr) {
        *outEnv = &g_jniVtable;
    }
    return 0;  // JNI_OK
}

// AttachCurrentThread(JavaVM*, JNIEnv** outEnv, void* args). getEnv() calls this
// when GetEnv reports JNI_EDETACHED; our GetEnv never does, but Nimble may still
// attach worker threads. Hand back the same single JNIEnv handle.
uintptr_t vmAttachCurrentThread(uintptr_t /*vm*/, void** outEnv,
                                void* /*args*/) {
    if (outEnv != nullptr) {
        *outEnv = &g_jniVtable;
    }
    return 0;  // JNI_OK
}

bool g_bridgeReady = false;

void ensureInitialized() {
    if (g_bridgeReady) {
        return;
    }
    for (size_t slot = 0; slot < kJniSlotCount; ++slot) {
        g_jniTable[slot] = reinterpret_cast<void*>(g_stubTable[slot]);
    }

    // Slots verified against the onCreateJNI decompilation.
    g_jniTable[4] = reinterpret_cast<void*>(&jniGetVersion);          // GetVersion
    g_jniTable[21] = reinterpret_cast<void*>(&jniNewGlobalRef);       // NewGlobalRef
    g_jniTable[23] = reinterpret_cast<void*>(&jniNoop);               // DeleteLocalRef
    g_jniTable[31] = reinterpret_cast<void*>(&jniGetObjectClass);     // GetObjectClass
    g_jniTable[33] = reinterpret_cast<void*>(&jniGetMethodID);        // GetMethodID
    g_jniTable[219] = reinterpret_cast<void*>(&jniGetJavaVM);         // GetJavaVM

    // Common, safely-answerable slots and the high-frequency calls observed
    // in the first onCreateJNI run. These wrappers deliberately do not inspect
    // va_list contents: the guest only needs stable handles/values to complete
    // the platform object binding pass.
    g_jniTable[19] = reinterpret_cast<void*>(&jniPushLocalFrame);          // PushLocalFrame
    g_jniTable[20] = reinterpret_cast<void*>(&jniPopLocalFrame);           // PopLocalFrame
    g_jniTable[29] = reinterpret_cast<void*>(&jniNewObjectV);              // NewObjectV
    g_jniTable[35] = reinterpret_cast<void*>(&jniCallObjectMethodV);        // CallObjectMethodV
    g_jniTable[34] = reinterpret_cast<void*>(&jniCallObjectMethod);         // CallObjectMethod
    g_jniTable[37] = reinterpret_cast<void*>(&jniCallBooleanMethod);        // CallBooleanMethod
    g_jniTable[38] = reinterpret_cast<void*>(&jniCallBooleanMethodV);       // CallBooleanMethodV
    g_jniTable[49] = reinterpret_cast<void*>(&jniCallIntMethod);            // CallIntMethod
    g_jniTable[50] = reinterpret_cast<void*>(&jniCallIntMethodV);           // CallIntMethodV
    g_jniTable[52] = reinterpret_cast<void*>(&jniCallLongMethod);           // CallLongMethod
    g_jniTable[55] = reinterpret_cast<void*>(&jniCallFloatMethod);          // CallFloatMethod
    g_jniTable[61] = reinterpret_cast<void*>(&jniCallVoidMethod);           // CallVoidMethod
    g_jniTable[62] = reinterpret_cast<void*>(&jniCallVoidMethodV);          // CallVoidMethodV
    g_jniTable[95] = reinterpret_cast<void*>(&jniGetObjectField);           // GetObjectField
    g_jniTable[113] = reinterpret_cast<void*>(&jniGetStaticMethodID);       // GetStaticMethodID
    g_jniTable[114] = reinterpret_cast<void*>(&jniCallStaticObjectMethod);  // CallStaticObjectMethod
    g_jniTable[115] = reinterpret_cast<void*>(&jniCallStaticObjectMethodV); // CallStaticObjectMethodV
    g_jniTable[117] = reinterpret_cast<void*>(&jniCallStaticBooleanMethod); // CallStaticBooleanMethod
    g_jniTable[118] = reinterpret_cast<void*>(&jniCallStaticBooleanMethodV);// CallStaticBooleanMethodV
    g_jniTable[129] = reinterpret_cast<void*>(&jniCallStaticIntMethod);     // CallStaticIntMethod
    g_jniTable[130] = reinterpret_cast<void*>(&jniCallStaticIntMethodV);    // CallStaticIntMethodV
    g_jniTable[141] = reinterpret_cast<void*>(&jniCallStaticVoidMethod);    // CallStaticVoidMethod
    g_jniTable[142] = reinterpret_cast<void*>(&jniCallStaticVoidMethodV);   // CallStaticVoidMethodV
    g_jniTable[146] = reinterpret_cast<void*>(&jniGetStaticBooleanField);   // GetStaticBooleanField
    g_jniTable[150] = reinterpret_cast<void*>(&jniGetStaticIntField);       // GetStaticIntField
    g_jniTable[232] = reinterpret_cast<void*>(&jniGetObjectRefType);        // GetObjectRefType
    g_jniTable[6] = reinterpret_cast<void*>(&jniFindClass);           // FindClass
    g_jniTable[15] = reinterpret_cast<void*>(&jniReturnZero);         // ExceptionOccurred
    g_jniTable[17] = reinterpret_cast<void*>(&jniNoop);               // ExceptionClear
    g_jniTable[22] = reinterpret_cast<void*>(&jniNoop);               // DeleteGlobalRef
    g_jniTable[24] = reinterpret_cast<void*>(&jniIsSameObject);       // IsSameObject
    g_jniTable[25] = reinterpret_cast<void*>(&jniNewLocalRef);        // NewLocalRef
    g_jniTable[26] = reinterpret_cast<void*>(&jniReturnZero);         // EnsureLocalCapacity
    g_jniTable[94] = reinterpret_cast<void*>(&jniGetFieldID);         // GetFieldID
    g_jniTable[100] = reinterpret_cast<void*>(&jniGetIntField);       // GetIntField
    g_jniTable[102] = reinterpret_cast<void*>(&jniGetFloatField);     // GetFloatField
    g_jniTable[144] = reinterpret_cast<void*>(&jniGetFieldID);        // GetStaticFieldID
    g_jniTable[167] = reinterpret_cast<void*>(&jniNewStringUTF);      // NewStringUTF
    g_jniTable[168] = reinterpret_cast<void*>(&jniGetStringUTFLength); // GetStringUTFLength
    g_jniTable[169] = reinterpret_cast<void*>(&jniGetStringUTFChars); // GetStringUTFChars
    g_jniTable[170] = reinterpret_cast<void*>(&jniNoop);              // ReleaseStringUTFChars
    g_jniTable[171] = reinterpret_cast<void*>(&jniGetArrayLength);     // GetArrayLength
    g_jniTable[176] = reinterpret_cast<void*>(&jniNewByteArray);       // NewByteArray
    g_jniTable[184] = reinterpret_cast<void*>(&jniGetByteArrayElements); // GetByteArrayElements
    g_jniTable[192] = reinterpret_cast<void*>(&jniReleaseByteArrayElements); // ReleaseByteArrayElements
    g_jniTable[200] = reinterpret_cast<void*>(&jniGetByteArrayRegion); // GetByteArrayRegion
    g_jniTable[208] = reinterpret_cast<void*>(&jniSetByteArrayRegion); // SetByteArrayRegion
    g_jniTable[215] = reinterpret_cast<void*>(&jniReturnZero);        // RegisterNatives
    g_jniTable[228] = reinterpret_cast<void*>(&jniReturnZero);        // ExceptionCheck

    for (size_t slot = 0; slot < 8; ++slot) {
        g_vmTable[slot] = reinterpret_cast<void*>(&jniSlotStub<0>);
    }
    // JNIInvokeInterface layout: [0..2] reserved, [3] DestroyJavaVM,
    // [4] AttachCurrentThread, [5] DetachCurrentThread, [6] GetEnv,
    // [7] AttachCurrentThreadAsDaemon. Both JNI_OnLoad and EA::Nimble::getEnv()
    // call [vtable+48] == index 6, so GetEnv MUST live at 6, not 3.
    g_vmTable[4] = reinterpret_cast<void*>(&vmAttachCurrentThread);   // AttachCurrentThread
    g_vmTable[6] = reinterpret_cast<void*>(&vmGetEnv);                // JavaVM::GetEnv
    g_bridgeReady = true;
}

} // namespace

// Externally linked on purpose: rr3_Platform.cpp's platform_show_message is the
// other route to this export and must not carry a second copy of it. It sits
// outside the anonymous namespace for the reason runGuestJniOnLoad below does -
// this is the file's cross-TU surface.
void rr3AlertAnswerPositiveCallback(uintptr_t env, int64_t idPositive,
                                    uintptr_t guestX1) {
    alertAnswerPositiveImpl(env, idPositive, guestX1);
}

// One-shot startup census: how many per-call file-log sites this file has, which
// of them are now capped, and the names. Sits beside the other two startup
// self-tests so a run that later shows no jni-obj lines can be read against
// this line instead of guessed at -- "capped" and "never called" are different
// findings and only one of them is a regression.
void perCallLogSiteCensus() {
    static const char* const kSharedBudgetSites[] = {
        "jni-obj",               // jniCallObjectMethodV
        "jni-static-obj",        // jniCallStaticObjectMethodV
        "jni-void",              // jniCallVoidMethodV
        "NewByteArray",          // jniNewByteArray
        "load-texture",          // makeTextureInfoHandle
        "load-texture-png",      // makeTextureInfoFromPng
        "load-texture-badpng",   // makeTextureInfoFromPng failure
        "load-texture-noargs",   // loadTextureFromMemoryArgs failure
        "load-texture-badarray", // loadTextureFromMemoryArgs failure
    };
    constexpr size_t kSharedCount =
        sizeof(kSharedBudgetSites) / sizeof(kSharedBudgetSites[0]);
    // Already bounded before this change, by their own per-site counters, and
    // listed so the reported total is the whole per-call file-write surface
    // rather than only the part that was unguarded.
    static const char* const kOwnCapSites[] = {
        "gate-static-void",  // jniCallStaticVoidMethodV, kGateMaxLines
        "gate-obj-bool",     // jniCallBooleanMethodV, kGateMaxLines
        "gate-obj-int",      // jniCallIntMethodV, kGateMaxLines
        "gate-static-bool",  // jniCallStaticBooleanMethodV, kGateMaxLines
        "gate-static-int",   // jniCallStaticIntMethodV, kGateMaxLines
        "gate-show-message", // gateTraceShowMessage, kGateMaxLines
        "gate-void",         // jniCallVoidMethodV name filter, ungated but
                             // only reachable for three splash/dialog names
    };
    constexpr size_t kOwnCapCount =
        sizeof(kOwnCapSites) / sizeof(kOwnCapSites[0]);

    char names[384];
    size_t used = 0;
    for (size_t index = 0; index < kSharedCount; ++index) {
        const char* name = kSharedBudgetSites[index];
        const size_t length = rawStrlen(name);
        if (used + length + 2 >= sizeof(names)) {
            break;
        }
        if (index != 0) {
            names[used++] = ',';
        }
        for (size_t i = 0; i < length; ++i) {
            names[used + i] = name[i];
        }
        used += length;
    }
    names[used] = '\0';

    char details[640];
    __builtin_snprintf(
        details, sizeof(details),
        "per_call_sites=%u shared_budget=%u own_cap_sites=%u own_cap_each=%u "
        "shared_capped_sites=[%s] suppressed_total reported in "
        "phase=per-call-log-cap / per-call-log-suppressed",
        (unsigned)kSharedCount, kPerCallLogBudget, (unsigned)kOwnCapCount,
        kGateMaxLines, names);
    guestViewWideLog("jni-per-call-log-sites", kSharedCount, kPerCallLogBudget,
                     details);
}

GuestJniOnLoadProbe runGuestJniOnLoad(const char* progressPath) {
    GuestJniOnLoadProbe probe{};
    probe.attempted = true;
    ensureInitialized();
    // First guest-facing entry point of the run, and the first point at which
    // the loader has mapped the guest images, so it is where the alert-answer
    // export can be checked for real. One line, guarded, and it happens before
    // the TLS scope below goes in.
    alertCallbackExportSelfTest();
    // Same moment, same cost class, wider net: the count for the layer the alert
    // callback is one member of. Ordered after it so the specific answer is the
    // first thing in the log.
    cloudcellNativesResolveSelfTest();
    // And the per-call file-log census, so a later "no jni-obj lines in the
    // log" is attributable to the cap rather than to the guest never calling.
    perCallLogSiteCensus();

    // Android's loader calls JNI_OnLoad(JavaVM*, void* reserved) on each guest
    // library at load time; that is where EA::Nimble caches the JavaVM pointer
    // its getEnv() (reached from main ctor #157) later dereferences. Without
    // this the cached pointer is null and the ctor faults on `ldr x8,[x0]`.
    constexpr size_t kMaxOnLoad = 8;
    uintptr_t targets[kMaxOnLoad]{};
    const size_t found = collectGuestExports("JNI_OnLoad", targets, kMaxOnLoad);
    probe.imagesFound = static_cast<uint32_t>(found);
    if (found == 0) {
        probe.failureCode = 1;  // no guest image exports JNI_OnLoad
        return probe;
    }

    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        probe.failureCode = 2;  // could not install guest TLS
        return probe;
    }

    for (size_t index = 0; index < found; ++index) {
        if (progressPath != nullptr) {
            writeGuestBootProgress(progressPath, "JNI_OnLoad",
                                   static_cast<unsigned long long>(index),
                                   static_cast<unsigned long long>(targets[index]));
        }
        // JNI_OnLoad receives the JavaVM handle (&g_vmVtable) and a reserved
        // arg (null on Android); it returns the required JNI version.
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "jniOnLoad-call", index, targets[index]);
        const uintptr_t version =
            reinterpret_cast<uintptr_t (*)(void*, void*)>(targets[index])(
                &g_vmVtable, nullptr);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "jniOnLoad-ret", index, version);
        probe.lastVersion = static_cast<uint32_t>(version);
        ++probe.imagesCalled;
    }
    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "JNI_OnLoad-complete",
                               static_cast<unsigned long long>(found), 0);
    }
    probe.onLoad = snapshotJniHistogram();
    probe.completed = true;
    return probe;
}

GuestEntryProbe runGuestEntryProbe(const char* progressPath) {
    GuestEntryProbe probe{};
    probe.attempted = true;
    ensureInitialized();

    // All targets must resolve before the guest TLS scope goes in; a missed
    // symbol is a host-side fix, not something a crash report would help with.
    const uintptr_t createTarget =
        resolveMainImageExport("Java_com_firemint_realracing_MainActivity_onCreateJNI");
    if (createTarget == 0) {
        probe.failureCode = 1;  // onCreateJNI export not found in main image
        return probe;
    }
    probe.resolved = true;
    const uintptr_t startTarget =
        resolveMainImageExport("Java_com_firemint_realracing_MainActivity_onStartJNI");
    if (startTarget == 0) {
        probe.failureCode = 3;  // onStartJNI export missing
        return probe;
    }
    probe.startResolved = true;
    const uintptr_t resumeTarget =
        resolveMainImageExport("Java_com_firemint_realracing_MainActivity_onResumeJNI");
    if (resumeTarget == 0) {
        probe.failureCode = 4;  // onResumeJNI export missing
        return probe;
    }
    probe.resumeResolved = true;

    const JniSlotHistogram before = snapshotJniHistogram();

    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        probe.failureCode = 2;  // could not install guest TLS
        return probe;
    }

    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "onCreateJNI", 0,
                               static_cast<unsigned long long>(createTarget));
    }
    probe.called = true;
    reinterpret_cast<void (*)(void*, void*)>(createTarget)(
        &g_jniVtable, &g_fakeActivityObject);
    probe.returned = true;
    g_gameState = 1;
    const JniSlotHistogram afterCreate = snapshotJniHistogram();

    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "onStartJNI", 0,
                               static_cast<unsigned long long>(startTarget));
    }
    probe.startCalled = true;
    reinterpret_cast<void (*)(void*, void*)>(startTarget)(
        &g_jniVtable, &g_fakeActivityObject);
    probe.startReturned = true;
    g_gameState = 2;
    const JniSlotHistogram afterStart = snapshotJniHistogram();

    if (progressPath != nullptr) {
        writeGuestBootProgress(progressPath, "onResumeJNI", 0,
                               static_cast<unsigned long long>(resumeTarget));
    }
    probe.resumeCalled = true;
    reinterpret_cast<void (*)(void*, void*)>(resumeTarget)(
        &g_jniVtable, &g_fakeActivityObject);
    probe.resumeReturned = true;
    g_gameState = 3;
    const JniSlotHistogram afterResume = snapshotJniHistogram();

    probe.onCreate = diffJniHistogram(afterCreate, before);
    probe.start = diffJniHistogram(afterStart, afterCreate);
    probe.resume = diffJniHistogram(afterResume, afterStart);
    return probe;
}

JniSlotHistogram captureJniHistogram() {
    ensureInitialized();
    // The JNI phase boundary guest_view_probe.cpp already snapshots at
    // (onViewCreated / onViewChanged / onViewRender). Reusing it is why there
    // is no new timer for the suppressed-write total: three calls a run, and
    // perCallLogReport self-gates on a doubling, so this costs one relaxed load
    // and one compare per snapshot and writes a line only as the total grows.
    perCallLogReport("jni-phase");
    return snapshotJniHistogram();
}

void* guestJniEnvironment() {
    ensureInitialized();
    return &g_jniVtable;
}

void* guestActivityObject() {
    ensureInitialized();
    return &g_fakeActivityObject;
}

JniSlotHistogram diffJniHistograms(const JniSlotHistogram& after,
                                   const JniSlotHistogram& before) {
    return diffJniHistogram(after, before);
}

} // namespace rr3
