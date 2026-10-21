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
 * Boxing a payload
 * ------------------------------------------------------------------ */

/*
 * Box a payload the way the guest's own writer boxes what it sends:
 * BoxData(1) (zlib) first, then BoxData(2) (XOR) on top of it. The guest's
 * reader walks two boxes to reach the payload (0x1d01380 and 0x1d0138c).
 *
 * The bytes are the ones the port has always delivered: the same key
 * (0x5a), the same compression level (9), the same checksum function. The
 * authenticate response and the sync response share this one builder so
 * there is a single place where the box format lives.
 */
static bool boxPayload(const std::vector<uint8_t> &payload,
                        std::vector<uint8_t> &out)
{
    if (payload.empty()) {
        return false;
    }

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

    return boxPayload(payload, out);
}

/* ------------------------------------------------------------------ *
 * The box reader (the guest's own UnboxData, guest raw 0x1cbd8f0)
 * ------------------------------------------------------------------ */

/*
 * Unbox a BinaryBlob the way the guest's reader does.
 *
 * MEASURED from the guest's disassembly:
 *
 *   0x1cbd914  ldr w8, [x0, #8]      ; the blob's size
 *   0x1cbd918  cmp w8, #9            ; a box needs at least 9 bytes
 *   0x1cbd920  ldr x9, [x0]          ; the data
 *   0x1cbd92c  ldp w21, w20, [x9]    ; w21 = checksum, w20 = mode
 *   0x1cbd930  add x0, x9, #8        ; the payload
 *   0x1cbd934  bl 0x1cb9734          ; the guest's own checksum
 *   0x1cbd938  cmp w21, w0           ; it must match, or the box is rejected
 *   0x1cbd940  cmp w20, #2           ; mode 2 = XOR
 *   0x1cbd948  cmp w20, #1           ; mode 1 = zlib
 *   0x1cbd950  cbnz w20, ...         ; mode 0 = plain
 *
 * and the modes:
 *
 *   mode 0  the payload is the bytes after the 8-byte header, moved to the
 *           front of the buffer (0x1cbd984).
 *   mode 1  the payload is [u32 uncompressed length][zlib stream]; the guest
 *           inflates it into a fresh buffer and installs THAT as the blob
 *           (0x1cbdb84..0x1cbdb94), so the reader's offset stays 0.
 *   mode 2  the payload is [u8 key][ciphertext]; the guest XORs the
 *           ciphertext in place, writing the plaintext to the FRONT of the
 *           buffer (the SIMD loop stores at data[0] and reads from data[9],
 *           0x1cbda10), so the next box starts at offset 0.
 *
 * The chain the guest walks is exactly two steps for a response
 * (0x1d01380 and 0x1d0138c) and two for a request the guest itself packs
 * (BoxData(1) then BoxData(2)). The loop here follows the chain for as long
 * as it is boxes, and stops when a mode-0 box or a bare payload is reached.
 */
bool unboxBlob(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
    out.clear();
    if (data == nullptr || size < 9u) {
        return false;
    }

    /* The chain is walked on a working buffer, because the guest's mode 2
     * rewrites the buffer in place and its mode 1 replaces it outright. */
    std::vector<uint8_t> current(data, data + size);

    for (int step = 0; step < 4; ++step) {
        if (current.size() < 9u) {
            break;
        }
        uint32_t checksum = 0;
        uint32_t mode     = 0;
        if (!readU32(current.data(), current.size(), 0, checksum) ||
            !readU32(current.data(), current.size(), 4, mode) ||
            mode > 2u) {
            break;
        }
        const size_t payloadSize = current.size() - 8u;
        if (blobChecksum(current.data() + 8, payloadSize) != checksum) {
            break;
        }

        std::vector<uint8_t> next;
        if (mode == 0u) {
            /* The payload, moved to the front - the guest's mode-0 branch. */
            next.assign(current.begin() + 8, current.end());
        } else if (mode == 1u) {
            /* [u32 uncompressed length][zlib stream] */
            uint32_t rawLen = 0;
            if (!readU32(current.data(), current.size(), 8, rawLen) ||
                rawLen == 0u || rawLen > (16u * 1024u * 1024u)) {
                return false;
            }
            const size_t compressed = current.size() - 12u;
            next.resize((size_t)rawLen);
            uLongf destLen = (uLongf)rawLen;
            if (uncompress(next.data(), &destLen,
                           current.data() + 12, (uLong)compressed) != Z_OK) {
                return false;
            }
            next.resize((size_t)destLen);
        } else {
            /* [u8 key][ciphertext]: the plaintext is written to the front. */
            const uint8_t key = current[8];
            const size_t cipherLen = current.size() - 9u;
            next.resize(cipherLen);
            for (size_t i = 0; i < cipherLen; ++i) {
                next[i] = static_cast<uint8_t>(
                    current[9 + i] ^
                    static_cast<uint8_t>((key + 3u * (unsigned)i) & 0xffu));
            }
        }
        current.swap(next);

        /* The chain ends when the bytes are no longer a box: what is left is
         * the payload. The guest reaches the same place by unboxing a fixed
         * number of times (twice for both a request and a response); here the
         * end is found by the checksum instead, which cannot be forged by a
         * payload. */
        if (current.size() < 9u) {
            break;
        }
        uint32_t nextChecksum = 0;
        uint32_t nextMode     = 0;
        if (!readU32(current.data(), current.size(), 0, nextChecksum) ||
            !readU32(current.data(), current.size(), 4, nextMode) ||
            nextMode > 2u ||
            blobChecksum(current.data() + 8, current.size() - 8u) !=
                nextChecksum) {
            break;
        }
    }

    if (current.empty() || current.size() == size) {
        /* Nothing was unboxed: the input was not a box to begin with. */
        return false;
    }
    out.swap(current);
    return true;
}

/* ------------------------------------------------------------------ *
 * The request the guest posts
 * ------------------------------------------------------------------ */

/*
 * The request payload the guest's writers produce, measured:
 *
 *   0x1d00f5c  mov w9, #0x22         ; the tag
 *   0x1d00f74  bl 0x1cbdd28          ; PackData(writer, &tag, 4)
 *   0x1d00fbc  ldr w8, [x8]          ; env1   (the global's +0x38 word 0)
 *   0x1d00fd4  ldr w8, [x8, #4]      ; env2   (the global's +0x38 word 1)
 *   0x1d00ffc  str w0, [sp, #8]      ; the client id
 *   0x1d01048  ...                   ; seven length-prefixed header strings
 *
 * and the sync writer (0x1d04438, SyncManagerV34.cpp) writes the SAME four
 * words and the same header strings, then a sub-sync list whose leading word
 * is the sub-sync COUNT. The authenticate writer's tail is either nothing or
 * the authenticate tail (state, member id, sub-sync count, entries), and the
 * state is always in 1..8, so a sub-sync count of 13 rejects it.
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
        out.tailWords = 0;
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
    out.tailWords = 3u + (size_t)subSyncs * 5u;
    return true;
}

/*
 * Classify the request the guest posted.
 *
 * The body is a BOX, so the first thing to do is unbox it. Only then is the
 * payload's shape meaningful.
 *
 * The two kinds are told apart by the payload's tail:
 *
 *   authenticate  the payload ends right after the seven header strings, or
 *                 carries the authenticate tail (state in 1..8, member id,
 *                 sub-sync count and the entries).
 *   sync          the payload carries a sub-sync list whose leading word is
 *                 the sub-sync COUNT. The guest logged 13 of them in the
 *                 measured run, and 13 is outside the authenticate state's
 *                 1..8, so the authenticate parse rejects it.
 */
RequestKind parseRequest(const uint8_t *body, size_t size, AuthRequest &out)
{
    out = AuthRequest();
    out.bodySize = size;

    if (body == nullptr || size == 0u) {
        return RequestKind::Unknown;
    }

    /* The box header, when the body is one. */
    uint32_t checksum = 0;
    uint32_t mode     = 0;
    if (readU32(body, size, 0, checksum) && readU32(body, size, 4, mode) &&
        mode <= 2u && size >= 9u) {
        out.boxed = true;
        out.boxMode = mode;
        out.boxChecksum = checksum;
    }

    std::vector<uint8_t> payload;
    if (!unboxBlob(body, size, payload)) {
        /* Not a box: the guest's own writers always box, so a body that
         * does not unbox is not a request this port can read. */
        return RequestKind::Unknown;
    }
    out.payloadSize = payload.size();

    /* The authenticate shape is checked first, on the PAYLOAD. */
    AuthRequest auth;
    if (parseAuthRequest(payload.data(), payload.size(), auth)) {
        out.authShaped    = true;
        out.hasMemberId   = auth.hasMemberId;
        out.memberId      = auth.memberId;
        out.env1          = auth.env1;
        out.env2          = auth.env2;
        out.clientId      = auth.clientId;
        out.headerCount   = auth.headerCount;
        out.consumed      = auth.consumed;
        out.tailWords     = auth.tailWords;
        return RequestKind::Authenticate;
    }

    /* Not the authenticate shape. It is still a Cloudcell request if the
     * head parses, so try the sync head on its own. */
    if (payload.size() >= 16u) {
        uint32_t tag = 0;
        if (readU32(payload.data(), payload.size(), 0, tag) && tag == 0x22u) {
            readU32(payload.data(), payload.size(), 4, out.env1);
            readU32(payload.data(), payload.size(), 8, out.env2);
            readU32(payload.data(), payload.size(), 12, out.clientId);
            out.headerCount = kHeaderStringCount;
            out.consumed = payload.size();
            out.tailWords = (payload.size() - 16u) / 4u;
            return RequestKind::Sync;
        }
    }
    return RequestKind::Unknown;
}

/* ------------------------------------------------------------------ *
 * The response bodies
 * ------------------------------------------------------------------ */

/*
 * The SYNC response.
 *
 * The shape is the guest's own SYNC-response reader, guest raw 0x1cfb784,
 * which reads the payload in this order:
 *
 *     0x1cfb7f4  read u32   -> the member id; compared with the one the
 *                              account manager holds, and a difference is
 *                              logged ("CC: Member changed [Member Id: %d]")
 *     0x1cfb850  read u32   -> the developer flag; the guest asserts the
 *                              value is < 2 (UnpackBool, 0x38ccf6)
 *     0x1cfb8cc  read u32   -> THE DEVICE ID; logged ("CC: Device Id: %d")
 *                              and set on the guest's server info
 *                              (0x1cfb908: vtable[0x150] then vtable[0x18])
 *     0x1cfb8e4  read u32   -> set on the guest's server info
 *                              (0x1cfb928: vtable[0x140] then vtable[0xe0])
 *     0x1cfb958  read u32   -> a bool; the guest asserts the value is < 2
 *
 * MEASURED, archive 20261021-145625-eden-bodyprobe2: the reader does NOT
 * start at the first word of the payload. The port answered with
 *
 *     [1][0][1001][0][0][0 ...]
 *
 * and the guest logged
 *
 *     CC: Member changed [Member Id: 1001]
 *     CC: Device Id: 0
 *
 * so the member id came from word 2 and the device id from word 4. The
 * payload therefore carries a TWO-WORD header in front of the member
 * fields.
 *
 * The header's VALUES are not checked by the guest's sync path: the port's
 * earlier answer (the authenticate payload) carried [1][0] there and the
 * guest parsed the member fields straight after them without complaint, so
 * [1][0] is what the port sends - the one pair of values MEASURED to be
 * accepted. What the guest's own writer puts in a two-word head of this
 * family is [0x079f8590][0] (guest raw 0x1ce5034), but that message is a
 * different one (its reader, 0x1ce4e58, checks the ENVIRONMENT word that
 * follows the head, and the sync response has the member id there).
 *
 * The device id is the field the port has to get right: a device id of 0 is
 * the one value the guest treats as "no device id at all", and the guest
 * then logs "CC: Invalid DeviceId! Have we connected to cloudcell yet?"
 * and falls back to polling /time. The measured run above had exactly
 * that: the device id landed on the wrong word and the guest gave up.
 *
 * The remaining words after the sixth are the SUB-SYNC results. The guest
 * logs the sub-syncs it receives ("CC: SubSync Received: %d") and, in the
 * measured run, the ids it logged were ITS OWN queued sub-syncs (13, 11,
 * 10738, 2, 80, ...), not values read out of the response - so the tail is
 * not needed for the sub-sync handling. The payload is padded with zeroes
 * so a reader that counts entries reads none and a reader that reads a
 * block gets zeroes rather than running off the end.
 */
bool buildSyncResponse(uint32_t memberId, uint32_t deviceId,
                        std::vector<uint8_t> &out)
{
    std::vector<uint8_t> payload;
    payload.reserve(256);
    appendU32(payload, 1u);          /* the head word 0                    */
    appendU32(payload, 0u);          /* the head word 1                    */
    appendU32(payload, memberId);    /* the member id                      */
    appendU32(payload, 0u);          /* the developer flag: this port is   */
                                     /* not a developer account            */
    appendU32(payload, deviceId);    /* THE DEVICE ID                      */
    appendU32(payload, 0u);          /* the server-info value              */
    appendU32(payload, 0u);          /* a bool                             */
    /* The sub-sync results: not modelled, so zero words. */
    for (int i = 0; i < 57; ++i) {
        appendU32(payload, 0u);
    }

    if (!boxPayload(payload, out)) {
        return false;
    }
    return true;
}

} // namespace cccloud
