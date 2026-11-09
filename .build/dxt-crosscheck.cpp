// Cross-check the port's DXT decoder against an independent decoder (Pillow)
// on a REAL texture, outside the repository.
//
// WHY: the two questions have to be separated before anything is "fixed":
//   (1) are the BYTES of the rival car's texture good?
//   (2) is the CPU decode of them good?
//   (3) is the GPU's decode of them good?
// Pillow and this decoder are two independent implementations, so if they agree
// on the real asset then (1) and (2) are answered, and whatever is left is (3)
// -- which is the part the port can fix.
//
// The asset itself is EA material and must NOT be committed, so this harness
// lives in .build/ and takes the DDS as an argument.
//
// Usage: dxt-crosscheck.exe <file.dds>
#include "rr3/dxt_decode.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dxt-crosscheck <file.dds>\n");
        return 2;
    }
    std::FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fclose(file);
        std::printf("short read\n");
        return 2;
    }
    std::fclose(file);

    const rr3::DdsHeader header = rr3::parseDdsHeader(bytes.data(), bytes.size());
    if (!header.valid) {
        std::printf("not a DDS\n");
        return 2;
    }
    const bool dxt5 = rr3::fourccIs(header, "DXT5");
    const bool dxt1 = rr3::fourccIs(header, "DXT1");
    std::printf("size=%u x %u  mips=%u  fourcc=%c%c%c%c\n", header.width,
                header.height, header.mipCount, header.fourcc[0], header.fourcc[1],
                header.fourcc[2], header.fourcc[3]);
    if (!dxt1 && !dxt5) {
        std::printf("not a DXT1/DXT5 texture\n");
        return 2;
    }

    // The DDS payload starts after the 128-byte header.
    const std::uint8_t* payload = bytes.data() + 128;
    const std::size_t payloadBytes = bytes.size() - 128;
    const std::size_t needed = dxt5 ? rr3::dxt5MipBytes(header.width, header.height)
                                     : rr3::dxt1MipBytes(header.width, header.height);
    if (payloadBytes < needed) {
        std::printf("payload too small: %zu < %zu\n", payloadBytes, needed);
        return 2;
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(header.width) *
                                    header.height * 4);
    if (!rr3::decodeDxtImage(payload, payloadBytes, header.width, header.height,
                              dxt5, rgba.data())) {
        std::printf("decode failed\n");
        return 2;
    }

    // The same statistics the port will report for the GPU's own answer, so
    // the two can be compared line by line.
    std::size_t greenZeroed = 0;
    std::size_t magenta = 0;
    double mean[3] = {0, 0, 0};
    const std::size_t pixels = static_cast<std::size_t>(header.width) * header.height;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::uint8_t r = rgba[pixel * 4 + 0];
        const std::uint8_t g = rgba[pixel * 4 + 1];
        const std::uint8_t b = rgba[pixel * 4 + 2];
        mean[0] += r;
        mean[1] += g;
        mean[2] += b;
        if (g == 0) {
            ++greenZeroed;
        }
        if (r > 150 && b > 150 && g < 60) {
            ++magenta;
        }
    }
    std::printf("cpu-decoded mean RGB = (%.1f, %.1f, %.1f)\n", mean[0] / pixels,
                mean[1] / pixels, mean[2] / pixels);
    std::printf("green-zeroed pixels = %zu (%.3f %%)\n", greenZeroed,
                100.0 * greenZeroed / pixels);
    std::printf("magenta-looking pixels = %zu (%.3f %%)\n", magenta,
                100.0 * magenta / pixels);
    std::printf("first pixels: ");
    for (int index = 0; index < 6; ++index) {
        std::printf("(%u,%u,%u,%u) ", rgba[index * 4 + 0], rgba[index * 4 + 1],
                    rgba[index * 4 + 2], rgba[index * 4 + 3]);
    }
    std::printf("\n");
    return 0;
}
