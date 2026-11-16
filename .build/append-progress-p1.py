# -*- coding: utf-8 -*-
"""The progress entry for the P1 A/B."""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TICK = chr(96)

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
with io.open(path, 'rb') as handle:
    raw = handle.read()
bom = raw.startswith(b'\xef\xbb\xbf')
if bom:
    raw = raw[3:]
crlf = b'\r\n' in raw
text = raw.decode('utf-8').replace('\r\n', '\n')

entry = """
### 2026-11-16 The P1 A/B: the lever works, and the answer is NO

**THE LEVER IS A FILE ON THE CARD.**  @@fmod=real@@ in
@@rr3-census.conf@@ makes the port stop answering the guest's FMOD symbols,
so the guest's own @@libfmodex.so@@ provides them.  **THE DEFAULT IS THE
PORT'S SHIM** -- every released build -- so the two rounds differ in one
thing and the experiment needs no rebuild.

**MEASURED (same build, same save, one variable).**

    cell A  the port's shim        the round ran to the end: frames 13080,
            guest log 60,605,298 B / 242,201 lines, start lights 6 / race 9,
            md5 mismatch 0, and **PCM enqueued 0 enqueues / 0 bytes** --
            the guest's audio path IS the port's API model, so there is no
            PCM to count.
    cell B  the guest's own FMOD   **the guest stopped at about 18 seconds**:
            frames 0, NO census line at all, guest log 511,410 B / 3,132
            lines, and Eden's own log spans 18.1 s instead of 405.7 s.

**WHERE IT STOPPED, TO THE WORD** -- the port's reaper, 385 times a second
of round:

    phase=reaper-asset-singleton ... asset=2109232e98/0/NULL-FIRST-WORD
        eng=210921e688 f8=0 vt=0 table=0 t0=00000000 t1=00000000

**=> THE GUEST'S FMOD ENGINE OBJECT IS ALL ZEROS** -- no vtable, no DSP
list head -- and the guest spun on it.  **=> THIS IS EXACTLY WHAT THE
SHIM'S HEADER DESCRIBES**, so the shim was the fix for this, not
paranoia.  **=> P1 IS ZERO IN BOTH CELLS FOR DIFFERENT REASONS**: in cell A
there is no PCM to count, in cell B the guest never got far enough to ask.

**AND THE INSTRUMENT HAD THE SAME DEFECT AS THE THING IT MEASURES.**  With
no census line, the summary printed "PCM enqueued 0 enqueues / 0 bytes"
and "FMOD in use 0" -- **a missing measurement read as a measured zero**.
The summary now prints the census line count and says **NOT MEASURED** when
the census never ran.

**THE NEXT QUESTION IS NOW SPECIFIC:** not "can R1 work" but **"why does
the real @@FMOD_System_Create@@ return an object with no vtable and no list
head"** -- its constructors, its allocator, its own thread, or a JNI call it
makes during create.  That is a read-only reverse-engineering question.
Full reasoning: docs/OPEN-ISSUES.md section 9.51.
"""
entry = entry.replace("@@", TICK)
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
if crlf:
    text = text.replace('\n', '\r\n')
data = text.encode('utf-8')
if bom:
    data = b'\xef\xbb\xbf' + data
with io.open(path, 'wb') as handle:
    handle.write(data)
print('OK PORT_PROGRESS.md')
