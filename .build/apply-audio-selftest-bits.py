# -*- coding: utf-8 -*-
"""The device's self-test result has to be KEPT, not just returned once."""
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

old = """FmodAudioStubStats gStats{};
"""
new = """FmodAudioStubStats gStats{};

// THE DEVICE'S SELF-TEST RESULT, KEPT.  The self-test runs once, at boot,
// and before this the only place its bits existed was the boot log line --
// which is exactly where nobody looks.  The census asks for them every
// frame, so they have to live here.
uint64_t gSelfTestBits = 0;
"""
text = sub(text, old, new, 'stub: the kept self-test bits')

old = """uint64_t fmodAudioStubSelfTest() {
    uint64_t passed = 0;
"""
new = """uint64_t runAudioStubSelfTest() {
    uint64_t passed = 0;
"""
text = sub(text, old, new, 'stub: the self-test body is renamed')

old = """uint64_t fmodAudioStubSelfTestExpected() {
"""
new = """uint64_t fmodAudioStubSelfTest() {
    const uint64_t passed = runAudioStubSelfTest();
    gSelfTestBits = passed;
    return passed;
}

uint64_t fmodAudioStubSelfTestExpected() {
"""
text = sub(text, old, new, 'stub: the self-test wrapper')

old = """    copy.deviceSampleRate = static_cast<uint32_t>(kDeviceSampleRate);
    copy.deviceChannels = static_cast<uint32_t>(kDeviceChannelCount);
    return copy;
"""
new = """    copy.deviceSampleRate = static_cast<uint32_t>(kDeviceSampleRate);
    copy.deviceChannels = static_cast<uint32_t>(kDeviceChannelCount);
    copy.selfTestBits = static_cast<uint32_t>(gSelfTestBits);
    return copy;
"""
text = sub(text, old, new, 'stub: the stats carry the bits')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.cpp')

path = os.path.join(ROOT, 'native-switch', 'include', 'rr3', 'fmod_audio_stub.hpp')
text, crlf, bom = load(path)
old = """    // The device's own format: what it would produce if it produced
    // anything.  It matches what the guest's FMOD asks the port for.
    uint32_t deviceSampleRate{};
    uint32_t deviceChannels{};
};
"""
new = """    // The device's own format: what it would produce if it produced
    // anything.  It matches what the guest's FMOD asks the port for.
    uint32_t deviceSampleRate{};
    uint32_t deviceChannels{};
    // The device's self-test bits, kept so a reader can see them later.
    uint32_t selfTestBits{};
};
"""
text = sub(text, old, new, 'stub header: the self-test bits field')
save(path, text, crlf, bom)
print('OK fmod_audio_stub.hpp')
