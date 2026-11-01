// The guest's track spline container, decoded.  The format, the evidence and
// the open question (the unit) are in include/rr3/guest_spline.hpp.
#include "rr3/guest_spline.hpp"

#include "rr3/recovery_guard.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

constexpr const char* kGuestViewLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";

constexpr uint16_t kMagic = 0xFFFF;
constexpr size_t kNodeBytes = 26;

// The chunk the racing line lives in.
constexpr const char* kCentreChunk = "centre_spline";

uint16_t readU16(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 2 > size) return 0;
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t readU32(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 4 > size) return 0;
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

int32_t readI32(const uint8_t* data, size_t size, size_t offset) {
    return static_cast<int32_t>(readU32(data, size, offset));
}

SplineFile g_lastOpened;
bool g_hasLastOpened = false;
uint32_t g_generation = 0;

}  // namespace

bool splineParse(const uint8_t* data, size_t size, SplineFile* out) {
    if (data == nullptr || out == nullptr || size < 12) {
        return false;
    }
    SplineFile parsed;
    size_t offset = 0;
    if (readU16(data, size, offset) != kMagic) {
        return false;
    }
    offset += 2;
    parsed.version = readU16(data, size, offset);
    offset += 2;
    if (parsed.version >= 10) {
        const uint32_t guidLength = readU32(data, size, offset);
        offset += 4;
        if (guidLength > size || offset + guidLength > size) {
            return false;
        }
        parsed.guid.assign(reinterpret_cast<const char*>(data + offset), guidLength);
        offset += guidLength;
    }
    const uint16_t chunkCount = readU16(data, size, offset);
    offset += 2;
    for (uint16_t c = 0; c < chunkCount; ++c) {
        const uint32_t nameLength = readU32(data, size, offset);
        offset += 4;
        if (nameLength > size || offset + nameLength > size) {
            return false;
        }
        SplineChunk chunk;
        chunk.name.assign(reinterpret_cast<const char*>(data + offset), nameLength);
        offset += nameLength;
        const uint16_t nodeCount = readU16(data, size, offset);
        offset += 2;
        chunk.nodes.reserve(nodeCount);
        for (uint16_t n = 0; n < nodeCount; ++n) {
            if (offset + kNodeBytes > size) {
                return false;
            }
            SplineNode node;
            node.x = readI32(data, size, offset + 0x00);
            node.y = readI32(data, size, offset + 0x04);
            node.z = readI32(data, size, offset + 0x08);
            node.tangent = readU16(data, size, offset + 0x0c);
            node.heading = readU16(data, size, offset + 0x0e);
            node.arc = readU32(data, size, offset + 0x16);
            chunk.nodes.push_back(node);
            offset += kNodeBytes;
        }
        parsed.chunks.push_back(std::move(chunk));
    }
    *out = std::move(parsed);
    return true;
}

bool splineLoad(const char* hostPath, SplineFile* out) {
    if (hostPath == nullptr || out == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(hostPath, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > (16 * 1024 * 1024)) {
        std::fclose(file);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        return false;
    }
    return splineParse(bytes.data(), bytes.size(), out);
}

bool splineSummarize(const SplineFile& file, const char* chunkName,
                     SplineSummary* out) {
    if (out == nullptr) return false;
    const SplineChunk* chunk = file.chunk(chunkName);
    if (chunk == nullptr || chunk->nodes.size() < 2) {
        return false;
    }
    SplineSummary summary;
    summary.chunk = chunkName;
    summary.nodes = chunk->nodes.size();
    summary.minX = summary.maxX = chunk->nodes[0].x;
    summary.minY = summary.maxY = chunk->nodes[0].y;
    double loop = 0.0;
    for (size_t i = 0; i < chunk->nodes.size(); ++i) {
        const SplineNode& node = chunk->nodes[i];
        if (node.x < summary.minX) summary.minX = node.x;
        if (node.x > summary.maxX) summary.maxX = node.x;
        if (node.y < summary.minY) summary.minY = node.y;
        if (node.y > summary.maxY) summary.maxY = node.y;
        const SplineNode& next = chunk->nodes[(i + 1) % chunk->nodes.size()];
        const double dx = static_cast<double>(next.x) - static_cast<double>(node.x);
        const double dy = static_cast<double>(next.y) - static_cast<double>(node.y);
        loop += std::sqrt(dx * dx + dy * dy);
    }
    summary.loopUnits = loop;
    *out = summary;
    return true;
}

const SplineFile* splineLastOpened() {
    return g_hasLastOpened ? &g_lastOpened : nullptr;
}

// Bumped every time the guest opens a different spline, so a consumer can
// tell "the line I fitted is still the current track's" from "the track
// changed and my fit belongs to the last one".
uint32_t splineGeneration() {
    return g_generation;
}

void splineObserveGuestOpen(const char* guestPath, const char* hostPath) {
    if (hostPath == nullptr) {
        return;
    }
    SplineFile file;
    if (!splineLoad(hostPath, &file)) {
        char details[256];
        std::snprintf(details, sizeof(details),
                      "path=%s parse=FAILED note=the_bytes_are_not_a_spline_container",
                      guestPath != nullptr ? guestPath : "-");
        rr3::appendGuestBootLine(kGuestViewLog, "spline-open", 0, 0, details);
        return;
    }
    g_lastOpened = std::move(file);
    g_hasLastOpened = true;
    ++g_generation;
    char details[512];
    int written = 0;
    written += std::snprintf(details + written, sizeof(details) - written,
                             "path=%s guid=%s ver=%u chunks=%zu",
                             guestPath != nullptr ? guestPath : "-",
                             g_lastOpened.guid.c_str(),
                             static_cast<unsigned>(g_lastOpened.version),
                             g_lastOpened.chunks.size());
    for (const SplineChunk& chunk : g_lastOpened.chunks) {
        SplineSummary summary;
        if (!splineSummarize(g_lastOpened, chunk.name.c_str(), &summary)) {
            continue;
        }
        written += std::snprintf(
            details + written, sizeof(details) - written,
            " | %s nodes=%zu loop=%.1f spanX=%d spanY=%d",
            chunk.name.c_str(), summary.nodes, summary.loopUnits,
            summary.maxX - summary.minX, summary.maxY - summary.minY);
    }
    written += std::snprintf(details + written, sizeof(details) - written,
                             " unit=UNKNOWN_note=the_stored_unit_is_not_yet_fixed"
                             " centre_chunk=%s",
                             kCentreChunk);
    rr3::appendGuestBootLine(kGuestViewLog, "spline-open", 0, 0, details);
}

}  // namespace rr3
