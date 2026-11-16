# -*- coding: utf-8 -*-
"""The OpenSL device, completed for the guest's real FMOD.

MEASURED 2026-11-16 (strings in the guest's own libfmodex.so): the library
looks up exactly ONE C entry point and FIVE IID symbols, and never touches
AAudio:

    slCreateEngine
    SL_IID_ENGINE  SL_IID_PLAY  SL_IID_ANDROIDSIMPLEBUFFERQUEUE
    SL_IID_ANDROIDCONFIGURATION  SL_IID_RECORD

The port's virtual libOpenSLES.so exported FOUR of them and was missing
SL_IID_ANDROIDCONFIGURATION -- the interface that tells the device its
sample rate and channel count -- and SL_IID_RECORD.

The other half of the gap is the shape of the answers:

  * there was NO volume interface, so a GetInterface(SL_IID_VOLUME) got
    the BUFFER QUEUE's vtable (the old code returned the queue for any
    unrecognised IID) and read a garbage level -- which is what
    "FMOD_ZERO_VOLUME_VIRTUALISATION" in the guest's log looks like;
  * the engine answered SL_RESULT_INTERFACE_UNAVAILABLE for the Play and
    queue IIDs but returned ITSELF for every unknown IID, which is the same
    hazard one level down.

So this change:
  * adds the two missing IID symbols (and the volume one), with their
    GUIDs;
  * implements the volume interface (level in millibels, volume as a float)
    and the Android configuration interface (sample rate, channels, buffer
    size, output channels, performance mode);
  * makes BOTH GetInterface paths IID-driven: the right interface for a
    known IID, SL_RESULT_INTERFACE_UNAVAILABLE for an unknown one.  NEVER a
    different interface's vtable.
  * extends the device's self-test with the new behaviour.

The guest-visible semantics of what already worked do not change:
slCreateEngine still succeeds, Enqueue still discards (and still fires the
callback inline, so no mixer thread ever waits for hardware), and audible
is still 0.
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    with io.open(path, 'rb') as handle:
        raw = handle.read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    if bom:
        raw = raw[3:]
    crlf = b'\r\n' in raw
    return raw.decode('utf-8').replace('\r\n', '\n'), crlf, bom


def save(path, text, crlf, bom):
    if crlf:
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    with io.open(path, 'wb') as handle:
        handle.write(data)


def sub(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit('FAIL %s: expected 1 occurrence, found %d' % (label, count))
    return text.replace(old, new)


path = os.path.join(ROOT, 'native-switch', 'source', 'fmod_audio_stub.cpp')
text, crlf, bom = load(path)

# ---------------------------------------------------------------- the GUIDs
old = """const SlInterfaceGuid kGuidAndroidSimpleBufferQueue{
    0x01147F9D, 0xB460, 0x11E1, 0xB4B6, {0x00, 0x02, 0xA5, 0xD5, 0xC5, 0x1B}};
"""
new = """const SlInterfaceGuid kGuidAndroidSimpleBufferQueue{
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
"""
text = sub(text, old, new, 'stub: the missing GUIDs')

old = """const SlInterfaceGuid* gIidAndroidSimpleBufferQueue =
    &kGuidAndroidSimpleBufferQueue;
"""
new = """const SlInterfaceGuid* gIidAndroidSimpleBufferQueue =
    &kGuidAndroidSimpleBufferQueue;
const SlInterfaceGuid* gIidVolume = &kGuidVolume;
const SlInterfaceGuid* gIidAndroidConfiguration = &kGuidAndroidConfiguration;
const SlInterfaceGuid* gIidRecord = &kGuidRecord;
"""
text = sub(text, old, new, 'stub: the missing IID pointers')

# ------------------------------------------------- the new interface shapes
old = """using SlBufferCallback = void (*)(void* queue, void* context);
struct SlBufferQueueItf {
    SlResult (*Enqueue)(SlSelf, const void*, uint32_t);
    SlResult (*Clear)(SlSelf);
    SlResult (*GetState)(SlSelf, void*);
    SlResult (*RegisterCallback)(SlSelf, SlBufferCallback, void*);
};
"""
new = """using SlBufferCallback = void (*)(void* queue, void* context);
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
"""
text = sub(text, old, new, 'stub: the new interface shapes')

# ------------------------------------------------------ the new vtables/cells
old = """const SlBufferQueueItf kBufferQueueItfVtable{
    bufferEnqueue, bufferClear, bufferGetState, bufferRegisterCallback,
};
"""
new = """const SlBufferQueueItf kBufferQueueItfVtable{
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
"""
text = sub(text, old, new, 'stub: the new vtables')

old = """const SlBufferQueueItf* gBufferQueueItfCell = &kBufferQueueItfVtable;
"""
new = """const SlBufferQueueItf* gBufferQueueItfCell = &kBufferQueueItfVtable;
const SlVolumeItf* gVolumeItfCell = &kVolumeItfVtable;
const SlAndroidConfigurationItf* gAndroidConfigItfCell =
    &kAndroidConfigItfVtable;
"""
text = sub(text, old, new, 'stub: the new cells')

# ----------------------------------------------- the forward declarations
old = """SlResult bufferRegisterCallback(SlSelf, SlBufferCallback callback,
                                void* context);
"""
new = """SlResult bufferRegisterCallback(SlSelf, SlBufferCallback callback,
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
"""
text = sub(text, old, new, 'stub: the new forward declarations')

# --------------------------------------------------- the device's own state
old = """FmodAudioStubStats gStats{};
uint32_t gPlayState = 1; // SL_PLAYSTATE_STOPPED
"""
new = """FmodAudioStubStats gStats{};
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
"""
text = sub(text, old, new, 'stub: the device state')

# ------------------------------------------------- the new implementations
old = """SlResult bufferRegisterCallback(SlSelf, SlBufferCallback callback,
                                void* context) {
"""
new = """SlResult volumeSetVolumeLevel(SlSelf, int32_t level) {
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
"""
text = sub(text, old, new, 'stub: the new implementations')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.cpp (part 1)')
