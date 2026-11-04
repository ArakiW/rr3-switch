#!/usr/bin/env python3
"""Check that a handoff document is written in English only.

WHY
---
`docs/OPEN-ISSUES.md` is THE handoff document: it is what a person (or an
agent) reads to pick the project up without the conversation.  Its rule is
that it is written in English, because the code, the log lines and the
source are all English and a mixed-language handoff is harder to read than
either.  The rule is only worth anything if it is CHECKED, so this script
counts the lines that contain CJK characters and exits non-zero when it
finds any.

A line counts as CJK when it contains a character from the CJK ideographs, the
CJK punctuation block, the full-width forms, the kana blocks, or the
half-width katakana block.  Latin text with a stray Chinese word is still a
violation: the point is that the document is readable by a reader who only
has English.

TWO EXCEPTIONS, BOTH COUNTED SEPARATELY
---------------------------------------
* A line inside a fenced code block is QUOTED EVIDENCE.  The guest speaks
  Chinese, so its own log lines are quoted verbatim all over these
  documents; those are reported as "quoted evidence" and do not fail the
  check.
* `PORT_PROGRESS.md` and `tools/README-hardware-test.md` are the user's own
  documents and are written in the user's language.  They are not bound by
  this rule; pass them only to see what they contain.

USAGE
-----
    python tools/check_doc_language.py docs/OPEN-ISSUES.md
    python tools/check_doc_language.py --list docs

Exit codes: 0 = clean, 1 = at least one CJK line, 2 = bad usage.
"""

import argparse
import io
import os
import sys

RANGES = (
    (0x4E00, 0x9FFF),    # CJK unified ideographs
    (0x3400, 0x4DBF),    # CJK unified ideographs extension A
    (0x3000, 0x303F),    # CJK punctuation
    (0xFF00, 0xFFEF),    # full-width forms
    (0xFF66, 0xFF9D),    # half-width katakana
    (0x3040, 0x309F),    # hiragana
    (0x30A0, 0x30FF),    # katakana
)


def has_cjk(text):
    for char in text:
        code = ord(char)
        for low, high in RANGES:
            if low <= code <= high:
                return True
    return False


def safe(text):
    """Printable on a GBK console as well as on UTF-8.

    The host's console code page is 936 and the documents are UTF-8, so a line
    with CJK in it cannot be printed verbatim without an encoding error --
    and a checker that crashes on the first violation is a checker that
    reports nothing.
    """
    return text.encode("ascii", "replace").decode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--list", metavar="DIR", default=None,
                        help="check every .md file in this directory")
    args = parser.parse_args()

    paths = list(args.paths)
    if args.list:
        for name in sorted(os.listdir(args.list)):
            if name.endswith(".md"):
                paths.append(os.path.join(args.list, name))
    if not paths:
        print(__doc__.strip())
        return 2

    total = 0
    for path in paths:
        if not os.path.isfile(path):
            print("not a file: %s" % path)
            return 2
        count = 0
        quoted = 0
        in_fence = False
        with io.open(path, encoding="utf-8", errors="replace") as handle:
            for number, line in enumerate(handle, 1):
                stripped = line.strip()
                # A fenced block is QUOTED EVIDENCE, and the guest speaks
                # Chinese: its own log lines are quoted verbatim all over
                # these documents.  The rule is about the prose, so a fenced
                # block is counted separately and reported, not failed.
                if stripped.startswith("```"):
                    in_fence = not in_fence
                    continue
                if has_cjk(line):
                    if in_fence:
                        quoted += 1
                        continue
                    count += 1
                    if count <= 20:
                        print("%s:%d: %s" % (path, number, safe(line.rstrip()[:120])))
        total += count
        print("%-40s CJK lines = %d (quoted evidence inside code fences: %d)"
              % (path, count, quoted))

    if total:
        print("FAIL: %d line(s) with CJK characters" % total)
        return 1
    print("OK: every checked document is English only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
