#!/usr/bin/env python3
"""The track spline's chunks, and how far the racing line is from the centre.

WHY THIS EXISTS
---------------
The port's pure-pursuit controller follows the chunk named "centre_spline".
MEASURED 2026-11-04 (q145's log, the Spa track):

    phase=spline-open ... chunks=3
      | centre_spline nodes=441 loop=112270.0 spanX=27466 spanY=28797
      | start_spline   nodes=882 loop=112030.5 spanX=27439 spanY=28797
      | ai_spline      nodes=882 loop=111197.5 spanX=27299 spanY=28921

The AI chunk is 1,072.5 units SHORTER than the centre line, which is the
signature of a racing line: it cuts the corners.  The stored unit is 1/16 of
a port unit (1/256 m), so the centre line is 7,017 m and the racing line is
6,949 m -- a 68 m difference a lap, which is what a real racing line buys.

=> A car that follows the centre line drives on the OUTSIDE of every corner,
and that is a plausible reason for the ten barrier hits a race.

This script answers the question the log cannot: HOW FAR APART are the two
lines, in metres?  That number decides whether switching the chunk is worth
a build.

THE CONTAINER
-------------
The format is the one the port decodes (native-switch/source/guest_spline.cpp):

    u16  0xFFFF                magic
    u16  version
    if version >= 10:
        u32  guidLength, then that many bytes of GUID text
    u16  chunkCount
    per chunk:
        u32  nameLength, then that many bytes of the chunk name
        u16  nodeCount
        per node (26 bytes):
            i32  x, i32 y, i32 z
            u16  tangent, u16 heading
            u32  arc          (at +0x16)

USAGE
-----
    python tools/spline_chunks.py <track.spline> [--compare centre_spline ai_spline]
"""

import argparse
import math
import struct
import sys

MAGIC = 0xFFFF
NODE_BYTES = 26
# The guest multiplies the stored coordinates by 16 to get port units, and a
# port unit is 1/256 m.
STORED_TO_METRES = 16.0 / 256.0


class Chunk(object):
    def __init__(self, name):
        self.name = name
        self.nodes = []          # (x, y)

    def loop_length(self):
        total = 0.0
        count = len(self.nodes)
        for i in range(count):
            x0, y0 = self.nodes[i]
            x1, y1 = self.nodes[(i + 1) % count]
            total += math.hypot(x1 - x0, y1 - y0)
        return total

    def bounds(self):
        xs = [n[0] for n in self.nodes]
        ys = [n[1] for n in self.nodes]
        return min(xs), max(xs), min(ys), max(ys)


def parse(data):
    """-> (version, guid, [Chunk, ...]) or raises ValueError."""
    if len(data) < 12:
        raise ValueError("file too short")
    offset = 0
    magic, version = struct.unpack_from("<HH", data, offset)
    offset += 4
    if magic != MAGIC:
        raise ValueError("bad magic 0x%04x" % magic)
    guid = ""
    if version >= 10:
        (guid_length,) = struct.unpack_from("<I", data, offset)
        offset += 4
        guid = data[offset:offset + guid_length].decode("utf-8", "replace")
        offset += guid_length
    (chunk_count,) = struct.unpack_from("<H", data, offset)
    offset += 2
    chunks = []
    for _ in range(chunk_count):
        (name_length,) = struct.unpack_from("<I", data, offset)
        offset += 4
        name = data[offset:offset + name_length].decode("utf-8", "replace")
        offset += name_length
        (node_count,) = struct.unpack_from("<H", data, offset)
        offset += 2
        chunk = Chunk(name)
        for _ in range(node_count):
            if offset + NODE_BYTES > len(data):
                raise ValueError("truncated node list")
            x, y, _z = struct.unpack_from("<iii", data, offset)
            chunk.nodes.append((float(x), float(y)))
            offset += NODE_BYTES
        chunks.append(chunk)
    return version, guid, chunks


def deviation(a, b):
    """How far apart are two polylines, in metres.

    For every node of A the distance to the nearest node of B is taken, and
    the distribution of those distances is what the number is made of.  The
    nodes are dense enough (one every ~125 stored units, i.e. ~8 m) that a
    node-to-node distance is a fair proxy for a line-to-line distance.
    """
    if not a.nodes or not b.nodes:
        return None
    distances = []
    for x, y in a.nodes:
        best = None
        for bx, by in b.nodes:
            d = (bx - x) ** 2 + (by - y) ** 2
            if best is None or d < best:
                best = d
        distances.append(math.sqrt(best) * STORED_TO_METRES)
    distances.sort()
    count = len(distances)
    return {
        "count": count,
        "min_m": distances[0],
        "median_m": distances[count // 2],
        "p90_m": distances[int(count * 0.90)],
        "max_m": distances[-1],
        "mean_m": sum(distances) / count,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--compare", nargs=2, metavar=("A", "B"),
                        default=["centre_spline", "ai_spline"],
                        help="the two chunks to compare (default: centre_spline ai_spline)")
    args = parser.parse_args()

    with open(args.path, "rb") as handle:
        data = handle.read()
    version, guid, chunks = parse(data)
    print("file      %s" % args.path)
    print("version   %d   guid %s" % (version, guid or "-"))
    print("")
    print("  chunk            nodes      loop(stored)      loop(m)   spanX    spanY")
    for chunk in chunks:
        loop = chunk.loop_length()
        minx, maxx, miny, maxy = chunk.bounds()
        print("  %-16s %5d %14.1f %12.1f %8.0f %8.0f"
              % (chunk.name, len(chunk.nodes), loop, loop * STORED_TO_METRES,
                 maxx - minx, maxy - miny))

    by_name = {c.name: c for c in chunks}
    a = by_name.get(args.compare[0])
    b = by_name.get(args.compare[1])
    if a is None or b is None:
        print("")
        print("no chunk pair %s / %s in this file" % (args.compare[0], args.compare[1]))
        return 0

    print("")
    print("DEVIATION: every %s node to the nearest %s node"
          % (args.compare[0], args.compare[1]))
    stats = deviation(a, b)
    if stats is None:
        print("  one of them has no nodes")
        return 1
    print("  samples %d" % stats["count"])
    print("  min    %6.2f m" % stats["min_m"])
    print("  median %6.2f m" % stats["median_m"])
    print("  p90    %6.2f m" % stats["p90_m"])
    print("  max    %6.2f m" % stats["max_m"])
    print("  mean   %6.2f m" % stats["mean_m"])
    loop_a = a.loop_length() * STORED_TO_METRES
    loop_b = b.loop_length() * STORED_TO_METRES
    print("")
    print("  %s is %.1f m a lap, %s is %.1f m a lap  (%.1f m shorter)"
          % (args.compare[0], loop_a, args.compare[1], loop_b, loop_a - loop_b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
