# -*- coding: utf-8 -*-
"""The census carries the P1 numbers and the lever's state; the round script
can turn the lever on.
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


# ================================================ 1. the census line
path = os.path.join(ROOT, 'native-switch', 'source', 'guest_view_probe.cpp')
text, crlf, bom = load(path)
old = """#include "rr3/fmod_audio_stub.hpp"
"""
new = """#include "rr3/fmod_audio_stub.hpp"
#include "rr3/symbol_resolver.hpp"
"""
text = sub(text, old, new, 'probe: the resolver header')

old = """                    "frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu ast=0x%llx arate=%u ach=%u census bound=%d viewport=%d,%d,%d,%d"
"""
new = """                    "frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu ast=0x%llx arate=%u ach=%u pcm_enq=%u pcm_bytes=%u fmt=%u/%u/%u fmrt=%u census bound=%d viewport=%d,%d,%d,%d"
"""
text = sub(text, old, new, 'probe: the census format')

old = """                    static_cast<unsigned int>(audioStats.deviceChannels),
"""
new = """                    static_cast<unsigned int>(audioStats.deviceChannels),
                    // THE P1 NUMBERS: what the guest enqueued into the audio
                    // device, and the format it asked for.  With the FMOD
                    // lever on (fmrt=1) this is the guest's OWN FMOD's
                    // output -- the answer to "can the guest make sound".
                    static_cast<unsigned int>(audioStats.pcmEnqueues),
                    static_cast<unsigned int>(audioStats.pcmBytes),
                    static_cast<unsigned int>(audioStats.formatRate),
                    static_cast<unsigned int>(audioStats.formatChannels),
                    static_cast<unsigned int>(audioStats.formatBits),
                    // AND WHICH FMOD IS IN USE: 0 = the port's shim (every
                    // released build), 1 = the guest's own libfmodex.so.
                    static_cast<unsigned int>(rr3::guestFmodUsesRealLibrary() ? 1 : 0),
"""
text = sub(text, old, new, 'probe: the census arguments')
save(path, text, crlf, bom)
print('OK guest_view_probe.cpp')

# ================================================ 2. the round census
path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)
old = """    msAudioChannels     = 0
"""
new = """    msAudioChannels     = 0
    # THE P1 NUMBERS: what the guest enqueued into the audio device, and the
    # format it asked for.  fmrt says WHICH FMOD is in use: 0 = the port's
    # shim (every released build), 1 = the guest's own libfmodex.so.
    msPcmEnqueues       = 0
    msPcmBytes          = 0
    msPcmFormat         = '(none)'
    msFmodReal          = 0
"""
text = sub(text, old, new, 'ps1: the P1 fields')

old = """    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
new = """    $pcmEnq = [regex]::Matches($censusText, ' pcm_enq=(\d+)')
    $pcmBytes = [regex]::Matches($censusText, ' pcm_bytes=(\d+)')
    $pcmFormat = [regex]::Matches($censusText, ' fmt=(\d+)/(\d+)/(\d+)')
    $fmodReal = [regex]::Matches($censusText, ' fmrt=(\d+)')
    if ($pcmEnq.Count -gt 0) {
        $summary.msPcmEnqueues = [int]$pcmEnq[$pcmEnq.Count - 1].Groups[1].Value
        $summary.msPcmBytes = [int]$pcmBytes[$pcmBytes.Count - 1].Groups[1].Value
        $last = $pcmFormat[$pcmFormat.Count - 1]
        $summary.msPcmFormat = '{0} Hz / {1} ch / {2} bit' -f $last.Groups[1].Value, $last.Groups[2].Value, $last.Groups[3].Value
        $summary.msFmodReal = [int]$fmodReal[$fmodReal.Count - 1].Groups[1].Value
    }
    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
text = sub(text, old, new, 'ps1: read the P1 numbers')

old = """    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
new = """    ('  PCM enqueued            {0} enqueues / {1} bytes   (what the guest handed the audio device)' -f $summary.msPcmEnqueues, $summary.msPcmBytes)
    ('  PCM format asked         {0}   (the guest own SLDataFormat)' -f $summary.msPcmFormat)
    ('  FMOD in use              {0}   (0 = the port own shim, 1 = the guest own libfmodex.so)' -f $summary.msFmodReal)
    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
text = sub(text, old, new, 'ps1: print the P1 numbers')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')

# ================================================ 3. the conf deployer
path = os.path.join(ROOT, '.build', 'deploy-census-conf.ps1')
text, crlf, bom = load(path)
old = """param(
    [switch]$Off
)
"""
new = """param(
    [switch]$Off,
    # THE FMOD LEVER.  With it, the port stops answering the guest's FMOD
    # symbols and the guest's own libfmodex.so provides them -- the R1
    # experiment, as a file on the card.  THE DEFAULT IS OFF, so a released
    # build behaves exactly as it did before.
    [switch]$FmodReal
)
"""
text = sub(text, old, new, 'deployer: the FmodReal switch')

old = """if ($Off) {
    $text = "census=off`n"
}
else {
    $text = "census=on`n"
}
"""
new = """if ($Off) {
    $text = "census=off`n"
}
else {
    $text = "census=on`n"
}
if ($FmodReal) {
    $text += "fmod=real`n"
}
"""
text = sub(text, old, new, 'deployer: the fmod key')
save(path, text, crlf, bom)
print('OK .build/deploy-census-conf.ps1')

# ================================================ 4. the round script
path = os.path.join(ROOT, '.build', 'run-pinned-round.ps1')
text, crlf, bom = load(path)
old = """    [switch]$CensusOff,
"""
new = """    [switch]$CensusOff,
    # THE FMOD LEVER (R1): the guest's own libfmodex.so provides the FMOD
    # API instead of the port's shim.  See .build/deploy-census-conf.ps1.
    [switch]$FmodReal,
"""
text = sub(text, old, new, 'round: the FmodReal switch')

old = """if ($CensusOff) {
    & (Join-Path $root '.build\deploy-census-conf.ps1') -Off
}
else {
    & (Join-Path $root '.build\deploy-census-conf.ps1')
}
"""
new = """if ($CensusOff) {
    & (Join-Path $root '.build\deploy-census-conf.ps1') -Off -FmodReal:$FmodReal
}
else {
    & (Join-Path $root '.build\deploy-census-conf.ps1') -FmodReal:$FmodReal
}
"""
text = sub(text, old, new, 'round: the lever passes through')
save(path, text, crlf, bom)
print('OK .build/run-pinned-round.ps1')
