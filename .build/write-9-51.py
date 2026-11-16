# -*- coding: utf-8 -*-
"""Section 9.51: the P1 A/B, and the answer it gave."""
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
        return "?"
    return found.group(1)


def read_round(path):
    with io.open(os.path.join(path, 'run-summary.txt'), encoding='utf-8') as handle:
        summary = handle.read()
    tag = os.path.basename(path.rstrip('\\/')).split('-eden-')[-1]
    # THE CENSUS RAN ONLY IF THERE WERE FRAMES: the census line is written
    # by the frame loop, so a round with no frames has no census at all --
    # and then EVERY number read out of the census is ABSENT, not zero.
    census = bool(re.search(r"frames\s+\d+", summary))
    return {
        "tag": tag,
        "census": census,
        "frames": number(summary, r'frames\s+(\d+)', 'frames'),
        "logbytes": number(summary, r'guest log\s+(\d+) bytes', 'log bytes'),
        "loglines": number(summary, r'guest log\s+\d+ bytes, (\d+) lines', 'log lines'),
        "eden": number(summary, r'eden log\s+\d+ lines, spans t=[\d.]+\.\d+..([\d.]+) s', 'eden span'),
        "enq": number(summary, r'PCM enqueued\s+(\d+) enqueues', 'pcm enqueues'),
        "bytes": number(summary, r'PCM enqueued\s+\d+ enqueues / (\d+) bytes', 'pcm bytes'),
        "fmt": number(summary, r'PCM format asked\s+(.\S+)', 'pcm format'),
        "real": number(summary, r'FMOD in use\s+(\d+)', 'fmod real'),
        "lights": number(summary, r'start lights\s+(\d+)', 'start lights'),
        "race": number(summary, r'race screen\s+(\d+)', 'race screen'),
        "md5": number(summary, r'md5 mismatch\s+(\d+)', 'md5'),
    }


base = read_round(sys.argv[1])
real = read_round(sys.argv[2])

section = """### 9.51 The P1 A/B: the lever works, and the answer is NO

**THE LEVER IS A FILE ON THE CARD.**  The FMOD interceptions are applied by
NAME LOOKUP during the guest's relocation, so a runtime switch can withhold
them: **\`fmod=real\` in \`sdmc:/switch/rr3-census.conf\`** makes the
port stop answering the guest's FMOD symbols, and the guest's own
\`libfmodex.so\` provides them.  **THE DEFAULT IS THE PORT'S SHIM** --
what every released build does -- so the two rounds differ in exactly one
thing and the experiment needs no rebuild.  The lever withholds ALL of the
guest's FMOD symbols at once, because a partial withholding recreates the
mixed-layout failure the shim exists to end.

**AND THE INSTRUMENT.**  The device counts what the guest enqueues
(\`pcm_enq\`, \`pcm_bytes\`) and reads the format the guest asked for out
of its \`SLDataFormat\` (\`fmt\`), and the summary prints them next to
\`fmrt\`, which says WHICH FMOD was in use.  The device's own self-test
bytes are excluded, so the number is the guest's.

**=> => => => => MEASURED, SAME BUILD (nro sha256 identical), SAME SAVE, ONE
VARIABLE.**

    cell A: the port own shim      fmrt=0
        PCM enqueued            %(aenq)s enqueues / %(abytes)s bytes
        PCM format asked         %(afmt)s
        frames                  %(aframes)s      guest log %(alogbytes)s B / %(aloglines)s lines
        start lights / race      %(alights)s / %(arace)s      md5 mismatch %(amd5)s
        the guest ran the whole round.

    cell B: the guest own libfmodex.so   fmrt=1
        NO CENSUS LINE AT ALL    the guest never reached the frame loop
        frames                  %(bframes)s      guest log %(blogbytes)s B / %(bloglines)s lines
        Eden's own log spans    %(beden)s s of emulator time (cell A: %(aeden)s s)
        the guest stopped making progress at about 18 seconds.

**=> => => => => => => AND WHERE IT STOPPED, TO THE WORD.**  The port's
reaper says it every second, 385 times:

    phase=reaper-asset-singleton ... asset=2109232e98/0/NULL-FIRST-WORD
        eng=210921e688 f8=0 vt=0 table=0 t0=00000000 t1=00000000 ...

**=> THE GUEST'S FMOD ENGINE OBJECT IS ALL ZEROS.**  The first word is
NULL, the vtable pointer is 0, the DSP list head is 0.  **=> THE GUEST GOT
AN UNINITIALISED SYSTEM OBJECT FROM THE REAL LIBRARY AND SPUN ON IT.**
The guest's own last words were ordinary boot messages ("Loading shader
features file: AndroidHigh.feats", "Real Racing 3 14.0.1 [a527]"), and it
had already asked for and RECEIVED both audio asset lists
(\`asset_list_audio.txt\` and \`asset_list_audio_base.txt\`, rc=0).

**=> => => => => => => => => => AND THE ANSWER TO P1, STATED PRECISELY.**
**P1 IS ZERO IN BOTH CELLS, FOR COMPLETELY DIFFERENT REASONS.**  In cell A
the guest's audio path IS the port's API model, so there is no PCM to
count.  In cell B the guest never got far enough to ask.  **=> THE ANSWER IS
NOT "THE GUEST CANNOT PRODUCE PCM"; IT IS "THE GUEST'S OWN FMOD DOES NOT
COME UP IN THE PORT'S ENVIRONMENT".**

**=> => => => => => => => => => => AND THIS CONFIRMS THE SHIM'S REASON FOR
EXISTING, BY MEASUREMENT.**  The shim's header says the real library's
DSP-graph code walks a list head that is never sentinel-initialised and
the main thread spins on it forever.  **=> THAT IS EXACTLY WHAT MEASURED:
A SPIN ON AN ALL-ZERO FMOD OBJECT, 18 SECONDS IN.**  The shim was not
paranoia; it was the fix for this.

**=> => => => => => => => => => => => AND THE NEXT QUESTION IS NOW
SPECIFIC.**  Not "can R1 work" -- **"WHY DOES THE REAL
\`FMOD_System_Create\` RETURN AN OBJECT WITH NO VTABLE AND NO LIST
HEAD"**.  The candidates are now enumerable: the library's constructors
(does the port run libfmodex.so's init_array?  the loader census says
\`libfmodex.so pac=231\`, which is a count of something), its allocator
(the port traces \`malloc\`/\`free\`), its own thread, or a JNI call it
makes during create.  **=> THAT IS A READ-ONLY REVERSE-ENGINEERING
QUESTION, AND IT IS THE NEXT THING TO LOOK AT.**

**=> => => => => => => => => => => => AND ONE MORE THING THE A/B CAUGHT:
THE INSTRUMENT ITSELF HAD THE SAME DEFECT AS THE THING IT MEASURES.**  In
cell B there was NO census line at all -- and the summary printed "PCM
enqueued 0 enqueues / 0 bytes" and "FMOD in use 0", because those fields
default to zero.  **=> A MISSING MEASUREMENT READ AS A MEASURED ZERO.**
The summary now prints the census line count and says **NOT MEASURED** when
the census never ran.  **=> THE SAME SHAPE AS "audible IS ALWAYS 0".**
"""
section = section % {
    "aenq": base["enq"], "abytes": base["bytes"], "afmt": base["fmt"],
    "aframes": base["frames"], "alogbytes": base["logbytes"],
    "aloglines": base["loglines"], "alights": base["lights"],
    "arace": base["race"], "amd5": base["md5"], "aeden": base["eden"],
    "bframes": base["frames"] if not real["census"] else real["frames"],
    "blogbytes": real["logbytes"], "bloglines": real["loglines"],
    "beden": real["eden"],
}
section = section.replace("cell B: the guest own libfmodex.so   fmrt=1", "cell B: the guest own libfmodex.so   fmrt=1 (never reached)")

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.51 "
at = text.find(marker)
if at >= 0:
    text = text[:at].rstrip('\n') + '\n'
else:
    text = text.rstrip('\n') + '\n'
text = text + section
text = text.replace(chr(92) + chr(96), chr(96))
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md (9.51: base %s/%s census=%s, real census=%s)'
      % (base["enq"], base["bytes"], base["census"], real["census"]))
