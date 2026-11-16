# -*- coding: utf-8 -*-
"""Add the second controlled round (racecount2) to section 9.47."""
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


path = os.path.join(ROOT, 'docs', 'OPEN-ISSUES.md')
text, crlf, bom = load(path)

old = """races continuously and never once sat still.  **=> THE 27 OF `yield2` AND THE
30 HERE ARE THE SAME GAME BEHAVIOUR**, measured through two different
instruments, and the "regression" was the instrument.
"""
new = """races continuously and never once sat still.  **=> THE 27 OF `yield2` AND THE
30 HERE ARE THE SAME KIND OF NUMBER** -- the guest's own resolution events --
and the "regression" was the instrument, not the game.

**=> => => => => => => => => => => => => => AND A SECOND CONTROLLED ROUND, ON
THE SAME BUILD, CONFIRMS THE POINT AND CORRECTS ONE IMPRESSION.**  Round
`racecount2` (2026-11-16, 408 s) restored the very state `racecount1` had
started from and ran the SAME binary:

    port counters      6 start-lights resolutions / 9 race-screen resolutions
    old text count     2 start lights / 2 race screen
    frames           12600        pixels lit  921600 of 921600
    md5 mismatch        0         TrackDesc::load failures  0
    race HUD string    22

**=> => => => => => => => => => => => => => AND THE VARIATION IS THE POINT.**
The port's number MOVED with the game: 30 and 45 in one round, 6 and 9 in
another, from the same save, because the guest's event progression is not
deterministic -- the two rounds ran at 5.9 and 31 frames a second, one of
them in a heavy race scene and the other in a light one, and the guest spent
this round mostly in the front end.  **=> THE TEXT COUNT DID NOT MOVE AT
ALL: 2 IN BOTH ROUNDS, AND 2 IN EVERY ROUND BEFORE THEM.**  **=> A NUMBER THAT
CANNOT MOVE IS NOT A MEASUREMENT.**  The 27 of `yield2` was the same text
count under a different log policy; 2 is the same text count under this one.
"""
text = sub(text, old, new, '9.47 second round')
save(path, text, crlf, bom)
print('OK docs/OPEN-ISSUES.md')

path = os.path.join(ROOT, 'PORT_PROGRESS.md')
text, crlf, bom = load(path)
old = """**WHAT IT MEASURES NOW.**  Round `racecount1` (2026-11-16, 409 s):"""
new = """**WHAT IT MEASURES NOW.**  Two rounds, the same build, the second one restored
the first one's starting save.  Round `racecount1` (409 s): 30 / 45; round
`racecount2` (408 s): **6 / 9** -- the port's number moves with the game,
because the guest's event progression is not deterministic, while the old
text count stayed at **2 / 2 in both rounds** and in every round before them.
A number that cannot move is not a measurement.  Round `racecount1`:"""
text = sub(text, old, new, 'PORT_PROGRESS two rounds')
save(path, text, crlf, bom)
print('OK PORT_PROGRESS.md')
