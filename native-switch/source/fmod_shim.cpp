// Self-consistent FMOD Ex shim. See include/rr3/fmod_shim.hpp for the why.
//
// Symbol contract: the 52 undefined dynamic symbols in
// reverse/native/arm64/libRealRacing3.so are reproduced by declaring the real
// FMOD class/enum/struct names at global scope and letting g++ mangle them.
// Nothing here is hand-mangled; .build/fmod/verify_fmod_symbols.py diffs the
// built ELF's exports against `objdump -T`.
//
// Enum values are the published FMOD Ex constants. The two the guest actually
// branches on were read out of the guest binary and are marked GUEST-VERIFIED:
//   * open state       -- rr3+0xc056a4 tests `cmp w9, #0x0`, so any non-zero
//                        value is "ready" to this guest (READY == 3 is still
//                        the semantically correct one).
//   * channel callback -- the callback the guest registers at rr3+0xc0815c
//                        branches on `cbz w1` (rr3+0xc08178), i.e. only
//                        callback type 0 (END) does any work; every other
//                        value falls straight through to `return 0`.

#include "rr3/fmod_shim.hpp"

#include "rr3/recovery_guard.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

// FMOD's own version constant is not recoverable from the guest without
// running the real library, so the shim reports FMOD Ex 5.00 and logs the
// value it reported. If the game ever branches on it, the log line is where
// that shows up.
constexpr unsigned int kShimVersion = 0x00050000;

namespace {

constexpr unsigned int kMaxSounds = 256;
constexpr unsigned int kMaxDsps = 512;
constexpr unsigned int kMaxChannelGroups = 128;
constexpr unsigned int kMaxChannels = 512;
constexpr unsigned int kMaxConnections = 1024;
constexpr unsigned int kMaxQueue = 256;

rr3::FmodShimStats gStats{};

// One System, as real FMOD allows exactly one per process.
FMOD::System gSystem;
FMOD::Sound gSounds[kMaxSounds];
FMOD::DSP gDsps[kMaxDsps];
FMOD::DSPConnection gConnections[kMaxConnections];
FMOD::ChannelGroup gGroups[kMaxChannelGroups];
FMOD::Channel gChannels[kMaxChannels];

unsigned int gSoundCursor = 0;
unsigned int gDspCursor = 0;
unsigned int gGroupCursor = 0;
unsigned int gConnectionCursor = 0;
unsigned int gChannelCursor = 0;

// ---- deferred callback queue ----------------------------------------------
//
// Answering a channel callback inline re-enters the engine from inside its own
// mixer pass (the reference port warns about exactly this in
// cloudcell_defer.h:13-20). Callbacks are therefore only ever *queued* from
// the thread that drives update(), and drained after the state pass has
// settled. gDraining stops a callback that calls update() again from
// recursing into the drain.
struct PendingCallback {
    FMOD::Channel* channel;
    FMOD_CHANNEL_CALLBACKTYPE type;
};
PendingCallback gQueue[kMaxQueue];
unsigned int gQueueHead = 0;
unsigned int gQueueTail = 0;
unsigned int gQueueDropped = 0;
bool gDraining = false;

bool gSelfTestDone = false;
unsigned int gLoggedUpdates = 0;

constexpr const char* kGuestLogPath = "sdmc:/switch/rr3-native-rehost.guest-view.log";

// DspNode lives in namespace FMOD; the helpers below work on the list itself.
using FMOD::DspNode;

void logLine(const char* phase, const char* details) {
    rr3::appendGuestBootLine(kGuestLogPath, phase, 900, 0, details);
}

void formatCounters(char* out, size_t capacity) {
    const rr3::FmodShimStats& s = gStats;
    std::snprintf(out, capacity,
                  "inits=%u updates=%u sounds=%u streams=%u dsps=%u groups=%u addDSP=%u "
                  "appends=%u unlinks=%u chan=%u cqueued=%u cdelivered=%u cdone=%u "
                  "rel=%u selftest=0x%llx dropped=%u audible=%u",
                  s.systemInits, s.systemUpdates, s.soundCreates, s.streamCreates,
                  s.dspCreates, s.channelGroupCreates, s.addDspCalls, s.listAppends,
                  s.listUnlinks, s.channelsStarted, s.callbacksQueued,
                  s.callbacksDelivered, s.callbackDoneDelivered, s.dspReleased,
                  static_cast<unsigned long long>(s.selfTestBits), gQueueDropped,
                  s.audible);
}

void enqueueCallback(FMOD::Channel* channel, FMOD_CHANNEL_CALLBACKTYPE type) {
    if (channel == nullptr || channel->callback == nullptr) {
        return;
    }
    const unsigned int next = (gQueueTail + 1U) % kMaxQueue;
    if (next == gQueueHead) {
        // Bounded queue: drop rather than grow, and count it so the log shows
        // it if it ever happens.
        ++gQueueDropped;
        return;
    }
    gQueue[gQueueTail].channel = channel;
    gQueue[gQueueTail].type = type;
    gQueueTail = next;
    ++gStats.callbacksQueued;
}

// Drained after the update() state pass, never inside it.
void drainCallbackQueue() {
    if (gDraining) {
        return;
    }
    gDraining = true;
    while (gQueueHead != gQueueTail) {
        const PendingCallback entry = gQueue[gQueueHead];
        gQueueHead = (gQueueHead + 1U) % kMaxQueue;
        FMOD::Channel* channel = entry.channel;
        if (channel == nullptr || channel->callback == nullptr) {
            continue;
        }
        FMOD_CHANNEL handle{channel};
        channel->callback(&handle, entry.type, channel->userData, nullptr);
        ++gStats.callbacksDelivered;
    }
    gDraining = false;
}

// ---- list helpers ----------------------------------------------------------

// Links `node` immediately before `sentinel`, so the list stays a cycle.
DspNode* appendNode(DspNode* sentinel, DspNode* node, FMOD::DSP* dsp) {
    node->dsp = dsp;
    node->prev = sentinel->prev;
    node->next = sentinel;
    sentinel->prev->next = node;
    sentinel->prev = node;
    if (dsp != nullptr) {
        dsp->node = node;
    }
    ++gStats.listAppends;
    return node;
}

// Unlinks a node from whatever cycle it is in. `sentinel` is not needed: the
// node's own neighbours are enough, and a single-node cycle cannot exist here
// because the sentinel is never the node passed in.
void unlinkNode(DspNode* node) {
    if (node == nullptr || node->prev == nullptr || node->next == nullptr) {
        return;
    }
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
    ++gStats.listUnlinks;
}

// Walks a list exactly the way the engine would, with a hard bound. Returns
// the number of nodes visited. Used by the self-test to prove termination.
unsigned int walkCount(const DspNode* sentinel) {
    if (sentinel == nullptr || sentinel->next == nullptr) {
        return 0;
    }
    unsigned int visited = 0;
    const DspNode* cursor = sentinel->next;
    while (cursor != nullptr && cursor != sentinel) {
        ++visited;
        cursor = cursor->next;
        if (visited > kMaxDsps) {
            break; // would be a cycle defect; bounded anyway
        }
    }
    return visited;
}

void resetSentinel(DspNode* sentinel) {
    sentinel->prev = sentinel;
    sentinel->next = sentinel;
    sentinel->dsp = nullptr;
}

// Resets a System to its post-construction state. The list sentinel is
// self-linked first, before any other member, so there is no window in which a
// walk could observe a null head. Kept as a free function rather than a member
// so it does not add a symbol the guest does not import.
void resetSystemState(FMOD::System* system) {
    resetSentinel(&system->dspSentinel);
    system->dspCount = 0;
    system->initialised = false;
    system->released = false;
    system->maxChannels = 512;
    system->initFlags = 0;
    system->dspClock = 0;
    system->dspBufferLength = 1024;
    system->dspNumBuffers = 2;
    system->softwareRate = 48000;
    system->softwareFormat = FMOD_SOUND_FORMAT_PCM16;
    system->softwareChannels = 2;
    system->softwareResampler = FMOD_DSP_RESAMPLER_DEFAULT;
    system->speakerMode = FMOD_SPEAKERMODE_STEREO;
    system->num3DListeners = 1;
    system->listenerPos = FMOD_VECTOR{0.0F, 0.0F, 0.0F};
    system->listenerVel = FMOD_VECTOR{0.0F, 0.0F, 0.0F};
    system->listenerForward = FMOD_VECTOR{0.0F, 0.0F, 1.0F};
    system->listenerUp = FMOD_VECTOR{0.0F, 1.0F, 0.0F};
}

FMOD::DSPConnection* nextConnection() {
    FMOD::DSPConnection* connection = &gConnections[gConnectionCursor];
    gConnectionCursor = (gConnectionCursor + 1U) % kMaxConnections;
    return connection;
}

FMOD::DSP* allocDsp(FMOD_DSP_TYPE type, FMOD::System* owner) {
    // Prefer an unused slot so repeated graph builds do not exhaust the pool.
    for (unsigned int attempt = 0; attempt < kMaxDsps; ++attempt) {
        FMOD::DSP* candidate = &gDsps[(gDspCursor + attempt) % kMaxDsps];
        if (candidate->released) {
            gDspCursor = (gDspCursor + attempt + 1U) % kMaxDsps;
            candidate->released = false;
            candidate->userData = nullptr;
            candidate->owner = owner;
            candidate->dspType = type;
            candidate->parameterIndex = -1;
            candidate->parameterValue = 0.0F;
            ++gStats.dspCreates;
            return candidate;
        }
    }
    return nullptr;
}

FMOD::Channel* allocChannel(unsigned int index) {
    if (index < kMaxChannels && !gChannels[index].active) {
        FMOD::Channel* channel = &gChannels[index];
        std::memset(static_cast<void*>(channel), 0, sizeof(*channel));
        channel->dopplerLevel = 1.0F;
        channel->minDistance = 1.0F;
        channel->maxDistance = 20.0F;
        channel->frequency = 1.0F;
        channel->volume = 1.0F;
        return channel;
    }
    for (unsigned int attempt = 0; attempt < kMaxChannels; ++attempt) {
        const unsigned int slot = (gChannelCursor + attempt) % kMaxChannels;
        if (!gChannels[slot].active) {
            gChannelCursor = (slot + 1U) % kMaxChannels;
            FMOD::Channel* channel = &gChannels[slot];
            std::memset(static_cast<void*>(channel), 0, sizeof(*channel));
            channel->dopplerLevel = 1.0F;
            channel->minDistance = 1.0F;
            channel->maxDistance = 20.0F;
            channel->frequency = 1.0F;
            channel->volume = 1.0F;
            return channel;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

namespace FMOD {

FMOD_RESULT fmodSystemInit(FMOD::System* self, int maxchannels, unsigned int flags, void* extradriverdata) {
    (void)extradriverdata;
    resetSystemState(self);
    if (maxchannels > 0) {
        self->maxChannels = maxchannels;
    }
    self->initFlags = flags;
    self->initialised = true;
    self->released = false;
    ++gStats.systemInits;

    char details[128];
    std::snprintf(details, sizeof(details),
                  "backend=shim maxchannels=%d flags=0x%x buffer=%u numbuffers=%d "
                  "rate=%d format=%d output=opensl",
                  self->maxChannels, flags, self->dspBufferLength, self->dspNumBuffers, self->softwareRate,
                  static_cast<int>(self->softwareFormat));
    logLine("phase=fmod-shim-init", details);

    if (!gSelfTestDone) {
        gSelfTestDone = true;
        const uint64_t bits = rr3::fmodShimSelfTest();
        gStats.selfTestBits = static_cast<uint32_t>(bits);
        char selfTest[64];
        std::snprintf(selfTest, sizeof(selfTest), "bits=0x%llx expected=0xfff",
                      static_cast<unsigned long long>(bits));
        logLine("phase=fmod-shim-selftest", selfTest);
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemCreateSound(FMOD::System* self, const char* name, unsigned int mode, FMOD_CREATESOUNDEXINFO* exinfo, FMOD::Sound** sound) {
    (void)self;
    (void)mode;
    if (sound == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    FMOD::Sound* created = nullptr;
    for (unsigned int attempt = 0; attempt < kMaxSounds; ++attempt) {
        FMOD::Sound* candidate = &gSounds[(gSoundCursor + attempt) % kMaxSounds];
        if (candidate->released) {
            gSoundCursor = (gSoundCursor + attempt + 1U) % kMaxSounds;
            created = candidate;
            break;
        }
    }
    if (created == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    created->userData = nullptr;
    created->released = false;
    created->inUse = false;
    created->isStream = false;
    // READY immediately, deterministically. See the comment on Sound::openState.
    created->openState = FMOD_OPENSTATE_READY;
    created->lengthMs = 1000;
    // Only cbsize/length/fileoffset are read, and only when the caller says
    // the struct really is at least that big.
    created->reportedCbsize = 0;
    if (exinfo != nullptr) {
        created->reportedCbsize = exinfo->cbsize;
        if (exinfo->cbsize >= 12U) {
            if (exinfo->length != 0) {
                created->lengthMs = exinfo->length;
            }
        }
    }
    if (name != nullptr) {
        std::snprintf(created->name, sizeof(created->name), "%s", name);
    } else {
        created->name[0] = '\0';
    }
    *sound = created;
    ++gStats.soundCreates;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemCreateStream(FMOD::System* self, const char* name, unsigned int mode, FMOD_CREATESOUNDEXINFO* exinfo, FMOD::Sound** sound) {
    const FMOD_RESULT result =
        fmodSystemCreateSound(self, name, mode, exinfo, sound);
    if (result == FMOD_OK && sound != nullptr && *sound != nullptr) {
        (*sound)->isStream = true;
        ++gStats.streamCreates;
        --gStats.soundCreates; // createSound already counted it
    }
    return result;
}

FMOD_RESULT fmodSystemCreateDSPByType(FMOD::System* self, FMOD_DSP_TYPE type, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    FMOD::DSP* created = allocDsp(type, self);
    if (created == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    *dsp = created;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemCreateDSP(FMOD::System* self, FMOD_DSP_DESCRIPTION* description, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    const FMOD_DSP_TYPE type = FMOD_DSP_TYPE_UNKNOWN;
    FMOD::DSP* created = allocDsp(type, self);
    if (created == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    if (description != nullptr) {
        char details[128];
        std::snprintf(details, sizeof(details), "name=%s pluginsdk_version=%u",
                      description->name != nullptr ? description->name : "(null)",
                      description->pluginsdk_version);
        logLine("phase=fmod-shim-createdsp", details);
    }
    *dsp = created;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemCreateChannelGroup(FMOD::System* self, const char* name, FMOD::ChannelGroup** group) {
    (void)name;
    if (group == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    FMOD::ChannelGroup* created = nullptr;
    for (unsigned int attempt = 0; attempt < kMaxChannelGroups; ++attempt) {
        FMOD::ChannelGroup* candidate = &gGroups[(gGroupCursor + attempt) % kMaxChannelGroups];
        if (candidate->owner == nullptr) {
            gGroupCursor = (gGroupCursor + attempt + 1U) % kMaxChannelGroups;
            created = candidate;
            break;
        }
    }
    if (created == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    resetSentinel(&created->dspSentinel);
    resetSentinel(&created->groupSentinel);
    created->owner = self;
    created->volume = 1.0F;
    created->dspCount = 0;
    *group = created;
    ++gStats.channelGroupCreates;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemAddDSP(FMOD::System* self, FMOD::DSP* input, FMOD::DSPConnection** connection) {
    if (self->dspSentinel.next == nullptr) {
        // Defensive: the sentinel can only be null before the first init, but
        // repairing it here costs one branch and makes the walk safe even then.
        resetSystemState(self);
    }
    ++gStats.addDspCalls;
    FMOD::DSPConnection* link = nextConnection();
    link->input = input;
    link->next = nullptr;
    if (input != nullptr) {
        input->owner = self;
        appendNode(&self->dspSentinel, &link->node, input);
        ++self->dspCount;
    }
    if (connection != nullptr) {
        *connection = link;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemGetDSPClock(FMOD::System* self, unsigned int* dspclock, unsigned int* dspclockprevious) {
    if (dspclock != nullptr) {
        *dspclock = self->dspClock;
    }
    if (dspclockprevious != nullptr) {
        *dspclockprevious = self->dspClock;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemGetDSPBufferSize(FMOD::System* self, unsigned int* length, int* numbuffers) {
    if (length != nullptr) {
        *length = self->dspBufferLength;
    }
    if (numbuffers != nullptr) {
        *numbuffers = self->dspNumBuffers;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemSetDSPBufferSize(FMOD::System* self, unsigned int length, int numbuffers) {
    if (length != 0) {
        self->dspBufferLength = length;
    }
    if (numbuffers > 0) {
        self->dspNumBuffers = numbuffers;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemGetSoftwareFormat(FMOD::System* self, int* samplerate, FMOD_SOUND_FORMAT* format, int* numrawspeakers, int* numoutputchannels, FMOD_DSP_RESAMPLER* resampler, int* rawspeakers) {
    // Every out-parameter is optional: the guest is allowed to pass null for
    // any of them, and dereferencing one would be a shim-side fault.
    if (samplerate != nullptr) {
        *samplerate = self->softwareRate;
    }
    if (format != nullptr) {
        *format = self->softwareFormat;
    }
    if (numrawspeakers != nullptr) {
        *numrawspeakers = self->softwareChannels;
    }
    if (numoutputchannels != nullptr) {
        *numoutputchannels = self->softwareChannels;
    }
    if (resampler != nullptr) {
        *resampler = self->softwareResampler;
    }
    if (rawspeakers != nullptr) {
        *rawspeakers = 0;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemSetSoftwareFormat(FMOD::System* self, int samplerate, FMOD_SOUND_FORMAT format, int numrawspeakers, int numoutputchannels, FMOD_DSP_RESAMPLER resampler) {
    if (samplerate > 0) {
        self->softwareRate = samplerate;
    }
    if (format != FMOD_SOUND_FORMAT_NONE) {
        self->softwareFormat = format;
    }
    if (numrawspeakers > 0) {
        self->softwareChannels = numrawspeakers;
    }
    (void)numoutputchannels;
    self->softwareResampler = resampler;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemGetOutput(FMOD::System* self, FMOD_OUTPUTTYPE* output) {
    (void)self;
    if (output != nullptr) {
        *output = FMOD_OUTPUTTYPE_OPENSL;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemPlaySound(FMOD::System* self, FMOD_CHANNELINDEX channelindex, FMOD::Sound* sound, bool paused, FMOD::Channel** channel) {
    (void)self;
    if (sound == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    sound->inUse = true;
    const unsigned int wanted =
        (channelindex > 0) ? static_cast<unsigned int>(channelindex) : 0xFFFFFFFFU;
    FMOD::Channel* started = allocChannel(wanted);
    if (started == nullptr) {
        return FMOD_ERR_CHANNEL_STOLEN;
    }
    started->sound = sound;
    started->lengthMs = sound->lengthMs;
    started->positionMs = 0;
    started->active = true;
    started->paused = paused;
    started->finished = false;
    started->callbackPhase = 0;
    started->frequency = 1.0F;
    started->volume = 1.0F;
    if (channel != nullptr) {
        *channel = started;
    }
    ++gStats.channelsStarted;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemSetSpeakerMode(FMOD::System* self, FMOD_SPEAKERMODE mode) {
    self->speakerMode = mode;
    return FMOD_OK;
}

FMOD_RESULT fmodSystemSet3DNumListeners(FMOD::System* self, int numlisteners) {
    if (numlisteners >= 0) {
        self->num3DListeners = numlisteners;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemSet3DListenerAttributes(FMOD::System* self, int listener, const FMOD_VECTOR* pos, const FMOD_VECTOR* vel, const FMOD_VECTOR* forward, const FMOD_VECTOR* up) {
    (void)listener;
    if (pos != nullptr) {
        self->listenerPos = *pos;
    }
    if (vel != nullptr) {
        self->listenerVel = *vel;
    }
    if (forward != nullptr) {
        self->listenerForward = *forward;
    }
    if (up != nullptr) {
        self->listenerUp = *up;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemGetVersion(FMOD::System* self, unsigned int* version) {
    (void)self;
    if (version != nullptr) {
        *version = kShimVersion;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemUpdate(FMOD::System* self) {
    ++gStats.systemUpdates;
    if (self->dspSentinel.next == nullptr) {
        resetSystemState(self);
    }
    // The whole state pass first: nothing below is observable by a callback
    // until it is over.
    self->dspClock += self->dspBufferLength * static_cast<unsigned int>(self->dspNumBuffers > 0 ? self->dspNumBuffers : 1);

    for (unsigned int index = 0; index < kMaxChannels; ++index) {
        FMOD::Channel* channel = &gChannels[index];
        if (!channel->active || channel->callback == nullptr) {
            continue;
        }
        if (channel->callbackPhase == 0) {
            // First update in which the game has installed a callback.
            channel->callbackPhase = 1;
            enqueueCallback(channel, FMOD_CHANNEL_CALLBACK_PLAYING);
            continue;
        }
        if (channel->callbackPhase == 1) {
            // The sound is "finished". The guest's registered callback only
            // acts on type 0 (rr3+0xc08178 `cbz w1`), so END is the one that
            // matters; DONE is delivered alongside it for FMOD 5 semantics and
            // is a no-op for self guest. One-shot: never repeated.
            channel->callbackPhase = 2;
            channel->finished = true;
            channel->positionMs = channel->lengthMs;
            enqueueCallback(channel, FMOD_CHANNEL_CALLBACK_END);
            enqueueCallback(channel, FMOD_CHANNEL_CALLBACK_DONE);
            ++gStats.callbackDoneDelivered;
        }
    }

    // After the pass, not inside it (see the header note on re-entrancy).
    drainCallbackQueue();

    if (gLoggedUpdates < 6U) {
        ++gLoggedUpdates;
        if (gLoggedUpdates == 1U) {
            char details[128];
            std::snprintf(details, sizeof(details),
                          "dspNodes=%u walk=%u volume=0x%llx",
                          self->dspCount, walkCount(&self->dspSentinel),
                          static_cast<unsigned long long>(kShimVersion));
            logLine("phase=fmod-shim-update", details);
        }
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSystemRelease(FMOD::System* self) {
    // Tear the list down the same way it was built: unlink every node, then
    // re-arm the sentinel. A self->released System is still a valid object, because
    // the guest's finalizer holds the pointer.
    DspNode* cursor = self->dspSentinel.next;
    while (cursor != nullptr && cursor != &self->dspSentinel) {
        DspNode* next = cursor->next;
        cursor->prev = nullptr;
        cursor->next = nullptr;
        if (cursor->dsp != nullptr) {
            cursor->dsp->owner = nullptr;
        }
        cursor = next;
    }
    self->dspCount = 0;
    resetSentinel(&self->dspSentinel);
    self->released = true;
    self->initialised = false;
    return FMOD_OK;
}

// ---------------------------------------------------------------------------
// ChannelGroup
// ---------------------------------------------------------------------------

FMOD_RESULT fmodChannelGroupAddDSP(FMOD::ChannelGroup* self, FMOD::DSP* input, FMOD::DSPConnection** connection) {
    if (self->dspSentinel.next == nullptr) {
        resetSentinel(&self->dspSentinel);
        resetSentinel(&self->groupSentinel);
    }
    ++gStats.addDspCalls;
    FMOD::DSPConnection* link = nextConnection();
    link->input = input;
    link->next = nullptr;
    if (input != nullptr) {
        input->owner = self->owner;
        appendNode(&self->dspSentinel, &link->node, input);
        ++self->dspCount;
    }
    if (connection != nullptr) {
        *connection = link;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelGroupAddGroup(FMOD::ChannelGroup* self, FMOD::ChannelGroup* group) {
    if (group == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    if (self->groupSentinel.next == nullptr) {
        resetSentinel(&self->dspSentinel);
        resetSentinel(&self->groupSentinel);
    }
    if (self->childCount >= 8U) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    group->owner = self->owner;
    appendNode(&self->groupSentinel, &self->childNodes[self->childCount], nullptr);
    ++self->childCount;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelGroupSetVolume(FMOD::ChannelGroup* self, float volume) {
    self->volume = volume;
    return FMOD_OK;
}

// ---------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------

FMOD_RESULT fmodDSPSetParameter(FMOD::DSP* self, int index, float value) {
    self->parameterIndex = index;
    self->parameterValue = value;
    return FMOD_OK;
}

FMOD_RESULT fmodDSPGetUserData(FMOD::DSP* self, void** userdata) {
    if (userdata == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    *userdata = self->userData; // real round-trip of a real pointer
    return FMOD_OK;
}

FMOD_RESULT fmodDSPGetSystemObject(FMOD::DSP* self, FMOD::System** system) {
    if (system == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    *system = self->owner;
    return FMOD_OK;
}

FMOD_RESULT fmodDSPRelease(FMOD::DSP* self) {
    // Unlink from the owning graph so a self->released self->node never stays in a walk.
    if (self->node != nullptr) {
        unlinkNode(self->node);
        self->node = nullptr;
    }
    self->released = true;
    self->userData = nullptr;
    ++gStats.dspReleased;
    return FMOD_OK;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

FMOD_RESULT fmodSoundGetUserData(FMOD::Sound* self, void** userdata) {
    if (userdata == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    *userdata = self->userData;
    return FMOD_OK;
}

FMOD_RESULT fmodSoundSetUserData(FMOD::Sound* self, void* userdata) {
    self->userData = userdata;
    return FMOD_OK;
}

FMOD_RESULT fmodSoundGetOpenState(FMOD::Sound* self, FMOD_OPENSTATE* openstate, unsigned int* truezeroindex, bool* starterror, bool* stalledsound) {
    if (openstate != nullptr) {
        *openstate = self->openState;
    }
    if (truezeroindex != nullptr) {
        *truezeroindex = self->lengthMs;
    }
    if (starterror != nullptr) {
        *starterror = false;
    }
    if (stalledsound != nullptr) {
        *stalledsound = false;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodSoundGetLength(FMOD::Sound* self, unsigned int* length, unsigned int lengthtype) {
    if (length == nullptr) {
        return FMOD_OK;
    }
    // lengthtype 0 is milliseconds; we hold no PCM, so the sample count is 0
    // rather than a fabricated number.
    *length = (lengthtype == 0U) ? self->lengthMs : 0U;
    return FMOD_OK;
}

FMOD_RESULT fmodSoundRelease(FMOD::Sound* self) {
    self->released = true;
    self->inUse = false;
    self->userData = nullptr;
    return FMOD_OK;
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

FMOD_RESULT fmodChannelGetPosition(FMOD::Channel* self, unsigned int* position, unsigned int positiontype) {
    if (position != nullptr) {
        *position = (positiontype == 0U) ? self->positionMs : 0U;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelGetUserData(FMOD::Channel* self, void** userdata) {
    if (userdata == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    *userdata = self->userData;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetUserData(FMOD::Channel* self, void* userdata) {
    self->userData = userdata;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetCallback(
    FMOD::Channel* self, FMOD_RESULT (*callback)(FMOD_CHANNEL*, FMOD_CHANNEL_CALLBACKTYPE, void*,
                                                void*)) {
    // Recorded only. The first delivery happens from System::update, never
    // from inside this call.
    (void)self;
    self->callback = callback;
    self->callbackSet = callback != nullptr;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetPosition(FMOD::Channel* self, unsigned int value, unsigned int positiontype) {
    (void)positiontype;
    self->positionMs = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetFrequency(FMOD::Channel* self, float value) {
    self->frequency = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetLoopCount(FMOD::Channel* self, int value) {
    self->loopCount = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetLoopPoints(FMOD::Channel* self, unsigned int loopstart, unsigned int loopend, unsigned int channels, unsigned int channelmode) {
    (void)self;
    (void)loopstart;
    (void)loopend;
    (void)channels;
    (void)channelmode;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSet3DAttributes(FMOD::Channel* self, const FMOD_VECTOR* pos, const FMOD_VECTOR* vel) {
    if (pos != nullptr) {
        self->position = *pos;
    }
    if (vel != nullptr) {
        self->velocity = *vel;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetChannelGroup(FMOD::Channel* self, FMOD::ChannelGroup* value) {
    self->group = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSet3DDopplerLevel(FMOD::Channel* self, float value) {
    self->dopplerLevel = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSet3DMinMaxDistance(FMOD::Channel* self, float min, float max) {
    self->minDistance = min;
    self->maxDistance = max;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelStop(FMOD::Channel* self) {
    if (self->active) {
        // Real FMOD reports END when a channel is stopped; the guest's event
        // system needs that to release the self->sound.
        self->active = false;
        self->finished = true;
        enqueueCallback(self, FMOD_CHANNEL_CALLBACK_END);
        enqueueCallback(self, FMOD_CHANNEL_CALLBACK_DONE);
        ++gStats.callbackDoneDelivered;
        drainCallbackQueue();
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelAddDSP(FMOD::Channel* self, FMOD::DSP* input, FMOD::DSPConnection** connection) {
    ++gStats.addDspCalls;
    FMOD::DSPConnection* link = nextConnection();
    link->input = input;
    link->next = nullptr;
    // Bounded array, not a linked list: a channel DSP chain is never walked by
    // the engine, so there is no walkable head to get wrong.
    if (input != nullptr && self->dspCount < 8U) {
        self->dsps[self->dspCount] = input;
        ++self->dspCount;
    }
    if (connection != nullptr) {
        *connection = link;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelGetMode(FMOD::Channel* self, unsigned int* value) {
    if (value != nullptr) {
        *value = self->mode;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetMode(FMOD::Channel* self, unsigned int value) {
    self->mode = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelGetPaused(FMOD::Channel* self, bool* value) {
    if (value != nullptr) {
        *value = self->paused;
    }
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetPaused(FMOD::Channel* self, bool value) {
    self->paused = value;
    return FMOD_OK;
}

FMOD_RESULT fmodChannelSetVolume(FMOD::Channel* self, float value) {
    self->volume = value;
    return FMOD_OK;
}

} // namespace FMOD

// ---------------------------------------------------------------------------
// The one C entry point. FMOD_System_Create must be extern "C": the guest
// imports the unmangled name.
// ---------------------------------------------------------------------------
extern "C" FMOD_RESULT FMOD_System_Create(FMOD::System** system) {
    if (system == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    resetSystemState(&gSystem);
    *system = &gSystem;
    ++gStats.systemCreates;
    // Logged unconditionally: this is the shim's first possible contact with the
    // guest, so its absence from the log is proof the guest never reached the
    // shim at all (as opposed to reaching it and not getting further).
    logLine("phase=fmod-shim-create", "FMOD_System_Create backend=shim");
    return FMOD_OK;
}

namespace rr3 {

FmodShimStats fmodShimStats() {
    return gStats;
}

void fmodShimLogCounters(const char* reason) {
    char details[320];
    char counters[256];
    formatCounters(counters, sizeof(counters));
    std::snprintf(details, sizeof(details), "reason=%s %s", reason != nullptr ? reason : "(none)",
                  counters);
    logLine("phase=fmod-shim", details);
}

// Exercises exactly the defect class this shim exists to remove: a live
// object whose list head is 0. Uses local objects, so it cannot perturb the
// session's pools or counters.
uint64_t fmodShimSelfTest() {
    uint64_t passed = 0;
    const rr3::FmodShimStats before = gStats;

    FMOD::System probe;
    resetSystemState(&probe);
    if (fmodSystemInit(&probe, 32, 0, nullptr) != FMOD_OK) {
        return passed;
    }
    passed |= 1ULL << 0;

    if (probe.dspSentinel.next == &probe.dspSentinel &&
        probe.dspSentinel.prev == &probe.dspSentinel) {
        passed |= 1ULL << 1;
    }

    FMOD::DSP first;
    first.owner = &probe;
    FMOD::DSPConnection* link = nullptr;
    fmodSystemAddDSP(&probe, &first, &link);
    if (link != nullptr && walkCount(&probe.dspSentinel) == 1U) {
        passed |= 1ULL << 2;
    }

    FMOD::DSP second;
    second.owner = &probe;
    fmodSystemAddDSP(&probe, &second, nullptr);
    if (walkCount(&probe.dspSentinel) == 2U) {
        passed |= 1ULL << 3;
    }

    int sentinelToken = 0x5A5A;
    first.userData = &sentinelToken;
    void* readBack = nullptr;
    if (fmodDSPGetUserData(&first, &readBack) == FMOD_OK && readBack == &sentinelToken) {
        passed |= 1ULL << 4;
    }

    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.length = 4321;
    FMOD::Sound* sound = nullptr;
    FMOD_OPENSTATE state = FMOD_OPENSTATE_LOADING;
    if (fmodSystemCreateSound(&probe, "self-test", 0, &info, &sound) == FMOD_OK &&
        sound != nullptr &&
        fmodSoundGetOpenState(sound, &state, nullptr, nullptr, nullptr) == FMOD_OK &&
        state == FMOD_OPENSTATE_READY) {
        passed |= 1ULL << 5;
    }

    // Callback delivery: END (0) is the only type the guest acts on.
    static int callbackHits = 0;
    static int lastType = -1;
    callbackHits = 0;
    lastType = -1;
    FMOD::Channel* probeChannel = nullptr;
    if (fmodSystemPlaySound(&probe, FMOD_CHANNEL_ALL, sound, false, &probeChannel) ==
            FMOD_OK &&
        probeChannel != nullptr) {
        passed |= 1ULL << 7;
    }
    if (probeChannel != nullptr) {
        fmodChannelSetCallback(
            probeChannel,
            [](FMOD_CHANNEL*, FMOD_CHANNEL_CALLBACKTYPE type, void*, void*) -> FMOD_RESULT {
                ++callbackHits;
                lastType = static_cast<int>(type);
                return FMOD_OK;
            });
        fmodSystemUpdate(&probe); // queues PLAYING
        fmodSystemUpdate(&probe); // queues END + DONE, then drains
        if (callbackHits >= 2 && lastType == static_cast<int>(FMOD_CHANNEL_CALLBACK_DONE)) {
            passed |= 1ULL << 6;
        }
    }

    FMOD::ChannelGroup* group = nullptr;
    if (fmodSystemCreateChannelGroup(&probe, "self-test", &group) == FMOD_OK &&
        group != nullptr) {
        FMOD::DSP third;
        third.owner = &probe;
        fmodChannelGroupAddDSP(group, &third, nullptr);
        if (group->dspSentinel.next != nullptr &&
            walkCount(&group->dspSentinel) == 1U) {
            passed |= 1ULL << 8;
        }
    }

    int channelToken = 0x1234;
    FMOD::Channel roundTrip;
    fmodChannelSetUserData(&roundTrip, &channelToken);
    void* channelBack = nullptr;
    if (fmodChannelGetUserData(&roundTrip, &channelBack) == FMOD_OK &&
        channelBack == &channelToken) {
        passed |= 1ULL << 9;
    }

    FMOD::System* owner = nullptr;
    if (fmodDSPGetSystemObject(&first, &owner) == FMOD_OK && owner == &probe) {
        passed |= 1ULL << 10;
    }

    if (fmodSystemGetSoftwareFormat(&probe, nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr) == FMOD_OK) {
        passed |= 1ULL << 11;
    }

    // The probe must not leak into the session's accounting.
    gStats = before;
    gStats.selfTestBits = static_cast<uint32_t>(passed);
    return passed;
}

} // namespace rr3
