// jni_bridge.cpp — 阶段 9：最小 JNIEnv 提供者
//
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

#include "rr3/jni_bridge.hpp"

#include <switch.h>
#include <GLES2/gl2.h>

#include <array>
#include <cstddef>
#include <cstdarg>

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/tls_probe.hpp"
#include "rr3/png_decode.hpp"

namespace rr3 {
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
// valid fake handle. The activity object must be non-null — the guest
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
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "NewByteArray",
                        static_cast<unsigned long long>(length),
                        static_cast<unsigned long long>(g_byteArrayCount));
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

uintptr_t rememberJniField(const char* name) {
    for (size_t index = 0; index < g_jniFieldCount; ++index) {
        if (g_jniFields[index].magic == kJniFieldMagic &&
            rawEquals(g_jniFields[index].name, name)) {
            return reinterpret_cast<uintptr_t>(&g_jniFields[index]);
        }
    }
    if (g_jniFieldCount >= kMaxJniFields) {
        return reinterpret_cast<uintptr_t>(&g_fakeFieldId);
    }
    JniFieldRecord& field = g_jniFields[g_jniFieldCount++];
    field.magic = kJniFieldMagic;
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
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "load-texture", static_cast<unsigned long long>(texture),
                        static_cast<unsigned long long>(g_textureInfoCount));
    return reinterpret_cast<uintptr_t>(&info);
}

// Decode a real PNG from guest memory and upload it as an RGBA texture.
// Falls back to the 64x64 checker placeholder on any failure.
uintptr_t makeTextureInfoFromPng(const uint8_t* bytes, size_t size) {
    PngDecodeSession session{};
    if (!pngDecodeBegin(&session, bytes, size)) {
        rawLog("[jni] loadTextureFromMemory: PNG decode failed -> placeholder\n");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "load-texture-badpng",
                            static_cast<unsigned long long>(size), 0);
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
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "load-texture-png", static_cast<unsigned long long>(texture),
                        (static_cast<unsigned long long>(session.width) << 16) |
                            session.height);
    return reinterpret_cast<uintptr_t>(&info);
}

// CallStaticObjectMethodV path for loadTextureFromMemory(byte[], int).
// Reads the jbyteArray handle and jint length from the guest va_list; if the
// handle cannot be resolved, falls back to the placeholder with a breadcrumb.
uintptr_t loadTextureFromMemoryArgs(void* args) {
    if (args == nullptr) {
        rawLog("[jni] loadTextureFromMemory: null va_list -> placeholder\n");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "load-texture-noargs", 0, 0);
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
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "load-texture-badarray",
                            static_cast<unsigned long long>(length),
                            reinterpret_cast<unsigned long long>(handle));
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
    // getExternalStorageState is an int method (mounted→2) — see
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
    // ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE — Switch default layout.
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
    // Stable dummy: native C++ stores the handle and calls void methods on it
    // (setThresholdLevel/writeWithTitle). Returning null made the C++ cloudcell
    // path skip work that gates the menu asset load.
    if (rawEquals(name, "getComponent") || rawEquals(name, "GetComponent")) {
        return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
    }
    if (rawEquals(name, "createAnalytics") || rawEquals(name, "createGluAds") ||
        rawEquals(name, "createCustomerSupport") ||
        rawEquals(name, "getClassLoader") || rawEquals(name, "loadClass") ||
        rawEquals(name, "getCustomProperties") || rawEquals(name, "keySet") ||
        rawEquals(name, "iterator") || rawEquals(name, "put")) {
        return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
    }
    if (jniMethodReturnsString(methodId)) {
        return makeJniString(platformStringForMethod(name));
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
    return reinterpret_cast<uintptr_t>(&g_fakeClassObject);
}

uintptr_t jniGetObjectClass(uintptr_t /*env*/, uintptr_t /*object*/) {
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

uintptr_t jniGetFieldID(uintptr_t /*env*/, uintptr_t /*clazz*/,
                        const char* name, const char* /*signature*/) {
    rawLog("[jni] GetFieldID ");
    rawLog(name);
    rawLog("\n");
    return rememberJniField(name);
}

int32_t jniGetIntField(uintptr_t /*env*/, uintptr_t object, uintptr_t fieldId) {
    if (object == 0) {
        return 0;
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

uintptr_t jniNewObjectV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                        uintptr_t /*methodId*/, void* /*args*/) {
    return reinterpret_cast<uintptr_t>(&g_fakeObjectObject);
}

uintptr_t jniCallObjectMethodV(uintptr_t /*env*/, uintptr_t /*object*/,
                               uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    if (name != nullptr) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "jni-obj", 0, static_cast<unsigned long long>(methodId),
                            name);
    }
    return jniObjectForMethod(methodId);
}

uintptr_t jniCallStaticObjectMethodV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                                     uintptr_t methodId, void* args) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return jniObjectForMethod(methodId);
    }
    // Full name dump: one line per unique static call so the next device run
    // shows the real service surface instead of another GL counter.
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "jni-static-obj", 0,
                        static_cast<unsigned long long>(methodId), name);
    if (rawEquals(name, "loadTextureFromMemory")) {
        return loadTextureFromMemoryArgs(args);
    }
    return jniObjectForMethod(methodId);
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

// Non-V CallFloatMethod (slot 55). No CallFloatMethodV yet; answer 0.f.
float jniCallFloatMethod(uintptr_t /*env*/, uintptr_t /*object*/,
                         uintptr_t /*methodId*/, ...) {
    return 0.f;
}

int32_t jniCallBooleanMethodV(uintptr_t /*env*/, uintptr_t /*object*/,
                              uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    if (name != nullptr && (rawEquals(name, "hasNext") ||
                            rawEquals(name, "isEmpty") ||
                            rawEquals(name, "isDataAvailable"))) {
        return platformBoolForMethod(name) ? 1u : 0u;
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
    return platformIntForMethod(jniMethodName(methodId));
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
    return platformIntForMethod(jniMethodName(methodId));
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

void jniCallVoidMethodV(uintptr_t /*env*/, uintptr_t /*object*/,
                       uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return;
    }
    static unsigned s_voidLog = 0;
    if (s_voidLog < 40) {
        ++s_voidLog;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "jni-void", s_voidLog,
                            static_cast<unsigned long long>(methodId), name);
    }
    if (rawEquals(name, "hideSplash") || rawEquals(name, "showSplash") ||
        rawEquals(name, "hideLoadingSplash")) {
        guestViewNoteAndroidLog(name);
    }
    // openURL / showMessage / exitApp / extractRes: intentional no-ops.
    // Host owns the surface; opening URLs, dialogs, or finishing the activity
    // would tear down guest progress.
}

void jniCallStaticVoidMethodV(uintptr_t /*env*/, uintptr_t /*clazz*/,
                              uintptr_t methodId, void* /*args*/) {
    const char* name = jniMethodName(methodId);
    if (name == nullptr) {
        return;
    }
    if (rawEquals(name, "hideSplash") || rawEquals(name, "showSplash")) {
        guestViewNoteAndroidLog(name);
    }
    // openURL / showMessage / exitApp / extractRes: intentional no-ops
    // (extractRes is actually boolean; if bound as void it is still safe).
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
    return platformBoolForMethod(name) ? 1u : 0u;
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
    jniCallVoidMethodV(env, object, methodId, &arguments);
    va_end(arguments);
}

// GetObjectField (slot 95). Sentinel handles are immortal; return a stable
// fake object so guest code can store the handle without a null check.
uintptr_t jniGetObjectField(uintptr_t /*env*/, uintptr_t /*object*/,
                            uintptr_t /*fieldId*/) {
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

GuestJniOnLoadProbe runGuestJniOnLoad(const char* progressPath) {
    GuestJniOnLoadProbe probe{};
    probe.attempted = true;
    ensureInitialized();

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
        const uintptr_t version =
            reinterpret_cast<uintptr_t (*)(void*, void*)>(targets[index])(
                &g_vmVtable, nullptr);
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
