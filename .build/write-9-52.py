# -*- coding: utf-8 -*-
"""Section 9.52: where the guest waits when the guest's own FMOD is in use."""
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


section = """### 9.52 Where the guest waits when the guest's own FMOD is in use

**=> => => => => THE ATTRIBUTION IS NOW SPECIFIC, AND IT IS NOT AN ASSET
PROBLEM.**  In the round of section 9.51 (the guest's own
\`libfmodex.so\`), the port's own instruments say:

**1. THE GUEST'S MAIN THREAD WAITS ON A CONDITION VARIABLE THAT IS NEVER
SIGNALLED.**  The port's cond-wait ledger, once a second:

    phase=cond-wait-ledger ... obj=0x211002fe60 known=1 waited_ms=120000
        waits=1 signals=1 deficit_at_entry=0 wait_caller=0x2109436a30
        budget_ms=120000 outcome=GIVING-UP
    phase=cond-wait-abandoned ... reason=no-signal-within-budget
        note=the_guest_was_waiting_on_this_object

**=> THE WAIT COUNT GROWS (1, 2, 3, ...) AND THE SIGNAL COUNT NEVER DOES
(1).**  Each wait exhausts the 120 s budget, the port turns it into a
spurious wakeup so the guest is not stuck forever, and the guest goes
straight back to waiting.  **=> THE GUEST'S BOOT IS WAITING FOR AN EVENT
THAT NEVER COMES.**  The ledger also records the WAITER'S OWN ADDRESS
(\`wait_caller=0x2109436a30\`), which is the address to attribute next.

**2. THE ASSET SYSTEM WAS HEALTHY IN THE SAME ROUND.**  The port's own
invariant, at the end of the round:

    phase=asset-invariant-summary checked=8649 agreed=8649 disagreed=0
        unresolved=0 published=8649 controls=2 agree_control=1
        detect_control=1 control_ok=1

**=> ALL 8,649 PUBLISHED ASSETS AGREED WITH WHAT WAS SERVED, AND BOTH
CONTROLS BEHAVED.**  **=> IT IS NOT THE ASSETS.**  The guest also asked for
and RECEIVED both audio asset lists (\`asset_list_audio.txt\` and
\`asset_list_audio_base.txt\`, rc=0).

**3. THE GAME'S OWN AUDIO PATH RAN IN THAT ROUND TOO.**  The guest printed
\`FMOD_ZERO_VOLUME_VIRTUALISATION\` there as well, and its last words
were ordinary boot lines ("Loading shader features file: AndroidHigh.feats",
"Real Racing 3 14.0.1 [a527]").

**=> => => => => => AND THE REAL LIBRARY'S CREATE PATH, READ FROM ITS OWN
BINARY (objdump).**  \`FMOD_System_Create\` lives at 0x21a70 (532 bytes)
and its first move is through a GLOBAL:

    21a8c: adrp x21, 139000
    21a90: ldr  x21, [x21, #2688]   ; a global slot -> a global object
    21aa0: ldr  x8,  [x21]           ; its VTABLE
    21aac: ldr  x0,  [x8, #8]        ; vtable[1]
    21ab0: bl   afb18                ; call through it
    21ab8: bl   4c068                ; and 4c068 is the CONSTRUCTOR

and the constructor writes the vtable itself:

    4c09c: stp  x9, x8, [x19]        ; vtable at [obj+0] and [obj+8]
    4c0a4: bl   42c1c                ; the base constructor
    4c0ac: bl   32e0c                ; two validators
    4c0c4: str  xzr, [x19, #1960]    ; ... and fields out to 1968
    4c0cc: str  x8,  [x19, #1944]

**=> THE SYSTEM OBJECT IS AT LEAST 1972 BYTES, AND THE CREATE PATH'S FIRST
STEP IS AN INDIRECT CALL THROUGH A GLOBAL VTABLE POINTER.**  If that global
is not initialised in the port's environment, the path reads \\[0x8] of a
null vtable -- which is EXACTLY the "unmapped Read64 @ 0x8 / Read32 @ 0x1C0
pair" the shim's header records.  **=> THE HISTORIC CRASH AND THIS HANG ARE
THE SAME SHAPE.**

**=> => => => => => => AND THE LIBRARY'S OWN DEPENDENCIES ARE FEW, AND THE
PORT DOES NOT BREAK ANY OF THEM.**  The library imports only \`malloc\`,
\`free\`, \`pthread_once\`, \`dlopen\`, \`dlsym\` and
\`__cxa_atexit\`.  The port's \`malloc\` is a real wrapper over
\`std::malloc\` (it adds tracing, it allocates for real), and the port does
NOT intercept \`operator new\`/\`delete\`.  **=> THE ALLOCATOR IS NOT
THE PROBLEM.**

**=> => => => => => => => => => AND ONE MEASUREMENT CAVEAT THAT HAS TO BE
STATED, BECAUSE IT IS THE SAME KIND OF MISTAKE.**  The port's reaper prints
the engine object's fields BY NAME (\`f8\`, \`vt\`, \`table\`,
\`t0..t3\`) -- and it reads them at THE PORT'S SHIM'S OFFSETS.  With the
guest's own library the object has a DIFFERENT layout, so **the
"all-zero engine" reading is layout-dependent and must not be taken as a
measurement of the real object.**  **=> THE INSTRUMENT MUST NOT ASSUME THE
LAYOUT IT WAS WRITTEN FOR: IT SHOULD PRINT THE OBJECT'S RAW WORDS.**  The
spin itself is real (the guest made no progress for 380 s); only the field
names are untrustworthy.

**=> => => => => => => => => => => => AND THE NEXT THREE STEPS ARE NOW
SPECIFIC.**

1. **ATTRIBUTE THE WAITER.**  The ledger records the waiter's own address
   (0x2109436a30).  With the round's image base, that is a guest function
   in the guest's own symbol table -- and it NAMES the code that is waiting.
2. **MAKE THE REAPER'S DUMP LAYOUT-INDEPENDENT** (raw words), so the next
   round can read the engine object truthfully.
3. **FIND OUT WHETHER THE GLOBAL VTABLE THE CREATE PATH USES IS
   INITIALISED** -- it is a GOT slot inside libfmodex.so, so it is either a
   RELATIVE relocation the port applies or something a constructor writes.
   The port's constructor accounting for the dependency is the place to look
   (the FMOD constructor analysis says the two FMOD constructors run on
   device).

**=> => => => => => => => => => => => => AND UNTIL THAT IS DONE, THE
PORT'S SHIM STAYS.**  It is not a workaround for a missing feature: it is
what keeps the guest's audio code paths from waiting for an event that
never arrives.
"""

path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)
marker = "\n### 9.52 "
at = text.find(marker)
if at >= 0:
    text = text[:at].rstrip('\n') + '\n'
else:
    text = text.rstrip('\n') + '\n'
text = text + section
text = text.replace(chr(92) + chr(96), chr(96))
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md (9.52)')
