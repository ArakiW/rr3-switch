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

// THE GUEST'S GENERATION, NOT MODERN FMOD 5.
//
// MEASURED FROM THE GUEST BINARY, NOT FROM THIS PORT'S MEMORY OF FMOD.
// libRealRacing3.so calls FMOD::Sound::getOpenState in exactly ONE place,
// 0xc05684, and the very next thing it does with the answer is
//   0xc0568c  cbz w8, 0xc056d4
// state == 0 takes the branch that calls playSound (0xc0570c); ANY other
// value takes the "not ready" path that notifies the event system instead
// (0xc056a4 cmp w9,#0 / 0xc056a8 cset w1,eq passes (state==0) to it).
//
// => FOR THIS GUEST, 0 IS "READY TO PLAY".
//
// THE PORT USED TO REPORT READY = 3 (modern FMOD 5 numbering).  THAT IS WHY
// THE PORT HAS PRODUCED NO SOUND AT ALL: the guest asked, the port answered
// 3, and the guest never took the play branch.  The port's own self-test
// compared the shim against the port's own constant, so it reported 0xfff
// while the guest heard nothing -- the instrument was answering the ledger,
// not the reality.
//
// THE SHAPE, FROM TWO INDEPENDENT PROOFS.
//
// 1. READY == 0 is proven by the GUEST: 0xc0568c takes the play branch only
//    for 0 (see above).
// 2. THE LIBRARY'S PUBLIC openstate HAS SEVEN VALUES, 0..6: the guest's own
//    libfmodex.so (FMODEx/4.44.65) compares an openstate against 6 at a
//    call site, and its public value is the RAW internal field value with
//    NO mapping table -- so 6 is a member of the public enum.
//
// => The only shape that satisfies both is seven members with READY at the
//    front.  FMOD's relative order of the other five has never changed, so
//    the assignment below is DERIVED, not invented.
//
// WHAT THE GUEST ACTUALLY DISTINGUISHES: only 0 against everything else.
// The port only ever REPORTS READY, so the other six numbers are never
// exchanged with the guest -- and the port's shim and the guest's own
// library are never mixed (the FMOD lever is all-or-nothing).
enum FMOD_OPENSTATE {
    FMOD_OPENSTATE_READY = 0,
    FMOD_OPENSTATE_LOADING = 1,
    FMOD_OPENSTATE_LOADING_CONNECTED = 2,
    FMOD_OPENSTATE_LOADING_BUFFERING = 3,
    FMOD_OPENSTATE_ERROR = 4,
    FMOD_OPENSTATE_UNINITIALIZED = 5,
    FMOD_OPENSTATE_MAX = 6,
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

// THE GUEST'S GENERATION.
//
// MEASURED: libRealRacing3.so 0xc05d6c `mov w1, #2` passes 2 to
// setSpeakerMode, and at the same time asks for 2 output channels
// (0xc05e90 `mov w4, #2`).  In the guest's generation 2 IS stereo.
//
// The port's modern list had DEFAULT = 0, so ITS 2 meant MONO: the guest's
// stereo request was recorded as mono, and the port's own default was a
// different number than the guest's stereo.
enum FMOD_SPEAKERMODE {
    FMOD_SPEAKERMODE_RAW = 0,
    FMOD_SPEAKERMODE_MONO = 1,
    FMOD_SPEAKERMODE_STEREO = 2,
    FMOD_SPEAKERMODE_QUAD = 3,
    FMOD_SPEAKERMODE_SURROUND = 4,
    FMOD_SPEAKERMODE_FIVEPOINTONE = 5,
    FMOD_SPEAKERMODE_SEVENPOINTONE = 6,
    FMOD_SPEAKERMODE_SEVENPOINTONE_4 = 7,
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
    // THE GUEST'S GENERATION: THE SAME LIST WITH "FADER" DELETED.
    //
    // MEASURED: the guest creates seven DSPs and the type numbers it
    // passes are 6, 11, 5, 10, 18, 8, 7 (libRealRacing3.so 0xc06a20,
    // 0xc06aa4, 0xc06ae4, 0xc06b2c, 0xc06b70, 0xc06b98, 0xc06bb8).  In the
    // guest's generation those are ECHO, PARAMEQ, HIGHPASS, LIMITER,
    // SFXREVERB, DISTORTION and FLANGE.  The port's list had FADER at 7,
    // which pushed every later entry up by one, so 5 of the 7 the guest
    // asks for were the wrong unit: its LIMITER was the port's PARAMEQ,
    // its SFXREVERB the port's COMPRESSOR, its DISTORTION the port's
    // FLANGE, its FLANGE the port's FADER.
    FMOD_DSP_TYPE_HIGHPASS = 5,
    FMOD_DSP_TYPE_ECHO = 6,
    FMOD_DSP_TYPE_FLANGE = 7,
    FMOD_DSP_TYPE_DISTORTION = 8,
    FMOD_DSP_TYPE_NORMALIZE = 9,
    FMOD_DSP_TYPE_LIMITER = 10,
    FMOD_DSP_TYPE_PARAMEQ = 11,
    FMOD_DSP_TYPE_PITCHSHIFT = 12,
    FMOD_DSP_TYPE_CHORUS = 13,
    FMOD_DSP_TYPE_VSTPLUGIN = 14,
    FMOD_DSP_TYPE_COMPRESSOR = 17,
    FMOD_DSP_TYPE_SFXREVERB = 18,
    FMOD_DSP_TYPE_LOWPASS_SIMPLE = 19,
    FMOD_DSP_TYPE_DELAY = 20,
    FMOD_DSP_TYPE_TREMOLO = 21,
    FMOD_DSP_TYPE_SEND = 23,
    FMOD_DSP_TYPE_RETURN = 24,
    FMOD_DSP_TYPE_HIGHPASS_SIMPLE = 25,
    FMOD_DSP_TYPE_PAN = 26,
    FMOD_DSP_TYPE_THREE_EQ = 27,
    FMOD_DSP_TYPE_FFT = 28,
    FMOD_DSP_TYPE_LOUDNESS_METER = 29,
    FMOD_DSP_TYPE_ENVELOPEFOLLOWER = 30,
    FMOD_DSP_TYPE_CONVOLUTIONREVERB = 31,
    FMOD_DSP_TYPE_CHANNELMIX = 32,
    FMOD_DSP_TYPE_TRANSCEIVER = 33,
    FMOD_DSP_TYPE_OBJECTPAN = 34,
    FMOD_DSP_TYPE_MULTIBAND_EQ = 35,
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

// THE GUEST'S OWN LAYOUT, AT THE GUEST'S OWN OFFSETS.
//
// MEASURED FROM libRealRacing3.so: the guest's createSound at 0xc075ec
// fills a 216-byte struct and passes its address (0xc075dc
// `add x3, sp, #0x68`).  The fields it actually writes are cbsize
// at +0x00 (216), numchannels at +0x0c, defaultfrequency at +0x10, format
// at +0x14, decodebufferlength at +0x18, and a PCM read callback at +0x38.
//
// THE PORT'S OLD STRUCT WAS 16 BYTES and put decodebufferlength at +0x0c,
// WHERE THE GUEST'S numchannels LIVES.  It did not fault -- the guest's
// cbsize of 216 satisfied the port's "is this struct big enough" guard --
// it just read the WRONG FIELDS, which is the quieter failure.
//
// The size is 0xd8 = 216 so that a guest struct and this one agree on every
// offset, and so that a cbsize of 216 is accepted without a partial read.
struct FMOD_CREATESOUNDEXINFO {
    unsigned int cbsize;               // +0x00
    unsigned int length;               // +0x04
    unsigned int fileoffset;           // +0x08
    unsigned int numchannels;          // +0x0c
    unsigned int defaultfrequency;     // +0x10
    unsigned int format;               // +0x14
    unsigned int decodebufferlength;   // +0x18
    unsigned int initialsubsound;      // +0x1c
    unsigned int numsubsounds;         // +0x20
    unsigned int reserved1;            // +0x24
    unsigned int reserved2;            // +0x28
    unsigned int reserved3;            // +0x2c
    unsigned int reserved4;            // +0x30
    unsigned int reserved5;            // +0x34
    void* pcmreadcallback;             // +0x38
    unsigned char reservedTail[152];    // +0x40 .. +0xd7
};

// THE SIZE IS NOT A CONVENTION.  The guest writes cbsize = 216
// (libRealRacing3.so 0xc075c0) and passes a struct of that size, so a
// port struct of a different size reads the guest's fields at the wrong
// offsets -- silently, because the guest's cbsize still satisfies the
// port's "is this struct big enough" guard.
static_assert(sizeof(FMOD_CREATESOUNDEXINFO) == 216,
              "the guest's FMOD_CREATESOUNDEXINFO is 216 bytes "
              "(libRealRacing3.so 0xc075c0)");

// THE CALLBACK TYPES, AS FMOD Ex 4.x DEFINES THEM (fmod_dsp.h).  The GUEST'S
// OWN read callback takes six arguments, which is what fixes the shape:
// FMOD_RESULT read(FMOD_DSP_STATE* state, float* inbuffer, float* outbuffer,
//                   unsigned int length, int inchannels, int outchannels).
typedef int (*FMOD_DSP_CREATECALLBACK)(void* state);
typedef int (*FMOD_DSP_RELEASECALLBACK)(void* state);
typedef int (*FMOD_DSP_RESETCALLBACK)(void* state);
typedef int (*FMOD_DSP_READCALLBACK)(void* state, float* inbuffer,
                                     float* outbuffer, unsigned int length,
                                     int inchannels, int outchannels);
typedef int (*FMOD_DSP_SETPOSITIONCALLBACK)(void* state, unsigned int position);
typedef int (*FMOD_DSP_SETPARAMCALLBACK)(void* state, int param, float value);
typedef int (*FMOD_DSP_GETPARAMCALLBACK)(void* state, int param, float* value,
                                         char* valuestr);

// ---------------------------------------------------------------------------
// FMOD_DSP_DESCRIPTION, AS THE GUEST BUILDS IT.
//
// NOT FMOD Studio 5.  The guest's libfmodex.so is FMOD Ex 4.x, and the GUEST
// ITSELF fills this struct, so the guest's binary is the authority.
//
// MEASURED IN THE GUEST (libRealRacing3.so; the only call site is 0xc05cd4,
// the PLT stub for _ZN4FMOD6System9createDSPEP20FMOD_DSP_DESCRIPTIONPPNS_3DSPE):
//
//   0xc05c6c  strncpy(desc, "decklink", 32) -> the name is an INLINE char[32]
//                                              at offset 0, NOT a pointer
//   0xc05ccc  str d0, [desc, #32]            -> version=1, channels=0
//   0xc05c8c  stp x9, x10, [desc, #40]       -> create@0x28, release@0x30
//   0xc05cb0  str x8, [desc, #56]            -> reset@0x38
//   0xc05ca8  stp x9, xzr, [desc, #64]       -> read@0x40 (THE REAL AUDIO
//                                                CALLBACK), setposition@0x48
//   0xc05cac  str wzr, [desc, #80]           -> numparameters@0x50
//   0xc05c84  str q0, [desc, #88]            -> paramdesc@0x58, setparameter
//                                                @0x60
//   0xc05c88  str q0, [desc, #104]           -> getparameter@0x68, config@0x70
//   0xc05cd0  stp xzr, x8, [desc, #120]      -> configwidth@0x78,
//                                                configheight@0x7C,
//                                                userdata@0x80
//
// AND THE REAL libfmodex.so AGREES: its createDSP copies the description
// field by field out of exactly these offsets (0x4f3e0-0x4f44c) into a
// 136-byte (0x88) internal struct, and strcpy's the name out of offset 0
// (0xb1128).
//
// THE SIZE IS THE POINT: 136 bytes with the name INLINE.  The old
// definition (pluginsdk_version plus a const char* name) was a different
// library's layout, and it made the port unable to read the guest's own
// description at all -- which is why the guest's audio callback could never
// be called.
struct FMOD_DSP_DESCRIPTION {
    char name[32];
    unsigned int version;
    int channels;
    FMOD_DSP_CREATECALLBACK create;
    FMOD_DSP_RELEASECALLBACK release;
    FMOD_DSP_RESETCALLBACK reset;
    FMOD_DSP_READCALLBACK read;
    FMOD_DSP_SETPOSITIONCALLBACK setposition;
    int numparameters;
    void* paramdesc;
    FMOD_DSP_SETPARAMCALLBACK setparameter;
    FMOD_DSP_GETPARAMCALLBACK getparameter;
    void* config;
    int configwidth;
    int configheight;
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
    // THE GUEST'S DESCRIPTION, KEPT.  It is the only thing that
    // carries the guest's audio callback.
    FMOD_DSP_DESCRIPTION* description = nullptr;
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
    // THE GUEST'S OWN exinfo FIELDS, AT THE GUEST'S OWN OFFSETS.
    // See FMOD_CREATESOUNDEXINFO and the measurement notes there.
    unsigned int numChannels = 0;
    unsigned int defaultFrequency = 0;
    unsigned int format = 0;
    unsigned int decodeBufferLength = 0;
    void* pcmReadCallback = nullptr;
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
    // WHAT THE GUEST LAST ASKED FOR.  The port's device is fixed, so this is
    // telemetry, not a capability: a request is not a device.
    int requestedRate = 0;
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
    // THE OPENSTATE GATE, COUNTED.  The guest asks getOpenState and takes
    // its play branch ONLY when the answer is 0 (libRealRacing3.so 0xc0568c
    // `cbz w8, 0xc056d4`).  openStateQueries is every question and
    // openStateReady is every question that was answered READY -- and every
    // one of those is a sound the guest will actually start.
    uint32_t openStateQueries{};
    uint32_t openStateReady{};
    // WHAT THE GUEST LAST ASKED THE MIXER FOR.  The port's device is fixed
    // at 48000 / 2ch / PCM16, so this is telemetry: it says whether the
    // guest wanted something the port does not have.
    int32_t requestedRate{};
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
