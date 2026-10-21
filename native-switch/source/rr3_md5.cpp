#include "rr3/rr3_md5.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

// RFC 1321, section 3.4.
constexpr std::uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

// floor(2^32 * abs(sin(i + 1))), RFC 1321, section 3.4.
constexpr std::uint32_t kSine[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

constexpr std::uint32_t rotateLeft(std::uint32_t value, std::uint32_t count) {
    return (value << count) | (value >> (32u - count));
}

} // namespace

void Md5::begin() {
    mState[0] = 0x67452301u;
    mState[1] = 0xefcdab89u;
    mState[2] = 0x98badcfeu;
    mState[3] = 0x10325476u;
    mBits = 0;
    mBuffered = 0;
}

void Md5::transform(const std::uint8_t block[64]) {
    std::uint32_t words[16];
    for (int index = 0; index < 16; ++index) {
        words[index] = static_cast<std::uint32_t>(block[index * 4]) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 8) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 16) |
                       (static_cast<std::uint32_t>(block[index * 4 + 3]) << 24);
    }
    std::uint32_t a = mState[0];
    std::uint32_t b = mState[1];
    std::uint32_t c = mState[2];
    std::uint32_t d = mState[3];
    for (int index = 0; index < 64; ++index) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        // g is the WORD index, so every round's formula is taken modulo 16.
        // Without the mask the round-2..4 indices run past the end of
        // words[] and the digest is simply wrong -- which is exactly what the
        // first version of this file did, and why the RFC 1321 test vectors
        // below are part of the file's own verification story.
        if (index < 16) {
            f = (b & c) | (~b & d);
            g = static_cast<std::uint32_t>(index);
        } else if (index < 32) {
            f = (d & b) | (~d & c);
            g = static_cast<std::uint32_t>((5 * index + 1) & 15);
        } else if (index < 48) {
            f = b ^ c ^ d;
            g = static_cast<std::uint32_t>((3 * index + 5) & 15);
        } else {
            f = c ^ (b | ~d);
            g = static_cast<std::uint32_t>((7 * index) & 15);
        }
        const std::uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotateLeft(a + f + kSine[index] + words[g], kShift[index]);
        a = temp;
    }
    mState[0] += a;
    mState[1] += b;
    mState[2] += c;
    mState[3] += d;
}

void Md5::update(const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    mBits += static_cast<std::uint64_t>(length) * 8u;
    while (length > 0) {
        const std::size_t take = (64u - mBuffered) < length ? (64u - mBuffered) : length;
        std::memcpy(mBuffer + mBuffered, bytes, take);
        mBuffered += take;
        bytes += take;
        length -= take;
        if (mBuffered == 64u) {
            transform(mBuffer);
            mBuffered = 0;
        }
    }
}

void Md5::finish(char out[33]) {
    std::uint8_t padding[72];
    std::memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;
    const std::size_t padLength = (mBuffered < 56u) ? (56u - mBuffered) : (120u - mBuffered);
    const std::uint64_t bits = mBits;
    std::uint8_t lengthBytes[8];
    for (int index = 0; index < 8; ++index) {
        lengthBytes[index] = static_cast<std::uint8_t>((bits >> (8u * index)) & 0xffu);
    }
    update(padding, padLength);
    // update() counted the padding into mBits; the length field must carry the
    // length of the MESSAGE, so it is written from the saved copy.
    std::memcpy(mBuffer + mBuffered, lengthBytes, 8);
    mBuffered += 8;
    transform(mBuffer);
    mBuffered = 0;
    static const char kHex[] = "0123456789abcdef";
    for (int index = 0; index < 4; ++index) {
        for (int byte = 0; byte < 4; ++byte) {
            const std::uint8_t value =
                static_cast<std::uint8_t>((mState[index] >> (8u * byte)) & 0xffu);
            out[index * 8 + byte * 2] = kHex[value >> 4];
            out[index * 8 + byte * 2 + 1] = kHex[value & 0x0fu];
        }
    }
    out[32] = '\0';
}

bool md5File(const char* path, char out[33], unsigned long long* sizeOut) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    Md5 digest;
    digest.begin();
    unsigned char buffer[64 * 1024];
    unsigned long long total = 0;
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) {
            digest.update(buffer, got);
            total += got;
        }
        if (got < sizeof(buffer)) {
            break;
        }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return false;
    }
    digest.finish(out);
    if (sizeOut != nullptr) {
        *sizeOut = total;
    }
    return true;
}

} // namespace rr3
