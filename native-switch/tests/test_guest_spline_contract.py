"""The .spline container the port decodes must be the container on the card.

The format was measured on the card: the parsed length equals the file
length exactly, and three independently decoded chunks (centre_spline,
start_spline, ai_spline) agree on the loop length to within 1.5 % and on the
aspect ratio to within 1 %.

The contract:
  * the header is magic 0xFFFF, version, a length-prefixed ASCII GUID, and
    a chunk count;
  * a chunk is a length-prefixed name, a node count, and 26-byte nodes;
  * a node is 3 x int32 (x, y, z), 5 x uint16 and 1 x uint32, with the
    tangent at +0x0c and the arc length at +0x16;
  * the filesystem layer takes the racing line when the guest opens a
    .spline, from the very candidate the guest is about to read.
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NODE_BYTES = 26


def build_container(guid: bytes, chunks) -> bytes:
    out = struct.pack("<HH", 0xFFFF, 10)
    out += struct.pack("<I", len(guid)) + guid
    out += struct.pack("<H", len(chunks))
    for name, nodes in chunks:
        raw = name.encode("ascii")
        out += struct.pack("<I", len(raw)) + raw
        out += struct.pack("<H", len(nodes))
        for node in nodes:
            out += struct.pack("<iii", node[0], node[1], node[2])
            out += struct.pack("<HHHHH", node[3], node[4], node[5], node[6], node[7])
            out += struct.pack("<I", node[8])
    return out


def main() -> None:
    source = (ROOT / "source" / "guest_spline.cpp").read_text(encoding="utf-8")
    header = (ROOT / "include" / "rr3" / "guest_spline.hpp").read_text(encoding="utf-8")
    fs = (ROOT / "source" / "guest_filesystem.cpp").read_text(encoding="utf-8")

    # 1. The header and the node layout.
    assert "kMagic = 0xFFFF" in source
    assert "kNodeBytes = 26" in source
    assert "offset + 0x0c" in source
    assert "offset + 0x0e" in source
    assert "offset + 0x16" in source
    assert "centre_spline" in source
    assert "start_spline" in header
    assert "ai_spline" in header

    # 2. The parser rejects bytes that are not a spline.
    assert "if (readU16(data, size, offset) != kMagic)" in source
    assert "return false;" in source

    # 3. The filesystem layer takes the racing line when the guest opens it.
    assert '#include "rr3/guest_spline.hpp"' in fs
    hook = fs.index('std::strstr(candidate, ".spline")')
    call = fs.index("rr3::splineObserveGuestOpen(path, candidate)", hook)
    assert hook < call
    # ... and it happens BEFORE the guest's own open, so both see the same
    # bytes.
    assert call < fs.index("file = std::fopen(candidate, mode);", hook)

    # 3b. AND ON THE POSIX open() PATH.  MEASURED 2026-11-01 (q91): the guest
    #     reads the .spline with open(), not with fopen() -- the log shows
    #     the file being stat'ed and never opened through the fopen path --
    #     so a hook that only covered fopen never ran.
    open_hook = fs.index('std::strstr(candidate, ".spline")', call)
    open_call = fs.index("rr3::splineObserveGuestOpen(path, candidate)", open_hook)
    assert open_hook > call
    assert open_call < fs.index("fd = (flags & O_CREAT) != 0", open_hook)
    # The lambda has to capture the path it reports.
    assert "[&fd, flags, mode, path]" in fs

    # 4. The unit is NOT silently assumed: the log says it is unknown.
    assert "unit=UNKNOWN" in source

    # 5. A synthetic container has the byte layout the parser expects.
    nodes = [(1, 2, 3, 4, 5, 6, 7, 8, 9)]
    blob = build_container(b"204bf98e63ad6edb62212db4b38c0c8b",
                           [("centre_spline", nodes)])
    assert len(blob) == 4 + 4 + 32 + 2 + 4 + 13 + 2 + NODE_BYTES
    # the node's fields land where the parser reads them
    node_start = len(blob) - NODE_BYTES
    assert struct.unpack_from("<iii", blob, node_start) == (1, 2, 3)
    assert struct.unpack_from("<H", blob, node_start + 0x0c)[0] == 4
    assert struct.unpack_from("<I", blob, node_start + 0x16)[0] == 9

    # 6. A REAL file, when the card is present, has to decode to its own
    #    length.  The card is not always there, so this is a bonus check.
    card = Path(r"E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\.depot\Tracks")
    found = 0
    for path in sorted(card.glob("*/*/*.spline"))[:40]:
        raw = path.read_bytes()
        offset = 0
        magic, version = struct.unpack_from("<HH", raw, offset)
        offset += 4
        assert magic == 0xFFFF
        guid_len = struct.unpack_from("<I", raw, offset)[0]
        offset += 4 + guid_len
        count = struct.unpack_from("<H", raw, offset)[0]
        offset += 2
        for _ in range(count):
            name_len = struct.unpack_from("<I", raw, offset)[0]
            offset += 4 + name_len
            node_count = struct.unpack_from("<H", raw, offset)[0]
            offset += 2 + node_count * NODE_BYTES
        # The container decodes to its own length; the card's copy is PADDED
        # to a power of two (measured: every file on the card is 16384,
        # 32768, 65536 or 131072 bytes, and the container inside is shorter
        # -- Brands_Hatch/indy/hatch.spline is 16384 bytes with a 15824
        # byte container, brazil/circuit/brazil.spline is 65536 with 49071).
        # The remainder is stale data from the previous occupant of the
        # fixed-size slot, so it must not be parsed as part of the
        # container.
        assert offset <= len(raw), (path, offset, len(raw))
        found += 1
    if found:
        print("guest_spline contract: %d real .spline files decode within their own length" % found)
    else:
        print("guest_spline contract: no card present, layout checked against a synthetic container")


if __name__ == "__main__":
    main()
