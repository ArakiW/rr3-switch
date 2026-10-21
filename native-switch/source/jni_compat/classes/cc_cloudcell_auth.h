#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/*
 * The Cloudcell LOGIN (authenticate) response, hand-built for the port.
 *
 * The guest is an Android ARM64 game (libRealRacing3.so) that talks to
 * Cloudcell over a custom binary protocol. Every request fails today, so the
 * guest's account manager never receives a server-issued device id and the
 * sync queue never drains ("CC: Invalid DeviceId! Have we connected to
 * cloudcell yet?").
 *
 * Everything in this file is measured from the guest image
 * (reverse/native/arm64/libRealRacing3.so) and the addresses below are the
 * guest's own virtual addresses (the same space the port's other Cloudcell
 * notes use).
 *
 * The wire format is a custom binary blob, not JSON:
 *
 *   BinaryBlob box (writer 0x1cbd66c, reader 0x1cbd8f0), little-endian:
 *       0x00  u32  checksum over the payload bytes [8 .. end)
 *       0x04  u32  mode: 0 = plain, 1 = zlib, 2 = XOR keystream
 *       0x08  ...  payload
 *   There is no length field in the box; the reader takes the total length
 *   from the blob's own length.
 *
 *   mode 0: the raw bytes
 *   mode 1: u32 LE uncompressed_length, then a zlib stream (the guest calls
 *           uncompress() at 0x15c0b18, so this is RFC1950, not raw DEFLATE)
 *   mode 2: u8 key, then C[i] = P[i] ^ ((key + 3*i) & 0xFF)
 *
 * The game SENDS BoxData(1) then BoxData(2) - zlib inside XOR - and the
 * response is unboxed TWICE (0x1d01380 and 0x1d0138c), so a response body is
 * built as:
 *
 *   inner_payload = u32le(len(payload)) + zlib.compress(payload, 9)
 *   inner_box     = u32le(checksum(inner_payload)) + u32le(1) + inner_payload
 *   outer_payload = bytes([key] + [inner_box[i] ^ ((key + 3*i) & 0xFF)])
 *   outer_box     = u32le(checksum(outer_payload)) + u32le(2) + outer_payload
 *   HTTP body     = outer_box
 */
namespace cccloud {

/*
 * What the port learned about the request the guest just posted.
 *
 * The name says "auth" because the first shape the port had to recognise was
 * the authenticate request. The struct now carries the whole request: the
 * two Cloudcell request kinds (authenticate and sync) share a path, a tag
 * and a prefix, and the port has to tell them apart to answer each with the
 * shape its reader expects.
 */
struct AuthRequest {
    bool     authShaped = false;   /* the body has the authenticate shape    */
    bool     hasMemberId = false;  /* the body carried a member id          */
    uint32_t memberId    = 0;
    uint32_t env1        = 0;
    uint32_t env2        = 0;
    uint32_t clientId    = 0;
    size_t   headerCount = 0;      /* header strings seen (the guest sends 7) */
    size_t   consumed    = 0;      /* bytes the shape consumed               */
    size_t   bodySize    = 0;

    /* The box the body arrived in. */
    bool     boxed       = false;  /* the body was a BinaryBlob box            */
    uint32_t boxMode     = 0;      /* the box's mode (0 plain, 1 zlib, 2 XOR) */
    uint32_t boxChecksum = 0;      /* the box's checksum word                 */
    size_t   payloadSize = 0;      /* bytes the unboxed payload holds         */
    size_t   tailWords   = 0;      /* words the payload holds after the head  */
};

/* The two Cloudcell request kinds the port has to tell apart. */
enum class RequestKind {
    Unknown = 0,   /* the body did not parse as either                     */
    Authenticate,  /* the authenticate request: answered with the login    */
    Sync,          /* the ordinary sync request: answered with sync data   */
};

/*
 * True when the posted body is the AUTHENTICATE request rather than the sync
 * request. The two share the same path (CC_Sync.php) and the same first four
 * words, so the shape of the tail is the discriminator.
 */
bool parseAuthRequest(const uint8_t *body, size_t size, AuthRequest &out);

/*
 * The request the guest just posted, classified.
 *
 * The body the guest posts is a BinaryBlob BOX, not a bare payload: the
 * guest packs BoxData(1) (zlib) and then BoxData(2) (XOR) before it
 * writes, so the bytes on the wire are
 *
 *     [u32 checksum][u32 mode=2][u8 key][ciphertext]
 *     ciphertext = [u32 checksum][u32 mode=1][u32 len][zlib stream]
 *
 * and the payload inside is what the request writer built (the tag, the
 * environment words, the header strings, the tail).
 *
 * The port used to parse the WIRE bytes as if they were the payload, so the
 * very first word it read was the box's checksum instead of the tag, every
 * parse failed, and the port fell back to matching the URL - which made it
 * answer the ordinary SYNC request with the authenticate response.
 *
 * parseRequest unboxes the body first and then classifies the payload.
 */
RequestKind parseRequest(const uint8_t *body, size_t size, AuthRequest &out);

/*
 * Unbox a BinaryBlob the way the guest's own reader does
 * (guest raw 0x1cbd8f0, UnboxData), following the box chain until a payload
 * is reached. Returns false when the chain is malformed, when a checksum
 * does not match, or when the chain is longer than the guest's two unbox
 * steps.
 */
bool unboxBlob(const uint8_t *data, size_t size, std::vector<uint8_t> &out);

/*
 * Build the HTTP response body for
 * AUTHENTICATE_STATE_AUTHENTICATE_SUCCESS (state 2 of the guest's table at
 * 0x1f66a68).
 */
bool buildAuthResponse(uint32_t memberId, std::vector<uint8_t> &out);

/*
 * Build the HTTP response body for the ordinary SYNC request.
 *
 * The shape is the guest's own SYNC-response reader (guest raw 0x1cfb784),
 * which reads, in this order:
 *
 *     u32  member id      compared with the one the account manager holds
 *     u32  is developer   a bool; the guest asserts the value is < 2
 *     u32  DEVICE ID     set on the guest's server info, and logged
 *                         ("CC: Device Id: %d")
 *     u32  a value       set on the guest's server info
 *     u32  a bool        asserted < 2
 *
 * The member id and the device id are the port's to choose: the port is
 * standing in for a server, and a device id of 0 is the one value the guest
 * treats as "no device id at all" ("CC: Invalid DeviceId! Have we connected
 * to cloudcell yet?").
 */
bool buildSyncResponse(uint32_t memberId, uint32_t deviceId,
                        std::vector<uint8_t> &out);

} // namespace cccloud
