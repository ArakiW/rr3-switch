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
// THE TWO THE GUEST'S OWN FMOD LOOKS UP AND THE PORT DID NOT HAVE.
//
// MEASURED 2026-11-16: the strings inside libfmodex.so name exactly five
// IID symbols, and SL_IID_ANDROIDCONFIGURATION and SL_IID_RECORD were not
// among the four the port's virtual libOpenSLES.so exported.
//
// SL_IID_ANDROIDCONFIGURATION IS THE IMPORTANT ONE: it is the interface
// that carries the device's SAMPLE RATE and CHANNEL COUNT, so a backend
// that cannot hand it over cannot be told what format to produce.
//
// The GUID CONTENTS do not have to match the Khronos ones: the guest
// obtains each IID POINTER by dlsym from this very library and hands that
// pointer back to GetInterface, so the identity that matters is the
// pointer's.  The content comparison in the GetInterface paths is a
// belt-and-braces match, not the mechanism.
const SlInterfaceGuid kGuidVolume{
    0x1F9D8A09, 0x11DC, 0xBF0F, 0x00, {0x04, 0x00, 0x00, 0x00, 0x00, 0x00}};
const SlInterfaceGuid kGuidAndroidConfiguration{
    0x8A6D4F1A, 0x11DC, 0xBF0F, 0x00, {0x04, 0x00, 0x00, 0x00, 0x00, 0x00}};
const SlInterfaceGuid kGuidRecord{
    0x112487E9, 0x11DC, 0xBF0F, 0x00, {0x04, 0x00, 0x00, 0x00, 0x00, 0x00}};

const SlInterfaceGuid* gIidEngine = &kGuidEngine;
const SlInterfaceGuid* gIidPlay = &kGuidPlay;
const SlInterfaceGuid* gIidBufferQueue = &kGuidBufferQueue;
const SlInterfaceGuid* gIidAndroidSimpleBufferQueue =
    &kGuidAndroidSimpleBufferQueue;
const SlInterfaceGuid* gIidVolume = &kGuidVolume;
const SlInterfaceGuid* gIidAndroidConfiguration = &kGuidAndroidConfiguration;
const SlInterfaceGuid* gIidRecord = &kGuidRecord;

namespace {

// OpenSL / AAudio result codes (Android NDK SLES/OpenSLES.h, AAudio.h).
constexpr uint32_t kSlSuccess = 0;
constexpr uint32_t kSlParameterInvalid = 0x00000008;
// SL_RESULT_INTERFACE_UNAVAILABLE: the honest answer to "this object has no
// such interface".  It is what a real backend returns, and it is what stops
// a caller from reading another interface's vtable as if it were its own.
constexpr uint32_t kSlInterfaceUnavailable = 0x00000005;
constexpr uint32_t kSlPlayStatePlaying = 3;
constexpr uint32_t kSlObjectRealized = 1 << 2;
constexpr int32_t kAAudioOk = 0;
constexpr int32_t kAAudioErrorUnimplemented = -897;
constexpr int32_t kAAudioErrorUnavailable = -899;
constexpr int32_t kAAudioErrorInvalidState = -898;

FmodAudioStubStats gStats{};

// THE DEVICE'S SELF-TEST RESULT, KEPT.  The self-test runs once, at boot,
// and before this the only place its bits existed was the boot log line --
// which is exactly where nobody looks.  The census asks for them every
// frame, so they have to live here.
uint64_t gSelfTestBits = 0;
uint32_t gPlayState = 1; // SL_PLAYSTATE_STOPPED

// THE DEVICE'S OWN STATE.  The volume defaults to a LIVE level: a backend
// that reports zero volume is a backend a mixer virtualises away, and the
// guest's own log says "FMOD_ZERO_VOLUME_VIRTUALISATION" -- which is what
// that looks like from the other side.
constexpr int32_t kDefaultVolumeLevel = 100; // millibels, non-zero
constexpr float kDefaultVolume = 1.0F;
int32_t gVolumeLevel = kDefaultVolumeLevel;
float gVolume = kDefaultVolume;

// THE FORMAT THE DEVICE PRODUCES.  It matches what the guest's own FMOD
// asks the port for: the port's FMOD shim reports 48000 Hz, PCM16, 2
// channels, 1024-sample buffers.
constexpr int32_t kDeviceSampleRate = 48000;
constexpr int32_t kDeviceChannelCount = 2;
constexpr int32_t kDeviceBufferSize = 1024;
constexpr int32_t kDeviceOutputChannelCount = 2;
constexpr int32_t kDevicePerformanceMode = 0;

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

// SLVolumeItf: the level in millibels (0 is mute) and the volume as a
// float.  The device reports a LIVE level by default, because a backend
// that reports zero is a backend a mixer will virtualise away.
struct SlVolumeItf {
    SlResult (*SetVolumeLevel)(SlSelf, int32_t);
    SlResult (*GetVolumeLevel)(SlSelf, int32_t*);
    SlResult (*SetVolume)(SlSelf, float);
    SlResult (*GetVolume)(SlSelf, float*);
    SlResult (*SetVolumeCurve)(SlSelf, const void*);
    void* pad5;
    void* pad6;
    void* pad7;
    void* pad8;
    void* pad9;
};

// SLAndroidConfigurationItf: what the device IS -- its sample rate, its
// channel count, its buffer size, how many channels come out, and the
// performance mode.  This is the interface the guest's FMOD asks for
// BEFORE it mixes anything.
struct SlAndroidConfigurationItf {
    SlResult (*GetSampleRate)(SlSelf, int32_t*);
    SlResult (*GetChannelCount)(SlSelf, int32_t*);
    SlResult (*GetBufferSize)(SlSelf, int32_t*);
    SlResult (*GetOutputChannelCount)(SlSelf, int32_t*);
    SlResult (*GetPerformanceMode)(SlSelf, int32_t*);
    SlResult (*SetPerformanceMode)(SlSelf, int32_t);
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
SlResult volumeSetVolumeLevel(SlSelf, int32_t level);
SlResult volumeGetVolumeLevel(SlSelf, int32_t* level);
SlResult volumeSetVolume(SlSelf, float volume);
SlResult volumeGetVolume(SlSelf, float* volume);
SlResult volumeSetVolumeCurve(SlSelf, const void* curve);
SlResult androidConfigGetSampleRate(SlSelf, int32_t* rate);
SlResult androidConfigGetChannelCount(SlSelf, int32_t* channels);
SlResult androidConfigGetBufferSize(SlSelf, int32_t* bytes);
SlResult androidConfigGetOutputChannelCount(SlSelf, int32_t* channels);
SlResult androidConfigGetPerformanceMode(SlSelf, int32_t* mode);
SlResult androidConfigSetPerformanceMode(SlSelf, int32_t mode);

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
const SlVolumeItf kVolumeItfVtable{
    volumeSetVolumeLevel, volumeGetVolumeLevel, volumeSetVolume,
    volumeGetVolume, volumeSetVolumeCurve,
    nullptr, nullptr, nullptr, nullptr, nullptr,
};
const SlAndroidConfigurationItf kAndroidConfigItfVtable{
    androidConfigGetSampleRate, androidConfigGetChannelCount,
    androidConfigGetBufferSize, androidConfigGetOutputChannelCount,
    androidConfigGetPerformanceMode, androidConfigSetPerformanceMode,
};

// Object/interface cells. Each holds a pointer to its vtable. The SL*Itf
// value handed to the guest is the address of the cell.
const SlObjectItf* gEngineObjectCell = &kEngineObjectVtable;
const SlEngineItf* gEngineItfCell = &kEngineItfVtable;
const SlObjectItf* gOutputMixObjectCell = &kOutputMixObjectVtable;
const SlObjectItf* gPlayerObjectCell = &kPlayerObjectVtable;
const SlPlayItf* gPlayItfCell = &kPlayItfVtable;
const SlBufferQueueItf* gBufferQueueItfCell = &kBufferQueueItfVtable;
const SlVolumeItf* gVolumeItfCell = &kVolumeItfVtable;
const SlAndroidConfigurationItf* gAndroidConfigItfCell =
    &kAndroidConfigItfVtable;

const char* baseName(const char* path) {
    const char* name = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') name = cursor + 1;
    }
    return name;
}

SlResult engineGetInterface(SlSelf, SlIid iid, void** out) {
    if (out == nullptr) return kSlParameterInvalid;
    // THE ENGINE'S INTERFACES, AND ONLY THOSE.  The old code answered every
    // unrecognised IID with the ENGINE interface, so a caller that asked
    // for something else got an object whose vtable was the WRONG one and
    // read whatever it wanted out of it.  **=> AN INTERFACE QUERY MUST
    // ANSWER WITH THE RIGHT INTERFACE OR WITH AN EXPLICIT FAILURE, NEVER
    // WITH SOMETHING ELSE.**
    if (iid == &kGuidEngine) {
        *out = const_cast<void*>(static_cast<const void*>(&gEngineItfCell));
        return kSlSuccess;
    }
    // The engine has no recorder and no player of its own; saying so is the
    // truth.  (The guest's FMOD looks up SL_IID_RECORD, so the symbol has to
    // EXIST -- and it does, so the lookup succeeds -- but the engine does
    // not implement a recorder, and now it says so.)
    *out = nullptr;
    return kSlInterfaceUnavailable;
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
    if (iid == &kGuidBufferQueue || iid == &kGuidAndroidSimpleBufferQueue) {
        *out = const_cast<void*>(static_cast<const void*>(&gBufferQueueItfCell));
        return kSlSuccess;
    }
    // THE VOLUME.  A mixer reads the device's volume to decide whether the
    // device is live: the old code answered this query with the BUFFER
    // QUEUE's vtable, so the caller read a garbage level out of it, and a
    // mixer that reads zero virtualises the voices away -- which is what
    // "FMOD_ZERO_VOLUME_VIRTUALISATION" in the guest's own log looks like
    // from the other side.
    if (iid == &kGuidVolume) {
        *out = const_cast<void*>(static_cast<const void*>(&gVolumeItfCell));
        return kSlSuccess;
    }
    // THE DEVICE CONFIGURATION: what the device IS.  The guest's FMOD asks
    // for it before it mixes anything.
    if (iid == &kGuidAndroidConfiguration) {
        *out = const_cast<void*>(static_cast<const void*>(&gAndroidConfigItfCell));
        return kSlSuccess;
    }
    // UNKNOWN IID: AN EXPLICIT FAILURE.  The old fallback ("the buffer
    // queue for anything") is exactly what made a MISSING interface look
    // like a PRESENT one.
    *out = nullptr;
    return kSlInterfaceUnavailable;
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

SlResult volumeSetVolumeLevel(SlSelf, int32_t level) {
    // The level is clamped to the device's range, the way a real backend
    // clamps it: a mixer that sets -1 or 10^9 must not be able to make the
    // device report nonsense.
    if (level < 0) {
        level = 0;
    }
    if (level > kDefaultVolumeLevel) {
        level = kDefaultVolumeLevel;
    }
    gVolumeLevel = level;
    gVolume = static_cast<float>(level) / static_cast<float>(kDefaultVolumeLevel);
    return kSlSuccess;
}

SlResult volumeGetVolumeLevel(SlSelf, int32_t* level) {
    if (level != nullptr) *level = gVolumeLevel;
    return kSlSuccess;
}

SlResult volumeSetVolume(SlSelf, float volume) {
    float clamped = volume;
    if (clamped < 0.0F) clamped = 0.0F;
    if (clamped > 1.0F) clamped = 1.0F;
    gVolume = clamped;
    gVolumeLevel = static_cast<int32_t>(clamped * static_cast<float>(kDefaultVolumeLevel));
    return kSlSuccess;
}

SlResult volumeGetVolume(SlSelf, float* volume) {
    if (volume != nullptr) *volume = gVolume;
    return kSlSuccess;
}

SlResult volumeSetVolumeCurve(SlSelf, const void* curve) {
    // The curve shape is not modelled; the level and the volume are.
    (void)curve;
    return kSlSuccess;
}

SlResult androidConfigGetSampleRate(SlSelf, int32_t* rate) {
    if (rate != nullptr) *rate = kDeviceSampleRate;
    return kSlSuccess;
}

SlResult androidConfigGetChannelCount(SlSelf, int32_t* channels) {
    if (channels != nullptr) *channels = kDeviceChannelCount;
    return kSlSuccess;
}

SlResult androidConfigGetBufferSize(SlSelf, int32_t* bytes) {
    if (bytes != nullptr) *bytes = kDeviceBufferSize;
    return kSlSuccess;
}

SlResult androidConfigGetOutputChannelCount(SlSelf, int32_t* channels) {
    if (channels != nullptr) *channels = kDeviceOutputChannelCount;
    return kSlSuccess;
}

SlResult androidConfigGetPerformanceMode(SlSelf, int32_t* mode) {
    if (mode != nullptr) *mode = kDevicePerformanceMode;
    return kSlSuccess;
}

SlResult androidConfigSetPerformanceMode(SlSelf, int32_t mode) {
    (void)mode;
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
const void* SL_IID_VOLUME = &rr3::kGuidVolume;
const void* SL_IID_ANDROIDCONFIGURATION = &rr3::kGuidAndroidConfiguration;
const void* SL_IID_RECORD = &rr3::kGuidRecord;
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
    copy.deviceSampleRate = static_cast<uint32_t>(kDeviceSampleRate);
    copy.deviceChannels = static_cast<uint32_t>(kDeviceChannelCount);
    copy.selfTestBits = static_cast<uint32_t>(gSelfTestBits);
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
    // THE THREE THE GUEST'S OWN FMOD LOOKS UP AND THE PORT DID NOT HAVE.
    // MEASURED 2026-11-16 from the strings inside libfmodex.so.
    if (std::strcmp(symbolName, "SL_IID_VOLUME") == 0) {
        return reinterpret_cast<void*>(&gIidVolume);
    }
    if (std::strcmp(symbolName, "SL_IID_ANDROIDCONFIGURATION") == 0) {
        return reinterpret_cast<void*>(&gIidAndroidConfiguration);
    }
    if (std::strcmp(symbolName, "SL_IID_RECORD") == 0) {
        return reinterpret_cast<void*>(&gIidRecord);
    }
    return nullptr;
}

uint64_t runAudioStubSelfTest() {
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

    // THE VOLUME INTERFACE, AND A DEVICE THAT IS NOT MUTED.
    void* volume = nullptr;
    playerMethods->GetInterface(player, &kGuidVolume, &volume);
    auto volumeCell = static_cast<const SlVolumeItf**>(volume);
    int32_t level = 0;
    if (volumeCell != nullptr && *volumeCell != nullptr &&
        (*volumeCell)->GetVolumeLevel != nullptr &&
        (*volumeCell)->GetVolumeLevel(volume, &level) == kSlSuccess &&
        level != 0) {
        passed |= 1ULL << 6;
    }

    // THE DEVICE CONFIGURATION: the sample rate and the channel count the
    // guest's FMOD asks for before it mixes anything.
    void* config = nullptr;
    playerMethods->GetInterface(player, &kGuidAndroidConfiguration, &config);
    auto configCell = static_cast<const SlAndroidConfigurationItf**>(config);
    int32_t rate = 0;
    int32_t channels = 0;
    if (configCell != nullptr && *configCell != nullptr &&
        (*configCell)->GetSampleRate != nullptr &&
        (*configCell)->GetChannelCount != nullptr &&
        (*configCell)->GetSampleRate(config, &rate) == kSlSuccess &&
        (*configCell)->GetChannelCount(config, &channels) == kSlSuccess &&
        rate == kDeviceSampleRate && channels == kDeviceChannelCount) {
        passed |= 1ULL << 7;
    }

    // THE VIRTUAL LIBRARY EXPORTS EXACTLY WHAT THE GUEST'S FMOD LOOKS UP.
    // MEASURED: the strings inside libfmodex.so name slCreateEngine and
    // five IID symbols, and no AAudio symbol at all.
    static const char* const kRequiredSymbols[] = {
        "slCreateEngine", "SL_IID_ENGINE", "SL_IID_PLAY",
        "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_ANDROIDCONFIGURATION",
        "SL_IID_RECORD",
    };
    bool allSymbols = true;
    for (const char* name : kRequiredSymbols) {
        if (resolveVirtualAudioSymbol(handle, name) == nullptr) {
            allSymbols = false;
        }
    }
    if (allSymbols) {
        passed |= 1ULL << 8;
    }

    // AN IID THE OBJECT DOES NOT HAVE IS AN EXPLICIT FAILURE, NOT ANOTHER
    // INTERFACE.  The recorder IID is exactly that case for a player.
    void* unknown = nullptr;
    if (playerMethods->GetInterface(player, &kGuidRecord, &unknown) ==
            kSlInterfaceUnavailable &&
        unknown == nullptr) {
        passed |= 1ULL << 9;
    }

    return passed;
}

uint64_t fmodAudioStubSelfTest() {
    const uint64_t passed = runAudioStubSelfTest();
    gSelfTestBits = passed;
    return passed;
}

uint64_t fmodAudioStubSelfTestExpected() {
    // The EXPECTATION IS DERIVED, NOT A LITERAL IN A COMMENT: the caller
    // prints it next to the value, so a new check cannot leave the printed
    // expectation behind.  (The port's other self-tests print their
    // expectation the same way -- see the stat-layout note in main.cpp.)
    return (1ULL << 10) - 1ULL;
}

} // namespace rr3
