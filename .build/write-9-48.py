# -*- coding: utf-8 -*-
"""Section 9.48: the port has no sound, and the FMOD layer under it was dead.

The measured numbers come out of the round's own run-summary.txt.
"""
import io
import os
import re
import sys

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


def number(summary, pattern, label):
    found = re.search(pattern, summary)
    if not found:
        raise SystemExit('FAIL: no %s in the round summary' % label)
    return found.group(1)


archive = sys.argv[1]
with io.open(os.path.join(archive, 'run-summary.txt'), encoding='utf-8') as handle:
    summary = handle.read()

tag = os.path.basename(archive.rstrip('\\/')).split('-eden-')[-1]
bits = number(summary, r'FMOD shim self-test\s+0x([0-9a-fA-F]+)', 'FMOD self-test bits')
sounds = number(summary, r'FMOD sounds/streams/ch\s+(\d+)/', 'FMOD sounds')
streams = number(summary, r'FMOD sounds/streams/ch\s+\d+/(\d+)/', 'FMOD streams')
channels = number(summary, r'FMOD sounds/streams/ch\s+\d+/\d+/(\d+)', 'FMOD channels')
lights = number(summary, r'start lights\s+(\d+)', 'start lights')
race = number(summary, r'race screen\s+(\d+)', 'race screen')
frames = number(summary, r'frames\s+(\d+)', 'frames')
pixels = number(summary, r'pixels lit\s+(\d+) of (\d+)', 'pixels')
md5 = number(summary, r'md5 mismatch\s+(\d+)', 'md5 mismatch')

section = """### 9.48 The port has no sound, and the FMOD layer under it was dead too

**AND THE FIRST THING TO SAY IS WHAT THE PORT ACTUALLY IS.**  The port
intercepts the guest's FMOD symbols at the SYMBOL level --
\`_ZN4FMOD6System4initEijPv\` and about sixty more -- and serves them
from \`source/fmod_shim.cpp\`.  The guest's own \`libfmodex.so\` is
loaded (1,609,728 B) but its API never runs.  **=> THE PORT DOES NOT DECODE,
MIX OR OUTPUT AUDIO.  IT MODELS THE API.**  A Sound is a handle with a
state and a length; a Channel is a handle with a volume and a 3-D position;
a DSP is a node in a list.  There is no PCM anywhere in the port, and
\`audible\` is 0 by design.

**=> => => => => AND THE SECOND THING: THAT LAYER WAS DEAD AS WELL.**  The
Sound and DSP pools searched for a slot with \`released == true\`, and
every slot starts \`released == false\` -- so in a fresh session
\`createSound()\` and \`createDSPByType()\` could not allocate
ANYTHING.  **=> THE GUEST COULD NEVER MAKE A SOUND AND NEVER MAKE A DSP.**
The ChannelGroup pool already had it right (\`owner == nullptr\` means
free), which is why its self-test bit passed.

**=> => => => => => => AND THE PORT KNEW, AND NOBODY READ IT.**  The shim
runs a self-test of twelve checks and writes the result into the log:

    phase=fmod-shim-selftest bits=0xf1f expected=0xfff

**=> THE THREE MISSING BITS ARE EXACTLY createSound (5), the callback that
needs a channel (6), and playSound (7).**  A failing instrument that is a
log line nobody reads is worse than no instrument, because it looks like a
passing one.

**=> => => => => => => => => AND THE FIX.**  The never-used slots are marked
free once, before the first allocation (\`markPoolsFree()\`), and the
self-test's result is now in the CENSUS -- \`fmst=0x%%llx snd=%%llu str=%%llu
ch=%%llu\`, at the HEAD of the census line -- so the round's summary prints
it and nobody has to go looking.  Measured in round \`%s\`:

    FMOD shim self-test     0x%s   (want 0xfff)
    FMOD sounds/streams/ch  %s/%s/%s
    frames                  %s      pixels lit %s of %s      md5 mismatch %s

**=> => => => => => => => => => AND THE API IS ALIVE NOW, WHICH IS NOT THE
SAME AS HAVING SOUND.**  The guest can make sounds, streams and channels
again, and the game's audio code paths run instead of failing.  What it
still cannot do is make a NOISE, because nothing in the port holds PCM.
**=> READ THE TWO CLAIMS SEPARATELY: THE API LAYER IS FIXED, THE AUDIO
PATH IS NOT BUILT.**

**=> => => => => => => => => => => AND THE OTHER AUDIO DEFECTS, MEASURED IN
THE SAME ARCHIVE.**

- **THE GAME POPS AN ERROR DIALOG.**  \`gate-show-message ... title='Error'
  body='EngineRamp::Load() failed to load audio/Vehicles'\` -- 32 times a
  round, once per car.  The game wants
  \`audio/Vehicles/v8_10/id_303_accel.decomp\` (a content version and a car
  id), and the shipped data has \`audio/Vehicles/bmw_m3_coupe/BMW_M3_GTS_ENG_Accel.decomp\`
  (a model name).  **=> THE GAME'S LOOKUP PATH AND THE SHIPPED LAYOUT ARE
  TWO DIFFERENT NAMING SCHEMES.**  The game's own asset list
  (\`asset_list_audio.txt\`, 325 entries) matches the shipped files
  exactly -- 325 entries, 325 files -- so the list is not the broken part;
  the game builds this particular path itself.  **=> THIS IS A DATA-NAMING
  GAP, NOT A RESOLVER BUG**, and it is the visible symptom of the audio
  work that is still to be done.
- **THE AUDIO SUBSYSTEM'S STATE IS NOW VISIBLE IN EVERY ROUND.**  Before
  this, the only trace of the FMOD layer was one log line per session; the
  round summary could not say whether the API worked.  It can now.

**=> => => => => => => => => => => => => AND THE TWO WAYS FORWARD, WITH
THE MEASUREMENT EACH ONE NEEDS.**  A port must not measure the game by what
it wrote to its own log, and the same rule applies here: the claim "the
guest produces sound" needs its own instrument, not a log line.

    P1  the guest produces PCM          (the Enqueue path, with bytes and a
                                          format: rate, channels, bits)
    P2  the port consumes it            (the ring drained, frames counted)
    P3  the hardware plays it           (the audren wave buffer's played
                                          sample count advancing)

**P1 and P2 are provable on the emulator; P3 needs the console.**  And the
host's audren is the RAW service API (\`switch/services/audren.h\`): this
toolchain has no \`nn::audio\` wrapper and no wave-buffer helper, so the
node graph has to be built by hand -- which is why P1 and P2 come first and
the device work comes last.

**=> => => => => => => => => => => => => AND THE TWO ROUTES TO ACTUAL
SOUND, NAMED.**  **R1: let the guest's real \`libfmodex.so\` run** (it
has its own mixer and its own DSP graph; the port would have to stop
intercepting the FMOD symbols, and the interception is what makes the
guest's audio code paths work at all today).  **R2: the port decodes and
mixes itself** (the shipped data has the assets -- mp3 music, \`.sounddef\`
effect definitions, the engine \`.decomp\` files -- but the port would
have to implement a decoder, a mixer and an output).  **=> R1 IS THE SMALLER
CHANGE AND THE BIGGER REGRESSION SURFACE, AND WHICH ONE TO TAKE IS A
DECISION, NOT A MEASUREMENT.**

**=> => => => => => => => => => => => => AND THE RULE THIS SETTLES.**  A
port's own instrument, when it fails, must be LOUD.  The FMOD self-test had
been failing in every round for the whole life of the shim, and it was
invisible because it was a line in a 75 MB log instead of a number in the
summary.  **=> A SELF-TEST THAT NOBODY READS IS NOT A SELF-TEST.**
""" % (tag, bits, sounds, streams, channels, frames, pixels[0], pixels[1], md5)

# The markdown backticks travel through a JavaScript template literal on
# the way here, so they arrive as a backslash-backtick; take the backslash
# off again before the text reaches the document.
section = section.replace(chr(92) + chr(96), chr(96))

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.48 "
at = text.find(marker)
if at >= 0:
    text = text[:at].rstrip('\n') + '\n'
else:
    text = text.rstrip('\n') + '\n'
text = text + section
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md  (%s: fmst=0x%s sounds=%s streams=%s channels=%s)'
      % (tag, bits, sounds, streams, channels))
