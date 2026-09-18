#include "rr3/fmod_audio_stub.hpp"

#include <cstring>

namespace rr3 {

// Interface IDs at rr3 scope so extern "C" IID pointer symbols can bind.
// Pointer identity is what OpenSL guests typically compare after dlsym.
struct SlInterfaceGuid {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t timeClockSeq;
    uint8_t node[6];
};

const SlInterfaceGuid kGuidEngine{
    0x0438B478, 0xE88A, 0x11DC, 0xB998, {0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};
const SlInterfaceGuid kGuidPlay{
    0x00000101, 0xE88A, 0x11DC, 0x8CD9, {0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};
const SlInterfaceGuid kGuidBufferQueue{
    0x2459A3B0, 0x11E1, 0xB4B6, 0x8E9C, {0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};
const SlInterfaceGuid kGuidAndroidSimpleBufferQueue{
    0x01147F9D, 0xB460, 0x11E1, 0xB4B6, {0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};

const SlInterfaceGuid* gIidEngine = &kGuidEngine;
const SlInterfaceGuid* gIidPlay = &kGuidPlay;
const SlInterfaceGuid* gIidBufferQueue = &kGuidBufferQueue;
const SlInterfaceGuid* gIidAndroidSimpleBufferQueue =
    &kGuidAndroidSimpleBufferQueue;

namespace {

// OpenSL / AAudio result codes (Android NDK SLES/OpenSLES.h, AAudio.h).
constexpr uint32_t kSlSuccess = 0;
constexpr uint32_t kSlParameterInvalid = 0x00000008;
constexpr uint32_t kSlPlayStatePlaying = 3;
constexpr uint32_t kSlObjectRealized = 1 << 2;
constexpr int32_t kAAudioOk = 0;
constexpr int32_t kAAudioErrorUnimplemented = -897;
constexpr int32_t kAAudioErrorUnavailable = -899;
constexpr int32_t kAAudioErrorInvalidState = -898;

FmodAudioStubStats gStats{};
uint32_t gPlayState = 1; // SL_PLAYSTATE_STOPPED

// OpenSL COM model: SLObjectItf is `const struct SLObjectItf_ * const *`.
// The guest object is the address of a cell that holds a vtable pointer.

using SlResult = uint32_t;
using SlSelf = void*;
using SlIid = const void*;

struct SlObjectItf {
    SlResult (*Realize)(SlSelf, uint32_t);
    SlResult (*GetInterface)(SlSelf, SlIid, void**);
    SlResult (*GetState)(SlSelf, uint32_t*);
    void* pad3;
    void* pad4;
    void* pad5;
    void (*Destroy)(SlSelf);
    void* pad7;
    void* pad8;
    void* pad9;
};

struct SlEngineItf {
    void* pad0;
    void* pad1;
    SlResult (*CreateAudioPlayer)(SlSelf, void**, void*, void*, uint32_t,
                                  const void*, const void*);
    void* pad3;
    SlResult (*CreateOutputMix)(SlSelf, void**, uint32_t, const void*,
                                const void*);
    void* pad5;
    void* pad6;
    void* pad7;
    void* pad8;
    void* pad9;
};

struct SlPlayItf {
    SlResult (*SetPlayState)(SlSelf, uint32_t);
    SlResult (*GetPlayState)(SlSelf, uint32_t*);
    void* pad2;
    void* pad3;
    void* pad4;
    void* pad5;
    void* pad6;
    void* pad7;
    void* pad8;
    void* pad9;
};

using SlBufferCallback = void (*)(void* queue, void* context);
struct SlBufferQueueItf {
    SlResult (*Enqueue)(SlSelf, const void*, uint32_t);
    SlResult (*Clear)(SlSelf);
    SlResult (*GetState)(SlSelf, void*);
    SlResult (*RegisterCallback)(SlSelf, SlBufferCallback, void*);
};

SlBufferCallback gBufferCallback{};
void* gBufferCallbackContext{};

SlResult objectRealize(SlSelf, uint32_t) {
    ++gStats.engineRealizes;
    return kSlSuccess;
}

SlResult objectGetState(SlSelf, uint32_t* state) {
    if (state != nullptr) *state = kSlObjectRealized;
    return kSlSuccess;
}

void objectDestroy(SlSelf) {}

SlResult engineGetInterface(SlSelf, SlIid iid, void** out);
SlResult engineCreateOutputMix(SlSelf, void** mix, uint32_t, const void*,
                               const void*);
SlResult engineCreateAudioPlayer(SlSelf, void** player, void*, void*,
                                 uint32_t, const void*, const void*);
SlResult playerGetInterface(SlSelf, SlIid iid, void** out);
SlResult playSetPlayState(SlSelf, uint32_t state);
SlResult playGetPlayState(SlSelf, uint32_t* state);
SlResult bufferEnqueue(SlSelf, const void* buffer, uint32_t size);
SlResult bufferClear(SlSelf);
SlResult bufferGetState(SlSelf, void* state);
SlResult bufferRegisterCallback(SlSelf, SlBufferCallback callback,
                                void* context);

const SlObjectItf kEngineObjectVtable{
    objectRealize, engineGetInterface, objectGetState,
    nullptr, nullptr, nullptr, objectDestroy,
    nullptr, nullptr, nullptr,
};
const SlEngineItf kEngineItfVtable{
    nullptr, nullptr, engineCreateAudioPlayer, nullptr,
    engineCreateOutputMix, nullptr, nullptr, nullptr, nullptr, nullptr,
};
const SlObjectItf kOutputMixObjectVtable{
    objectRealize, playerGetInterface, objectGetState,
    nullptr, nullptr, nullptr, objectDestroy,
    nullptr, nullptr, nullptr,
};
const SlObjectItf kPlayerObjectVtable{
    objectRealize, playerGetInterface, objectGetState,
    nullptr, nullptr, nullptr, objectDestroy,
    nullptr, nullptr, nullptr,
};
const SlPlayItf kPlayItfVtable{
    playSetPlayState, playGetPlayState,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
const SlBufferQueueItf kBufferQueueItfVtable{
    bufferEnqueue, bufferClear, bufferGetState, bufferRegisterCallback,
};

// Object/interface cells. Each holds a pointer to its vtable. The SL*Itf
// value handed to the guest is the address of the cell.
const SlObjectItf* gEngineObjectCell = &kEngineObjectVtable;
const SlEngineItf* gEngineItfCell = &kEngineItfVtable;
const SlObjectItf* gOutputMixObjectCell = &kOutputMixObjectVtable;
const SlObjectItf* gPlayerObjectCell = &kPlayerObjectVtable;
const SlPlayItf* gPlayItfCell = &kPlayItfVtable;
const SlBufferQueueItf* gBufferQueueItfCell = &kBufferQueueItfVtable;

const char* baseName(const char* path) {
    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return name;
}

SlResult engineGetInterface(SlSelf, SlIid iid, void** out) {
    if (out == nullptr) return kSlParameterInvalid;
    if (iid == &kGuidPlay || iid == &kGuidBufferQueue ||
        iid == &kGuidAndroidSimpleBufferQueue) {
        *out = nullptr;
        return kSlParameterInvalid;
    }
    // Engine IID or unknown IID: silent engine interface.
    *out = const_cast<void*>(static_cast<const void*>(&gEngineItfCell));
    return kSlSuccess;
}

SlResult engineCreateOutputMix(SlSelf, void** mix, uint32_t, const void*,
                               const void*) {
    if (mix == nullptr) return kSlParameterInvalid;
    ++gStats.outputMixCreates;
    *mix = const_cast<void*>(static_cast<const void*>(&gOutputMixObjectCell));
    return kSlSuccess;
}

SlResult engineCreateAudioPlayer(SlSelf, void** player, void*, void*,
                                 uint32_t, const void*, const void*) {
    if (player == nullptr) return kSlParameterInvalid;
    ++gStats.playerCreates;
    *player = const_cast<void*>(static_cast<const void*>(&gPlayerObjectCell));
    return kSlSuccess;
}

SlResult playerGetInterface(SlSelf, SlIid iid, void** out) {
    if (out == nullptr) return kSlParameterInvalid;
    if (iid == &kGuidPlay) {
        *out = const_cast<void*>(static_cast<const void*>(&gPlayItfCell));
        return kSlSuccess;
    }
    // Buffer-queue IID or unknown: the path that deadlocks if missing.
    *out = const_cast<void*>(static_cast<const void*>(&gBufferQueueItfCell));
    return kSlSuccess;
}

SlResult playSetPlayState(SlSelf, uint32_t state) {
    gPlayState = state;
    ++gStats.playStateTransitions;
    if (state == kSlPlayStatePlaying) {
        ++gStats.playRequested;
    }
    // Requested state recorded. audible stays 0 — no Horizon Android device.
    return kSlSuccess;
}

SlResult playGetPlayState(SlSelf, uint32_t* state) {
    if (state != nullptr) *state = gPlayState;
    return kSlSuccess;
}

SlResult bufferEnqueue(SlSelf, const void* buffer, uint32_t size) {
    // Silent discard: consume immediately so the mixer never waits for
    // hardware. Do not claim the samples were heard.
    (void)buffer;
    (void)size;
    ++gStats.bufferEnqueues;
    if (gBufferCallback != nullptr) {
        ++gStats.bufferCallbacks;
        gBufferCallback(
            const_cast<void*>(static_cast<const void*>(&gBufferQueueItfCell)),
            gBufferCallbackContext);
    }
    return kSlSuccess;
}

SlResult bufferClear(SlSelf) { return kSlSuccess; }

SlResult bufferGetState(SlSelf, void* state) {
    if (state != nullptr) {
        auto* words = static_cast<uint32_t*>(state);
        words[0] = 0; // count — already discarded
        words[1] = 0; // index
    }
    return kSlSuccess;
}

SlResult bufferRegisterCallback(SlSelf, SlBufferCallback callback,
                                void* context) {
    gBufferCallback = callback;
    gBufferCallbackContext = context;
    return kSlSuccess;
}

} // namespace

extern "C" {
const void* SL_IID_ENGINE = &rr3::kGuidEngine;
const void* SL_IID_PLAY = &rr3::kGuidPlay;
const void* SL_IID_BUFFERQUEUE = &rr3::kGuidBufferQueue;
const void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE =
    &rr3::kGuidAndroidSimpleBufferQueue;
}

uint32_t slCreateEngine(void** engine, uint32_t numOptions, const void* options,
                        uint32_t numInterfaces, const void* interfaceIds,
                        const void* interfaceRequired) {
    (void)numOptions;
    (void)options;
    (void)numInterfaces;
    (void)interfaceIds;
    (void)interfaceRequired;
    if (engine == nullptr) return kSlParameterInvalid;
    ++gStats.engineCreates;
    // *engine is SLObjectItf: address of the cell holding the vtable pointer.
    *engine = const_cast<void*>(static_cast<const void*>(&gEngineObjectCell));
    return kSlSuccess;
}

int32_t aaudioCreateStreamBuilder(void** builder) {
    ++gStats.aaudioAttempts;
    if (builder == nullptr) return kAAudioErrorUnimplemented;
    *builder = nullptr;
    ++gStats.aaudioRefusals;
    ++gStats.openFailures;
    return kAAudioErrorUnavailable;
}

int32_t aaudioStreamBuilderOpenStream(void*, void** stream) {
    ++gStats.aaudioAttempts;
    if (stream != nullptr) *stream = nullptr;
    ++gStats.aaudioRefusals;
    ++gStats.openFailures;
    return kAAudioErrorUnavailable;
}

void aaudioStreamBuilderDelete(void*) {}
int32_t aaudioStream_requestStart(void*) { return kAAudioErrorInvalidState; }
int32_t aaudioStream_requestStop(void*) { return kAAudioErrorInvalidState; }
int32_t aaudioStream_close(void*) { return kAAudioOk; }
int32_t aaudioStream_write(void*, const void*, int32_t, int64_t) {
    return kAAudioErrorUnavailable;
}
int32_t aaudioStream_getState(void*) { return kAAudioErrorInvalidState; }

FmodAudioStubStats fmodAudioStubStats() {
    FmodAudioStubStats copy = gStats;
    copy.audible = 0;
    return copy;
}

bool isVirtualAudioLibrary(const char* fileName) {
    if (fileName == nullptr) return false;
    const char* name = baseName(fileName);
    return std::strcmp(name, "libOpenSLES.so") == 0 ||
        std::strcmp(name, "libaaudio.so") == 0 ||
        std::strcmp(name, "libOpenSLES_util.so") == 0;
}

void* virtualAudioLibraryHandle(const char* fileName) {
    if (!isVirtualAudioLibrary(fileName)) return nullptr;
    if (std::strcmp(baseName(fileName), "libaaudio.so") == 0) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(0xAA1D10));
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x051E5));
}

void* resolveVirtualAudioSymbol(void* handle, const char* symbolName) {
    if (symbolName == nullptr) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    const bool aaudio = value == 0xAA1D10;
    const bool opensl = value == 0x051E5;
    if (!aaudio && !opensl) return nullptr;

    if (aaudio) {
        if (std::strcmp(symbolName, "AAudioStreamBuilder_create") == 0 ||
            std::strcmp(symbolName, "AAudio_createStreamBuilder") == 0) {
            return reinterpret_cast<void*>(&aaudioCreateStreamBuilder);
        }
        if (std::strcmp(symbolName, "AAudioStreamBuilder_openStream") == 0) {
            return reinterpret_cast<void*>(&aaudioStreamBuilderOpenStream);
        }
        if (std::strcmp(symbolName, "AAudioStreamBuilder_delete") == 0) {
            return reinterpret_cast<void*>(&aaudioStreamBuilderDelete);
        }
        if (std::strcmp(symbolName, "AAudioStream_requestStart") == 0) {
            return reinterpret_cast<void*>(&aaudioStream_requestStart);
        }
        if (std::strcmp(symbolName, "AAudioStream_requestStop") == 0) {
            return reinterpret_cast<void*>(&aaudioStream_requestStop);
        }
        if (std::strcmp(symbolName, "AAudioStream_close") == 0) {
            return reinterpret_cast<void*>(&aaudioStream_close);
        }
        if (std::strcmp(symbolName, "AAudioStream_write") == 0) {
            return reinterpret_cast<void*>(&aaudioStream_write);
        }
        if (std::strcmp(symbolName, "AAudioStream_getState") == 0) {
            return reinterpret_cast<void*>(&aaudioStream_getState);
        }
        return nullptr;
    }

    if (std::strcmp(symbolName, "slCreateEngine") == 0) {
        return reinterpret_cast<void*>(&slCreateEngine);
    }
    if (std::strcmp(symbolName, "SL_IID_ENGINE") == 0) {
        return reinterpret_cast<void*>(&gIidEngine);
    }
    if (std::strcmp(symbolName, "SL_IID_PLAY") == 0) {
        return reinterpret_cast<void*>(&gIidPlay);
    }
    if (std::strcmp(symbolName, "SL_IID_BUFFERQUEUE") == 0) {
        return reinterpret_cast<void*>(&gIidBufferQueue);
    }
    if (std::strcmp(symbolName, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0) {
        return reinterpret_cast<void*>(&gIidAndroidSimpleBufferQueue);
    }
    return nullptr;
}

uint64_t fmodAudioStubSelfTest() {
    uint64_t passed = 0;
    const FmodAudioStubStats before = gStats;

    void* engine = nullptr;
    if (slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr) != kSlSuccess ||
        engine == nullptr) {
        return passed;
    }
    passed |= 1ULL << 0;

    auto object = static_cast<const SlObjectItf**>(engine);
    const SlObjectItf* methods = *object;
    if (methods == nullptr || methods->Realize == nullptr ||
        methods->GetInterface == nullptr) {
        return passed;
    }
    if (methods->Realize(engine, 0) != kSlSuccess) return passed;

    void* engineItf = nullptr;
    methods->GetInterface(engine, &kGuidEngine, &engineItf);
    if (engineItf == nullptr) return passed;
    passed |= 1ULL << 1;

    auto eng = static_cast<const SlEngineItf**>(engineItf);
    if (*eng == nullptr || (*eng)->CreateOutputMix == nullptr ||
        (*eng)->CreateAudioPlayer == nullptr) {
        return passed;
    }
    void* mix = nullptr;
    void* player = nullptr;
    if ((*eng)->CreateOutputMix(engineItf, &mix, 0, nullptr, nullptr) !=
            kSlSuccess ||
        mix == nullptr) {
        return passed;
    }
    if ((*eng)->CreateAudioPlayer(engineItf, &player, nullptr, nullptr, 0,
                                  nullptr, nullptr) != kSlSuccess ||
        player == nullptr) {
        return passed;
    }
    passed |= 1ULL << 2;

    auto playerObject = static_cast<const SlObjectItf**>(player);
    const SlObjectItf* playerMethods = *playerObject;
    if (playerMethods == nullptr || playerMethods->GetInterface == nullptr) {
        return passed;
    }
    void* queue = nullptr;
    playerMethods->GetInterface(player, &kGuidAndroidSimpleBufferQueue, &queue);
    auto queueCell = static_cast<const SlBufferQueueItf**>(queue);
    if (queueCell == nullptr || *queueCell == nullptr ||
        (*queueCell)->Enqueue == nullptr) {
        return passed;
    }
    alignas(16) unsigned char silent[256]{};
    const uint32_t enqueuesBefore = gStats.bufferEnqueues;
    if ((*queueCell)->Enqueue(queue, silent, sizeof(silent)) == kSlSuccess &&
        gStats.bufferEnqueues == enqueuesBefore + 1 &&
        fmodAudioStubStats().audible == 0) {
        passed |= 1ULL << 3;
    }

    void* builder = nullptr;
    if (aaudioCreateStreamBuilder(&builder) == kAAudioErrorUnavailable &&
        builder == nullptr && gStats.aaudioRefusals > before.aaudioRefusals) {
        passed |= 1ULL << 4;
    }

    void* handle = virtualAudioLibraryHandle("libOpenSLES.so");
    void* create = resolveVirtualAudioSymbol(handle, "slCreateEngine");
    void* aaudioHandle = virtualAudioLibraryHandle("libaaudio.so");
    void* aaudioCreate =
        resolveVirtualAudioSymbol(aaudioHandle, "AAudioStreamBuilder_create");
    if (handle != nullptr && create != nullptr && aaudioHandle != nullptr &&
        aaudioCreate != nullptr) {
        passed |= 1ULL << 5;
    }

    return passed;
}

} // namespace rr3
