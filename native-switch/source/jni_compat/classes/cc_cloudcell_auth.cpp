#include "cc_cloudcell_auth.h"

#include <zlib.h>

namespace cccloud {

/* ------------------------------------------------------------------ *
 * Small readers
 * ------------------------------------------------------------------ */

static bool readU32(const uint8_t *body, size_t size, size_t off, uint32_t &out)
{
    if (off + 4 > size) {
        return false;
    }
    out = (uint32_t)body[off] |
          ((uint32_t)body[off + 1] << 8) |
          ((uint32_t)body[off + 2] << 16) |
          ((uint32_t)body[off + 3] << 24);
    return true;
}

static void appendU32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back((uint8_t)(v & 0xffu));
    out.push_back((uint8_t)((v >> 8) & 0xffu));
    out.push_back((uint8_t)((v >> 16) & 0xffu));
    out.push_back((uint8_t)((v >> 24) & 0xffu));
}

/* ------------------------------------------------------------------ *
 * BinaryBlob checksum (guest raw 0x1cb9734)
 * ------------------------------------------------------------------ */

/*
 * NOT crc32 and NOT adler32. The guest multiplies the four bytes of each
 * group together and divides by the fourth one, which no standard hash does.
 * Read off the disassembly at 0x1cb9734:
 *
 *     sum = 0
 *     for i in range(0, n, 4):
 *         v = (int8)p[i]
 *         if i+1 < n: v = (int32)((uint32)v * (uint32)(int8)p[i+1])
 *         if i+2 < n: v = (int32)((uint32)v * (uint32)(int8)p[i+2])
 *         if i+3 < n and (int8)p[i+3] != 0: v = v / (int8)p[i+3]
 *         sum = (uint32)(sum + (uint32)v)
 *
 * The multiply is the low 32 bits of the product (the guest's `mul w`), the
 * division is ARM64 `sdiv` (truncates toward zero), and the accumulation
 * wraps. The `i|1 / i|2 / i|3` in the binary are just i+1 / i+2 / i+3 for
 * i % 4 == 0, which is what the loop counter always is.
 */
static uint32_t blobChecksum(const uint8_t *data, size_t size)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 4) {
        int32_t v = (int32_t)(int8_t)data[i];
        if (i + 1 < size) {
            v = (int32_t)((uint32_t)v * (uint32_t)(int32_t)(int8_t)data[i + 1]);
        }
        if (i + 2 < size) {
            v = (int32_t)((uint32_t)v * (uint32_t)(int32_t)(int8_t)data[i + 2]);
        }
        if (i + 3 < size) {
            int32_t d = (int32_t)(int8_t)data[i + 3];
            if (d != 0) {
                /* C++ integer division truncates toward zero, like sdiv. */
                v = v / d;
            }
        }
        sum += (uint32_t)v;
    }
    return sum;
}

/* One BinaryBlob box: [u32 checksum][u32 mode][payload]. The checksum
 * covers the payload only - everything from offset 8 on - which is exactly
 * what the guest's UnboxData recomputes at 0x1cbd92c..0x1cbd93c before it
 * dispatches on the mode. */
static void appendBox(std::vector<uint8_t> &out, uint32_t mode,
                      const std::vector<uint8_t> &payload)
{
    appendU32(out, blobChecksum(payload.data(), payload.size()));
    appendU32(out, mode);
    out.insert(out.end(), payload.begin(), payload.end());
}

/* The mode-2 keystream, from the guest raw 0x1cbd9c0..0x1cbda54: the key
 * byte is payload[0] and C[i] = P[i] ^ ((key + 3*i) & 0xFF). The guest
 * decrypts in place with the same walk, so the two are inverses. */
static std::vector<uint8_t> xorKeystream(const std::vector<uint8_t> &plain,
                                         uint8_t key)
{
    std::vector<uint8_t> out;
    out.reserve(plain.size() + 1);
    out.push_back(key);
    for (size_t i = 0; i < plain.size(); ++i) {
        out.push_back((uint8_t)(plain[i] ^
                                 (uint8_t)((key + 3u * (unsigned)i) & 0xffu)));
    }
    return out;
}

/* ------------------------------------------------------------------ *
 * The authenticate response
 * ------------------------------------------------------------------ */

/*
 * The authenticate-success payload, in the exact order the guest's
 * CC_SyncManager_Class::AuthenticationCallback (guest raw 0x1d0131c) reads
 * it. Each row says which guest check the value satisfies; the check
 * addresses are the guest's own.
 *
 *  f0  int32  1         must be non-zero: 0x1d013dc jumps to the failure
 *                       path when it is 0.
 *  f1  int32  0         handed to a setter on the account manager
 *                       (0x1d01414). The guest logs it as a log index and
 *                       nothing reads it back, so 0 is the honest default.
 *  f2  int32  2         AUTHENTICATE_STATE_AUTHENTICATE_SUCCESS. The state
 *                       names live in a table at 0x1f66a68 (read out of
 *                       .data.rel.ro): 0 LEGACY, 1 INITIATE,
 *                       2 AUTHENTICATE_SUCCESS, 3 AUTHENTICATED,
 *                       4 REAUTHENTICATE, 5 MEMBER_CONFLICT,
 *                       6 DEVICE_CONFLICT, 7 SOCIAL_AUTH_CHANGED,
 *                       8 REAUTHENTICATE_STRICT. The guest branches on
 *                       state == 2 at 0x1d01484 and only THAT branch parses
 *                       the member id and the token at all.
 *  f3  int32  memberId  the CCMemberID. The guest compares it against a
 *                       read-only constant of 1 at guest raw 0x600f30
 *                       (reached through the GOT slot 0x1f6e270) at
 *                       0x1d01dc4, so 1 satisfies that comparison. A
 *                       mismatch there is not fatal - it only changes the
 *                       "current auth token timeout" - but the guest also
 *                       compares it against the member id the account
 *                       manager already holds at 0x1d01e20, and THAT
 *                       mismatch sets the error flag.
 *  f4  int32  0         a bool (the guest asserts the value is < 2 at
 *                       0x1d014c8). 1 would mean "member is developer"
 *                       (the guest logs "CC: Member Is Developer"), so 0
 *                       is the honest value for a local port.
 *  f5  int32  0         handed to a global setter at 0x1d01d78.
 *  f6  int32  0         handed to a global setter at 0x1d01d98.
 *  f7  int32  0         a bool (asserted < 2 at 0x1d0154c).
 *  f8  int32 len + raw  the auth token. The length must be non-zero: the
 *             bytes     guest jumps to the failure path when it is 0
 *                       (0x1d0158c). The bytes are raw, no NUL.
 *  f9  int32  86400    the new auth token timeout, read after the token
 *                       at 0x1d01d9c. With f3 matching, the guest stores 0
 *                       as the "current" timeout (0x1d01dc8), so this is
 *                       only the value the server-side lifetime would
 *                       have; a day is the honest non-zero answer.
 */
static const char kAuthToken[] = "rr3-switch-local-0123456789abcdef";

bool buildAuthResponse(uint32_t memberId, std::vector<uint8_t> &out)
{
    std::vector<uint8_t> payload;
    payload.reserve(96);
    appendU32(payload, 1u);                          /* f0 */
    appendU32(payload, 0u);                          /* f1 */
    appendU32(payload, 2u);                          /* f2 */
    appendU32(payload, memberId);                    /* f3 */
    appendU32(payload, 0u);                          /* f4 */
    appendU32(payload, 0u);                          /* f5 */
    appendU32(payload, 0u);                          /* f6 */
    appendU32(payload, 0u);                          /* f7 */
    appendU32(payload, (uint32_t)(sizeof(kAuthToken) - 1u));
    payload.insert(payload.end(), kAuthToken, kAuthToken + sizeof(kAuthToken) - 1u);
    appendU32(payload, 86400u);                      /* f9 */

    /* mode 1 box: u32 uncompressed length, then the zlib stream. */
    uLongf bound = compressBound((uLong)payload.size());
    std::vector<uint8_t> zipped((size_t)bound);
    if (compress2(zipped.data(), &bound, payload.data(),
                  (uLong)payload.size(), 9) != Z_OK) {
        return false;
    }
    zipped.resize((size_t)bound);

    std::vector<uint8_t> innerPayload;
    appendU32(innerPayload, (uint32_t)payload.size());
    innerPayload.insert(innerPayload.end(), zipped.begin(), zipped.end());

    std::vector<uint8_t> innerBox;
    appendBox(innerBox, 1u, innerPayload);

    std::vector<uint8_t> outerPayload = xorKeystream(innerBox, 0x5au);
    std::vector<uint8_t> outerBox;
    appendBox(outerBox, 2u, outerPayload);

    out.swap(outerBox);
    return true;
}

/* ------------------------------------------------------------------ *
 * The request the guest posts
 * ------------------------------------------------------------------ */

/*
 * The authenticate request body, from the guest's own writer at raw 0x1d00f38
 * (called from 0x1d00454, the function that also builds the CC_Sync.php URL
 * at 0x1d006a8):
 *
 *     int32  0x22                          field 0 - the same tag the sync
 *                                           request uses (0x1d00f74)
 *     int32  env1                         (0x1d00fc0)
 *     int32  env2                         (0x1d00fdc)
 *     int32  X                            (0x1d0100c)
 *     7 x (int32 length, length bytes)    the header strings (0x1d01048 ..)
 *
 * The sync request (writer 0x1d04440) starts with the SAME four words and
 * the same first header string, so the first four words cannot tell the two
 * apart. The tail can: the authenticate request either ends after the seven
 * header strings or carries the authenticate tail
 *
 *     int32  auth state (1 .. 8)
 *     int32  member id
 *     int32  sub-sync count
 *     count x (int32 id, 4 x (int32 length, length bytes))
 *
 * while the sync request carries a sub-sync list whose leading word is the
 * sub-sync COUNT. The guest logs that count as 13 in the measured run, so it
 * is outside 1..8 and the authenticate-state read rejects it.
 */
static const size_t kHeaderStringCount = 7;
static const size_t kMaxSubSyncs = 64;

bool parseAuthRequest(const uint8_t *body, size_t size, AuthRequest &out)
{
    out = AuthRequest();
    out.bodySize = size;

    if (body == nullptr || size < 16) {
        return false;
    }

    uint32_t tag = 0;
    if (!readU32(body, size, 0, tag) || tag != 0x22u) {
        return false;
    }
    if (!readU32(body, size, 4, out.env1) ||
        !readU32(body, size, 8, out.env2) ||
        !readU32(body, size, 12, out.clientId)) {
        return false;
    }

    size_t off = 16;
    for (size_t i = 0; i < kHeaderStringCount; ++i) {
        uint32_t len = 0;
        if (!readU32(body, size, off, len)) {
            return false;
        }
        off += 4;
        if ((size_t)len > size - off) {
            return false;
        }
        off += (size_t)len;
    }
    out.headerCount = kHeaderStringCount;

    if (off == size) {
        /* Exactly the eleven words the writer produces: the authenticate
         * request. */
        out.authShaped = true;
        out.consumed = off;
        return true;
    }

    /* Otherwise the authenticate tail has to follow exactly. */
    uint32_t authState = 0;
    uint32_t memberId = 0;
    uint32_t subSyncs = 0;
    if (!readU32(body, size, off, authState) ||
        !readU32(body, size, off + 4, memberId) ||
        !readU32(body, size, off + 8, subSyncs)) {
        return false;
    }
    if (authState < 1u || authState > 8u) {
        return false;
    }
    if (subSyncs > (uint32_t)kMaxSubSyncs) {
        return false;
    }
    off += 12;

    for (uint32_t s = 0; s < subSyncs; ++s) {
        uint32_t id = 0;
        if (!readU32(body, size, off, id)) {
            return false;
        }
        off += 4;
        for (int k = 0; k < 4; ++k) {
            uint32_t len = 0;
            if (!readU32(body, size, off, len)) {
                return false;
            }
            off += 4;
            if ((size_t)len > size - off) {
                return false;
            }
            off += (size_t)len;
        }
    }

    if (off != size) {
        return false;
    }
    out.authShaped = true;
    out.hasMemberId = true;
    out.memberId = memberId;
    out.consumed = off;
    return true;
}

} // namespace cccloud
