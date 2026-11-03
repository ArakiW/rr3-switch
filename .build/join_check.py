#!/usr/bin/env python3
"""JOIN THE PARTS BACK AND PROVE THEY ARE THE ARCHIVE.

The parts are what the user downloads, so the question that matters is:
does concatenating them give back the archive, byte for byte?

The answer is a SHA-256 over the concatenation and a SHA-256 over the
archive.  They are computed in one pass each, over 7.89 GiB.
"""

import hashlib
import os
import sys

PARTS = r"E:\AI\RealRacing3-switch\dist\parts-20261103"
ZIP = r"E:\AI\RealRacing3-switch\dist\rr3-switch-20261103.zip"
JOINED = r"E:\AI\RealRacing3-switch\.build\join-test-20261103.zip"


def sha256_of_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(8 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    names = sorted(n for n in os.listdir(PARTS) if n.endswith(".001") or
                   ".zip.0" in n)
    names = sorted(n for n in os.listdir(PARTS)
                   if n.startswith("rr3-switch-20261103.zip."))
    if not names:
        print("no parts found")
        return 1

    total = 0
    with open(JOINED, "wb") as out:
        for name in names:
            part = os.path.join(PARTS, name)
            size = os.path.getsize(part)
            with open(part, "rb") as handle:
                while True:
                    block = handle.read(8 * 1024 * 1024)
                    if not block:
                        break
                    out.write(block)
            total += size
            print("  + %-46s %14d bytes" % (name, size))

    archive_size = os.path.getsize(ZIP)
    print("")
    print("parts total : %d bytes" % total)
    print("archive     : %d bytes" % archive_size)
    if total != archive_size:
        print("MISMATCH: the sizes differ")
        return 1

    joined_hash = sha256_of_file(JOINED)
    archive_hash = sha256_of_file(ZIP)
    print("joined  sha256 = %s" % joined_hash)
    print("archive sha256 = %s" % archive_hash)
    if joined_hash == archive_hash:
        print("JOIN MATCHES THE ARCHIVE BYTE FOR BYTE")
        return 0
    print("MISMATCH")
    return 1


if __name__ == "__main__":
    sys.exit(main())
