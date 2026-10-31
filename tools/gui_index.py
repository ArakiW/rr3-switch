#!/usr/bin/env python3
"""Index the guest's own UI definition files (`.gui`), so that a control id
can be located without booting the game.

WHAT THIS TOOL DOES
--------------------
Every screen the guest owns is an XML file under `gui/processed/` on the
card, and every one of them is ENCRYPTED with the guest's own RC4 variant.
Decrypted, they are plain XML: a root element named after the screen, and
a tree of widgets under it, each carrying an `id_string` -- the id the
guest's own code looks up at runtime ("BTN_RACE_NOW").

This tool decrypts those files, reads them, and writes ONE machine-readable
index containing:

  * every element that carries an `id_string`, with its screen, its tag,
    its numeric id, its own <GuiTransform> (the layout box), its ancestor
    chain and its position in the file;
  * a per-screen map, keyed by the file the screen came from;
  * a global reverse index, `id_string -> [screen, ...]`.

THE QUESTION IT ANSWERS
------------------------
    "where is BTN_BANK?"                which screen holds it, and where
                                        in that file
    "which screens have BTN_RACE_NOW?"  the reverse index, straight out

THE CIPHER
----------
The `.gui` files use the SAME RC4 variant and the SAME key as the device
profiles (`eds/<device>.plist`), and `tools/rr3-plist.py` already
implements that cipher.  This tool IMPORTS it rather than copying it, so
there is exactly one cipher in the repository and the two tools cannot
silently disagree about what the guest's bytes decrypt to.  A second copy
would have been the thing to avoid: two implementations of one cipher is
how a port ends up with a tool that "works" and a guest that does not.

WHAT IT MUST NOT BE USED FOR
----------------------------
It is a READER.  It opens the card's files read-only, it never writes to
the card, it does not re-encrypt anything, and it must not be used to ship
modified UI back to the game -- the port's rule is that the guest's own
data is left exactly as it was found.  It is also not a general XML tool:
it knows the guest's layout, not XML in general.

THE OUTPUT IS EA MATERIAL
-------------------------
The index contains EA's control ids and screen names, so it is
EA-DERIVED MATERIAL and it MUST NOT BE COMMITTED to this repository (or
to any other public remote).  The tool therefore writes to exactly one
path: the one `--out` names.  The default output path lives under
`.build/`, which the repository's `.gitignore` already excludes, and the
tool warns on stderr if it is ever pointed at a path inside the
repository that is not ignored.

THE INPUTS ARE THE CARD'S
-------------------------
Nothing is written to the input directory, and nothing EA-owned is ever
copied into the repository by this tool: it reads the card and writes the
index, and the index is disposable.
"""

import argparse
import importlib.util
import io
import json
import os
import sys
import xml.etree.ElementTree as ET
import xml.parsers.expat as expat
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The cipher lives with its sibling tool, so that there is one of it.
PLIST_TOOL = os.path.join(HERE, "rr3-plist.py")

# Where the card's UI definitions live on a developer machine: the
# emulator's SD card image inside the repository's tooling area.  On a real
# card the same tree is under
# `sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/gui/processed`.
DEFAULT_DIR = os.path.join(
    REPO, ".tools", "emulators", "ryubing-mcp-0.2.0", "portable", "sdcard",
    "switch", "rr3-native-rehost", "data", "loose", "files", ".depot", "gui",
    "processed")

# The index is EA-derived material, so the default output goes under
# `.build/`, which .gitignore already excludes.
DEFAULT_OUT = os.path.join(REPO, ".build", "guiindex", "gui-index.json")

# The guest's key, spelled out here so the index can record what it was
# built with.  The cipher itself is imported, not restated.
KEY_HEX = "a535b3b1e843e7cf"
CIPHER_NAME = ("RC4 variant: KSA, 133 dropped bytes, PRGA from 0x86 "
               "(tools/rr3-plist.py)")

# The positive control: an id the guest's own code looks up, which the
# index MUST contain for the index to be believed.
CONTROL_ID = "BTN_RACE_NOW"

# The guest's layout numbers, in the order they are printed.
LAYOUT_KEYS = ("x", "y", "width", "height")


# ---------------------------------------------------------------------------
# the cipher
# ---------------------------------------------------------------------------

def load_cipher():
    """Import the guest's RC4 variant from `tools/rr3-plist.py`.

    Importing, not copying: one cipher in the repository.  The module has
    no import-time side effects -- its `main()` is behind the usual
    `__main__` guard -- so loading it is safe.
    """
    if not os.path.exists(PLIST_TOOL):
        raise SystemExit('the cipher tool is missing: %s' % PLIST_TOOL)
    spec = importlib.util.spec_from_file_location('rr3_plist', PLIST_TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# one `.gui` file may hold more than one XML document
# ---------------------------------------------------------------------------

def _skip_misc(data, pos):
    """Skip whitespace, XML declarations, comments and doctypes."""
    while True:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data.startswith(b"<?", pos):
            end = data.find(b"?>", pos)
            if end < 0:
                return len(data)
            pos = end + 2
        elif data.startswith(b"<!--", pos):
            end = data.find(b"-->", pos)
            if end < 0:
                return len(data)
            pos = end + 3
        elif data.startswith(b"<!", pos):
            # a doctype, possibly with an internal subset
            depth = 0
            index = pos
            while index < len(data):
                char = data[index:index + 1]
                if char == b"[":
                    depth += 1
                elif char == b"]":
                    depth -= 1
                elif char == b">" and depth <= 0:
                    index += 1
                    break
                index += 1
            pos = index
        else:
            return pos


def split_documents(data):
    """Split the decrypted bytes into the XML documents they hold.

    Most `.gui` files hold one document (the screen).  Some hold the
    screen plus an appended `<animation>` or `<Themes>` document, and the
    guest's XML reader is happy to read them back to back.  A plain
    `ET.fromstring` rejects those with "junk after document element", so
    the documents are cut apart first, at the end of the root element.
    """
    docs = []
    pos = 0
    end = len(data)
    while pos < end:
        pos = _skip_misc(data, pos)
        if pos >= end:
            break
        parser = expat.ParserCreate()
        state = {'depth': 0, 'end': None}

        def _start(_name, _attrs):
            state['depth'] += 1

        def _stop(_name):
            state['depth'] -= 1
            if state['depth'] == 0:
                state['end'] = parser.CurrentByteIndex

        parser.StartElementHandler = _start
        parser.EndElementHandler = _stop
        try:
            parser.Parse(data[pos:], True)
        except expat.ExpatError as exc:
            if state['end'] is None:
                raise ValueError('the document at byte %d does not parse: %s'
                                 % (pos, expat.ErrorString(exc.code)))
            stop = pos + state['end']
            close = data.find(b'>', stop)
            if close < 0:
                raise ValueError('the document at byte %d has no closing tag'
                                 % pos)
            docs.append(data[pos:close + 1])
            pos = close + 1
        else:
            docs.append(data[pos:])
            break
    return docs


# ---------------------------------------------------------------------------
# reading one file
# ---------------------------------------------------------------------------

def _label(element):
    """How an ancestor is named in a chain: its id_string, else its tag."""
    return element.get('id_string') or element.tag


def _numeric_id(raw):
    """The guest's numeric id, as an int (every id_string element has one)."""
    if raw is None:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def read_gui(path, relative, cipher):
    """Read one `.gui` file and return its screen entry.

    The entry holds the screen's name (the root tag of its first
    document), the tags of every document it holds, and one record per
    element that carries an `id_string`.
    """
    with open(path, 'rb') as handle:
        raw = handle.read()
    text = cipher.decrypt(raw)
    documents = split_documents(text)
    roots = []
    for document in documents:
        try:
            roots.append(ET.fromstring(document))
        except ET.ParseError as exc:
            raise ValueError('%s does not parse: %s' % (relative, exc))
    if not roots:
        raise ValueError('%s holds no XML document' % relative)

    screen = roots[0].tag
    elements = []
    order = [0]

    def walk(element, ancestors):
        for child in element:
            order[0] += 1
            chain = ancestors + [_label(element)]
            ident = child.get('id_string')
            if ident:
                transform = None
                for grandchild in child:
                    if grandchild.tag == 'GuiTransform':
                        transform = dict(grandchild.attrib)
                        break
                elements.append({
                    'screen': screen,
                    'type': child.tag,
                    'id': _numeric_id(child.get('id')),
                    'id_string': ident,
                    'transform': transform,
                    'ancestors': list(chain),
                    'order': order[0],
                })
            walk(child, chain)

    for root in roots:
        walk(root, [])

    return {
        'screen': screen,
        'file': relative,
        'documents': [root.tag for root in roots],
        'element_count': order[0],
        'elements': elements,
    }


# ---------------------------------------------------------------------------
# the index
# ---------------------------------------------------------------------------

def build_index(source, recursive, cipher):
    """Decrypt and index every `.gui` file under `source`."""
    if not os.path.isdir(source):
        raise ValueError('%s is not a directory' % source)
    names = []
    if recursive:
        for root, _dirs, files in os.walk(source):
            for name in files:
                if name.endswith('.gui'):
                    names.append(os.path.join(root, name))
    else:
        for name in os.listdir(source):
            full = os.path.join(source, name)
            if name.endswith('.gui') and os.path.isfile(full):
                names.append(full)
    names.sort()
    if not names:
        raise ValueError('no .gui files under %s' % source)

    screens = {}
    by_screen_name = {}
    ids = {}
    locations = {}
    id_counts = Counter()
    element_count = 0
    id_element_count = 0
    document_count = 0
    failures = []

    for path in names:
        relative = os.path.relpath(path, source).replace(os.sep, '/')
        try:
            entry = read_gui(path, relative, cipher)
        except (ValueError, OSError) as exc:
            failures.append((relative, str(exc)))
            continue
        screens[relative] = entry
        by_screen_name.setdefault(entry['screen'], []).append(relative)
        document_count += len(entry['documents'])
        element_count += entry['element_count']
        id_element_count += len(entry['elements'])
        for element in entry['elements']:
            ident = element['id_string']
            id_counts[ident] += 1
            ids.setdefault(ident, set()).add(element['screen'])
            locations.setdefault(ident, []).append(
                (relative, entry['screen'], element))

    reverse = {}
    for ident in sorted(ids):
        reverse[ident] = sorted(ids[ident])

    index = {
        'metadata': {
            'tool': 'tools/gui_index.py',
            'source_dir': os.path.abspath(source),
            'recursive': bool(recursive),
            'file_count': len(screens),
            'document_count': document_count,
            'element_count': element_count,
            'id_string_element_count': id_element_count,
            'distinct_id_count': len(reverse),
            'cipher': CIPHER_NAME,
            'key': KEY_HEX,
            'failed_files': failures,
        },
        'screens': screens,
        'by_screen_name': by_screen_name,
        'ids': reverse,
    }
    return index, locations, id_counts


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------

def layout_text(transform):
    """The four layout numbers of a transform, as they read in the file."""
    if not transform:
        return '(no transform)'
    parts = []
    for key in LAYOUT_KEYS:
        parts.append('%s=%s' % (key, transform.get(key, '-')))
    return ' '.join(parts)


def print_summary(index, id_counts, top, out_path):
    meta = index['metadata']
    print('gui index: %d screens, %d documents, %d id_string elements'
          % (meta['file_count'], meta['document_count'],
             meta['id_string_element_count']))
    print('            %d distinct control ids' % meta['distinct_id_count'])
    print('source:     %s' % meta['source_dir'])
    print('cipher:     %s' % meta['cipher'])
    print('key:        %s' % meta['key'])
    print('wrote:      %s' % out_path)
    if top > 0 and id_counts:
        print('')
        print('the most common control ids:')
        for ident, count in id_counts.most_common(top):
            print('  %-28s %5d' % (ident, count))


def print_find(ident, locations, index):
    """Report where one control id lives."""
    hits = locations.get(ident, [])
    if not hits:
        print('')
        print('== %s: NOT FOUND in %d screens (%d distinct ids)'
              % (ident, index['metadata']['file_count'],
                 index['metadata']['distinct_id_count']))
        return False
    screens = index['ids'][ident]
    print('')
    print('== %s' % ident)
    print('   screens: %s' % ', '.join(screens))
    print('   %d occurrence(s)' % len(hits))
    for relative, screen, element in hits:
        print('   %s  (%s)' % (relative, screen))
        print('      type=%s id=%s order=%d'
              % (element['type'], element['id'], element['order']))
        print('      %s' % layout_text(element['transform']))
        print('      in: %s' % ' > '.join(element['ancestors']))
    return True


def verify(index, locations, source, cipher):
    """The positive control: prove the index can find something known.

    A tool that has never been shown to find a known thing has not been
    tested.  This decrypts a file, checks the plaintext really is XML,
    checks it parses, and checks the known control id is in the index and
    resolves back to the screen it came from.
    """
    checks = []

    def ok(label):
        checks.append((True, label))
        print('  ok  %s' % label)

    def bad(label, detail):
        checks.append((False, label))
        print('  FAIL  %s: %s' % (label, detail))

    print('verify:')
    names = sorted(index['screens'])
    if not names:
        bad('the index is not empty', 'no screens were indexed')
        return False

    # the file that holds the control id, else the first file indexed
    control_file = None
    for relative, _screen, _element in locations.get(CONTROL_ID, []):
        control_file = relative
        break
    if control_file is None:
        control_file = names[0]

    path = os.path.join(source, control_file.replace('/', os.sep))
    try:
        with open(path, 'rb') as handle:
            raw = handle.read()
    except OSError as exc:
        bad('the control file can be read', str(exc))
        return False
    print('  control file: %s' % control_file)

    text = cipher.decrypt(raw)
    if text.startswith(b'<?xml') or text.startswith(b'<'):
        ok('the plaintext starts with XML')
    else:
        bad('the plaintext starts with XML',
            'it starts with %r' % text[:16])
        return False

    try:
        documents = split_documents(text)
        roots = [ET.fromstring(document) for document in documents]
    except (ValueError, ET.ParseError) as exc:
        bad('the plaintext parses as XML', str(exc))
        return False
    ok('the plaintext parses as XML (%d document(s), root %s)'
       % (len(roots), roots[0].tag))

    if CONTROL_ID in index['ids']:
        ok('%s is in the index (%s)'
           % (CONTROL_ID, ', '.join(index['ids'][CONTROL_ID])))
    else:
        bad('%s is in the index' % CONTROL_ID, 'it is not there')
        return False

    screens = {screen for _relative, screen, _element in locations.get(CONTROL_ID, [])}
    if screens:
        ok('%s resolves back to %s' % (CONTROL_ID, ', '.join(sorted(screens))))
    else:
        bad('%s resolves back to a screen' % CONTROL_ID, 'it has no location')
        return False

    return all(passed for passed, _label in checks)


# ---------------------------------------------------------------------------

def warn_if_tracked(path):
    """Warn when the output lands somewhere the repository might commit."""
    absolute = os.path.abspath(path)
    root = os.path.abspath(REPO)
    if not (absolute == root or absolute.startswith(root + os.sep)):
        return
    ignored = os.path.join(root, '.build')
    if absolute == ignored or absolute.startswith(ignored + os.sep):
        return
    print('warning: %s is inside the repository but not under .build/; the '
          'index is EA-derived material and must not be committed'
          % absolute, file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='exit codes: 0 everything asked for was found; 1 the '
               'positive control failed; 2 the source could not be read; '
               '3 an id given with --find was not in the index.')
    ap.add_argument('directory', nargs='?', default=None,
                    help='the directory of .gui files to index (default: the '
                         'card\'s gui/processed under the emulator SD image '
                         'in .tools/)')
    ap.add_argument('--out', default=DEFAULT_OUT,
                    help='where to write the index (default: %s -- under '
                         '.build/, which .gitignore excludes)'
                         % os.path.relpath(DEFAULT_OUT, REPO))
    ap.add_argument('--find', action='append', default=[], metavar='ID_STRING',
                    help='report where this control id lives; may be given '
                         'more than once')
    ap.add_argument('--verify', action='store_true',
                    help='run the positive control: decrypt a known file, '
                         'check it is XML, and check a known control id is in '
                         'the index')
    ap.add_argument('--recursive', action='store_true',
                    help='also index the sub-directories of the source '
                         '(default: the top level only, which is the 939 '
                         'screen files)')
    ap.add_argument('--top', type=int, default=10, metavar='N',
                    help='how many of the most common control ids to print '
                         '(default: 10; 0 prints none)')
    args = ap.parse_args()

    source = args.directory if args.directory else DEFAULT_DIR
    if not os.path.isdir(source):
        print('%s is not a directory; pass the card\'s gui/processed path'
              % source, file=sys.stderr)
        return 2

    cipher = load_cipher()
    try:
        index, locations, id_counts = build_index(source, args.recursive, cipher)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    meta = index['metadata']
    if meta['failed_files']:
        print('%d file(s) could not be read:' % len(meta['failed_files']),
              file=sys.stderr)
        for relative, detail in meta['failed_files']:
            print('   %s: %s' % (relative, detail), file=sys.stderr)
    if not meta['file_count']:
        print('no .gui file could be read under %s' % source, file=sys.stderr)
        return 2

    out_path = os.path.abspath(args.out)
    parent = os.path.dirname(out_path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent, exist_ok=True)
    warn_if_tracked(out_path)
    with io.open(out_path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(index, handle, indent=2, sort_keys=False)
        handle.write('\n')
    print_summary(index, id_counts, args.top, out_path)

    found_all = True
    for ident in args.find:
        if not print_find(ident, locations, index):
            found_all = False

    if args.verify:
        if not verify(index, locations, source, cipher):
            return 1

    if args.find and not found_all:
        return 3
    return 0


if __name__ == '__main__':
    sys.exit(main())
