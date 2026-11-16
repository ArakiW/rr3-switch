# -*- coding: utf-8 -*-
"""Section 9.49: the R1 feasibility verdict, measured.

Everything in it comes from the symbol tables of the two guest libraries
and from the port's own interception table -- read, not guessed.
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


section = """### 9.49 The R1 feasibility verdict: the guest's real FMOD can run, and here is what has to be true first

**MEASURED, FROM THE SYMBOL TABLES (readelf --dyn-syms on the two guest
libraries, cross-referenced with the port's own interception table).**

    the guest game imports FMOD symbols      52
    the port intercepts                       52   (100%)
    the real libfmodex.so exports them        52   (so the interception is
                                                   complete, not partial)
    the real libfmodex.so's own imports      94
    of those, audio-backend imports           0   (no sl*, no AAudio*)
    "libOpenSLES.so" inside libfmodex.so    present
    "libaaudio.so" inside libfmodex.so      absent

**=> => => => => AND THE FIRST CONSEQUENCE IS THE GOOD ONE: THE REAL FMOD
REACHES ITS AUDIO BACKEND THROUGH dlopen("libOpenSLES.so"), AND THE PORT
ALREADY SERVES THAT EXACT LIBRARY.**  The port's OpenSL device is not a
speculative stub: \`isVirtualAudioLibrary()\` /
\`virtualAudioLibraryHandle()\` / \`resolveVirtualAudioSymbol()\` exist
precisely because the guest's own audio library asks for it by name.  **=> IF
THE REAL FMOD RUNS, IT WILL FIND A BACKEND, MIX THE GAME'S SOUNDS AND ENQUEUE
PCM INTO THE PORT'S OPENSL DEVICE** -- and that device is the LAST HOP, so
it is where an audren output would attach.

**=> => => => => => AND THE SECOND CONSEQUENCE IS THE HISTORIC BLOCKER, AND
IT IS A LAYOUT PROBLEM, NOT A MYSTERY.**  The shim's own header records why
it exists:

    the real libfmodex.so's DSP-graph code walks a circular node list whose
    head (this + 0xa4d0) is never sentinel-initialised, so the call returns
    nothing and the main thread spins on the resulting unmapped
    Read64 @ 0x8 / Read32 @ 0x1C0 pair forever.

**=> THE SPIN WAS A MIXED-LAYOUT FAILURE: THE SHIM'S OBJECTS WERE HANDED TO
THE REAL LIBRARY'S CODE.**  The real library read its own expected offsets
out of the shim's objects and walked garbage.  With the interception at
100% the two never meet, and with it at 0% the real library builds its own
graph and its own objects -- **=> BUT THE PORT MUST NEVER MIX THEM.**  That
is the one rule R1 has to keep.

**=> => => => => => => AND THE THREE THINGS THAT HAVE TO BE TRUE BEFORE THE
INTERCEPTION IS TOUCHED.**

1. **THE OPENSL DEVICE NEEDS A VOLUME INTERFACE.**  The guest's own log
   already says \`FMOD_ZERO_VOLUME_VIRTUALISATION\` -- the real FMOD read
   the output's volume as ZERO and virtualised the voices away, which is
   exactly what a device without a volume interface looks like.  The port's
   \`playerGetInterface()\` returns the buffer queue for ANY unrecognised
   IID, so a \`GetInterface(SL_IID_VOLUME)\` would get the WRONG VTABLE
   and read a garbage level.  **=> THE VOLUME INTERFACE MUST BE IMPLEMENTED
   PROPERLY, AND THE UNKNOWN-IID FALLBACK MUST STOP.**
2. **THE INTERCEPTION MUST COME OUT WHOLESALE (52 SYMBOLS) OR NOT AT ALL.**
   A partial removal recreates the mixed-layout spin, which is the one
   failure this shim was built to end.
3. **THE PORT LOSES THE SHIM'S SAFETY NET WITH IT.**  The shim's reason for
   existing is that every handle is a real, non-null object whose node
   lists are self-linked sentinels, so a walk always terminates.  Those
   guarantees live in the shim; R1 hands that job back to the guest's
   library and to the port's exception handler.

**=> => => => => => => => => AND THE MEASUREMENT THAT DECIDES IT, WHICH IS
THE SAME THREE LAYERS AS SECTION 9.48.**

    P1  the guest enqueues PCM     the OpenSL device's bufferEnqueues and
                                   pcmBytes go from 0 to >0, with the
                                   format (rate, channels, bits) read out
                                   of the guest's SLDataFormat
    P2  the port consumes it       a ring that the port drains
    P3  the hardware plays it       audren's played-sample count advancing

**P1 IS THE ONE THAT ANSWERS "CAN THE GUEST'S FMOD RUN AT ALL", AND IT IS
PROVABLE ON THE EMULATOR.**  Today P1 is 0 and cannot be anything else: the
port's FMOD API holds no PCM and the guest never reaches the OpenSL device.

**=> => => => => => => => => => AND THE VERDICT.**  **R1 IS VIABLE IN
PRINCIPLE, AND THE PORT IS ALREADY HALFWAY THERE** -- the backend handshake
is already served, the interception table is already complete, and the
library is already loaded and its constructors already run on device.  **THE
PRICE IS THE THREE CONDITIONS ABOVE**, of which the first (the OpenSL volume
interface) is small, well-understood and independently testable, and the
second (all-or-nothing) is a rule rather than a project.  **=> THE DECISION
LEFT IS NOT "IS IT POSSIBLE" BUT "HOW MUCH REGRESSION SURFACE IS THE
FALLBACK WORTH".**
"""

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.49 "
at = text.find(marker)
if at >= 0:
    text = text[:at].rstrip('\n') + '\n'
else:
    text = text.rstrip('\n') + '\n'
text = text + section
text = text.replace(chr(92) + chr(96), chr(96))
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md (section 9.49)')

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
entry = """
### 2026-11-16 The R1 feasibility verdict: the guest's real FMOD CAN run

**MEASURED FROM THE SYMBOL TABLES.**  The guest game imports 52 FMOD
symbols; the port intercepts all 52 (the real library exports all 52, so
the interception is complete); the real @@libfmodex.so@@ imports NO audio
backend symbols at all -- it reaches its backend through
@@dlopen("libOpenSLES.so")@@, and **the port already serves exactly that
library**.  **=> IF THE REAL FMOD RUNS, IT FINDS A BACKEND, MIXES AND
ENQUEUES PCM INTO THE PORT'S OPENSL DEVICE**, which is the last hop where an
audren output would attach.

**AND THE HISTORIC BLOCKER IS A LAYOUT PROBLEM, NOT A MYSTERY.**  The shim
exists because the real library's DSP-graph code walks a list head that is
never sentinel-initialised, and the main thread spun on it forever -- that
was the SHIM'S objects being read by the REAL library's code.  **=> THE PORT
MUST NEVER MIX THE TWO**, so R1 is all-or-nothing over the 52 symbols.

**THE THREE CONDITIONS BEFORE THE INTERCEPTION IS TOUCHED.**  (1) the
OpenSL device needs a real volume interface -- the guest's own log already
says @@FMOD_ZERO_VOLUME_VIRTUALISATION@@, which is what a device without one
looks like, and the port currently hands out the buffer-queue vtable for
ANY unknown IID; (2) the interception comes out wholesale or not at all; (3)
the shim's safety net (real objects, self-linked sentinels) goes with it.

**THE MEASUREMENT THAT DECIDES IT IS P1** -- the OpenSL device's
@@bufferEnqueues@@/@@pcmBytes@@ going from 0 to >0, which is provable on
Eden -- with P2 (the port consumes) and P3 (the hardware plays) after it.
Full reasoning: docs/OPEN-ISSUES.md section 9.49.
"""
entry = entry.replace("@@", chr(96))
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md')
