#!/usr/bin/env python3
"""The guest's device profile (`eds/<device>.plist`) is ENCRYPTED -- and the
cipher is reversible, so the port can edit it and the guest will use it.

WHY THIS TOOL EXISTS
--------------------
The guest picks its GUI atlas tree with its own rule (cc::ImageResManager,
ELF 0x10f8cf8): every folder in the gui_assets listing is kept only if

    width <= screenW  OR  height <= screenH

against the screen size the port reports.  With 1280x720 the 2048x1536 tree
fails BOTH tests, so it is dropped before the sort and the HD tree on the
card is never a candidate -- which is why the picture is soft and why
changing the graphics costs a shader recompile.

The screen size is only half of it.  The other half is the device profile:
`eds/<device>.plist`, which carries the guest's own switches

    EXPECTED_SCREEN_WIDTH / EXPECTED_SCREEN_HEIGHT   what the guest thinks
                                                      the screen is
    GUI_LAYOUT_EXTENSION                             which atlas tree to use
    HIGH_RES_MENU / AA_LEVEL / FONT_PAGE_SIZE / ...  the rest of the quality

THE CIPHER
----------
RC4, with two instruction-level quirks that a textbook RC4 does not have:

  * the S box lives at B[2..257] inside a 262-byte frame;
  * the 133-byte drop loop reads the UN-OFFSETED B[i] while writing the
    offsetted B[2+j].

Key: 8 bytes  a5 35 b3 b1 e8 43 e7 cf  (the guest's immediate
0xCFE743E8_B1B335A5).

It is a stream cipher, so encrypt == decrypt: the plaintext and the
ciphertext have the SAME LENGTH, which is why the profile can be edited
in place inside a ROMFS volume without moving a single byte.

WHAT IT DOES NOT DO
-------------------
It does not put any EA material in the repository.  It only edits files
that are already on the SD card, in place, and it prints the plaintext on
demand for inspection.
"""

import argparse
import os
import re
import struct
import sys

KEY = bytes.fromhex('a535b3b1e843e7cf')
# The guest's file table lives near the end of a ROMFS image; the records
# are found by their NAMES, so the offset of the table does not matter.
NAME_RE = re.compile(rb'Android\w*\.plist')


def keystream(length: int) -> bytes:
    """The guest's RC4 variant: KSA, 133 dropped bytes, then the PRGA."""
    box = bytearray(262)
    for i in range(256):
        box[2 + i] = i
    j = 0
    for i in range(256):
        j = (j + box[2 + i] + KEY[i & 7]) & 0xFF
        box[2 + i], box[2 + j] = box[2 + j], box[2 + i]
    # the drop: reads box[i] (un-offset), writes box[2+j]
    j = 0
    i = 3
    for _ in range(133):
        value = box[i]
        j = (j + value) & 0xFF
        box[i] = box[2 + j]
        box[2 + j] = value
        i += 1
    out = bytearray()
    i = 0x86
    while len(out) < length:
        old = box[2 + i]
        j = (j + old) & 0xFF
        box[2 + i], box[2 + j] = box[2 + j], box[2 + i]
        k = (box[2 + i] + old) & 0xFF
        out.append(box[2 + k])
        i = (i + 1) & 0xFF
    return bytes(out)


def decrypt(data: bytes) -> bytes:
    stream = keystream(len(data))
    return bytes(a ^ b for a, b in zip(data, stream))


def encrypt(data: bytes) -> bytes:
    # A stream cipher: the same operation both ways.
    return decrypt(data)


def find_profiles(blob: bytes):
    """The (name, offset, size) of every Android*.plist inside a ROMFS."""
    found = []
    for m in NAME_RE.finditer(blob):
        # the record starts 32 bytes before the name
        rec = m.start() - 32
        if rec < 0:
            continue
        try:
            _parent, _sib, doff, size, _f20, nlen = struct.unpack_from(
                '<IIQQII', blob, rec)
        except struct.error:
            continue
        if nlen == 0 or nlen > 64:
            continue
        name = blob[rec + 32:rec + 32 + nlen]
        if not name.endswith(b'.plist'):
            continue
        # ROMFS data starts at 0x200
        start = 0x200 + doff
        if start + size > len(blob):
            continue
        found.append((name.decode(), start, size))
    # The name appears in the file table AND in a path string, so the same
    # profile is found twice.  Keep the first hit of each name.
    seen = set()
    unique = []
    for item in found:
        if item[0] in seen:
            continue
        seen.add(item[0])
        unique.append(item)
    return unique


def read_key_values(text: bytes):
    """The <key>/<value> pairs of the profile's XML."""
    out = {}
    for m in re.finditer(rb'<key>([^<]*)</key>\s*(?:<([^>/]+)(?:/>|>([^<]*)<))', text):
        key = m.group(1).decode('utf-8', 'replace')
        kind = m.group(2).decode('utf-8', 'replace')
        value = (m.group(3) or b'').decode('utf-8', 'replace')
        out[key] = (kind, value)
    return out


def write_key_value(text: bytes, key: str, kind: str, value: str) -> bytes:
    """Replace one <key>/<value> pair, keeping the byte length EXACTLY.

    The length must not change: the ciphertext is the plaintext XORed with a
    stream of the same length, and the profile sits in a ROMFS record whose
    size is fixed.
    """
    pat = re.compile(
        rb'(<key>' + re.escape(key.encode()) + rb'</key>\s*)<([^>/]+)(?:/>|>([^<]*)<)')
    m = pat.search(text)
    if m is None:
        raise KeyError(key)
    if kind == 'string':
        new = ('<key>%s</key><string>%s</string>' % (key, value)).encode()
    elif kind == 'integer':
        new = ('<key>%s</key><integer>%s</integer>' % (key, value)).encode()
    elif kind == 'real':
        new = ('<key>%s</key><real>%s</real>' % (key, value)).encode()
    elif kind == 'true':
        new = ('<key>%s</key><true/>' % key).encode()
    elif kind == 'false':
        new = ('<key>%s</key><false/>' % key).encode()
    else:
        raise ValueError('unknown value kind %r' % kind)
    old = m.group(0)
    if len(new) != len(old):
        # The length is fixed at the END (fit_to_length), because a profile
        # that comes from ANOTHER profile may need a different length and
        # the whitespace around the XML is what absorbs the difference.
        pass
    return text[:m.start()] + new + text[m.end():]


def fit_to_length(text: bytes, size: int) -> bytes:
    """Make the plaintext exactly `size` bytes.

    The profile sits in a ROMFS record whose size is fixed, and the cipher
    is a stream cipher, so the plaintext must be exactly that long.

    Whitespace BETWEEN elements is what absorbs the difference: XML does not
    care about it, and it is not inside a text value, so shortening it
    cannot change what the guest reads.  A shorter plaintext simply grows
    trailing whitespace instead.
    """
    if len(text) == size:
        return text
    if len(text) < size:
        return text + b' ' * (size - len(text))
    out = bytearray(text)
    while len(out) > size:
        shrunk = False
        for m in re.finditer(rb'>(\s+)<', bytes(out)):
            if len(m.group(1)) > 1:
                del out[m.end(1) - 1]
                shrunk = True
                break
        if not shrunk:
            raise ValueError(
                'the edited profile is %d bytes and the record is only %d, and '
                'there is no whitespace left to shorten' % (len(out), size))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('volume', help='the ROMFS volume to work on')
    ap.add_argument('--dump', action='store_true',
                    help='print the decrypted profiles and their keys')
    ap.add_argument('--set', action='append', default=[], metavar='KEY=VALUE',
                    help='set one key (VALUE may be int/float/string; '
                         'true/false are recognised)')
    ap.add_argument('--profile', default='AndroidHigh.plist',
                    help='which profile to edit (default: AndroidHigh.plist)')
    ap.add_argument('--from-profile',
                    help='take this profile\'s content as the base instead of '
                         'the one being edited (the guest asks for ONE name, '
                         'so the content of another profile can be shipped '
                         'under that name)')
    ap.add_argument('--out', help='write the edited profile here instead of '
                                   'editing the volume in place')
    ap.add_argument('--copy-record-from',
                    help='take this profile\'s record (the CIPHERTEXT, byte '
                         'for byte) out of another volume file and write it '
                         'here -- the way to put a profile back exactly as it '
                         'was')
    args = ap.parse_args()

    with open(args.volume, 'rb') as f:
        blob = bytearray(f.read())
    profiles = find_profiles(bytes(blob))
    if not profiles:
        print('no device profile found in %s' % args.volume, file=sys.stderr)
        return 2

    edits = []
    for item in args.set:
        key, _, value = item.partition('=')
        if value in ('true', 'false'):
            edits.append((key, 'true' if value == 'true' else 'false', value))
        else:
            edits.append((key, None, value))

    if args.dump:
        for name, start, size in profiles:
            text = decrypt(bytes(blob[start:start + size]))
            good = text.startswith(b'<?xml')
            print('== %s  (%d bytes, xml=%s)' % (name, size, good))
            if not good:
                continue
            for key, (kind, value) in sorted(read_key_values(text).items()):
                if key in ('EXPECTED_SCREEN_WIDTH', 'EXPECTED_SCREEN_HEIGHT',
                           'EXPECTED_RENDERER', 'GUI_LAYOUT_EXTENSION',
                           'GUI_ATLAS_OVERRIDE', 'HIGH_RES_MENU', 'AA_LEVEL',
                           'FONT_PAGE_SIZE', 'MIPMAP_BIAS', 'MENU_RES'):
                    print('     %-26s = %s %s' % (key, kind, value))
        if not args.set:
            return 0

    # ---- edit one profile in place ----
    target = None
    for name, start, size in profiles:
        if name == args.profile:
            target = (name, start, size)
            break
    if target is None:
        print('no profile named %r (have: %s)'
              % (args.profile, ', '.join(n for n, _, _ in profiles)), file=sys.stderr)
        return 2
    name, start, size = target
    if args.copy_record_from:
        with open(args.copy_record_from, 'rb') as f:
            other = f.read()
        donor = None
        for n2, s2, z2 in find_profiles(other):
            if n2 == args.profile:
                donor = (n2, s2, z2)
                break
        if donor is None:
            print('no profile named %r in %s' % (args.profile, args.copy_record_from),
                  file=sys.stderr)
            return 2
        raw = other[donor[1]:donor[1] + donor[2]]
        if len(raw) != size:
            print('the donor record is %d bytes and this one is %d; they must '
                  'be the same size' % (len(raw), size), file=sys.stderr)
            return 2
        blob[start:start + size] = raw
        check = decrypt(bytes(blob[start:start + size]))
        if not check.startswith(b'<?xml'):
            print('the copied record does not decrypt to a profile; the volume '
                  'has NOT been written', file=sys.stderr)
            return 9
        if args.out:
            with open(args.out, 'wb') as f:
                f.write(bytes(blob))
            print('wrote %s' % args.out)
        else:
            with open(args.volume, 'wb') as f:
                f.write(bytes(blob))
            print('restored %s from %s (the record decrypts to a profile again)'
                  % (name, args.copy_record_from))
        return 0
    raw = bytes(blob[start:start + size])
    text = decrypt(raw)
    if not text.startswith(b'<?xml'):
        # SELF-HEALING: a record that is ALREADY a plaintext profile is one
        # that a tool wrote the wrong way round.  The ciphertext belongs
        # there; the plaintext does not.  Take it as the plaintext and
        # encrypt it back.
        if raw.startswith(b'<?xml'):
            print('%s holds a PLAIN TEXT profile; encrypting it back'
                  % name, file=sys.stderr)
            text = raw
        else:
            print('%s does not decrypt to XML; refusing to touch it' % name,
                  file=sys.stderr)
            return 3
    if args.from_profile:
        base = None
        for n2, s2, z2 in profiles:
            if n2 == args.from_profile:
                base = (n2, s2, z2)
                break
        if base is None:
            print('no profile named %r' % args.from_profile, file=sys.stderr)
            return 2
        text = decrypt(bytes(blob[base[1]:base[1] + base[2]]))
        if not text.startswith(b'<?xml'):
            print('%s does not decrypt to XML' % base[0], file=sys.stderr)
            return 3
        print('%s: taking the content of %s' % (name, base[0]))
    current = read_key_values(text)
    for key, kind, value in edits:
        if key not in current:
            print('the profile has no key %r; refusing to add one (the byte '
                  'length must not change)' % key, file=sys.stderr)
            return 4
        old_kind = current[key][0]
        if kind is None:
            if old_kind == 'integer':
                kind = 'integer'
                try:
                    int(value)
                except ValueError:
                    print('%s is an integer key and %r is not an integer'
                          % (key, value), file=sys.stderr)
                    return 5
            elif old_kind == 'real':
                kind = 'real'
            else:
                kind = 'string'
        try:
            text = write_key_value(text, key, kind, value)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 6
        print('%s: %s = %s (%s)' % (name, key, value, kind))
    if not edits:
        return 0
    try:
        text = fit_to_length(text, size)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 7
    # THE CIPHERTEXT GOES IN, NOT THE PLAINTEXT.
    cipher = encrypt(text)
    if len(cipher) != size:
        print('the ciphertext is %d bytes and the record is %d; refusing to '
              'write it' % (len(cipher), size), file=sys.stderr)
        return 8
    blob[start:start + size] = cipher
    # THE SELF-CHECK: read it back the way the guest will and make sure it
    # is a profile again.  A tool that writes the plaintext where the
    # ciphertext belongs leaves the guest with a file it cannot read, and
    # the only symptom is a silent failure a hundred frames later.
    check = decrypt(bytes(blob[start:start + size]))
    if not check.startswith(b'<?xml'):
        print('THE WRITE DID NOT ROUND TRIP: the record does not decrypt to a '
              'profile.  The volume has NOT been written.', file=sys.stderr)
        return 9
    if args.out:
        with open(args.out, 'wb') as f:
            f.write(bytes(blob))
        print('wrote %s' % args.out)
    else:
        with open(args.volume, 'wb') as f:
            f.write(bytes(blob))
        print('patched %s in place (the record decrypts to a profile again)'
              % args.volume)
    return 0


if __name__ == '__main__':
    sys.exit(main())
