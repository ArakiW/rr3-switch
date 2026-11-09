# Extract one file out of an RR3 external RomFS volume, inflating the ".z" wrapper.
#
# WHY: the magenta rivals are drawn from a texture the port DID deliver
# (stat-layout size matched the volume entry exactly, open-ok followed). So the
# question is no longer "did the file arrive" but "what are the BYTES inside".
# If the source DDS is magenta, the data pack is the problem. If the source DDS
# is a normal white DTM car, then the decode/upload path is the problem. This
# script gets those bytes out so the two can be told apart.
#
# Usage:
#   python romfs-extract.py <package-root> <member-path> <out-file>
#
# The ".z" wrapper: the port inflates it with uncompress() over the bytes AFTER
# the first four (see graphics_probe.cpp), so the first four bytes are a
# header and the rest is a zlib stream.
import json
import struct
import sys
import zlib
from pathlib import Path

INVALID = 0xFFFFFFFF


class RomFS:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.stream = path.open("rb")
        header = self.stream.read(80)
        fields = struct.unpack("<10Q", header)
        if fields[0] != 80:
            raise ValueError(f"unexpected RomFS header: {path}")
        self.dir_table = fields[3]
        self.file_table = fields[7]

    def close(self) -> None:
        self.stream.close()

    def directory(self, offset: int):
        self.stream.seek(self.dir_table + offset)
        raw = self.stream.read(24)
        _, sibling, child_dir, child_file, _, name_len = struct.unpack("<6I", raw)
        name = self.stream.read(name_len).decode("utf-8", "replace")
        return sibling, child_dir, child_file, name

    def file(self, offset: int):
        self.stream.seek(self.file_table + offset)
        raw = self.stream.read(32)
        _, sibling, _, size, _, name_len = struct.unpack("<IIQQII", raw)
        name = self.stream.read(name_len).decode("utf-8", "replace")
        return sibling, size, name

    def find(self, path: str):
        parts = [p for p in path.strip("/").split("/") if p]
        node = 0
        for part in parts[:-1]:
            found = None
            _, child, _, _ = self.directory(node)
            while child != INVALID:
                sibling, _, _, name = self.directory(child)
                if name == part:
                    found = child
                    break
                child = sibling
            if found is None:
                return None, 0
            node = found
        # The third field of a directory record is the FIRST CHILD FILE; the
        # second is the first child DIRECTORY. Walking the directory chain
        # with the directory field and then reading it as a file record is how
        # this first failed: the file table read then ran off the end.
        _, _, child, _ = self.directory(node)
        while child != INVALID:
            sibling, size, name = self.file(child)
            if name == parts[-1]:
                self.stream.seek(self.file_table + child)
                offset = struct.unpack("<I", self.stream.read(4))[0]
                return offset, size
            child = sibling
        return None, 0


def main() -> int:
    package = Path(sys.argv[1]).resolve()
    member = sys.argv[2]
    out = Path(sys.argv[3])
    data = package / "switch" / "rr3-native-rehost" / "data"
    manifest = json.loads((data / "rr3-data-manifest.json").read_text(encoding="utf-8-sig"))

    hit = None
    for name, record in manifest["volumes"].items():
        romfs = RomFS(data / record["file"])
        try:
            offset, size = romfs.find(member)
            if offset is not None:
                romfs.stream.seek(offset)
                blob = romfs.stream.read(size)
                hit = (name, blob)
                break
        finally:
            romfs.close()

    if hit is None:
        print(f"NOT FOUND: {member}")
        return 1

    name, blob = hit
    print(f"volume    : {name}")
    print(f"raw bytes : {len(blob)}")
    payload = blob
    if blob[:4] != b"DDS " and len(blob) > 4:
        try:
            payload = zlib.decompress(blob[4:])
            print(f"inflated  : {len(payload)} bytes (zlib over bytes[4:])")
        except zlib.error as exc:
            print(f"inflate failed: {exc}")
    out.write_bytes(payload)
    print(f"written   : {out} ({len(payload)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
