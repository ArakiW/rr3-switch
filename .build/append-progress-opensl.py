#!/usr/bin/env python3
"""The progress entry for the OpenSL device."""
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
bits = number(summary, r'audio device self-test\s+0x([0-9a-fA-F]+)', 'audio bits')
rate = number(summary, r'audio device format\s+(\d+) Hz', 'rate')
channels = number(summary, r'audio device format\s+\d+ Hz / (\d+) ch', 'channels')

entry = """
### 2026-11-16 The OpenSL device, completed: it is now the device the guest's FMOD asks for

**THE ORACLE IS THE GUEST'S OWN LIBRARY.**  The strings inside
@@libfmodex.so@@ name exactly what it looks up for an audio backend:
@@slCreateEngine@@ plus five @@SL_IID_*@@ symbols, and **no AAudio symbol at
all**.  The port's virtual @@libOpenSLES.so@@ exported four of the five IIDs
-- **@@SL_IID_ANDROIDCONFIGURATION@@ (the one that carries the device's
sample rate and channel count) and @@SL_IID_RECORD@@ were missing**.

**AND THE SHAPE OF THE ANSWERS WAS WORSE THAN THE MISSING NAMES.**  There
was **no volume interface**; an unknown IID got the **buffer queue's
vtable** (so a @@GetInterface(SL_IID_VOLUME)@@ read a garbage level -- and a
mixer that reads zero virtualises its voices away: the guest's own log says
@@FMOD_ZERO_VOLUME_VIRTUALISATION@@); and the engine answered every unknown
IID with itself.  **=> AN INTERFACE QUERY MUST ANSWER WITH THE RIGHT
INTERFACE OR WITH AN EXPLICIT FAILURE, NEVER WITH SOMETHING ELSE.**

**NOW.**  The device has the volume interface (live level by default), the
Android configuration interface (%s Hz / %s ch), the two missing exports, and
two IID-driven @@GetInterface@@ paths that fail explicitly.  Its self-test
grew from six checks to ten with a **derived** expectation (the boot log
used to print a hardcoded "expected=3f"), and the bits are in the census as
@@ast=0x%%llx@@ so nobody has to go looking in a log.

**MEASURED** (round @@%s@@): **audio device self-test 0x%s** (want 0x3ff),
format %s Hz / %s ch.  The contract test reads the guest's own lookup list
out of the binary and requires the port to answer every name in it, so a
guest that starts asking for something new fails the gate instead of the
runtime; it is mutation-tested (six ways of breaking the property, all six
caught).

**WHAT IS STILL OPEN.**  The device is ready; the interception is not.  The
52 FMOD interceptions come out together or not at all, and the decisive
measurement is P1: does the guest's real FMOD enqueue PCM into this device?
Only after that does the audren output question arise.  Full reasoning:
docs/OPEN-ISSUES.md section 9.50.
""" % (rate, channels, tag, bits, rate, channels)
entry = entry.replace("@@", TICK)

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
if not text.endswith('\n'):
    text += '\n'
text = text.rstrip('\n') + '\n' + entry
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md (%s: ast=0x%s rate=%s channels=%s)'
      % (tag, bits, rate, channels))
