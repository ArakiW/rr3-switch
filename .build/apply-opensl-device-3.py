# -*- coding: utf-8 -*-
"""Part three: the device's self-test becomes a number somebody reads.
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


# --------------------------------------------------------------- 1. main.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'main.cpp')
text, crlf, bom = load(path)
old = """                static_cast<unsigned long long>(fmodAudioSelfTest));
"""
new = """                static_cast<unsigned long long>(fmodAudioSelfTest),
                static_cast<unsigned long long>(rr3::fmodAudioStubSelfTestExpected()));
"""
old2 = """expected=3f silent device"""
new2 = """expected=%02llx silent device"""
text = sub(text, old, new, 'main.cpp: the derived expectation')
text = sub(text, old2, new2, 'main.cpp: the format')
save(path, text, crlf, bom)
print('OK main.cpp')

# ------------------------------------------------------ 2. symbol_resolver.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'symbol_resolver.cpp')
text, crlf, bom = load(path)
old = """extern const void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
"""
new = """extern const void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
extern const void* SL_IID_VOLUME;
extern const void* SL_IID_ANDROIDCONFIGURATION;
extern const void* SL_IID_RECORD;
"""
text = sub(text, old, new, 'resolver: the extern declarations')
old = """    RR3_SHIM("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE),
"""
new = """    RR3_SHIM("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE),
    // THE IIDS THE GUEST'S OWN FMOD LOOKS UP AND THE PORT DID NOT HAVE.
    // MEASURED 2026-11-16 from the strings inside libfmodex.so.
    RR3_SHIM("SL_IID_VOLUME", SL_IID_VOLUME),
    RR3_SHIM("SL_IID_ANDROIDCONFIGURATION", SL_IID_ANDROIDCONFIGURATION),
    RR3_SHIM("SL_IID_RECORD", SL_IID_RECORD),
"""
text = sub(text, old, new, 'resolver: the shim rows')
save(path, text, crlf, bom)
print('OK symbol_resolver.cpp')

# ---------------------------------------------------- 3. guest_view_probe.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'guest_view_probe.cpp')
text, crlf, bom = load(path)
old = """#include "rr3/fmod_shim.hpp"
"""
new = """#include "rr3/fmod_shim.hpp"
#include "rr3/fmod_audio_stub.hpp"
"""
text = sub(text, old, new, 'probe: include the audio stub header')

old = """                    "frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu census bound=%d viewport=%d,%d,%d,%d"
"""
new = """                    "frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu ast=0x%llx arate=%u ach=%u census bound=%d viewport=%d,%d,%d,%d"
"""
text = sub(text, old, new, 'probe: the census format')

old = """                    static_cast<unsigned long long>(fmodStats.channelsStarted),
"""
new = """                    static_cast<unsigned long long>(fmodStats.channelsStarted),
                    // THE AUDIO DEVICE'S OWN SELF-TEST.  It used to live in a
                    // boot log line ("FMOD audio stub selftest=...") that
                    // nobody read, which is how a device with no volume
                    // interface and two missing IIDs survived for the whole
                    // life of the port.  The number is in the census now.
                    static_cast<unsigned long long>(audioStats.selfTestBits),
                    static_cast<unsigned int>(audioStats.deviceSampleRate),
                    static_cast<unsigned int>(audioStats.deviceChannels),
"""
text = sub(text, old, new, 'probe: the census arguments')

old = """                const rr3::FmodShimStats fmodStats = rr3::fmodShimStats();
"""
new = """                const rr3::FmodShimStats fmodStats = rr3::fmodShimStats();
                const rr3::FmodAudioStubStats audioStats = rr3::fmodAudioStubStats();
"""
text = sub(text, old, new, 'probe: the audio stats')
save(path, text, crlf, bom)
print('OK guest_view_probe.cpp')

# ------------------------------------------------------- 4. tools/Test-OnEden.ps1
path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)
old = """    msFmodChannels      = 0   # channels the guest started
"""
new = """    msFmodChannels      = 0   # channels the guest started
    # THE AUDIO DEVICE'S SELF-TEST.  ast is the device's own bits (want
    # 0x3ff: every check the device makes about itself) and arate/ach are
    # the format it would produce.  It was a boot log line nobody read.
    msAudioSelfTest     = '000'
    msAudioRate         = 0
    msAudioChannels     = 0
"""
text = sub(text, old, new, 'ps1: the summary fields')

old = """    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
new = """    # THE AUDIO DEVICE'S SELF-TEST, read out of the census line.
    $audioBits = [regex]::Matches($censusText, ' ast=0x([0-9a-fA-F]+)')
    $audioRate = [regex]::Matches($censusText, ' arate=(\d+)')
    $audioCh = [regex]::Matches($censusText, ' ach=(\d+)')
    if ($audioBits.Count -gt 0) {
        $summary.msAudioSelfTest = $audioBits[$audioBits.Count - 1].Groups[1].Value
        if ($audioRate.Count -gt 0) {
            $summary.msAudioRate = [int]$audioRate[$audioRate.Count - 1].Groups[1].Value
        }
        if ($audioCh.Count -gt 0) {
            $summary.msAudioChannels = [int]$audioCh[$audioCh.Count - 1].Groups[1].Value
        }
    }
    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
text = sub(text, old, new, 'ps1: read the audio self-test')

old = """    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
new = """    ('  audio device self-test  0x{0}   (want 0x3ff: every check the OpenSL device makes about itself)' -f $summary.msAudioSelfTest)
    ('  audio device format     {0} Hz / {1} ch   (what the device would produce)' -f $summary.msAudioRate, $summary.msAudioChannels)
    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
text = sub(text, old, new, 'ps1: print the audio self-test')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')
