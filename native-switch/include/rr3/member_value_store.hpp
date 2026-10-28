#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// THE GUEST'S MEMBER VALUE STORE.
//
// The guest's registered members ("m_nTotalLaps", "m_nTopGameSpeed", ...) do
// NOT hold their values. A member slot holds a 24-byte DESCRIPTOR:
//
//     slot+0x00  a 64-bit member id
//     slot+0x08  the hash of the member's name
//     slot+0x10  len ^ hash
//
// and the VALUE lives in a global four-table hash registry: a .bss singleton
// at link VA 0x1f97980, reached through the accessor 0xb5741c.
//
// THE READ, DECODED FROM THE GUEST'S OWN CODE
// -------------------------------------------
// The guest's own read path is 0x991d1c (the value) and 0x991fd8 (the bucket
// lookup).  With reg = the registry base (x21) and id = x20.
//
// THE LISTING IS IN ADDRESS ORDER, AND THAT ORDER IS THE WHO ARGUMENT.  The
// eor at 0x991de4 sits BETWEEN table 2's cell load (0x991dcc) and table 3's
// cell load (0x991e00), so x8 still holds table 2's cell when the eor runs.
// An earlier reading of this same code listed the instructions OUT of address
// order, took x8 for table 3's cell, and made the port compute
// ~(cell1 ^ cell3); the address order says otherwise.
//
//     0x991d40  add  x19, x0, #0xc0     ; the registry's LOCK object
//     0x991d54  bl   0x1c76fe4          ; lock
//     0x991d58  ldr  x8, [x21]          ; seed0 = reg+0x00
//     0x991d64  add  x0, x21, #0x20     ; TABLE 1
//     0x991d6c  eon  x8, x8, x20        ; key0 = ~(seed0 ^ id)
//     0x991d88  bl   0x991fd8           ; -> the node of table 1
//     0x991d8c  ldr  x8, [x0, #0x18]    ; the node's value-cell pointer
//     0x991d9c  ldr  x23, [x8]          ; cell1  (x23)
//     0x991d90  ldr  x9, [x21, #8]      ; seed1 = reg+0x08
//     0x991d94  add  x0, x21, #0x48     ; TABLE 2
//     0x991da0  eon  x8, x9, x20        ; key1 = ~(seed1 ^ id)
//     0x991db8  bl   0x991fd8           ; -> the node of table 2
//     0x991dbc  ldr  x8, [x0, #0x18]    ; the node's value-cell pointer
//     0x991dcc  ldr  x8, [x8]           ; cell2  (x8)
//     0x991dc0  ldr  x9, [x21, #0x10]   ; seed2 = reg+0x10
//     0x991dc4  add  x0, x21, #0x70     ; TABLE 3
//     0x991dd0  eon  x9, x9, x20        ; key2 = ~(seed2 ^ id)
//     0x991de4  eor  x25, x8, x23       ; *** x25 = cell2 ^ cell1 ***
//     0x991dec  bl   0x991fd8           ; -> the node of table 3
//     0x991df0  ldr  x8, [x0, #0x18]    ; the node's value-cell pointer
//     0x991e00  ldr  x8, [x8]           ; cell3  (x8)
//     0x991df4  movk x2, ...            ; the FNV-1a basis 0xcbf29ce484222325
//     0x991e10  eor  x8, x8, x25        ; cell3 ^ cell2 ^ cell1
//     0x991e14  str  x8, [sp, #0x10]    ; the eight bytes that are hashed
//     0x991e18  bl   0x15c3640          ; fnv1a64 of those 8 LE bytes
//     0x991e1c  ldr  x8, [x21, #0x18]   ; seed3 = reg+0x18
//     0x991e20  mov  x23, x0            ; the hash moves into x23
//     0x991e24  add  x0, x21, #0x98     ; TABLE 4
//     0x991e2c  eon  x8, x8, x20        ; key3 = ~(seed3 ^ id)
//     0x991e44  bl   0x991fd8           ; -> the node of table 4
//     0x991e48  ldr  x8, [x0, #0x18]    ; the node's value-cell pointer
//     0x991e4c  ldr  x8, [x8]           ; cell4  (x8)
//     0x991e50  cmp  x23, x8            ; the hash == cell4 ?
//     0x991e54  b.eq #0x991e70          ; yes -> done
//     0x991e58  bl   0x991eac            ; no -> mint a NEW id, re-insert
//     0x991e74  bl   0x1c76fec          ; unlock
//     0x991e88  mvn  x0, x25            ; *** THE VALUE = ~(cell1 ^ cell2) ***
//
// THE VALUE IS MADE OF CELLS 1 AND 2; TABLES 3 AND 4 ARE THE INTEGRITY
// CHECK.  The port used to compute ~(cell1 ^ cell3) and to look the member up
// in tables 1 and 3 -- the wrong pair -- which is the bug the 2026-10-28 round
// found and fixed.
//
// TABLES 3 AND 4 ARE THE INTEGRITY CHECK, NOT THE VALUE.  The guest hashes
// cell1 ^ cell2 ^ cell3 and compares the result with the cell in table 4; if
// they differ it MINTS A NEW ID and re-inserts the value (0x991e58), i.e.
// the value it read is one it considers tampered with.  The port does NOT
// re-key (that would mutate the guest), so it reports the value together
// with whether the guest's own check would have passed.
//
// THE CHECK IS ALSO THE PORT'S PROOF.  When the port's four cells make
// fnv1a64(cell1^cell2^cell3) == cell4, the guest's own checksum agrees, and
// that is the strongest statement available about a pure read: the port and
// the guest are looking at the same four cells.  The 2026-10-28 lap
// measurement reported "lapsOk=1" and "lapsCheck=none" for the first time in
// the port's history, and the value was re-derived by hand from the logged
// cells.
//
// THE HASH IS FNV-1a 64-bit.  The guest's hash function (ELF 0x15c3640) is
//
//     0x15c3648  mov  x9, #0x1b3
//     0x15c3650  movk x9, #0x100, lsl #32   ; the prime 0x100000001b3
//     0x15c3654  ldrb w10, [x0], #1
//     0x15c3658  eor  x10, x2, x10           ; h = h ^ byte
//     0x15c3660  mul  x2, x10, x9            ; h = h * prime
//
// with the offset basis 0xcbf29ce484222325 built at 0x991df4-0x991e0c, which
// is the standard FNV-1a 64-bit basis.  So the check is reproducible.
//
// WHY THE SLOT CANNOT BE READ AS A NUMBER
// ----------------------------------------
// The accessor 0x991c8c does not merely read a member: it RE-KEYS it. It
// looks the value up, mints a NEW id, re-inserts the value under the new id,
// supersedes the old one and stores the new id back into the slot:
//
//     0x991ca8  ldr  x1, [x19]     ; the old id
//     0x991cb0  bl   0x991d1c     ; the value, looked up under the old id
//     0x991cc0  bl   0x1c76fe4    ; lock the registry
//     0x991cc4  bl   0x991eac     ; mint a NEW id
//     0x991cd4  bl   0x992b40     ; is the new id free?
//     0x991ce8  bl   0x992518     ; insert the same value under the new id
//     0x991cf4  bl   0x9926c4     ; supersede the old id
//     0x991cfc  str  x23, [x19]   ; slot+0x00 = the new id
//     0x991d00  bl   0x1c76fec    ; unlock
//
// So the 64-bit word in the slot is a ROTATING TOKEN, never a stable handle,
// and a probe that calls the accessor mutates the guest's state. This header
// therefore implements the PURE READ: no guest call, no mutation, and the id
// is read fresh every time.
//
// ALSO NOTE: the "+0xc0" of this registry is its LOCK OBJECT, not an "id
// map" (0x991d54 locks it, 0x991e74 unlocks it).  The earlier comment in
// race_state_probe.cpp called it an id map; it is not one, and the id map and
// the value table are not the same thing.
//
// The bucket lookup (0x991fd8) works like this, in address order:
//
//     0x991fec  ldr  x22, [x0, #8]     ; the bucket count, at table+8
//     0x991ffc  cbz  x22, #0x992030    ; no buckets -> not found
//     0x992004  cnt  v0.8b, v0.8b      ; popcount of the count
//     0x992010  cmp  x8, #2
//     0x992014  b.hs #0x992034        ; popcount >= 2 -> modulo
//     0x992018  ldr  x8, [x19]         ; the bucket array
//     0x99201c  sub  x9, x22, #1
//     0x992020  and  x24, x9, x23     ; index = key & (count - 1)
//     0x992024  ldr  x20, [x8, x24, lsl #3] ; the chain head of that bucket
//     ...
//     0x992034  cmp  x23, x22          ; the modulo path
//     0x992040  udiv x9, x23, x22
//     0x992044  msub x24, x9, x22, x23 ; index = key % count
//     0x99204c  ldr  x20, [x9, x24, lsl #3] ; the chain head of that bucket
//
// and the chain node is 32 bytes, which the guest's own INSERT path writes
// out (0x9920a4 stp xzr, x23, [x0] and 0x9920a8 stp x8, xzr, [x0, #0x10]):
//
//     node+0x00  the next node
//     node+0x08  the key
//     node+0x10  the key again (the guest compares it too)
//     node+0x18  a pointer to the 8-byte value cell
//
// BUT THE BUCKET SLOT IS NOT THE NODE.  The guest does
//
//     0x992024  ldr  x20, [x8, x24, lsl #3]   ; x20 = buckets[index]
//     0x992070  ldr  x20, [x20]                ; x20 = *buckets[index]
//
// so the slot holds the ADDRESS of a cell whose first word is the node.
//
// TWO INDEPENDENT PLACES IN THE GUEST SAY SO.  The lookup path above reads
// node+0x08 and node+0x18 AFTER that extra indirection, so *buckets[index]
// must be the node.  And the insert path agrees:
//
//     0x9920d4  ldr  x8, [x9, x24, lsl #3]    ; x8 = buckets[index]
//     0x9920dc  ldr  x9, [x8]                 ; x9 = *buckets[index]
//     0x9920e0  str  x9, [x20]               ; the NEW node's next
//
// the new node's next field has to become the OLD HEAD NODE, and only
// *buckets[index] can be that.  If the slot held the node itself, [x8] would
// be the old head's NEXT pointer and the chain would lose its head.
//
// THE SAME INSTRUCTION SERVES BOTH ROLES.  0x992070 is "slot -> node" on the
// first turn of the walk and "node -> next node" on every turn after it,
// which is why the extra indirection is easy to miss: it looks like the chain
// step.
//
// ONE SMALL DIVERGENCE FROM THE GUEST, AND IT IS BENIGN.  The guest's found
// condition is node+0x10 == key; the guest's node+0x08 is a fast path that
// only decides whether to compare node+0x10.  This port accepts a match on
// EITHER copy.  The guest's own insert path writes the same key into both,
// so the two copies cannot disagree in a live guest.
// Treating the slot as the node -- which is what this port used to do --
// made every key read garbage and every lookup stop with
// "the-node-is-in-another-bucket": the 2026-10-28 measurement, where seven
// candidate slots and two independent probes all failed the same way.
//
// The guest also STOPS the walk as soon as a node belongs to a different
// bucket (0x992084 for the masked path, 0x992110 for the modulo path), and
// this header stops there too.
//
// THE READS ARE NOT DONE HERE
// ---------------------------
// This header never touches guest memory itself: every read goes through the
// caller's reader, which is the port's safe-read path (the plausibility gate,
// then the guest-image check, then svcQueryMemory, then the read). A
// diagnostic that can fault the run it is measuring is worse than no
// diagnostic.
// ---------------------------------------------------------------------------
namespace rr3 {
namespace member_value_store {

// The registry, in the guest's .bss (a link-time VA; the runtime address is
// bias + this).
constexpr uintptr_t kRegistryBssVa = 0x1F97980ULL;
// The four hash seeds, one per table.
constexpr uintptr_t kSeed0Off = 0x00ULL;  // table 1
constexpr uintptr_t kSeed1Off = 0x08ULL;  // table 2
constexpr uintptr_t kSeed2Off = 0x10ULL;  // table 3
constexpr uintptr_t kSeed3Off = 0x18ULL;  // table 4
// The four tables: the bucket array and its count.
constexpr uintptr_t kTable1Off = 0x20ULL;
constexpr uintptr_t kTable1CountOff = 0x28ULL;
constexpr uintptr_t kTable2Off = 0x48ULL;
constexpr uintptr_t kTable2CountOff = 0x50ULL;
constexpr uintptr_t kTable3Off = 0x70ULL;
constexpr uintptr_t kTable3CountOff = 0x78ULL;
constexpr uintptr_t kTable4Off = 0x98ULL;
constexpr uintptr_t kTable4CountOff = 0xA0ULL;
// The chain node.
constexpr uintptr_t kNodeNextOff = 0x00ULL;
constexpr uintptr_t kNodeKeyOff = 0x08ULL;
constexpr uintptr_t kNodeHashOff = 0x10ULL;
constexpr uintptr_t kNodeCellOff = 0x18ULL;
// How many chain steps the port is willing to walk. The guest's own chains
// are short (a hash table with a load factor of 1.0); the bound only exists
// so that a corrupt pointer cannot turn the probe into a hang.
constexpr int kMaxChainSteps = 64;
// FNV-1a 64-bit, the guest's own hash (ELF 0x15c3640) and the basis it is
// called with (0x991df4-0x991e0c).
constexpr uint64_t kFnv1a64Basis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnv1a64Prime = 0x100000001b3ULL;

// What one lookup found. Every piece of it is reported, so that a lookup
// which goes wrong can be debugged from the log alone.
struct Lookup {
    bool ok{false};
    const char* reason{nullptr};  // why it failed, when it failed
    uint64_t id{0};
    uint64_t key0{0};
    uint64_t key1{0};
    uint64_t key2{0};
    uint64_t key3{0};
    uint64_t node1{0};  // the chain node in table 1
    uint64_t node2{0};  // the chain node in table 2
    uint64_t node3{0};  // the chain node in table 3
    uint64_t node4{0};  // the chain node in table 4
    uint64_t cell1{0};  // the 8-byte cell in table 1
    uint64_t cell2{0};  // the 8-byte cell in table 2
    uint64_t cell3{0};  // the 8-byte cell in table 3
    uint64_t cell4{0};  // the 8-byte cell in table 4
    int32_t value{0};
    // The guest's own integrity check: fnv1a64(cell1^cell2^cell3) == cell4.
    // The guest re-keys the member when it fails; the port cannot (that would
    // mutate the guest), so it reports the verdict instead.
    bool checkRan{false};
    bool verified{false};
    const char* checkReason{nullptr};
};

// The guest's own bucket index (ELF 0x991fd8).  The dispatch is by
// POPCOUNT, not by "is it a power of two":
//
//     0x99200c  fmov w8, s0           ; popcount(count)
//     0x992010  cmp  x8, #2
//     0x992014  b.hs #0x992034        ; popcount >= 2 -> modulo
//     0x99201c  sub  x9, x22, #1
//     0x992020  and  x24, x9, x23     ; index = key & (count - 1)
//     0x992040  udiv x9, x23, x22     ; index = key % count
//
// The two forms agree for every count this port has ever seen, but the
// guest's form is the one that decides, so it is the one that is written
// down here: a count of 1 is a mask (index 0) and a count of 3 is a modulo.
inline bool bucketIndex(uint64_t count, uint64_t key, uint64_t* index) {
    if (count == 0 || index == nullptr) {
        return false;
    }
    unsigned bits = 0;
    for (uint64_t c = count; c != 0; c &= (c - 1)) {
        ++bits;
    }
    if (bits < 2) {
        *index = key & (count - 1);
        return true;
    }
    *index = key % count;
    return true;
}

// The guest's own decode (ELF 0x991de4 / 0x991e88): the value is the
// complement of the exclusive-or of cell1 and cell2.
//
//     0x991de4  eor x25, x8, x23   ; x25 = cell2 ^ cell1
//     0x991e88  mvn x0, x25         ; the value is ~(cell1 ^ cell2)
//
// x8 holds the cell of TABLE 2 (reg+0x48) and x23 the cell of TABLE 1
// (reg+0x20).  The instruction at 0x991de4 EXECUTES BEFORE the table-3
// lookup at 0x991dec, so x8 still holds table 2's cell at that point.  An
// earlier reading of the same instruction took x8 for table 3's cell and
// made the port compute ~(cell1 ^ cell3); the order of the instructions
// says otherwise, and the fix is to look the member up in tables 1 and 2.
inline int32_t decodeValue(uint64_t cell1, uint64_t cell2) {
    const uint64_t mixed = cell1 ^ cell2;
    return static_cast<int32_t>(static_cast<uint32_t>(~mixed));
}

// FNV-1a 64-bit, byte for byte what ELF 0x15c3640 does.
inline uint64_t fnv1a64(const uint8_t* data, size_t length, uint64_t basis) {
    uint64_t hash = basis;
    if (data == nullptr) {
        return hash;
    }
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

// The guest hashes the EIGHT LITTLE-ENDIAN BYTES of (cell1 ^ cell2 ^ cell3)
// (ELF 0x991e10 stores the word at sp+0x10 and calls the hash with w1 = 8).
inline uint64_t guestIntegrityHash(uint64_t cell1, uint64_t cell2, uint64_t cell3) {
    const uint64_t mixed = cell1 ^ cell2 ^ cell3;
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((mixed >> (i * 8)) & 0xFF);
    }
    return fnv1a64(bytes, sizeof(bytes), kFnv1a64Basis);
}

// Walks ONE bucket chain the way the guest's own lookup helper does.
template <typename Read>
const char* findChainNode(Read& read, uint64_t buckets, uint64_t count,
                           uint64_t key, uint64_t* nodeOut, uint64_t* indexOut) {
    uint64_t index = 0;
    if (!bucketIndex(count, key, &index)) {
        return "the-table-has-no-buckets";
    }
    if (indexOut != nullptr) {
        *indexOut = index;
    }
    if (buckets == 0) {
        return "the-bucket-array-is-null";
    }
    uint64_t cell = 0;
    if (!read(buckets + index * 8, &cell)) {
        return "the-bucket-is-unreadable";
    }
    // THE SLOT IS NOT THE NODE: it holds the address of a cell whose first
    // word is the node (ELF 0x992024 then 0x992070).  One indirection more
    // than this port used to do.
    uint64_t node = 0;
    if (cell == 0) {
        return "the-chain-ended";
    }
    if (!read(cell, &node)) {
        return "the-bucket-cell-is-unreadable";
    }
    for (int step = 0; step < kMaxChainSteps; ++step) {
        if (node == 0) {
            // The chain ran out.  This is a DIFFERENT failure from the one
            // below and the port used to report both as the same string,
            // which is what made the 2026-10-28 audit unable to say why a
            // lookup failed.
            return "the-chain-ended";
        }
        uint64_t nodeKey = 0;
        if (!read(node + kNodeKeyOff, &nodeKey)) {
            return "the-chain-node-is-unreadable";
        }
        if (nodeKey == key) {
            if (nodeOut != nullptr) {
                *nodeOut = node;
            }
            return nullptr;
        }
        // The guest compares the node's SECOND copy of the key too (ELF
        // 0x992064), so a node that matches there matches here.
        uint64_t nodeHash = 0;
        if (read(node + kNodeHashOff, &nodeHash) && nodeHash == key) {
            if (nodeOut != nullptr) {
                *nodeOut = node;
            }
            return nullptr;
        }
        // The guest stops the walk at the first node that belongs to a
        // DIFFERENT bucket, so the port stops there too.  "Belongs to a
        // different bucket" means: the index that THIS table's rule gives
        // for the node's own key is not the index being walked.
        uint64_t nodeIndex = 0;
        if (bucketIndex(count, nodeKey, &nodeIndex) && nodeIndex != index) {
            return "the-node-is-in-another-bucket";
        }
        uint64_t next = 0;
        if (!read(node + kNodeNextOff, &next)) {
            return "the-chain-node-is-unreadable";
        }
        if (next == node) {
            return "the-chain-links-to-itself";
        }
        node = next;
    }
    return "the-chain-is-too-long";
}

// Reads one table's cell: the node's value-cell pointer, then the cell.
template <typename Read>
const char* readCell(Read& read, uint64_t node, uint64_t* cellOut) {
    uint64_t cellPointer = 0;
    if (!read(node + kNodeCellOff, &cellPointer)) {
        return "the-value-cell-is-unreadable";
    }
    if (cellPointer == 0) {
        return "the-value-cell-is-null";
    }
    if (!read(cellPointer, cellOut)) {
        return "the-value-is-unreadable";
    }
    return nullptr;
}

// The guest's own read path (ELF 0x991d1c), without the guest call.
//
// `read` is the port's safe read: it takes a guest address and gives back the
// 64-bit word there, or false when that address cannot be read.
template <typename Read>
const char* lookup(Read& read, uintptr_t registry, uint64_t id, Lookup* out) {
    if (out == nullptr) {
        return "there-is-no-output";
    }
    out->id = id;

    uint64_t seeds[4] = {0, 0, 0, 0};
    const uintptr_t seedOffsets[4] = {kSeed0Off, kSeed1Off, kSeed2Off, kSeed3Off};
    for (int i = 0; i < 4; ++i) {
        if (!read(registry + seedOffsets[i], &seeds[i])) {
            return "a-seed-is-unreadable";
        }
    }
    out->key0 = ~(seeds[0] ^ id);
    out->key1 = ~(seeds[1] ^ id);
    out->key2 = ~(seeds[2] ^ id);
    out->key3 = ~(seeds[3] ^ id);

    uint64_t buckets[4] = {0, 0, 0, 0};
    uint64_t counts[4] = {0, 0, 0, 0};
    const uintptr_t tableOffsets[4] = {kTable1Off, kTable2Off, kTable3Off, kTable4Off};
    const uintptr_t countOffsets[4] = {kTable1CountOff, kTable2CountOff,
                                       kTable3CountOff, kTable4CountOff};
    for (int i = 0; i < 4; ++i) {
        if (!read(registry + tableOffsets[i], &buckets[i])) {
            return "a-table-is-unreadable";
        }
        if (!read(registry + countOffsets[i], &counts[i])) {
            return "a-table-count-is-unreadable";
        }
    }

    // THE VALUE IS MADE OF CELLS 1 AND 2 (tables 1 and 2); the INTEGRITY
    // CHECK is made of CELLS 3 AND 4 (tables 3 and 4).  The guest reads the
    // four tables in that order -- 0x991d88 (table 1), 0x991db8 (table 2),
    // 0x991dec (table 3), 0x991e44 (table 4) -- assembles the value from
    // the first two (0x991de4), feeds the first three to the hash
    // (0x991e10) and compares the result with the fourth (0x991e50).
    uint64_t index1 = 0;
    uint64_t index2 = 0;
    const char* why = findChainNode(read, buckets[0], counts[0], out->key0,
                                    &out->node1, &index1);
    if (why != nullptr) {
        return why;
    }
    why = findChainNode(read, buckets[1], counts[1], out->key1, &out->node2,
                         &index2);
    if (why != nullptr) {
        return why;
    }
    why = readCell(read, out->node1, &out->cell1);
    if (why != nullptr) {
        return why;
    }
    why = readCell(read, out->node2, &out->cell2);
    if (why != nullptr) {
        return why;
    }

    out->value = decodeValue(out->cell1, out->cell2);

    // TABLES 3 and 4 carry the integrity check.  They are best effort: the
    // value is already read, and the guest would have re-keyed the member
    // rather than refused to answer.
    uint64_t index3 = 0;
    uint64_t index4 = 0;
    const char* why3 = findChainNode(read, buckets[2], counts[2], out->key2,
                                     &out->node3, &index3);
    const char* why4 = findChainNode(read, buckets[3], counts[3], out->key3,
                                     &out->node4, &index4);
    if (why3 == nullptr) {
        why3 = readCell(read, out->node3, &out->cell3);
    }
    if (why4 == nullptr) {
        why4 = readCell(read, out->node4, &out->cell4);
    }
    out->checkRan = (why3 == nullptr) && (why4 == nullptr);
    if (!out->checkRan) {
        out->checkReason = (why3 != nullptr) ? why3 : why4;
        out->verified = false;
    } else {
        out->verified =
            (guestIntegrityHash(out->cell1, out->cell2, out->cell3) == out->cell4);
        out->checkReason = out->verified ? "none" : "the-integrity-check-did-not-match";
    }

    out->ok = true;
    return nullptr;
}

}  // namespace member_value_store
}  // namespace rr3
