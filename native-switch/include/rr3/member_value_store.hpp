#pragma once

#include <cstdint>

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
// therefore implements the PURE READ (option B of the write-up): no guest call,
// no mutation, and the id is read fresh every time.
//
// THE READ, DECODED FROM THE GUEST'S OWN CODE
// -------------------------------------------
// The guest's own read path is 0x991d1c (the value) and 0x991fd8 (the bucket
// lookup). With reg = the registry base:
//
//     0x991d58  ldr  x8, [x21]        ; seed0 = reg+0x00
//     0x991d6c  eon  x8, x8, x20      ; key0 = ~(seed0 ^ id)
//     0x991d64  add  x0, x21, #0x20   ; table 1
//     0x991d88  bl   0x991fd8         ; -> the node
//     0x991d8c  ldr  x8, [x0, #0x18]  ; the node's value-cell pointer
//     0x991d9c  ldr  x23, [x8]        ; cell1
//     0x991d90  ldr  x9, [x21, #8]    ; seed1 = reg+0x08
//     0x991da0  eon  x8, x9, x20      ; key1 = ~(seed1 ^ id)
//     0x991d94  add  x0, x21, #0x48   ; table 2
//     0x991db8  bl   0x991fd8         ; -> the node
//     0x991dcc  ldr  x8, [x8]         ; cell2
//     0x991de4  eor  x25, x8, x23     ; cell1 ^ cell2
//     0x991e88  mvn  x0, x25          ; the value = ~(cell1 ^ cell2)
//
// The bucket lookup (0x991fd8) works like this:
//
//     0x991fec  ldr  x22, [x0, #8]     ; the bucket count, at table+8
//     0x99200c  fmov w8, s0           ; popcount(count)
//     0x992010  cmp  x8, #2
//     0x992014  b.hs #0x992034        ; not a power of two -> modulo
//     0x99201c  sub  x9, x22, #1
//     0x992020  and  x24, x9, x23     ; index = key & (count - 1)
//     0x992040  udiv x9, x23, x22
//     0x992044  msub x24, x9, x22, x23 ; index = key % count
//     0x992024  ldr  x20, [x8, x24, lsl #3] ; the chain head of that bucket
//
// and the chain node is 32 bytes:
//
//     node+0x00  the next node
//     node+0x08  the key
//     node+0x10  the key again (the guest compares it too)
//     node+0x18  a pointer to the 8-byte value cell
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
// The two hash seeds the lookup keys are derived from.
constexpr uintptr_t kSeed0Off = 0x00ULL;
constexpr uintptr_t kSeed1Off = 0x08ULL;
// The two tables the VALUE is read from: the bucket array and its count.
constexpr uintptr_t kTable1Off = 0x20ULL;
constexpr uintptr_t kTable1CountOff = 0x28ULL;
constexpr uintptr_t kTable2Off = 0x48ULL;
constexpr uintptr_t kTable2CountOff = 0x50ULL;
// The chain node.
constexpr uintptr_t kNodeNextOff = 0x00ULL;
constexpr uintptr_t kNodeKeyOff = 0x08ULL;
constexpr uintptr_t kNodeHashOff = 0x10ULL;
constexpr uintptr_t kNodeCellOff = 0x18ULL;
// How many chain steps the port is willing to walk. The guest's own chains
// are short (a hash table with a load factor of 1.0); the bound only exists
// so that a corrupt pointer cannot turn the probe into a hang.
constexpr int kMaxChainSteps = 64;

// What one lookup found. Every piece of it is reported, so that a lookup
// which goes wrong can be debugged from the log alone.
struct Lookup {
    bool ok{false};
    const char* reason{nullptr};  // why it failed, when it failed
    uint64_t id{0};
    uint64_t key0{0};
    uint64_t key1{0};
    uint64_t node1{0};  // the chain node in table 1
    uint64_t node2{0};  // the chain node in table 2
    uint64_t cell1{0};  // the 8-byte cell in table 1
    uint64_t cell2{0};  // the 8-byte cell in table 2
    int32_t value{0};
};

// The guest's own bucket index (ELF 0x991fd8). The count decides: a power of
// two is masked, anything else is a modulo.
inline bool bucketIndex(uint64_t count, uint64_t key, uint64_t* index) {
    if (count == 0 || index == nullptr) {
        return false;
    }
    if ((count & (count - 1)) == 0) {
        *index = key & (count - 1);
        return true;
    }
    *index = key % count;
    return true;
}

// The guest's own decode (ELF 0x991de4 / 0x991e88): the value is the
// complement of the exclusive-or of the two cells.
inline int32_t decodeValue(uint64_t cell1, uint64_t cell2) {
    const uint64_t mixed = cell1 ^ cell2;
    return static_cast<int32_t>(static_cast<uint32_t>(~mixed));
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
    uint64_t node = 0;
    if (!read(buckets + index * 8, &node)) {
        return "the-bucket-is-unreadable";
    }
    for (int step = 0; step < kMaxChainSteps; ++step) {
        if (node == 0) {
            return "the-key-is-not-in-the-table";
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
        // DIFFERENT bucket, so the port stops there too.
        uint64_t nodeIndex = 0;
        if (bucketIndex(count, nodeKey, &nodeIndex) && nodeIndex != index) {
            return "the-key-is-not-in-the-table";
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
    uint64_t seed0 = 0;
    if (!read(registry + kSeed0Off, &seed0)) {
        return "the-first-seed-is-unreadable";
    }
    uint64_t seed1 = 0;
    if (!read(registry + kSeed1Off, &seed1)) {
        return "the-second-seed-is-unreadable";
    }
    out->key0 = ~(seed0 ^ id);
    out->key1 = ~(seed1 ^ id);

    uint64_t buckets1 = 0;
    uint64_t count1 = 0;
    uint64_t buckets2 = 0;
    uint64_t count2 = 0;
    if (!read(registry + kTable1Off, &buckets1)) {
        return "the-first-table-is-unreadable";
    }
    if (!read(registry + kTable1CountOff, &count1)) {
        return "the-first-table-count-is-unreadable";
    }
    if (!read(registry + kTable2Off, &buckets2)) {
        return "the-second-table-is-unreadable";
    }
    if (!read(registry + kTable2CountOff, &count2)) {
        return "the-second-table-count-is-unreadable";
    }

    uint64_t index1 = 0;
    uint64_t index2 = 0;
    const char* why = findChainNode(read, buckets1, count1, out->key0,
                                     &out->node1, &index1);
    if (why != nullptr) {
        return why;
    }
    why = findChainNode(read, buckets2, count2, out->key1, &out->node2, &index2);
    if (why != nullptr) {
        return why;
    }

    uint64_t cellPointer1 = 0;
    if (!read(out->node1 + kNodeCellOff, &cellPointer1)) {
        return "the-first-value-cell-is-unreadable";
    }
    if (cellPointer1 == 0) {
        return "the-first-value-cell-is-null";
    }
    if (!read(cellPointer1, &out->cell1)) {
        return "the-first-value-is-unreadable";
    }
    uint64_t cellPointer2 = 0;
    if (!read(out->node2 + kNodeCellOff, &cellPointer2)) {
        return "the-second-value-cell-is-unreadable";
    }
    if (cellPointer2 == 0) {
        return "the-second-value-cell-is-null";
    }
    if (!read(cellPointer2, &out->cell2)) {
        return "the-second-value-is-unreadable";
    }

    out->value = decodeValue(out->cell1, out->cell2);
    out->ok = true;
    return nullptr;
}

}  // namespace member_value_store
}  // namespace rr3
