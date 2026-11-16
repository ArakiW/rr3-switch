# -*- coding: utf-8 -*-
"""The OpenSL device, part two: exact IID answers, the missing exports, and
a self-test that can fail.
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


# ------------------------------------------------------- 1. the stub itself
path = os.path.join(ROOT, 'native-switch', 'source', 'fmod_audio_stub.cpp')
text, crlf, bom = load(path)

old = """constexpr uint32_t kSlParameterInvalid = 0x00000008;
"""
new = """constexpr uint32_t kSlParameterInvalid = 0x00000008;
// SL_RESULT_INTERFACE_UNAVAILABLE: the honest answer to "this object has no
// such interface".  It is what a real backend returns, and it is what stops
// a caller from reading another interface's vtable as if it were its own.
constexpr uint32_t kSlInterfaceUnavailable = 0x00000005;
"""
text = sub(text, old, new, 'stub: the unavailable result code')

old = """SlResult engineGetInterface(SlSelf, SlIid iid, void** out) {
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
"""
new = """SlResult engineGetInterface(SlSelf, SlIid iid, void** out) {
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
"""
text = sub(text, old, new, 'stub: engineGetInterface')

old = """SlResult playerGetInterface(SlSelf, SlIid iid, void** out) {
    if (out == nullptr) return kSlParameterInvalid;
    if (iid == &kGuidPlay) {
        *out = const_cast<void*>(static_cast<const void*>(&gPlayItfCell));
        return kSlSuccess;
    }
    // Buffer-queue IID or unknown: the path that deadlocks if missing.
    *out = const_cast<void*>(static_cast<const void*>(&gBufferQueueItfCell));
    return kSlSuccess;
}
"""
new = """SlResult playerGetInterface(SlSelf, SlIid iid, void** out) {
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
"""
text = sub(text, old, new, 'stub: playerGetInterface')

old = """    if (std::strcmp(symbolName, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0) {
        return reinterpret_cast<void*>(&gIidAndroidSimpleBufferQueue);
    }
    return nullptr;
}
"""
new = """    if (std::strcmp(symbolName, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0) {
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
"""
text = sub(text, old, new, 'stub: the new exports')

old = """const void* SL_IID_ENGINE = &rr3::kGuidEngine;
"""
new = """const void* SL_IID_VOLUME = &rr3::kGuidVolume;
const void* SL_IID_ANDROIDCONFIGURATION = &rr3::kGuidAndroidConfiguration;
const void* SL_IID_RECORD = &rr3::kGuidRecord;
const void* SL_IID_ENGINE = &rr3::kGuidEngine;
"""
text = sub(text, old, new, 'stub: the new IID symbols')

# ------------------------------------------------------ the self-test bits
old = """    void* handle = virtualAudioLibraryHandle("libOpenSLES.so");
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
"""
new = """    void* handle = virtualAudioLibraryHandle("libOpenSLES.so");
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

uint64_t fmodAudioStubSelfTestExpected() {
    // The EXPECTATION IS DERIVED, NOT A LITERAL IN A COMMENT: the caller
    // prints it next to the value, so a new check cannot leave the printed
    // expectation behind.  (The port's other self-tests print their
    // expectation the same way -- see the stat-layout note in main.cpp.)
    return (1ULL << 10) - 1ULL;
}
"""
text = sub(text, old, new, 'stub: the new self-test bits')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.cpp (part 2)')

# ------------------------------------------------------------ 2. the header
path = os.path.join(ROOT, 'native-switch', 'include', 'rr3', 'fmod_audio_stub.hpp')
text, crlf, bom = load(path)
old = """// Self-test bits:
//  bit0 slCreateEngine silent success
//  bit1 Realize + GetInterface engine
//  bit2 CreateOutputMix / CreateAudioPlayer silent success
//  bit3 Enqueue discards without blocking and without claiming audible output
//  bit4 AAudio stream create returns a clear unavailable error
//  bit5 Virtual OpenSL / AAudio dlopen handles resolve entry points
uint64_t fmodAudioStubSelfTest();
"""
new = """// Self-test bits:
//  bit0 slCreateEngine silent success
//  bit1 Realize + GetInterface engine
//  bit2 CreateOutputMix / CreateAudioPlayer silent success
//  bit3 Enqueue discards without blocking and without claiming audible output
//  bit4 AAudio stream create returns a clear unavailable error
//  bit5 Virtual OpenSL / AAudio dlopen handles resolve entry points
//  bit6 the volume interface answers, and the device reports a LIVE level
//  bit7 the Android configuration interface reports the device's format
//  bit8 the virtual libOpenSLES.so exports every symbol the guest's real
//       FMOD looks up (MEASURED from the strings inside libfmodex.so)
//  bit9 an IID the object does not have is an EXPLICIT failure
//
// THE EXPECTED MASK IS A FUNCTION, NOT A LITERAL: the boot log prints it
// next to the value, so a new check cannot leave the printed expectation
// behind.
uint64_t fmodAudioStubSelfTest();
uint64_t fmodAudioStubSelfTestExpected();
"""
text = sub(text, old, new, 'stub header: the self-test bits')
old = """    // Always 0. There is no Horizon Android audio device behind this stub.
    uint32_t audible{};
};
"""
new = """    // Always 0. There is no Horizon Android audio device behind this stub.
    uint32_t audible{};
    // The device's own format: what it would produce if it produced
    // anything.  It matches what the guest's FMOD asks the port for.
    uint32_t deviceSampleRate{};
    uint32_t deviceChannels{};
};
"""
text = sub(text, old, new, 'stub header: the device format')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.hpp')

# --------------------------------------------- 3. the stats carry the format
path = os.path.join(ROOT, 'native-switch', 'source', 'fmod_audio_stub.cpp')
text, crlf, bom = load(path)
old = """FmodAudioStubStats fmodAudioStubStats() {
    FmodAudioStubStats copy = gStats;
    copy.audible = 0;
    return copy;
}
"""
new = """FmodAudioStubStats fmodAudioStubStats() {
    FmodAudioStubStats copy = gStats;
    copy.audible = 0;
    copy.deviceSampleRate = static_cast<uint32_t>(kDeviceSampleRate);
    copy.deviceChannels = static_cast<uint32_t>(kDeviceChannelCount);
    return copy;
}
"""
text = sub(text, old, new, 'stub: the stats carry the device format')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.cpp (stats)')
