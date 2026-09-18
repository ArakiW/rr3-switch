// png_decode.cpp — minimal PNG (RFC 2083) + DEFLATE (RFC 1951) + zlib (RFC 1950)
// decoder. Written for the guest-TLS-critical JNI path: no malloc, no stdio,
// no zlib library. All working memory is file-static or stack.

#include "rr3/png_decode.hpp"

#include <cstring>

namespace rr3 {
namespace {

// ---- static working memory -------------------------------------------------

// Inflated filtered scanlines, then unfiltered in place (filter byte kept).
// 2048 * (1 + 2048*4) = 16,785,408.
constexpr size_t kMaxRawBytes =
    static_cast<size_t>(kPngMaxDimension) *
    (1u + static_cast<size_t>(kPngMaxDimension) * 4u);
uint8_t g_pngRaw[kMaxRawBytes];

// Concatenated IDAT payload (compressed). 2 MiB covers typical UI atlases.
constexpr size_t kMaxIdatBytes = 2u * 1024u * 1024u;
uint8_t g_pngIdat[kMaxIdatBytes];

// Palette for color type 3 (RGB triples, up to 256 entries).
uint8_t g_pngPalette[256 * 3];
uint32_t g_pngPaletteCount = 0;
bool g_pngHasPalette = false;
bool g_pngHasTrns = false;
uint8_t g_pngTrns[256];

// ---- bit reader + inflate state --------------------------------------------

struct Huffman {
    int16_t count[16];
    int16_t symbol[288];
};

struct InflateState {
    const uint8_t* in;
    size_t inLen;
    size_t inPos;
    uint32_t bitBuf;
    int bitCnt;
    uint8_t* out;
    size_t outCap;
    size_t outPos;
    uint8_t window[32768];
    uint32_t winPos;
};

InflateState g_inflate{};

// RFC 1951 length / distance tables.
const uint16_t kLenBase[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,
                                4, 4, 5, 5, 6, 6, 7, 7,  8,  8,
                                9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                11, 4, 12, 3, 13, 2, 14, 1, 15};

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int bits(InflateState* s, int need) {
    while (s->bitCnt < need) {
        if (s->inPos >= s->inLen) return -1;
        s->bitBuf |= static_cast<uint32_t>(s->in[s->inPos++]) << s->bitCnt;
        s->bitCnt += 8;
    }
    int value = static_cast<int>(s->bitBuf & ((1u << need) - 1u));
    s->bitBuf >>= need;
    s->bitCnt -= need;
    return value;
}

void alignToByte(InflateState* s) {
    s->bitBuf = 0;
    s->bitCnt = 0;
}

int buildHuffman(Huffman* h, const int16_t* lengths, int n) {
    for (int i = 0; i < 16; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) {
        if (lengths[i] > 15) return -1;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) {
        offs[len + 1] = static_cast<int16_t>(offs[len] + h->count[len]);
    }
    for (int sym = 0; sym < n; ++sym) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = static_cast<int16_t>(sym);
        }
    }
    return left;  // 0 complete, >0 incomplete (allowed for some trees)
}

int decodeSymbol(InflateState* s, const Huffman* h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; ++len) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

bool emitByte(InflateState* s, uint8_t value) {
    if (s->outPos >= s->outCap) return false;
    s->window[s->winPos & 32767u] = value;
    s->winPos++;
    s->out[s->outPos++] = value;
    return true;
}

bool inflateStored(InflateState* s) {
    alignToByte(s);
    if (s->inPos + 4 > s->inLen) return false;
    uint32_t len = static_cast<uint32_t>(s->in[s->inPos]) |
                   (static_cast<uint32_t>(s->in[s->inPos + 1]) << 8);
    uint32_t nlen = static_cast<uint32_t>(s->in[s->inPos + 2]) |
                    (static_cast<uint32_t>(s->in[s->inPos + 3]) << 8);
    s->inPos += 4;
    if ((len ^ 0xffffu) != nlen) return false;
    if (s->inPos + len > s->inLen) return false;
    for (uint32_t i = 0; i < len; ++i) {
        if (!emitByte(s, s->in[s->inPos + i])) return false;
    }
    s->inPos += len;
    return true;
}

bool inflateBlockData(InflateState* s, const Huffman* lit, const Huffman* dist) {
    for (;;) {
        int symbol = decodeSymbol(s, lit);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (!emitByte(s, static_cast<uint8_t>(symbol))) return false;
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        int extra = bits(s, kLenExtra[symbol]);
        if (extra < 0) return false;
        unsigned length = kLenBase[symbol] + static_cast<unsigned>(extra);
        int dsym = decodeSymbol(s, dist);
        if (dsym < 0 || dsym >= 30) return false;
        extra = bits(s, kDistExtra[dsym]);
        if (extra < 0) return false;
        unsigned distance = kDistBase[dsym] + static_cast<unsigned>(extra);
        if (distance == 0 || distance > s->winPos) return false;
        for (unsigned i = 0; i < length; ++i) {
            uint8_t value = s->window[(s->winPos - distance) & 32767u];
            if (!emitByte(s, value)) return false;
        }
    }
}

bool inflateFixed(InflateState* s) {
    int16_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    Huffman lit{};
    if (buildHuffman(&lit, lengths, 288) < 0) return false;
    int16_t dlengths[30];
    for (int i = 0; i < 30; ++i) dlengths[i] = 5;
    Huffman dist{};
    if (buildHuffman(&dist, dlengths, 30) < 0) return false;
    return inflateBlockData(s, &lit, &dist);
}

bool inflateDynamic(InflateState* s) {
    int hlit = bits(s, 5);
    int hdist = bits(s, 5);
    int hclen = bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 32) return false;

    int16_t clenLengths[19];
    for (int i = 0; i < 19; ++i) clenLengths[i] = 0;
    for (int i = 0; i < hclen; ++i) {
        int value = bits(s, 3);
        if (value < 0) return false;
        clenLengths[kClenOrder[i]] = static_cast<int16_t>(value);
    }
    Huffman clen{};
    if (buildHuffman(&clen, clenLengths, 19) < 0) return false;

    int16_t lengths[288 + 32];
    int total = hlit + hdist;
    int index = 0;
    while (index < total) {
        int symbol = decodeSymbol(s, &clen);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<int16_t>(symbol);
            continue;
        }
        int repeat = 0;
        int16_t value = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            repeat = bits(s, 2);
            if (repeat < 0) return false;
            repeat += 3;
        } else if (symbol == 17) {
            repeat = bits(s, 3);
            if (repeat < 0) return false;
            repeat += 3;
        } else if (symbol == 18) {
            repeat = bits(s, 7);
            if (repeat < 0) return false;
            repeat += 11;
        } else {
            return false;
        }
        if (index + repeat > total) return false;
        for (int i = 0; i < repeat; ++i) lengths[index++] = value;
    }

    Huffman lit{};
    if (buildHuffman(&lit, lengths, hlit) < 0) return false;
    Huffman dist{};
    if (buildHuffman(&dist, lengths + hlit, hdist) < 0) return false;
    return inflateBlockData(s, &lit, &dist);
}

bool zlibInflate(const uint8_t* data, size_t size, uint8_t* out,
                 size_t outCap, size_t* outLen) {
    if (size < 2) return false;
    const uint8_t cmf = data[0];
    const uint8_t flg = data[1];
    if ((cmf & 0x0f) != 8) return false;  // CM = deflate
    if (((static_cast<unsigned>(cmf) << 8) + flg) % 31u != 0u) return false;
    if ((flg & 0x20) != 0) return false;  // no preset dictionary

    InflateState& s = g_inflate;
    s.in = data + 2;
    s.inLen = size - 2;
    // Trailing Adler-32 is not required to decode pixels; stop at stream end.
    if (s.inLen >= 4) {
        s.inLen -= 4;  // drop Adler-32 when present
    }
    s.inPos = 0;
    s.bitBuf = 0;
    s.bitCnt = 0;
    s.out = out;
    s.outCap = outCap;
    s.outPos = 0;
    s.winPos = 0;

    for (;;) {
        int finalBlock = bits(&s, 1);
        int type = bits(&s, 2);
        if (finalBlock < 0 || type < 0) return false;
        bool ok = false;
        if (type == 0) {
            ok = inflateStored(&s);
        } else if (type == 1) {
            ok = inflateFixed(&s);
        } else if (type == 2) {
            ok = inflateDynamic(&s);
        } else {
            return false;
        }
        if (!ok) return false;
        if (finalBlock == 1) break;
    }
    *outLen = s.outPos;
    return true;
}

// ---- PNG scanline filters --------------------------------------------------

void unfilterRow(uint8_t filter, uint8_t* row, const uint8_t* prev,
                 size_t rowBytes, unsigned bpp) {
    switch (filter) {
        case 0:
            break;
        case 1:  // Sub
            for (size_t i = bpp; i < rowBytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + row[i - bpp]);
            }
            break;
        case 2:  // Up
            if (prev != nullptr) {
                for (size_t i = 0; i < rowBytes; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + prev[i]);
                }
            }
            break;
        case 3:  // Average
            for (size_t i = 0; i < rowBytes; ++i) {
                unsigned left = i >= bpp ? row[i - bpp] : 0u;
                unsigned up = prev != nullptr ? prev[i] : 0u;
                row[i] = static_cast<uint8_t>(row[i] + ((left + up) >> 1));
            }
            break;
        case 4:  // Paeth
            for (size_t i = 0; i < rowBytes; ++i) {
                int a = i >= bpp ? row[i - bpp] : 0;
                int b = prev != nullptr ? prev[i] : 0;
                int c = (prev != nullptr && i >= bpp) ? prev[i - bpp] : 0;
                int p = a + b - c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                row[i] = static_cast<uint8_t>(row[i] + pred);
            }
            break;
        default:
            break;
    }
}

void rowToRgba(const PngDecodeSession* session, const uint8_t* src,
               uint8_t* dst) {
    const uint32_t width = session->width;
    const uint32_t channels = session->channels;
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = src + x * channels;
        uint8_t* d = dst + x * 4;
        switch (channels) {
            case 1: {
                uint8_t v = p[0];
                if (g_pngHasTrns) {
                    // 8-bit gray tRNS is a single sample.
                    d[0] = d[1] = d[2] = v;
                    d[3] = (v == g_pngTrns[0]) ? 0 : 255;
                } else {
                    d[0] = d[1] = d[2] = v;
                    d[3] = 255;
                }
                break;
            }
            case 2:
                d[0] = d[1] = d[2] = p[0];
                d[3] = p[1];
                break;
            case 3:
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = 255;
                break;
            default:
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = p[3];
                break;
        }
    }
}

void paletteRowToRgba(const PngDecodeSession* session, const uint8_t* src,
                      uint8_t* dst) {
    const uint32_t width = session->width;
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t index = src[x];
        uint8_t* d = dst + x * 4;
        if (static_cast<uint32_t>(index) < g_pngPaletteCount) {
            d[0] = g_pngPalette[index * 3 + 0];
            d[1] = g_pngPalette[index * 3 + 1];
            d[2] = g_pngPalette[index * 3 + 2];
        } else {
            d[0] = d[1] = d[2] = 0;
        }
        d[3] = g_pngHasTrns && index < 256 ? g_pngTrns[index] : 255;
    }
}

}  // namespace

bool pngProbeHeader(const uint8_t* data, size_t size, uint32_t* outWidth,
                    uint32_t* outHeight, uint8_t* outColorType,
                    uint8_t* outBitDepth) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G',
                                          0x0d, 0x0a, 0x1a, 0x0a};
    if (data == nullptr || size < 8 + 25) return false;
    if (std::memcmp(data, kSignature, 8) != 0) return false;
    const uint8_t* chunk = data + 8;
    const uint32_t length = readBe32(chunk);
    if (length != 13) return false;
    if (std::memcmp(chunk + 4, "IHDR", 4) != 0) return false;
    const uint8_t* ihdr = chunk + 8;
    const uint32_t width = readBe32(ihdr);
    const uint32_t height = readBe32(ihdr + 4);
    const uint8_t bitDepth = ihdr[8];
    const uint8_t colorType = ihdr[9];
    if (width == 0 || height == 0) return false;
    if (width > kPngMaxDimension || height > kPngMaxDimension) return false;
    if (ihdr[10] != 0 || ihdr[11] != 0 || ihdr[12] != 0) {
        return false;  // compression/filter/interlace must be 0
    }
    if (bitDepth != 8) return false;  // minimal decoder: 8-bit only
    if (colorType != 0 && colorType != 2 && colorType != 3 && colorType != 4 &&
        colorType != 6) {
        return false;
    }
    if (outWidth != nullptr) *outWidth = width;
    if (outHeight != nullptr) *outHeight = height;
    if (outColorType != nullptr) *outColorType = colorType;
    if (outBitDepth != nullptr) *outBitDepth = bitDepth;
    return true;
}

bool pngDecodeBegin(PngDecodeSession* session, const uint8_t* data,
                    size_t size) {
    if (session == nullptr) return false;
    session->valid = false;
    session->width = 0;
    session->height = 0;
    session->channels = 0;
    session->stride = 0;
    session->nextRow = 0;
    session->colorType = 0;
    session->raw = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t colorType = 0;
    uint8_t bitDepth = 0;
    if (!pngProbeHeader(data, size, &width, &height, &colorType, &bitDepth)) {
        return false;
    }

    unsigned channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return false;
    }

    g_pngHasPalette = false;
    g_pngHasTrns = false;
    g_pngPaletteCount = 0;

    // Walk chunks after IHDR. Concatenate IDAT. Capture PLTE/tRNS.
    size_t idatSize = 0;
    size_t offset = 8;
    bool sawIend = false;
    while (offset + 8 <= size) {
        const uint32_t length = readBe32(data + offset);
        if (offset + 12ull + length > size) return false;
        const uint8_t* type = data + offset + 4;
        const uint8_t* payload = data + offset + 8;
        if (std::memcmp(type, "IDAT", 4) == 0) {
            if (idatSize + length > kMaxIdatBytes) return false;
            std::memcpy(g_pngIdat + idatSize, payload, length);
            idatSize += length;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            if (length == 0 || length > 256 * 3 || (length % 3) != 0) {
                return false;
            }
            std::memcpy(g_pngPalette, payload, length);
            g_pngPaletteCount = length / 3;
            g_pngHasPalette = true;
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (colorType == 3 && length <= 256) {
                std::memset(g_pngTrns, 255, sizeof(g_pngTrns));
                std::memcpy(g_pngTrns, payload, length);
                g_pngHasTrns = true;
            } else if (colorType == 0 && length == 2) {
                g_pngTrns[0] = payload[1];  // low byte of 16-bit sample
                g_pngHasTrns = true;
            } else if (colorType == 2 && length == 6) {
                // RGB tRNS ignored for 8-bit RGB (no per-pixel compare here).
                g_pngHasTrns = false;
            }
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            sawIend = true;
            break;
        }
        offset += 12ull + length;
    }
    if (idatSize == 0) return false;
    if (colorType == 3 && !g_pngHasPalette) return false;
    (void)sawIend;

    const size_t stride = 1u + static_cast<size_t>(width) * channels;
    const size_t rawNeed = stride * height;
    if (rawNeed > kMaxRawBytes) return false;

    size_t inflated = 0;
    if (!zlibInflate(g_pngIdat, idatSize, g_pngRaw, rawNeed, &inflated)) {
        return false;
    }
    if (inflated < rawNeed) return false;

    // Unfilter in place. Filter byte stays at the start of each scanline.
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = g_pngRaw + static_cast<size_t>(y) * stride;
        const uint8_t filter = row[0];
        uint8_t* pixels = row + 1;
        const uint8_t* prev =
            y > 0 ? (g_pngRaw + static_cast<size_t>(y - 1) * stride + 1)
                  : nullptr;
        unfilterRow(filter, pixels, prev, stride - 1, channels);
    }

    session->width = width;
    session->height = height;
    session->channels = channels;
    session->stride = static_cast<uint32_t>(stride);
    session->nextRow = 0;
    session->colorType = colorType;
    session->raw = g_pngRaw;
    session->valid = true;
    return true;
}

bool pngDecodeNextRow(PngDecodeSession* session, uint8_t* rgbaRow,
                      bool* outDone) {
    if (outDone != nullptr) *outDone = false;
    if (session == nullptr || !session->valid || rgbaRow == nullptr) {
        return false;
    }
    if (session->nextRow >= session->height) {
        if (outDone != nullptr) *outDone = true;
        return true;
    }
    const uint8_t* src = session->raw +
                         static_cast<size_t>(session->nextRow) * session->stride +
                         1u;
    if (session->colorType == 3) {
        paletteRowToRgba(session, src, rgbaRow);
    } else {
        rowToRgba(session, src, rgbaRow);
    }
    session->nextRow++;
    return true;
}

}  // namespace rr3
