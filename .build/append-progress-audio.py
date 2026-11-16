# -*- coding: utf-8 -*-
"""The progress entry for the audio work."""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TICK = chr(96)


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


def number(summary, pattern, label):
    found = re.search(pattern, summary)
    if not found:
        raise SystemExit('FAIL: no %s in the round summary' % label)
    return found.group(1)


archive = sys.argv[1]
with io.open(os.path.join(archive, 'run-summary.txt'), encoding='utf-8') as handle:
    summary = handle.read()
tag = os.path.basename(archive.rstrip('\\/')).split('-eden-')[-1]
bits = number(summary, r'FMOD shim self-test\s+0x([0-9a-fA-F]+)', 'FMOD bits')
sounds = number(summary, r'FMOD sounds/streams/ch\s+(\d+)/', 'sounds')
streams = number(summary, r'FMOD sounds/streams/ch\s+\d+/(\d+)/', 'streams')
channels = number(summary, r'FMOD sounds/streams/ch\s+\d+/\d+/(\d+)', 'channels')
lights = number(summary, r'start lights\s+(\d+)', 'start lights')
race = number(summary, r'race screen\s+(\d+)', 'race screen')

entry = """
### 2026-11-16 The port has no sound, and the FMOD layer under it was dead too

**WHAT THE PORT ACTUALLY IS.**  The port intercepts the guest's FMOD symbols
at the symbol level (about sixty mangled C++ names) and serves them from
@@source/fmod_shim.cpp@@.  The guest's @@libfmodex.so@@ is loaded, but its
API never runs.  **=> THE PORT MODELS THE FMOD API; IT NEVER DECODES, MIXES
OR OUTPUTS AUDIO.**  No PCM exists anywhere in the port, and @@audible@@ is
0 by design.

**AND THAT LAYER WAS BROKEN TOO.**  The Sound and DSP pools searched for a
slot with @@released == true@@, and every slot starts @@released == false@@,
so @@createSound()@@ and @@createDSPByType()@@ could not allocate anything
in a fresh session: the guest could never make a sound and never make a DSP.
The shim's own self-test knew -- it logged @@bits=0xf1f expected=0xfff@@ in
every round, and the three missing bits are exactly createSound, the
callback that needs a channel, and playSound -- and nobody read it, because
it was a line in a 75 MB log and not a number in the summary.  **=> A
SELF-TEST THAT NOBODY READS IS NOT A SELF-TEST.**

**THE FIX.**  The never-used slots are marked free once, before the first
allocation, and the self-test's result is now in the census
(@@fmst=0x%%llx snd=%%llu str=%%llu ch=%%llu@@, at the head of the line), so the
round's summary prints it.  Measured in round @@%s@@: **self-test 0x%s**,
**%s sounds / %s streams / %s channels** created by the guest, against
0xf1f and nothing at all before.  The API layer is alive; the AUDIO PATH is
still not built, and the two claims are separate.

**AND THE OTHER AUDIO DEFECTS ARE NOW MEASURED TOO.**  The game pops an
error dialog every round (@@EngineRamp::Load() failed to load
audio/Vehicles@@, 32 times): it wants
@@audio/Vehicles/v8_10/id_303_accel.decomp@@ (content version + car id) and
the shipped data has the model-named layout.  The game's own asset list
matches the shipped files exactly (325 entries, 325 files), so it is a
DATA-NAMING gap, not a resolver bug.

**THE TWO ROUTES TO ACTUAL SOUND, NAMED.**  R1: let the guest's real
@@libfmodex.so@@ run (it has its own mixer; the port would have to stop
intercepting the FMOD symbols -- a bigger regression surface).  R2: the port
decodes and mixes itself (the assets are shipped; the decoder, the mixer
and the output are not).  Which one to take is a decision, not a
measurement.  The measurement plan is P1 (the guest produces PCM) / P2 (the
port consumes it) / P3 (the hardware plays it): P1 and P2 are provable on
Eden, P3 needs the console, and this host's audren is the raw service API
(no @@nn::audio@@ wrapper, no wave-buffer helper) so the node graph has to
be built by hand.  Full reasoning: docs/OPEN-ISSUES.md section 9.48.
""" % (tag, bits, sounds, streams, channels)
entry = entry.replace("@@", TICK)

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md (%s: bits=0x%s sounds=%s streams=%s channels=%s)'
      % (tag, bits, sounds, streams, channels))
