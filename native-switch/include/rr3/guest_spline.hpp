// The guest's track spline container, decoded.
//
// WHY THE PORT NEEDS IT
// --------------------
// The port's closed-loop controller steers the car by following a TARGET
// POINT, and until now the only source of that point was the guest's own
// memory -- where the port read a value that turned out to be garbage
// (measured 2026-11-01, q81: ctgt=312,8992131,-19347, i.e. a target 35 km
// away), so the pursuit stayed OFF and the car never completed a lap.
//
// The racing line itself is in the guest's `.spline` file, which the guest
// opens on the card when it loads a track.  The port intercepts every guest
// file open (source/guest_filesystem.cpp), so the port can take the file
// the guest is about to read and decode it.
//
// THE FORMAT, AS MEASURED
// -----------------------
// The container is NOT compressed and NOT obfuscated: it is a plain
// little-endian structure.  Measured on the card: the parsed length equals
// the file length exactly for every track tried (bathurst 50664/50664,
// catalunya 25314/25314, daytona 45977/45977).
//
//   u16  magic      0xFFFF
//   u16  version    10
//   u32  guidLen   length of the GUID text
//   ...  guid      ASCII hex, e.g. "204bf98e63ad6edb62212db4b38c0c8b"
//   u16  chunkCount
//   per chunk:
//     u32 nameLen; name bytes          ("centre_spline", "start_spline",
//                                        "ai_spline")
//     u16 nodeCount
//     per node, 26 bytes:
//       +0x00 i32 x
//       +0x04 i32 y
//       +0x08 i32 z
//       +0x0c u16 tangent   65536 units per turn
//       +0x0e u16 heading   the guest's own heading at this node
//       +0x10 u16 (unused on the centre line)
//       +0x12 u16 (unused on the centre line)
//       +0x14 u16 (unused on the centre line)
//       +0x16 u32 arc       cumulative arc length
//       +0x1a u16 (unused on the centre line)
//
// THE THREE CHUNKS, AND WHY THE DECODE IS RIGHT
// ---------------------------------------------
// `centre_spline` is the centre line: the tangent at +0x0c turns exactly
// once around the loop (65536 units per turn) and the four fields after it
// are all zero.  `start_spline` and `ai_spline` sample the SAME line at
// twice the density (they carry the guest's own heading and the arc
// length), and the three of them agree on the loop length to within 1.5 %
// (catalunya: 49392 / 49255 / 48656 units) and on the aspect ratio to
// within 1 % (2.641 / 2.639 / 2.620).  Three chunks that were decoded
// independently agreeing on the geometry is the check; a decode that
// happened to fit one chunk would not.
//
// THE UNIT IS NOT YET FIXED.  The coordinates are in some unit U, and the
// guest multiplies them by 16 when it uses them (the node accessor at ELF
// 0x1425d54), so the port's own 1/256 m car position needs a factor of 16
// per stored unit -- but that factor is a GEOMETRIC ESTIMATE, not a
// measurement, and it is exactly what the captured spline plus the car's
// own positions will settle.  Until then the port logs the numbers and
// does not steer from them.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rr3 {

// ONE NODE.  26 bytes on the wire.
struct SplineNode {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t tangent = 0;  // +0x0c, 65536 units per turn
    uint16_t heading = 0;  // +0x0e, the guest's own heading
    uint32_t arc = 0;      // +0x16, cumulative arc length
};

struct SplineChunk {
    std::string name;
    std::vector<SplineNode> nodes;
};

struct SplineFile {
    uint16_t version = 0;
    std::string guid;
    std::vector<SplineChunk> chunks;

    const SplineChunk* chunk(const char* chunkName) const;
};

// The bytes of a .spline file.  False when the bytes are not a spline.
bool splineParse(const uint8_t* data, size_t size, SplineFile* out);

// A .spline file on the card.
bool splineLoad(const char* hostPath, SplineFile* out);

// WHAT THE GUEST LAST OPENED.  Null until the guest opens a .spline.
const SplineFile* splineLastOpened();

// Called by the filesystem layer when the guest opens a .spline.  It reads
// the file the guest is about to read and decodes it.  The guest's own
// stream is NOT disturbed: the file is opened a second time for the port's
// own read.
void splineObserveGuestOpen(const char* guestPath, const char* hostPath);

// The geometry summary the log prints: node count, the loop length in
// stored units, and the bounding box.  These are the numbers the unit
// calibration is done against.
struct SplineSummary {
    const char* chunk = "";
    size_t nodes = 0;
    double loopUnits = 0.0;
    int32_t minX = 0;
    int32_t maxX = 0;
    int32_t minY = 0;
    int32_t maxY = 0;
};

bool splineSummarize(const SplineFile& file, const char* chunkName,
                     SplineSummary* out);

}  // namespace rr3
