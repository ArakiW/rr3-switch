#pragma once

// Self-consistent FMOD Ex shim for the RR3 native rehost.
//
// The guest reaches FMOD through 52 undefined dynamic symbols in
// reverse/native/arm64/libRealRacing3.so. The real libfmodex.so is also a
// loaded guest image, and its DSP-graph code walks a circular node list whose
// head (this + 0xa4d0) is never sentinel-initialised, so the call returns
// nothing and the main thread spins on the resulting unmapped
// Read64 @ 0x8 / Read32 @ 0x1C0 pair forever.
//
// This translation unit provides the same 52 entry points with a real object
// graph: every handle is a distinct, non-null static object that owns its own
// state, and every node list is a proper circular list with a self-linked
// sentinel so a walk always terminates.
//
// The exported symbols are produced by declaring real `FMOD::System`,
// `FMOD::Sound`, ... classes and letting the compiler mangle them, so the
// names are byte-identical to the guest's imports by construction rather than
// by hand. `.build/fmod/verify_fmod_symbols.py` proves that against
// `objdump -T`.

#include <cstdint>

// ---------------------------------------------------------------------------
// FMOD Ex public types. Spelling and nesting are load-bearing: they are what
// the mangled guest symbol names encode.
// ---------------------------------------------------------------------------

enum FMOD_RESULT {
    FMOD_OK = 0,
    FMOD_ERR_INVALID_HANDLE = 1,
    FMOD_ERR_NOT_INITIALIZED = 2,
    FMOD_ERR_CHANNEL_STOLEN = 4,
};

enum FMOD_OPENSTATE {
    FMOD_OPENSTATE_LOADING = 0,
    FMOD_OPENSTATE_LOADING_CONNECTED = 1,
    FMOD_OPENSTATE_LOADING_BUFFERING = 2,
    FMOD_OPENSTATE_READY = 3,
    FMOD_OPENSTATE_ERROR = 4,
    FMOD_OPENSTATE_UNINITIALIZED = 5,
};

enum FMOD_CHANNEL_CALLBACKTYPE {
    FMOD_CHANNEL_CALLBACK_END = 0x00000000,
    FMOD_CHANNEL_CALLBACK_VIRTUALVOICE = 0x00000001,
    FMOD_CHANNEL_CALLBACK_STEAL = 0x00000002,
    FMOD_CHANNEL_CALLBACK_SYNTHCOMPLETE = 0x00000004,
    FMOD_CHANNEL_CALLBACK_DONE = 0x00000008,
    FMOD_CHANNEL_CALLBACK_PLAYING = 0x00000100,
    FMOD_CHANNEL_CALLBACK_SILENCE = 0x00000200,
    FMOD_CHANNEL_CALLBACK_RESUME = 0x00000400,
    FMOD_CHANNEL_CALLBACK_PAUSED = 0x00000800,
    FMOD_CHANNEL_CALLBACK_RESTART = 0x00001000,
};

enum FMOD_SPEAKERMODE {
    FMOD_SPEAKERMODE_DEFAULT = 0,
    FMOD_SPEAKERMODE_RAW = 1,
    FMOD_SPEAKERMODE_MONO = 2,
    FMOD_SPEAKERMODE_STEREO = 3,
    FMOD_SPEAKERMODE_QUAD = 4,
    FMOD_SPEAKERMODE_SURROUND = 5,
    FMOD_SPEAKERMODE_FIVEPOINTONE = 6,
    FMOD_SPEAKERMODE_SEVENPOINTONE = 7,
};

enum FMOD_OUTPUTTYPE {
    FMOD_OUTPUTTYPE_AUTODETECT = 0,
    FMOD_OUTPUTTYPE_UNKNOWN = 1,
    FMOD_OUTPUTTYPE_NOSOUND = 2,
    FMOD_OUTPUTTYPE_WAVWRITER = 3,
    // The shim claims OpenSL because fmod_audio_stub.cpp really does provide a
    // silent OpenSL device behind it. It is not a claim about a speaker:
    // fmodShimStats().audible stays 0 for the whole session.
    FMOD_OUTPUTTYPE_OPENSL = 12,
};

enum FMOD_SOUND_FORMAT {
    FMOD_SOUND_FORMAT_NONE = 0,
    FMOD_SOUND_FORMAT_PCM8 = 1,
    FMOD_SOUND_FORMAT_PCM16 = 2,
    FMOD_SOUND_FORMAT_PCM24 = 3,
    FMOD_SOUND_FORMAT_PCM32 = 4,
    FMOD_SOUND_FORMAT_PCMFLOAT = 5,
    FMOD_SOUND_FORMAT_BITSTREAM = 6,
};

enum FMOD_DSP_RESAMPLER {
    FMOD_DSP_RESAMPLER_DEFAULT = 0,
    FMOD_DSP_RESAMPLER_NOINTERP = 1,
    FMOD_DSP_RESAMPLER_LINEAR = 2,
    FMOD_DSP_RESAMPLER_CUBIC = 3,
    FMOD_DSP_RESAMPLER_SPLINE = 4,
};

enum FMOD_DSP_TYPE {
    FMOD_DSP_TYPE_UNKNOWN = 0,
    FMOD_DSP_TYPE_MIXER = 1,
    FMOD_DSP_TYPE_OSCILLATOR = 2,
    FMOD_DSP_TYPE_LOWPASS = 3,
    FMOD_DSP_TYPE_HIGHPASS = 5,
    FMOD_DSP_TYPE_ECHO = 6,
    FMOD_DSP_TYPE_FADER = 7,
    FMOD_DSP_TYPE_FLANGE = 8,
    FMOD_DSP_TYPE_DISTORTION = 9,
    FMOD_DSP_TYPE_NORMALIZE = 10,
    FMOD_DSP_TYPE_LIMITER = 11,
    FMOD_DSP_TYPE_PARAMEQ = 12,
    FMOD_DSP_TYPE_PITCHSHIFT = 13,
    FMOD_DSP_TYPE_CHORUS = 14,
    FMOD_DSP_TYPE_VSTPLUGIN = 15,
    FMOD_DSP_TYPE_COMPRESSOR = 18,
    FMOD_DSP_TYPE_SFXREVERB = 19,
    FMOD_DSP_TYPE_LOWPASS_SIMPLE = 20,
    FMOD_DSP_TYPE_DELAY = 21,
    FMOD_DSP_TYPE_TREMOLO = 22,
    FMOD_DSP_TYPE_SEND = 24,
    FMOD_DSP_TYPE_RETURN = 25,
    FMOD_DSP_TYPE_HIGHPASS_SIMPLE = 26,
    FMOD_DSP_TYPE_PAN = 27,
    FMOD_DSP_TYPE_THREE_EQ = 28,
    FMOD_DSP_TYPE_FFT = 29,
    FMOD_DSP_TYPE_LOUDNESS_METER = 30,
    FMOD_DSP_TYPE_ENVELOPEFOLLOWER = 31,
    FMOD_DSP_TYPE_CONVOLUTIONREVERB = 32,
    FMOD_DSP_TYPE_CHANNELMIX = 33,
    FMOD_DSP_TYPE_TRANSCEIVER = 34,
    FMOD_DSP_TYPE_OBJECTPAN = 35,
    FMOD_DSP_TYPE_MULTIBAND_EQ = 36,
};

enum FMOD_CHANNELINDEX {
    FMOD_CHANNEL_INVALID = -1,
    FMOD_CHANNEL_ALL = 0,
};

struct FMOD_VECTOR {
    float x;
    float y;
    float z;
};

// Only the leading fields are read, and only when the caller's cbsize covers
// them (offsets 0/4/8 are stable across every FMOD Ex release). The rest is
// padding so the type is a plausible size; nothing beyond cbsize is touched,
// because the guest filled this struct with the real FMOD layout, not ours.
struct FMOD_CREATESOUNDEXINFO {
    unsigned int cbsize;
    unsigned int length;
    unsigned int fileoffset;
    unsigned int decodebufferlength;
};

struct FMOD_DSP_DESCRIPTION {
    unsigned int pluginsdk_version;
    const char* name;
    unsigned int numinputbuffers;
    unsigned int numoutputbuffers;
    void* userdata;
};

// The guest imports FMOD_CHANNEL as a distinct named type in the callback
// signature (_ZN4FMOD7Channel11setCallbackEPF11FMOD_RESULTP12FMOD_CHANNEL25
// FMOD_CHANNEL_CALLBACKTYPEPvS5_E), so it must exist at global scope. It is
// declared before namespace FMOD because FMOD::Channel::setCallback names it.
namespace FMOD {
class Channel;
} // namespace FMOD

struct FMOD_CHANNEL {
    FMOD::Channel* channel;
};

namespace FMOD {

class Channel;
class ChannelGroup;
class DSP;
class Sound;
class System;

// ---------------------------------------------------------------------------
// Circular, self-linked node list.
//
// This is the exact structure the real libfmodex got wrong: it left the list
// head at 0 and then walked it. Here `sentinel.next` and `sentinel.prev` both
// point at `sentinel`, so an empty list is a one-node cycle and any walk
// terminates at the sentinel on the first step. appendNode() links before the
// sentinel, never after a null.
// ---------------------------------------------------------------------------
struct DspNode {
    DspNode* prev = nullptr;
    DspNode* next = nullptr;
    DSP* dsp = nullptr;
};

// A real, non-null connection object. FMOD hands one back from every addDSP so
// the caller can call setParameter on the connection; the game stores it and
// never dereferences it, but a null here would be the same bug class again.
// The connection also carries the graph node, which is how real FMOD does it:
// one append yields both the link object and the list node.
class DSPConnection {
public:
    DSP* input = nullptr;
    DSPConnection* next = nullptr;
    unsigned int index = 0;
    DspNode node{};
};

// ---------------------------------------------------------------------------
// Circular, self-linked node list (defined above, next to DSPConnection).
// ---------------------------------------------------------------------------
class DSP {
public:

    // Shim state. Kept in the object so `this` is always meaningful: no
    // side table, so there is no way for a live handle to lose its state.
    void* userData = nullptr;
    System* owner = nullptr;
    // The graph node this DSP was appended as, so release() can unlink it
    // instead of leaving a released node in the walk.
    DspNode* node = nullptr;
    FMOD_DSP_TYPE dspType = FMOD_DSP_TYPE_UNKNOWN;
    int parameterIndex = -1;
    float parameterValue = 0.0F;
    bool released = false;
};

class ChannelGroup {
public:

    System* owner = nullptr;
    float volume = 1.0F;
    unsigned int dspCount = 0;
    DspNode dspSentinel{};
    DspNode groupSentinel{};
    // Bounded storage the child-group nodes are handed out from. The list is
    // still a real cycle through groupSentinel; the array is only where its
    // nodes come from, so a walk always terminates on the sentinel.
    DspNode childNodes[8]{};
    unsigned int childCount = 0;
};

class Sound {
public:

    void* userData = nullptr;
    // Deterministic by decision: a Sound is READY from the moment it is
    // created. We never decode audio, so holding LOADING forever would be the
    // same stall in a new place -- and the guest polls this in a loop
    // (rr3+0xc05684) before it will release the sound from its event system.
    FMOD_OPENSTATE openState = FMOD_OPENSTATE_READY;
    unsigned int lengthMs = 1000;
    bool isStream = false;
    bool inUse = false;
    bool released = false;
    unsigned int reportedCbsize = 0;
    char name[64] = {0};
};

class Channel {
public:
    using Callback = FMOD_RESULT (*)(FMOD_CHANNEL*, FMOD_CHANNEL_CALLBACKTYPE, void*,
                                     void*);


    // Shim state.
    Sound* sound = nullptr;
    ChannelGroup* group = nullptr;
    void* userData = nullptr;
    Callback callback = nullptr;
    bool callbackSet = false;
    bool active = false;
    bool paused = false;
    bool finished = false;
    // 0 = nothing sent yet, 1 = PLAYING sent, 2 = END/DONE sent. One-shot
    // per channel: a bounded cadence, so a sound can never spin the event
    // system on a repeating callback.
    int callbackPhase = 0;
    unsigned int positionMs = 0;
    unsigned int lengthMs = 0;
    float frequency = 1.0F;
    float volume = 1.0F;
    float dopplerLevel = 1.0F;
    float minDistance = 1.0F;
    float maxDistance = 20.0F;
    int loopCount = 0;
    unsigned int mode = 0;
    FMOD_VECTOR position{0.0F, 0.0F, 0.0F};
    FMOD_VECTOR velocity{0.0F, 0.0F, 0.0F};
    // Bounded array, not a linked list: a channel's DSP chain is never walked
    // by the engine, so an array cannot present a walkable null head.
    DSP* dsps[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    unsigned int dspCount = 0;
};

class System {
public:

    // ---- shim state -------------------------------------------------------
    DspNode dspSentinel{};
    unsigned int dspCount = 0;
    bool initialised = false;
    bool released = false;
    int maxChannels = 512;
    unsigned int initFlags = 0;
    unsigned int dspClock = 0;
    unsigned int dspBufferLength = 1024;
    int dspNumBuffers = 2;
    int softwareRate = 48000;
    FMOD_SOUND_FORMAT softwareFormat = FMOD_SOUND_FORMAT_PCM16;
    int softwareChannels = 2;
    FMOD_DSP_RESAMPLER softwareResampler = FMOD_DSP_RESAMPLER_DEFAULT;
    FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_STEREO;
    int num3DListeners = 1;
    FMOD_VECTOR listenerPos{0.0F, 0.0F, 0.0F};
    FMOD_VECTOR listenerVel{0.0F, 0.0F, 0.0F};
    FMOD_VECTOR listenerForward{0.0F, 0.0F, 1.0F};
    FMOD_VECTOR listenerUp{0.0F, 1.0F, 0.0F};
};

} // namespace FMOD

// ----------------------------------------------------------------------
// The 52 guest entry points.
//
// Each is a free function taking the object as its first parameter, which is
// the same AArch64 AAPCS signature the corresponding FMOD member function has,
// so the guest can call it through a relocation. The __asm__ label pins the
// exported symbol to the exact mangled name the guest imports, which is why
// this works from symbol_resolver.cpp: a plain function pointer, not a
// pointer-to-member. Names come from .build/fmod/imports.txt (objdump -T).
// ----------------------------------------------------------------------
namespace FMOD {

FMOD_RESULT fmodSoundGetUserData(FMOD::Sound* self, void**) __asm__("_ZN4FMOD5Sound11getUserDataEPPv");
FMOD_RESULT fmodChannelGroupAddDSP(FMOD::ChannelGroup* self, FMOD::DSP*, FMOD::DSPConnection**) __asm__("_ZN4FMOD12ChannelGroup6addDSPEPNS_3DSPEPPNS_13DSPConnectionE");
FMOD_RESULT fmodChannelGroupAddGroup(FMOD::ChannelGroup* self, FMOD::ChannelGroup*) __asm__("_ZN4FMOD12ChannelGroup8addGroupEPS0_");
FMOD_RESULT fmodChannelGroupSetVolume(FMOD::ChannelGroup* self, float) __asm__("_ZN4FMOD12ChannelGroup9setVolumeEf");
FMOD_RESULT fmodDSPGetUserData(FMOD::DSP* self, void**) __asm__("_ZN4FMOD3DSP11getUserDataEPPv");
FMOD_RESULT fmodDSPSetParameter(FMOD::DSP* self, int, float) __asm__("_ZN4FMOD3DSP12setParameterEif");
FMOD_RESULT fmodDSPGetSystemObject(FMOD::DSP* self, FMOD::System**) __asm__("_ZN4FMOD3DSP15getSystemObjectEPPNS_6SystemE");
FMOD_RESULT fmodDSPRelease(FMOD::DSP* self) __asm__("_ZN4FMOD3DSP7releaseEv");
FMOD_RESULT fmodSoundSetUserData(FMOD::Sound* self, void*) __asm__("_ZN4FMOD5Sound11setUserDataEPv");
FMOD_RESULT fmodSoundGetOpenState(FMOD::Sound* self, FMOD_OPENSTATE*, unsigned int*, bool*, bool*) __asm__("_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_");
FMOD_RESULT fmodSoundRelease(FMOD::Sound* self) __asm__("_ZN4FMOD5Sound7releaseEv");
FMOD_RESULT fmodSoundGetLength(FMOD::Sound* self, unsigned int*, unsigned int) __asm__("_ZN4FMOD5Sound9getLengthEPjj");
FMOD_RESULT fmodSystemGetVersion(FMOD::System* self, unsigned int*) __asm__("_ZN4FMOD6System10getVersionEPj");
FMOD_RESULT fmodSystemCreateSound(FMOD::System* self, char const*, unsigned int, FMOD_CREATESOUNDEXINFO*, FMOD::Sound**) __asm__("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE");
FMOD_RESULT fmodSystemGetDSPClock(FMOD::System* self, unsigned int*, unsigned int*) __asm__("_ZN4FMOD6System11getDSPClockEPjS1_");
FMOD_RESULT fmodSystemCreateStream(FMOD::System* self, char const*, unsigned int, FMOD_CREATESOUNDEXINFO*, FMOD::Sound**) __asm__("_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE");
FMOD_RESULT fmodSystemSetSpeakerMode(FMOD::System* self, FMOD_SPEAKERMODE) __asm__("_ZN4FMOD6System14setSpeakerModeE16FMOD_SPEAKERMODE");
FMOD_RESULT fmodSystemCreateDSPByType(FMOD::System* self, FMOD_DSP_TYPE, FMOD::DSP**) __asm__("_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE");
FMOD_RESULT fmodSystemGetDSPBufferSize(FMOD::System* self, unsigned int*, int*) __asm__("_ZN4FMOD6System16getDSPBufferSizeEPjPi");
FMOD_RESULT fmodSystemSetDSPBufferSize(FMOD::System* self, unsigned int, int) __asm__("_ZN4FMOD6System16setDSPBufferSizeEji");
FMOD_RESULT fmodSystemGetSoftwareFormat(FMOD::System* self, int*, FMOD_SOUND_FORMAT*, int*, int*, FMOD_DSP_RESAMPLER*, int*) __asm__("_ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_");
FMOD_RESULT fmodSystemSet3DNumListeners(FMOD::System* self, int) __asm__("_ZN4FMOD6System17set3DNumListenersEi");
FMOD_RESULT fmodSystemSetSoftwareFormat(FMOD::System* self, int, FMOD_SOUND_FORMAT, int, int, FMOD_DSP_RESAMPLER) __asm__("_ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER");
FMOD_RESULT fmodSystemCreateChannelGroup(FMOD::System* self, char const*, FMOD::ChannelGroup**) __asm__("_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE");
FMOD_RESULT fmodSystemSet3DListenerAttributes(FMOD::System* self, int, FMOD_VECTOR const*, FMOD_VECTOR const*, FMOD_VECTOR const*, FMOD_VECTOR const*) __asm__("_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_");
FMOD_RESULT fmodSystemInit(FMOD::System* self, int, unsigned int, void*) __asm__("_ZN4FMOD6System4initEijPv");
FMOD_RESULT fmodSystemAddDSP(FMOD::System* self, FMOD::DSP*, FMOD::DSPConnection**) __asm__("_ZN4FMOD6System6addDSPEPNS_3DSPEPPNS_13DSPConnectionE");
FMOD_RESULT fmodSystemUpdate(FMOD::System* self) __asm__("_ZN4FMOD6System6updateEv");
FMOD_RESULT fmodSystemRelease(FMOD::System* self) __asm__("_ZN4FMOD6System7releaseEv");
FMOD_RESULT fmodSystemCreateDSP(FMOD::System* self, FMOD_DSP_DESCRIPTION*, FMOD::DSP**) __asm__("_ZN4FMOD6System9createDSPEP20FMOD_DSP_DESCRIPTIONPPNS_3DSPE");
FMOD_RESULT fmodSystemGetOutput(FMOD::System* self, FMOD_OUTPUTTYPE*) __asm__("_ZN4FMOD6System9getOutputEP15FMOD_OUTPUTTYPE");
FMOD_RESULT fmodSystemPlaySound(FMOD::System* self, FMOD_CHANNELINDEX, FMOD::Sound*, bool, FMOD::Channel**) __asm__("_ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE");
FMOD_RESULT fmodChannelGetPosition(FMOD::Channel* self, unsigned int*, unsigned int) __asm__("_ZN4FMOD7Channel11getPositionEPjj");
FMOD_RESULT fmodChannelGetUserData(FMOD::Channel* self, void**) __asm__("_ZN4FMOD7Channel11getUserDataEPPv");
FMOD_RESULT fmodChannelSetCallback(FMOD::Channel* self, FMOD_RESULT (*)(FMOD_CHANNEL*, FMOD_CHANNEL_CALLBACKTYPE, void*, void*)) __asm__("_ZN4FMOD7Channel11setCallbackEPF11FMOD_RESULTP12FMOD_CHANNEL25FMOD_CHANNEL_CALLBACKTYPEPvS5_E");
FMOD_RESULT fmodChannelSetPosition(FMOD::Channel* self, unsigned int, unsigned int) __asm__("_ZN4FMOD7Channel11setPositionEjj");
FMOD_RESULT fmodChannelSetUserData(FMOD::Channel* self, void*) __asm__("_ZN4FMOD7Channel11setUserDataEPv");
FMOD_RESULT fmodChannelSetFrequency(FMOD::Channel* self, float) __asm__("_ZN4FMOD7Channel12setFrequencyEf");
FMOD_RESULT fmodChannelSetLoopCount(FMOD::Channel* self, int) __asm__("_ZN4FMOD7Channel12setLoopCountEi");
FMOD_RESULT fmodChannelSetLoopPoints(FMOD::Channel* self, unsigned int, unsigned int, unsigned int, unsigned int) __asm__("_ZN4FMOD7Channel13setLoopPointsEjjjj");
FMOD_RESULT fmodChannelSet3DAttributes(FMOD::Channel* self, FMOD_VECTOR const*, FMOD_VECTOR const*) __asm__("_ZN4FMOD7Channel15set3DAttributesEPK11FMOD_VECTORS3_");
FMOD_RESULT fmodChannelSetChannelGroup(FMOD::Channel* self, FMOD::ChannelGroup*) __asm__("_ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE");
FMOD_RESULT fmodChannelSet3DDopplerLevel(FMOD::Channel* self, float) __asm__("_ZN4FMOD7Channel17set3DDopplerLevelEf");
FMOD_RESULT fmodChannelSet3DMinMaxDistance(FMOD::Channel* self, float, float) __asm__("_ZN4FMOD7Channel19set3DMinMaxDistanceEff");
FMOD_RESULT fmodChannelStop(FMOD::Channel* self) __asm__("_ZN4FMOD7Channel4stopEv");
FMOD_RESULT fmodChannelAddDSP(FMOD::Channel* self, FMOD::DSP*, FMOD::DSPConnection**) __asm__("_ZN4FMOD7Channel6addDSPEPNS_3DSPEPPNS_13DSPConnectionE");
FMOD_RESULT fmodChannelGetMode(FMOD::Channel* self, unsigned int*) __asm__("_ZN4FMOD7Channel7getModeEPj");
FMOD_RESULT fmodChannelSetMode(FMOD::Channel* self, unsigned int) __asm__("_ZN4FMOD7Channel7setModeEj");
FMOD_RESULT fmodChannelGetPaused(FMOD::Channel* self, bool*) __asm__("_ZN4FMOD7Channel9getPausedEPb");
FMOD_RESULT fmodChannelSetPaused(FMOD::Channel* self, bool) __asm__("_ZN4FMOD7Channel9setPausedEb");
FMOD_RESULT fmodChannelSetVolume(FMOD::Channel* self, float) __asm__("_ZN4FMOD7Channel9setVolumeEf");

} // namespace FMOD

// The one unmangled C entry point the guest imports.
extern "C" FMOD_RESULT FMOD_System_Create(FMOD::System** system);

namespace rr3 {

// Honest telemetry. Nothing here claims audible output: there is no Horizon
// Android audio device, and `audible` is always 0 like fmod_audio_stub.
struct FmodShimStats {
    uint32_t systemCreates{};
    uint32_t systemInits{};
    uint32_t systemUpdates{};
    uint32_t soundCreates{};
    uint32_t streamCreates{};
    uint32_t dspCreates{};
    uint32_t channelGroupCreates{};
    uint32_t addDspCalls{};
    uint32_t listAppends{};
    uint32_t listUnlinks{};
    uint32_t channelsStarted{};
    uint32_t callbacksQueued{};
    uint32_t callbacksDelivered{};
    uint32_t callbackDoneDelivered{};
    uint32_t dspReleased{};
    uint32_t selfTestBits{};
    // Always 0. The shim reports FMOD_OUTPUTTYPE_OPENSL, which is backed by
    // the silent OpenSL device in fmod_audio_stub.cpp, never by a speaker.
    uint32_t audible{};
};

FmodShimStats fmodShimStats();

// Self-test bits (run once, lazily, from the first System::init):
//   0  System::init returns FMOD_OK
//   1  the DSP list head is a self-linked sentinel straight after a reset
//   2  one addDSP leaves a walkable, terminating list of length 1
//   3  two addDSPs leave a walkable, terminating list of length 2
//   4  DSP getUserData/setUserData round-trips a real pointer
//   5  createSound then getOpenState reports FMOD_OPENSTATE_READY
//   6  setCallback + System::update delivers FMOD_CHANNEL_CALLBACK_END once
//   7  playSound hands back a real, non-null Channel
//   8  ChannelGroup::addDSP list is walkable and terminating
//   9  Channel getUserData/setUserData round-trips a real pointer
//  10  DSP::getSystemObject returns the owning System
//  11  getSoftwareFormat tolerates null out-parameters
uint64_t fmodShimSelfTest();

// Forces one summary line into the guest log. Safe to call when idle.
void fmodShimLogCounters(const char* reason);

} // namespace rr3
