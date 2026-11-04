#!/usr/bin/env python3
"""Append a section to PORT_PROGRESS.md without touching the rest of it.

WHY A SCRIPT: the file is 13,000+ lines with mixed line endings, and a
PowerShell Add-Content after a `cd` is refused by the host's write policy
("recursive write/delete target cannot be verified inside the workspace").
The script appends in the file's OWN line-ending style and does not rewrite
anything else.

Usage: python tools/append_port_progress.py <section-file>
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(ROOT, "PORT_PROGRESS.md")


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    section_path = sys.argv[1]
    with io.open(section_path, "r", encoding="utf-8") as handle:
        section = handle.read()
    with io.open(TARGET, "rb") as handle:
        raw = handle.read()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    before = len(raw)
    if not raw.endswith(b"\n"):
        raw += newline.encode("ascii")
    body = section.replace("\r\n", "\n").replace("\n", newline)
    with io.open(TARGET, "wb") as handle:
        handle.write(raw + body.encode("utf-8"))
    print("appended %d bytes to %s (newline style %r)"
          % (len(body), TARGET, newline))
    print("file grew from %d to %d bytes" % (before, os.path.getsize(TARGET)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
