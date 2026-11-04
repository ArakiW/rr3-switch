#!/usr/bin/env python3
"""JOIN THE PARTS BACK AND PROVE THEY ARE THE ARCHIVE.

The parts are what the user downloads, so the question that matters is:
does concatenating them give back the archive, byte for byte?

The answer is a SHA-256 over the concatenation and a SHA-256 over the
archive.  They are computed in one pass each, over 7.89 GiB.

WHY THE STAMP IS AN ARGUMENT
----------------------------
It was not, and that made the proof mean nothing.  MEASURED 2026-11-04: the
packers had been given a `-Stamp` parameter, but this checker still had
20261103 hard-wired in it, so when the chain was re-run for 20261104 the
check proved that the OLD parts joined to the OLD archive and said nothing
about the new ones -- while the log right above it showed the new archive
being written.

That is the same class of defect as the stale release archive (the packer
reusing an existing zip) and the "substituted=" field that was always 1: a
part of the toolchain measuring something other than what it claims to.  The
stamp now comes from the caller, and the checker refuses to run when the
parts or the archive it was asked about are not there.

Usage:
    python .build/join_check.py --stamp 20261104
    python .build/join_check.py --stamp 20261104 --keep   # keep the joined copy
"""

import argparse
import hashlib
import os
import sys

ROOT = r"E:\AI\RealRacing3-switch"
DIST = os.path.join(ROOT, "dist")
BUILD = os.path.join(ROOT, ".build")


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
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp", required=True,
                        help="the deliverable stamp, e.g. 20261104")
    parser.add_argument("--keep", action="store_true",
                        help="keep the joined copy instead of deleting it")
    args = parser.parse_args()

    parts_dir = os.path.join(DIST, "parts-%s" % args.stamp)
    archive = os.path.join(DIST, "rr3-switch-%s.zip" % args.stamp)
    joined = os.path.join(BUILD, "join-test-%s.zip" % args.stamp)

    for path in (parts_dir, archive):
        if not os.path.exists(path):
            print("not there: %s" % path)
            print("=> refusing to check a deliverable that does not exist")
            return 1

    names = sorted(n for n in os.listdir(parts_dir)
                   if n.startswith("rr3-switch-%s.zip." % args.stamp))
    if not names:
        print("no parts for stamp %s in %s" % (args.stamp, parts_dir))
        return 1

    total = 0
    with open(joined, "wb") as out:
        for name in names:
            part = os.path.join(parts_dir, name)
            size = os.path.getsize(part)
            with open(part, "rb") as handle:
                while True:
                    block = handle.read(8 * 1024 * 1024)
                    if not block:
                        break
                    out.write(block)
            total += size
            print("  + %-46s %14d bytes" % (name, size))

    archive_size = os.path.getsize(archive)
    print("")
    print("parts total : %d bytes" % total)
    print("archive     : %d bytes" % archive_size)
    if total != archive_size:
        print("MISMATCH: the sizes differ")
        return 1

    joined_hash = sha256_of_file(joined)
    archive_hash = sha256_of_file(archive)
    print("joined  sha256 = %s" % joined_hash)
    print("archive sha256 = %s" % archive_hash)
    if not args.keep:
        try:
            os.remove(joined)
        except OSError as error:
            print("could not remove the joined copy: %s" % error)
    if joined_hash == archive_hash:
        print("JOIN MATCHES THE ARCHIVE BYTE FOR BYTE (%s)" % args.stamp)
        return 0
    print("MISMATCH")
    return 1


if __name__ == "__main__":
    sys.exit(main())
