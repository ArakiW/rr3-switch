# -*- coding: utf-8 -*-
"""The FMOD shim's pools had no free slots, so the guest could not create a
sound or a DSP at all.

MEASURED 2026-11-16, from the round archive racecount3: the shim's own
self-test logged "bits=0xf1f expected=0xfff" -- three checks missing, and
they are exactly createSound (5), the callback that needs a channel (6) and
playSound (7).  The Sound and DSP pools searched for a slot with
released==true, and EVERY slot starts released==false, so in a fresh session
createSound() and createDSPByType() could not allocate anything.  The
ChannelGroup pool already had it right (owner==nullptr means free), which is
why its bit passed.

The fix marks the never-used slots free once, before the first allocation.
The pools' meaning does not change: released==true still means "free", and
the guest never sees the flag.

The census also learns about the FMOD state, so a failing self-test can
never again be a log line nobody reads.
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


# ------------------------------------------------------------ 1. fmod_shim.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'fmod_shim.cpp')
text, crlf, bom = load(path)

old = """unsigned int gSoundCursor = 0;
unsigned int gDspCursor = 0;
unsigned int gGroupCursor = 0;
unsigned int gConnectionCursor = 0;
unsigned int gChannelCursor = 0;
"""
new = """unsigned int gSoundCursor = 0;
unsigned int gDspCursor = 0;
unsigned int gGroupCursor = 0;
unsigned int gConnectionCursor = 0;
unsigned int gChannelCursor = 0;

// THE POOLS' FREE MARKERS.  A slot is free when it has been released -- and
// a NEVER-USED slot is free too.
//
// MEASURED 2026-11-16 (round racecount3): the shim's own self-test logged
// "bits=0xf1f expected=0xfff", and the three missing bits are exactly
// createSound (5), the callback that needs a channel (6) and playSound (7).
// The cause: the Sound and DSP pools searched for a slot with
// released==true, and every slot starts released==false -- so in a fresh
// session createSound() and createDSPByType() could not allocate ANYTHING.
// The guest could never make a sound and never make a DSP, and the game's
// audio code paths all failed on that.
//
// The ChannelGroup pool already had it right (owner==nullptr means free),
// which is why ITS bit passed.
//
// The marker is set once, before the first allocation, so the pools' meaning
// does not change: released==true still means "free", and the guest never
// sees the flag.
void markPoolsFree() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    for (unsigned int index = 0; index < kMaxSounds; ++index) {
        gSounds[index].released = true;
        gSounds[index].inUse = false;
    }
    for (unsigned int index = 0; index < kMaxDsps; ++index) {
        gDsps[index].released = true;
        gDsps[index].userData = nullptr;
        gDsps[index].owner = nullptr;
    }
}
"""
text = sub(text, old, new, 'fmod_shim.cpp: markPoolsFree')

old = """FMOD_RESULT fmodSystemCreateSound(FMOD::System* self, const char* name, unsigned int mode, FMOD_CREATESOUNDEXINFO* exinfo, FMOD::Sound** sound) {
    (void)self;
    (void)mode;
    if (sound == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
"""
new = """FMOD_RESULT fmodSystemCreateSound(FMOD::System* self, const char* name, unsigned int mode, FMOD_CREATESOUNDEXINFO* exinfo, FMOD::Sound** sound) {
    (void)self;
    (void)mode;
    if (sound == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    // THE POOL MUST HAVE A FREE SLOT.  See markPoolsFree().
    markPoolsFree();
"""
text = sub(text, old, new, 'fmod_shim.cpp: createSound')

old = """FMOD_RESULT fmodSystemCreateDSPByType(FMOD::System* self, FMOD_DSP_TYPE type, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
"""
new = """FMOD_RESULT fmodSystemCreateDSPByType(FMOD::System* self, FMOD_DSP_TYPE type, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    // THE POOL MUST HAVE A FREE SLOT.  See markPoolsFree().
    markPoolsFree();
"""
text = sub(text, old, new, 'fmod_shim.cpp: createDSPByType')

old = """FMOD_RESULT fmodSystemCreateDSP(FMOD::System* self, FMOD_DSP_DESCRIPTION* description, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
"""
new = """FMOD_RESULT fmodSystemCreateDSP(FMOD::System* self, FMOD_DSP_DESCRIPTION* description, FMOD::DSP** dsp) {
    if (dsp == nullptr) {
        return FMOD_ERR_INVALID_HANDLE;
    }
    // THE POOL MUST HAVE A FREE SLOT.  See markPoolsFree().
    markPoolsFree();
"""
text = sub(text, old, new, 'fmod_shim.cpp: createDSP')
save(path, text, crlf, bom)
print('OK fmod_shim.cpp')

# ------------------------------------------------------ 2. guest_view_probe.cpp
path = os.path.join(ROOT, 'native-switch', 'source', 'guest_view_probe.cpp')
text, crlf, bom = load(path)

old = """#include "rr3/guest_filesystem.hpp"
"""
new = """#include "rr3/guest_filesystem.hpp"
#include "rr3/fmod_shim.hpp"
"""
text = sub(text, old, new, 'guest_view_probe.cpp: include fmod_shim')

old = """                    "frame=%llu lights=%llu race=%llu census bound=%d viewport=%d,%d,%d,%d"
"""
new = """                    "frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu census bound=%d viewport=%d,%d,%d,%d"
"""
text = sub(text, old, new, 'guest_view_probe.cpp: census format')

old = """                    static_cast<unsigned long long>(frames),
                    // THE TWO RACE NUMBERS COME FIRST, ON PURPOSE."""
new = """                    static_cast<unsigned long long>(frames),
                    // THE RACE NUMBERS AND THE FMOD STATE COME FIRST, ON
                    // PURPOSE.  The log file is written by two threads into
                    // one file, so a line can be SPLIT: the head keeps its
                    // "phase=..." prefix and the tail loses it.  The fields
                    // at the END of a line are the ones a split can
                    // separate from their line, so the numbers the census
                    // reads are at the HEAD, where a split cannot take
                    // them away.
                    static_cast<unsigned long long>(rr3::guestStartLightsOpens()),
                    static_cast<unsigned long long>(rr3::guestRaceScreenOpens()),
                    // THE FMOD SHIM'S OWN STATE.  The shim's self-test used
                    // to log a FAILING value into the log where nobody read
                    // it (bits=0xf1f, expected=0xfff): the guest could not
                    // create a sound or a DSP at all, and the port's own
                    // instrument said so.  It is in the census now.
                    static_cast<unsigned long long>(fmodStats.selfTestBits),
                    static_cast<unsigned long long>(fmodStats.soundCreates),
                    static_cast<unsigned long long>(fmodStats.streamCreates),
                    static_cast<unsigned long long>(fmodStats.channelsStarted),
                    static_cast<int>(g_hdBoundAfterRender),
                    static_cast<int>(g_hdViewportAfterRender[0]),
                    static_cast<int>(g_hdViewportAfterRender[1]),
                    static_cast<int>(g_hdViewportAfterRender[2]),
                    static_cast<int>(g_hdViewportAfterRender[3]),
                    // THE TWO RACE NUMBERS COME FIRST, ON PURPOSE."""
text = sub(text, old, new, 'guest_view_probe.cpp: census arguments')

old = """                    static_cast<unsigned long long>(rr3::guestStartLightsOpens()),
                    static_cast<unsigned long long>(rr3::guestRaceScreenOpens()),
                    static_cast<int>(g_hdBoundAfterRender),
                    static_cast<int>(g_hdViewportAfterRender[0]),
                    static_cast<int>(g_hdViewportAfterRender[1]),
                    static_cast<int>(g_hdViewportAfterRender[2]),
                    static_cast<int>(g_hdViewportAfterRender[3]),
                    stats.lastNonZeroFramebuffer, stats.lastNonZeroFboWidth,
"""
new = """                    stats.lastNonZeroFramebuffer, stats.lastNonZeroFboWidth,
"""
text = sub(text, old, new, 'guest_view_probe.cpp: census argument dedupe')
save(path, text, crlf, bom)
print('OK guest_view_probe.cpp')

# -------------------------------------------------------- 3. tools/Test-OnEden.ps1
path = os.path.join(ROOT, 'tools', 'Test-OnEden.ps1')
text, crlf, bom = load(path)

old = """    msRaceHud           = 0   # "Reload HUD planes" (the race HUD was built)
"""
new = """    # THE FMOD SHIM'S STATE.  fmst is the shim's own self-test bits: every
    # check the shim makes about itself, and 0xfff means all twelve pass.
    # It was a log line nobody read (the round archive shows bits=0xf1f),
    # and it is in the census now.
    msFmodSelfTest      = '000'
    msFmodSounds        = 0   # sounds the guest asked the port for
    msFmodStreams       = 0   # streams the guest asked the port for
    msFmodChannels      = 0   # channels the guest started
    msRaceHud           = 0   # "Reload HUD planes" (the race HUD was built)
"""
text = sub(text, old, new, 'Test-OnEden.ps1: summary fields')

old = """    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
new = """    # THE FMOD SHIM'S STATE, read out of the census line.
    $fmodBits = [regex]::Matches($censusText, ' fmst=0x([0-9a-fA-F]+)')
    $fmodSnd = [regex]::Matches($censusText, ' snd=(\d+)')
    $fmodStr = [regex]::Matches($censusText, ' str=(\d+)')
    $fmodCh = [regex]::Matches($censusText, ' ch=(\d+)')
    if ($fmodBits.Count -gt 0) {
        $summary.msFmodSelfTest = $fmodBits[$fmodBits.Count - 1].Groups[1].Value
        if ($fmodSnd.Count -gt 0) {
            $summary.msFmodSounds = [int]$fmodSnd[$fmodSnd.Count - 1].Groups[1].Value
        }
        if ($fmodStr.Count -gt 0) {
            $summary.msFmodStreams = [int]$fmodStr[$fmodStr.Count - 1].Groups[1].Value
        }
        if ($fmodCh.Count -gt 0) {
            $summary.msFmodChannels = [int]$fmodCh[$fmodCh.Count - 1].Groups[1].Value
        }
    }
    $summary.msRaceHud     = [regex]::Matches($guestText, 'Reload HUD planes').Count
"""
text = sub(text, old, new, 'Test-OnEden.ps1: read the FMOD state')

old = """    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
new = """    ('  FMOD shim self-test     0x{0}   (want 0xfff: every check the shim makes about itself)' -f $summary.msFmodSelfTest)
    ('  FMOD sounds/streams/ch  {0}/{1}/{2}   (the guest asked the port for this many sounds, streams and channels)' -f $summary.msFmodSounds, $summary.msFmodStreams, $summary.msFmodChannels)
    ('  race marker: HUD string  {0}   ("Reload HUD planes" -- the guest RENDERS this debug string during a race; it is a race marker, NOT evidence that the HUD was built)' -f $summary.msRaceHud)
"""
text = sub(text, old, new, 'Test-OnEden.ps1: print the FMOD state')
save(path, text, crlf, bom)
print('OK tools/Test-OnEden.ps1')
