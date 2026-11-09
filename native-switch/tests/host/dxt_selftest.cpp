// Host self-test for the DXT decoder the port ships.
//
// WHY: the rival cars render magenta and the pixels say the green channel is
// zeroed, which means the texture is DECODED and decoded wrong. To find out
// whether the CPU decode or the GPU decode is at fault, both have to be
// measured against the same bytes -- and a decoder that is itself wrong would
// make that comparison meaningless.
//
// So the decoder is executed here, against hand-computed blocks. Every
// expectation below is derived from the DXT1/DXT5 specification, not from
// another decoder's output.
//
// The cases cover the two rules that are easy to get wrong:
//   * DXT1's THREE-COLOUR mode: colour0 <= colour1 means index 3 is
//     TRANSPARENT, not a fourth colour.
//   * DXT5's block layout: the colour endpoints are at bytes 12..15 and the
//     colour indices at bytes 8..11 -- reading the endpoints out of bytes 8..11
//     produces a plausible-looking but wrong image.

#include "rr3/dxt_decode.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const std::string& what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void checkRgba(const rr3::Rgba& got, int r, int g, int b, int a,
               const std::string& what) {
    ++gChecks;
    if (got.r != r || got.g != g || got.b != b || got.a != a) {
        ++gFailures;
        std::printf("FAIL: %s (got %u,%u,%u,%u want %d,%d,%d,%d)\n", what.c_str(),
                    got.r, got.g, got.b, got.a, r, g, b, a);
    }
}

void put16(std::vector<std::uint8_t>& block, std::size_t offset,
           std::uint16_t value) {
    block[offset + 0] = static_cast<std::uint8_t>(value & 0xff);
    block[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

// DXT1, four-colour mode: colour0 = red, colour1 = blue.
void testDxt1FourColour() {
    std::vector<std::uint8_t> block(8, 0);
    put16(block, 0, 0xf800);  // colour0 = (31,0,0) -> (255,0,0)
    put16(block, 2, 0x001f);  // colour1 = (0,0,31) -> (0,0,255)
    // index 3 on pixel 0, index 0 everywhere else.
    block[4] = 0x03;

    std::uint8_t pixels[64] = {};
    rr3::decodeDxt1Block(block.data(), pixels);

    // palette[3] = (colour0 + 2*colour1) / 3 = ((255+0)/3, 0, (0+510)/3)
    //             = (85, 0, 170)
    checkRgba({pixels[0], pixels[1], pixels[2], pixels[3]}, 85, 0, 170, 255,
               "dxt1 four-colour palette[3]");
    // palette[2] = (2*colour0 + colour1) / 3 = (170, 0, 85)
    std::vector<std::uint8_t> block2(8, 0);
    put16(block2, 0, 0xf800);
    put16(block2, 2, 0x001f);
    block2[4] = 0x02;  // pixel0 -> index 2
    std::uint8_t pixels2[64] = {};
    rr3::decodeDxt1Block(block2.data(), pixels2);
    checkRgba({pixels2[0], pixels2[1], pixels2[2], pixels2[3]}, 170, 0, 85, 255,
               "dxt1 four-colour palette[2]");
    checkRgba({pixels[4], pixels[5], pixels[6], pixels[7]}, 255, 0, 0, 255,
               "dxt1 four-colour palette[0]");
    checkRgba({pixels[60], pixels[61], pixels[62], pixels[63]}, 255, 0, 0, 255,
               "dxt1 four-colour last pixel");
}

// DXT1, three-colour mode: colour0 <= colour1, so index 3 is TRANSPARENT.
void testDxt1ThreeColourAlpha() {
    std::vector<std::uint8_t> block(8, 0);
    put16(block, 0, 0x0000);  // colour0 = black
    put16(block, 2, 0xffff);  // colour1 = white
    // pixel0 -> index 3 (transparent), pixel1 -> index 2 (the midpoint).
    block[4] = 0x03 | (0x02 << 2);

    std::uint8_t pixels[64] = {};
    rr3::decodeDxt1Block(block.data(), pixels);

    checkRgba({pixels[0], pixels[1], pixels[2], pixels[3]}, 0, 0, 0, 0,
               "dxt1 three-colour index 3 is transparent");
    // (colour0 + colour1) / 2 = (127,127,127)
    checkRgba({pixels[4], pixels[5], pixels[6], pixels[7]}, 127, 127, 127, 255,
               "dxt1 three-colour midpoint");
    checkRgba({pixels[8], pixels[9], pixels[10], pixels[11]}, 0, 0, 0, 255,
               "dxt1 three-colour colour0");
}

// DXT5: alpha block first, colour endpoints at bytes 12..15, colour indices
// at bytes 8..11.
void testDxt5Layout() {
    std::vector<std::uint8_t> block(16, 0);
    block[0] = 0xff;  // alpha0
    block[1] = 0x80;  // alpha1  (alpha0 > alpha1 -> the 7-step ramp)
    // alpha indices: pixel0 -> 0, pixel1 -> 1, the rest 0. The indices are
    // 3 bits each, so pixel1's index lives in bits 3..5.
    block[2] = 0x08;
    // colour indices: every pixel 0.
    block[8] = 0x00;
    put16(block, 12, 0xf800);  // colour0 = red
    put16(block, 14, 0x001f);  // colour1 = blue

    std::uint8_t pixels[64] = {};
    rr3::decodeDxt5Block(block.data(), pixels);

    checkRgba({pixels[0], pixels[1], pixels[2], pixels[3]}, 255, 0, 0, 255,
               "dxt5 alpha index 0");
    checkRgba({pixels[4], pixels[5], pixels[6], pixels[7]}, 255, 0, 0, 128,
               "dxt5 alpha index 1");
    // The colour endpoints have to come from bytes 12..15. If they were read
    // out of bytes 8..11 the whole block would decode to a different palette,
    // which is the bug this layout note exists for.
    checkRgba({pixels[8], pixels[9], pixels[10], pixels[11]}, 255, 0, 0, 255,
               "dxt5 colour comes from the trailing endpoints");
}

// DXT5's other alpha ramp: alpha0 <= alpha1 gives six interpolated values
// plus the two specials 0 and 255.
void testDxt5AlphaRampLow() {
    std::vector<std::uint8_t> block(16, 0);
    block[0] = 0x80;  // alpha0
    block[1] = 0xff;  // alpha1  (alpha0 <= alpha1 -> the 5-step ramp)
    // alpha index 6 -> 0, index 7 -> 255
    block[2] = 0x06 | (0x07 << 3);
    put16(block, 12, 0xffff);
    put16(block, 14, 0x0000);

    std::uint8_t pixels[64] = {};
    rr3::decodeDxt5Block(block.data(), pixels);
    checkRgba({pixels[0], pixels[1], pixels[2], pixels[3]}, 255, 255, 255, 0,
               "dxt5 low ramp index 6 is 0");
    checkRgba({pixels[4], pixels[5], pixels[6], pixels[7]}, 255, 255, 255, 255,
               "dxt5 low ramp index 7 is 255");
}

// A whole 4x4 image, so the block-to-texel placement is exercised too.
void testImagePlacement() {
    std::vector<std::uint8_t> block(8, 0);
    put16(block, 0, 0xf800);  // red
    put16(block, 2, 0xf800);  // red  (colour0 == colour1 -> three-colour mode)
    block[4] = 0x00;

    std::vector<std::uint8_t> image(4 * 4 * 4, 0);
    check(rr3::decodeDxtImage(block.data(), block.size(), 4, 4, false,
                               image.data()),
          "dxt1 image decodes");
    bool allRed = true;
    for (std::size_t pixel = 0; pixel < 16; ++pixel) {
        if (image[pixel * 4 + 0] != 255 || image[pixel * 4 + 1] != 0 ||
            image[pixel * 4 + 2] != 0 || image[pixel * 4 + 3] != 255) {
            allRed = false;
        }
    }
    check(allRed, "dxt1 image pixels are all red");
    check(!rr3::decodeDxtImage(block.data(), 4, 4, 4, false, image.data()),
          "dxt1 image refuses a short buffer");
}

// The two sizes that make a DXT1/DXT5 mix-up visible without knowing where
// the bytes came from.
void testMipSizes() {
    check(rr3::dxt1MipBytes(1024, 1024) == 524288u,
          "dxt1 1024x1024 is 524288 bytes");
    check(rr3::dxt5MipBytes(1024, 1024) == 1048576u,
          "dxt5 1024x1024 is 1048576 bytes");
    check(rr3::dxt1MipBytes(4, 4) == 8u, "dxt1 4x4 is 8 bytes");
    check(rr3::dxt5MipBytes(4, 4) == 16u, "dxt5 4x4 is 16 bytes");
    check(rr3::dxt1MipBytes(1024, 512) == 262144u,
          "dxt1 1024x512 is 262144 bytes");
}

// The DDS header fields the decoder reads.
void testDdsHeader() {
    std::vector<std::uint8_t> header(128, 0);
    std::memcpy(header.data(), "DDS ", 4);
    header[12] = 0x00;  // height 256
    header[13] = 0x01;
    header[16] = 0x00;  // width 1024
    header[17] = 0x04;
    header[28] = 0x0b;  // 11 mip levels
    std::memcpy(header.data() + 84, "DXT1", 4);

    const rr3::DdsHeader parsed = rr3::parseDdsHeader(header.data(), header.size());
    check(parsed.valid, "dds header parses");
    check(parsed.width == 1024, "dds width");
    check(parsed.height == 256, "dds height");
    check(parsed.mipCount == 11, "dds mip count");
    check(rr3::fourccIs(parsed, "DXT1"), "dds fourcc is DXT1");
    check(!rr3::fourccIs(parsed, "DXT5"), "dds fourcc is not DXT5");
    check(!rr3::parseDdsHeader(header.data(), 16).valid,
          "a short buffer is not a dds header");
}

}  // namespace

int main() {
    testDxt1FourColour();
    testDxt1ThreeColourAlpha();
    testDxt5Layout();
    testDxt5AlphaRampLow();
    testImagePlacement();
    testMipSizes();
    testDdsHeader();

    if (gFailures != 0) {
        std::printf("FAIL: %d of %d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("PASS: %d checks\n", gChecks);
    return 0;
}
