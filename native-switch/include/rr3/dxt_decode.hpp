// DXT1 / DXT5 block decoding, and just enough DDS header parsing to use it.
//
// WHY THIS EXISTS
// ---------------
// The rival cars in a race render MAGENTA. Measured pixel by pixel, that is
// not the missing-texture colour: no pixel in the frame is exactly
// (255,0,255). What is there is (255,4,255), (192,4,192), (221,4,229) -- a
// normal grey/white car with the GREEN CHANNEL ZEROED, and the window glass
// comes out dark purple. So the texture is DECODED, but decoded with the wrong
// layout.
//
// The texture the guest loads for a rival car is a DDS inside the game's
// "[u32 size][zlib]" container. Measured on the pack member
//   files/.depot/vehicles/2017_bmw_m4_dtm/livery/2017_bmw_m4_dtm_ext.dxt.dds.z
// (DXT data pack, 266,228 B on disk) it inflates to a VALID 1024x1024 DDS with
// 11 mip levels and FourCC 'DXT1'. The bytes are good. So the only way the
// green channel can disappear is in the decode/upload path -- and the only way
// to TELL which path is at fault is to decode the same bytes on the CPU and
// compare with what the GPU shows.
//
// That comparison needs a decoder that is known to be correct, so it lives
// here, in a header, with a host test that runs it against hand-computed
// blocks (native-switch/tests/host/dxt_selftest.cpp). A decoder with no test
// is a decoder with no evidence.
//
// THE BLOCK LAYOUTS, EXACTLY (the second one is where DXT5 goes wrong):
//
//   DXT1, 8 bytes:
//     0..1  colour0, 5:6:5
//     2..3  colour1, 5:6:5
//     4..7  16 x 2-bit colour indices, little endian
//
//   DXT5, 16 bytes:
//     0     alpha0
//     1     alpha1
//     2..7  8 x 3-bit alpha indices, little endian (24 bits)
//     8..11 16 x 2-bit colour indices, little endian
//     12..15 colour0, colour1 (the DXT1 endpoints, but the indices are the
//            ones in 8..11, NOT the ones in 12..15)
//
// THE DXT1 THREE-COLOUR RULE, WHICH IS THE OTHER ONE EVERYBODY GETS WRONG:
// when colour0 <= colour1 the block is THREE colour plus ONE BIT OF ALPHA,
// and index 3 means TRANSPARENT, not a fourth colour. When colour0 > colour1
// it is four opaque colours. Getting that backwards turns every dark texture
// into noise. A DXT5 colour block is ALWAYS four-colour mode.

#ifndef RR3_DXT_DECODE_HPP
#define RR3_DXT_DECODE_HPP

#include <cstddef>
#include <cstdint>

namespace rr3 {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

inline std::uint8_t expand5(unsigned value) {
    return static_cast<std::uint8_t>((value * 255 + 15) / 31);
}

inline std::uint8_t expand6(unsigned value) {
    return static_cast<std::uint8_t>((value * 255 + 31) / 63);
}

inline Rgba unpack565(std::uint16_t packed) {
    Rgba out;
    out.r = expand5((packed >> 11) & 0x1fu);
    out.g = expand6((packed >> 5) & 0x3fu);
    out.b = expand5(packed & 0x1fu);
    out.a = 255;
    return out;
}

inline Rgba mixRgba(const Rgba& a, const Rgba& b, int aWeight, int bWeight,
                    int total) {
    Rgba out;
    out.r = static_cast<std::uint8_t>((a.r * aWeight + b.r * bWeight) / total);
    out.g = static_cast<std::uint8_t>((a.g * aWeight + b.g * bWeight) / total);
    out.b = static_cast<std::uint8_t>((a.b * aWeight + b.b * bWeight) / total);
    out.a = 255;
    return out;
}

// The four palette entries of a colour block.
//   endpoints: the two 5:6:5 colours (2 bytes each)
// `forceFourColour` is true for DXT5, whose colour block is always four-colour.
// The 2-bit colour indices are read separately by readIndex2(), because where
// they live depends on the format: bytes 4..7 in DXT1, bytes 8..11 in DXT5.
inline void decodeColourBlock(const std::uint8_t* endpoints,
                              bool forceFourColour, Rgba palette[4],
                              bool* fourColourOut) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(endpoints[0] |
                                                         (endpoints[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(endpoints[2] |
                                                         (endpoints[3] << 8));
    const bool fourColour = forceFourColour || c0 > c1;
    palette[0] = unpack565(c0);
    palette[1] = unpack565(c1);
    if (fourColour) {
        palette[2] = mixRgba(palette[0], palette[1], 2, 1, 3);
        palette[3] = mixRgba(palette[0], palette[1], 1, 2, 3);
    } else {
        palette[2] = mixRgba(palette[0], palette[1], 1, 1, 2);
        palette[3] = palette[0];
    }
    if (fourColourOut != nullptr) {
        *fourColourOut = fourColour;
    }
}

inline unsigned readIndex2(const std::uint8_t* indices, int pixel) {
    const std::uint32_t bits = static_cast<std::uint32_t>(indices[0]) |
                               (static_cast<std::uint32_t>(indices[1]) << 8) |
                               (static_cast<std::uint32_t>(indices[2]) << 16) |
                               (static_cast<std::uint32_t>(indices[3]) << 24);
    return (bits >> (2 * pixel)) & 3u;
}

// One DXT1 block: 8 bytes in, 16 RGBA pixels (64 bytes) out.
inline void decodeDxt1Block(const std::uint8_t* block, std::uint8_t* out) {
    Rgba palette[4];
    bool fourColour = true;
    decodeColourBlock(block, false, palette, &fourColour);
    for (int pixel = 0; pixel < 16; ++pixel) {
        const unsigned index = readIndex2(block + 4, pixel);
        Rgba colour = palette[index];
        if (!fourColour && index == 3u) {
            // Three-colour mode: index 3 is the transparent slot.
            colour.a = 0;
        }
        out[pixel * 4 + 0] = colour.r;
        out[pixel * 4 + 1] = colour.g;
        out[pixel * 4 + 2] = colour.b;
        out[pixel * 4 + 3] = colour.a;
    }
}

// One DXT5 block: 16 bytes in, 16 RGBA pixels (64 bytes) out.
inline void decodeDxt5Block(const std::uint8_t* block, std::uint8_t* out) {
    std::uint8_t alpha[8];
    const std::uint8_t a0 = block[0];
    const std::uint8_t a1 = block[1];
    if (a0 > a1) {
        alpha[0] = a0;
        alpha[1] = a1;
        for (int index = 0; index < 6; ++index) {
            alpha[2 + index] = static_cast<std::uint8_t>(
                ((6 - index) * a0 + (1 + index) * a1) / 7);
        }
    } else {
        alpha[0] = a0;
        alpha[1] = a1;
        for (int index = 0; index < 4; ++index) {
            alpha[2 + index] = static_cast<std::uint8_t>(
                ((4 - index) * a0 + (1 + index) * a1) / 5);
        }
        alpha[6] = 0;
        alpha[7] = 255;
    }
    const std::uint32_t alphaBits = static_cast<std::uint32_t>(block[2]) |
                                    (static_cast<std::uint32_t>(block[3]) << 8) |
                                    (static_cast<std::uint32_t>(block[4]) << 16) |
                                    ((static_cast<std::uint32_t>(block[5]) & 0x0fu) << 24);
    Rgba palette[4];
    decodeColourBlock(block + 12, true, palette, nullptr);
    for (int pixel = 0; pixel < 16; ++pixel) {
        const unsigned alphaIndex = (alphaBits >> (3 * pixel)) & 7u;
        const Rgba colour = palette[readIndex2(block + 8, pixel)];
        out[pixel * 4 + 0] = colour.r;
        out[pixel * 4 + 1] = colour.g;
        out[pixel * 4 + 2] = colour.b;
        out[pixel * 4 + 3] = alpha[alphaIndex];
    }
}

// The bits of a DDS header the decoder needs. Offsets are from the "DDS "
// magic at the start of the file.
struct DdsHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    char fourcc[4] = {' ', ' ', ' ', ' '};
    bool valid = false;
};

inline DdsHeader parseDdsHeader(const std::uint8_t* bytes, std::size_t size) {
    DdsHeader header;
    if (size < 128 || bytes[0] != 'D' || bytes[1] != 'D' || bytes[2] != 'S' ||
        bytes[3] != ' ') {
        return header;
    }
    header.height = static_cast<std::uint32_t>(bytes[12]) |
                    (static_cast<std::uint32_t>(bytes[13]) << 8) |
                    (static_cast<std::uint32_t>(bytes[14]) << 16) |
                    (static_cast<std::uint32_t>(bytes[15]) << 24);
    header.width = static_cast<std::uint32_t>(bytes[16]) |
                   (static_cast<std::uint32_t>(bytes[17]) << 8) |
                   (static_cast<std::uint32_t>(bytes[18]) << 16) |
                   (static_cast<std::uint32_t>(bytes[19]) << 24);
    header.mipCount = static_cast<std::uint32_t>(bytes[28]) |
                      (static_cast<std::uint32_t>(bytes[29]) << 8) |
                      (static_cast<std::uint32_t>(bytes[30]) << 16) |
                      (static_cast<std::uint32_t>(bytes[31]) << 24);
    header.fourcc[0] = static_cast<char>(bytes[84]);
    header.fourcc[1] = static_cast<char>(bytes[85]);
    header.fourcc[2] = static_cast<char>(bytes[86]);
    header.fourcc[3] = static_cast<char>(bytes[87]);
    header.valid = true;
    return header;
}

inline bool fourccIs(const DdsHeader& header, const char* wanted) {
    return header.valid && header.fourcc[0] == wanted[0] &&
           header.fourcc[1] == wanted[1] && header.fourcc[2] == wanted[2] &&
           header.fourcc[3] == wanted[3];
}

// The size one mip level of a DXT1/DXT5 texture occupies. These two numbers
// are how a format mismatch is DETECTED without knowing where the bytes came
// from: a DXT1 block is 8 bytes per 4x4 texel group, a DXT5 block is 16.
inline std::size_t dxt1MipBytes(std::uint32_t width, std::uint32_t height) {
    const std::size_t blocks = ((static_cast<std::size_t>(width) + 3) / 4) *
                               ((static_cast<std::size_t>(height) + 3) / 4);
    return blocks * 8;
}

inline std::size_t dxt5MipBytes(std::uint32_t width, std::uint32_t height) {
    return dxt1MipBytes(width, height) * 2;
}

// Decode a whole DXT1/DXT5 image (one mip level) into RGBA8 rows.
inline bool decodeDxtImage(const std::uint8_t* blocks, std::size_t blockBytes,
                            std::uint32_t width, std::uint32_t height,
                            bool dxt5, std::uint8_t* rgbaOut) {
    const std::size_t blocksWide = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocksHigh = (static_cast<std::size_t>(height) + 3) / 4;
    if (blockBytes < blocksWide * blocksHigh * (dxt5 ? 16u : 8u)) {
        return false;
    }
    for (std::size_t by = 0; by < blocksHigh; ++by) {
        for (std::size_t bx = 0; bx < blocksWide; ++bx) {
            const std::uint8_t* block =
                blocks + (by * blocksWide + bx) * (dxt5 ? 16u : 8u);
            std::uint8_t pixels[64];
            if (dxt5) {
                decodeDxt5Block(block, pixels);
            } else {
                decodeDxt1Block(block, pixels);
            }
            for (int row = 0; row < 4; ++row) {
                const std::uint32_t y = static_cast<std::uint32_t>(by * 4 + row);
                if (y >= height) {
                    continue;
                }
                for (int column = 0; column < 4; ++column) {
                    const std::uint32_t x = static_cast<std::uint32_t>(bx * 4 + column);
                    if (x >= width) {
                        continue;
                    }
                    const std::uint8_t* source = pixels + (row * 4 + column) * 4;
                    std::uint8_t* target =
                        rgbaOut + (static_cast<std::size_t>(y) * width + x) * 4;
                    target[0] = source[0];
                    target[1] = source[1];
                    target[2] = source[2];
                    target[3] = source[3];
                }
            }
        }
    }
    return true;
}

}  // namespace rr3

#endif  // RR3_DXT_DECODE_HPP
