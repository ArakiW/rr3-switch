# -*- coding: utf-8 -*-
"""Section 9.50: the OpenSL device, completed for the guest's real FMOD."""
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
bits = number(summary, r'audio device self-test\s+0x([0-9a-fA-F]+)', 'audio self-test')
rate = number(summary, r'audio device format\s+(\d+) Hz', 'audio rate')
channels = number(summary, r'audio device format\s+\d+ Hz / (\d+) ch', 'audio channels')
lights = number(summary, r'start lights\s+(\d+)', 'start lights')
race = number(summary, r'race screen\s+(\d+)', 'race screen')

section = """### 9.50 The OpenSL device, completed: what the guest's real FMOD asks for, measured

**THE ORACLE IS THE GUEST'S OWN LIBRARY.**  The strings inside
\`libfmodex.so\` name exactly what the library looks up when it needs an
audio backend, and nothing else:

    slCreateEngine
    SL_IID_ENGINE   SL_IID_PLAY   SL_IID_ANDROIDSIMPLEBUFFERQUEUE
    SL_IID_ANDROIDCONFIGURATION   SL_IID_RECORD

**=> NO AAUDIO SYMBOL AT ALL**, so the AAudio refusal path is irrelevant to
this route.  And the port's virtual \`libOpenSLES.so\` exported FOUR of
the five IIDs: **\`SL_IID_ANDROIDCONFIGURATION\` and \`SL_IID_RECORD\`
were missing.**  The first one is the important one -- it is the interface
that carries the device's SAMPLE RATE and CHANNEL COUNT, so a backend that
cannot hand it over cannot be told what to produce.

**=> => => AND THE SHAPE OF THE ANSWERS WAS WORSE THAN THE MISSING
NAMES.**  Three defects, in increasing order of how long they would have
stayed hidden:

1. **THERE WAS NO VOLUME INTERFACE AT ALL.**  A mixer reads the device's
   volume to decide whether the device is live.
2. **AN UNKNOWN IID GOT THE BUFFER QUEUE'S VTABLE.**  The old
   \`playerGetInterface()\` answered every IID it did not recognise with the
   queue -- so a \`GetInterface(SL_IID_VOLUME)\` returned an object whose
   vtable was the queue's, and the caller read a garbage level out of it.
   **=> A MIXER THAT READS ZERO VIRTUALISES ITS VOICES AWAY, AND THE PORT
   LOOKS LIKE "NO SOUND" INSTEAD OF LIKE A BUG.**  The guest's own log says
   \`FMOD_ZERO_VOLUME_VIRTUALISATION\`.
3. **THE ENGINE ANSWERED EVERY UNKNOWN IID WITH ITSELF**, which is the same
   hazard one level down.

**=> => => => => AND THE RULE ALL THREE BREAKS.**  **AN INTERFACE QUERY
MUST ANSWER WITH THE RIGHT INTERFACE OR WITH AN EXPLICIT FAILURE, NEVER WITH
SOMETHING ELSE.**  A stub that returns "something" instead of "nothing" is
worse than a stub that fails, because the caller cannot tell.

**=> => => => => => AND WHAT WAS DONE.**  The device now has the volume
interface (level in millibels, volume as a float, and it reports a LIVE
level by default -- a backend that reports zero is a backend a mixer
virtualises), the Android configuration interface (48000 Hz, 2 channels,
1024-byte buffers -- what the guest's FMOD asks the port for), the two
missing IID exports, and two \`GetInterface\` paths that are IID-driven
and fail explicitly on anything they do not have.

**=> => => => => => => AND THE DEVICE'S SELF-TEST GREW FROM SIX CHECKS TO
TEN, AND ITS EXPECTATION IS NOW DERIVED.**  The boot log used to print a
hardcoded "expected=3f"; a new check would have left that number behind, so
the expectation is a function now
(\`fmodAudioStubSelfTestExpected()\`), and the census carries the bits as
\`ast=0x%%llx\` next to the device's format.

**=> => => => => => => => MEASURED** (round \`%s\`):

    audio device self-test   0x%s   (want 0x3ff: every check the device makes
                                    about itself)
    audio device format      %s Hz / %s ch   (what it would produce)
    start lights / race screen   %s / %s

**=> => => => => => => => AND WHAT IS LEFT BEFORE THE INTERCEPTION CAN BE
TOUCHED.**  The device is ready; the interception is not.  The remaining
work for R1 is: take the 52 FMOD interceptions out (all at once -- a partial
removal recreates the mixed-layout spin), and then measure P1 -- does the
guest's real FMOD enqueue PCM into this device?  **=> AND ONLY AFTER THAT
DOES THE QUESTION OF AN AUDREN OUTPUT ARISE, because the PCM has to exist
before it can be played.**
""" % (tag, bits, rate, channels, lights, race)

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.50 "
at = text.find(marker)
if at >= 0:
    text = text[:at].rstrip('\n') + '\n'
else:
    text = text.rstrip('\n') + '\n'
text = text + section
text = text.replace(chr(92) + chr(96), chr(96))
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md (9.50: ast=0x%s rate=%s channels=%s)'
      % (bits, rate, channels))
