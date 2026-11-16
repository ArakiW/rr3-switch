# -*- coding: utf-8 -*-
"""The P1 instrument, and the lever that makes R1 an A/B instead of a rebuild.

P1 IS "DOES THE GUEST PRODUCE PCM FOR THIS DEVICE".  The device now counts
what the guest enqueues and reads the format the guest asked for, so the
answer is a number in the census rather than an opinion.

THE LEVER.  The FMOD interceptions are applied by NAME LOOKUP during the
guest's relocation (resolveHostSymbol), so a runtime switch can withhold
them: "fmod=real" in sdmc:/switch/rr3-census.conf makes the port stop
answering the guest's FMOD symbols, and the guest's own libfmodex.so
provides them instead.  THE DEFAULT IS THE PORT'S SHIM -- what every
released build does -- so the switch is the only thing that changes, and
the A/B needs no rebuild.

WHY IT HAS TO BE ALL AT ONCE: a partial withholding recreates the
mixed-layout failure the shim exists to end (the real library reading the
port's objects at its own offsets).  The switch withholds ALL of the guest's
FMOD symbols, never a few.
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


# ================================================== 1. the device: the P1 counters
path = os.path.join(ROOT, 'native-switch', 'source', 'fmod_audio_stub.cpp')
text, crlf, bom = load(path)

old = """// SLDataFormat_PCM: the format the guest hands the device.
"""
new = """// SLDataFormat_PCM: the format the guest hands the device.
"""
# (the struct does not exist yet; insert it before the SlObjectItf struct)
old = """// OpenSL COM model: SLObjectItf is `const struct SLObjectItf_ * const *`.
"""
new = """// SLDataFormat_PCM: WHAT THE GUEST ASKS THE DEVICE TO PRODUCE.  It is the
// first argument of CreateAudioPlayer's engineInfo, and it is how a backend
// learns the format -- so it is also how the port learns what to expect.
struct SlDataFormatPcm {
    uint32_t formatType;   // SL_DATAFORMAT_PCM
    uint32_t numChannels;
    uint32_t sampleRate;
    uint32_t bitsPerSample;
    uint32_t containerSize;
    uint32_t channelMask;
    uint32_t endianness;
};

// OpenSL COM model: SLObjectItf is `const struct SLObjectItf_ * const *`.
"""
text = sub(text, old, new, 'stub: the data format struct')

old = """SlResult engineCreateAudioPlayer(SlSelf, void** player, void*, void*,
                                 uint32_t, const void*, const void*) {
    if (player == nullptr) return kSlParameterInvalid;
    ++gStats.playerCreates;
"""
new = """SlResult engineCreateAudioPlayer(SlSelf, void** player, void*, void* engineInfo,
                                 uint32_t, const void*, const void*) {
    if (player == nullptr) return kSlParameterInvalid;
    ++gStats.playerCreates;
    // THE FORMAT THE GUEST ASKED FOR.  This is where a backend learns what
    // to produce, and it is the P1 measurement's other half: the bytes the
    // guest enqueues, and the format it enqueues them in.
    if (engineInfo != nullptr) {
        const auto* format = static_cast<const SlDataFormatPcm*>(engineInfo);
        gStats.formatChannels = format->numChannels;
        gStats.formatRate = format->sampleRate;
        gStats.formatBits = format->bitsPerSample;
    }
"""
text = sub(text, old, new, 'stub: the requested format')

old = """    ++gStats.bufferEnqueues;
    if (gBufferCallback != nullptr) {
"""
new = """    ++gStats.bufferEnqueues;
    // THE PCM COUNT -- THE P1 MEASUREMENT.  Does the guest's own FMOD
    // produce sound for this device to carry?  The self-test's own bytes are
    // excluded, so the number is the GUEST's.
    if (!gInSelfTest) {
        ++gStats.pcmEnqueues;
        gStats.pcmBytes += size;
        uint32_t bytesPerFrame = gStats.formatChannels * (gStats.formatBits / 8u);
        if (bytesPerFrame == 0u) {
            bytesPerFrame = 4u; // 16-bit stereo
        }
        gStats.pcmFrames += size / bytesPerFrame;
    }
    if (gBufferCallback != nullptr) {
"""
text = sub(text, old, new, 'stub: the PCM counters')

old = """uint64_t gSelfTestBits = 0;
"""
new = """uint64_t gSelfTestBits = 0;

// TRUE WHILE THE DEVICE'S OWN SELF-TEST RUNS, so the self-test's bytes do
// not count as the guest's.
bool gInSelfTest = false;
"""
text = sub(text, old, new, 'stub: the self-test flag')

old = """uint64_t fmodAudioStubSelfTest() {
    const uint64_t passed = runAudioStubSelfTest();
    gSelfTestBits = passed;
    return passed;
}
"""
new = """uint64_t fmodAudioStubSelfTest() {
    gInSelfTest = true;
    const uint64_t passed = runAudioStubSelfTest();
    gInSelfTest = false;
    gSelfTestBits = passed;
    return passed;
}
"""
text = sub(text, old, new, 'stub: the self-test excludes its own bytes')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.cpp (P1 counters)')

# ===================================================== 2. the header: the fields
path = os.path.join(ROOT, 'native-switch', 'include', 'rr3', 'fmod_audio_stub.hpp')
text, crlf, bom = load(path)
old = """    // The device's self-test bits, kept so a reader can see them later.
    uint32_t selfTestBits{};
};
"""
new = """    // The device's self-test bits, kept so a reader can see them later.
    uint32_t selfTestBits{};
    // THE PCM THE GUEST ENQUEUED.  This is the P1 measurement: does the
    // guest's own FMOD produce sound for this device to carry?  The
    // device's self-test bytes are excluded.
    uint32_t pcmEnqueues{};
    uint32_t pcmBytes{};
    uint32_t pcmFrames{};
    // THE FORMAT THE GUEST ASKED FOR, read out of its SLDataFormat.
    uint32_t formatRate{};
    uint32_t formatChannels{};
    uint32_t formatBits{};
};
"""
text = sub(text, old, new, 'stub header: the P1 fields')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.hpp')

# ============================================= 3. the resolver: the FMOD lever
path = os.path.join(ROOT, 'native-switch', 'source', 'symbol_resolver.cpp')
text, crlf, bom = load(path)

old = """uintptr_t resolveHostSymbol(const char* name) {
"""
new = """// THE FMOD LEVER.
//
// "fmod=real" in sdmc:/switch/rr3-census.conf makes the port STOP
// answering the guest's FMOD symbols, so the guest's own libfmodex.so
// provides them -- which is R1, the one experiment that decides whether
// the guest can produce sound at all.
//
// THE DEFAULT IS THE PORT'S SHIM.  That is what every released build does,
// and this file's default must not change: the port's shim is what makes
// the guest's audio code paths work at all today.  The switch only turns
// the experiment on, and it turns it on for ALL of the guest's FMOD
// symbols at once -- a partial withholding would recreate the
// mixed-layout failure the shim exists to end.
//
// THE FILE IS READ ONCE, AND ONLY IF IT CAN BE READ: the first symbol
// resolution happens during the guest's relocation, which is early enough
// for the card to be there, but a missing file must not be cached as
// "the shim" forever.
bool guestFmodUsesRealLibrary() {
    static int cached = 0; // 0 = not read yet, 1 = the port's shim, 2 = real
    if (cached == 0) {
        std::FILE* file = std::fopen("sdmc:/switch/rr3-census.conf", "r");
        if (file == nullptr) {
            return false;
        }
        char line[128];
        bool real = false;
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (std::strstr(line, "fmod=real") != nullptr) {
                real = true;
                break;
            }
        }
        std::fclose(file);
        cached = real ? 2 : 1;
    }
    return cached == 2;
}

// IS THIS ONE OF THE GUEST'S FMOD SYMBOLS?  The port's table holds them
// under their mangled names (_ZN4FMOD...) plus FMOD_System_Create.
bool isGuestFmodSymbol(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return std::strncmp(name, "_ZN4FMOD", 8) == 0 ||
        std::strncmp(name, "FMOD_", 5) == 0;
}

uintptr_t resolveHostSymbol(const char* name) {
"""
text = sub(text, old, new, 'resolver: the FMOD lever')

old = """    for (const HostSymbol& symbol : kHostSymbols) {
        if (std::strcmp(symbol.name, name) == 0) {
            return symbol.address;
        }
    }
    return 0;
}
"""
new = """    for (const HostSymbol& symbol : kHostSymbols) {
        if (std::strcmp(symbol.name, name) == 0) {
            // THE LEVER, AT THE ONE PLACE THE INTERCEPTION HAPPENS.  With
            // "fmod=real" on the card the port answers nothing for the
            // guest's FMOD symbols, and the guest's own libfmodex.so
            // provides them.
            if (isGuestFmodSymbol(symbol.name) && guestFmodUsesRealLibrary()) {
                return 0;
            }
            return symbol.address;
        }
    }
    return 0;
}
"""
text = sub(text, old, new, 'resolver: the lever at the lookup')
save(path, text, crlf, bom)
print('OK symbol_resolver.cpp (the lever)')

# =========================================== 4. the header: the lever accessor
path = os.path.join(ROOT, 'native-switch', 'include', 'rr3', 'symbol_resolver.hpp')
text, crlf, bom = load(path)
old = """uintptr_t resolveHostSymbol(const char* name);
"""
new = """uintptr_t resolveHostSymbol(const char* name);
// TRUE when the guest's FMOD symbols are NOT answered by the port, so the
// guest's own libfmodex.so provides them.  "fmod=real" in
// sdmc:/switch/rr3-census.conf turns it on; the default is the port's
// shim, which is what every released build does.
bool guestFmodUsesRealLibrary();
"""
text = sub(text, old, new, 'resolver header: the accessor')
save(path, text, crlf, bom)
print('OK symbol_resolver.hpp')
