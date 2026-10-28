//
// A host self-test for the guest's member VALUE store.
//
// Build and run (from the repo root):
//   g++ -std=c++20 -Wall -Wextra -O1 -I native-switch/include
//       native-switch/tests/host/member_value_store_selftest.cpp -o mvs
//   ./mvs
//
// Exit code 0 means every case below held.
//
// WHAT IT CHECKS
// --------------
// The port reads the guest's member values (m_nTotalLaps, m_nTopGameSpeed)
// out of the guest's FOUR-table registry, and the algorithm that does it is
// in native-switch/include/rr3/member_value_store.hpp.
//
// This self-test builds a registry that looks like the guest's -- the same
// four seeds, the same bucket geometry, the same chain nodes, the same 8-byte
// value cells -- inserts a member the way the guest's own write path does,
// and reads it back with the port's lookup.
//
// THE FORMULA IS THE GUEST'S (ELF 0x991d1c):
//
//     value = ~(cell1 ^ cell2)
//
// cell1 comes from TABLE 1 (reg+0x20, seed reg+0x00) and cell2 from TABLE 2
// (reg+0x48, seed reg+0x08).  The port used to compute ~(cell1 ^ cell3) --
// the wrong pair of cells, and the wrong pair of tables -- and to look the
// member up in tables 1 and 3; that is what kept every lookup failing.
//
// TABLES 3 and 4 carry the guest's integrity check:
// fnv1a64(cell1 ^ cell2 ^ cell3) == cell4, with FNV-1a 64-bit over the
// eight little-endian bytes.  The self-test inserts a cell4 that satisfies
// it, so the check must PASS, and it also inserts a member whose cell4 does
// NOT satisfy it, so the check must FAIL while the value still comes out.
//
//   THE POSITIVE CONTROL: the value that goes in is the value that comes out.
//   THE NEGATIVE CONTROL: a member id that was never inserted is NOT found.
//
// All four bucket rules are exercised: table 1 has a count that is NOT a
// power of two (the modulo path) and table 3 has one that is (the masked
// path).
//
#include "rr3/member_value_store.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint64_t kMask = 0xFFFFFFFFFFFFFFFFULL;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok    %s\n", what);
        return;
    }
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
}

// A flat piece of memory with absolute addresses, so the header's reader can
// be handed an address the way the guest's own memory would be.
class Arena {
public:
    explicit Arena(uintptr_t base) : base_(base) {}

    uintptr_t base() const { return base_; }

    uintptr_t alloc(size_t bytes) {
        const uintptr_t address = base_ + data_.size();
        data_.resize(data_.size() + bytes, 0);
        return address;
    }

    void put(uintptr_t address, uint64_t value) {
        if (address < base_ || address - base_ + sizeof(value) > data_.size()) {
            return;
        }
        std::memcpy(&data_[static_cast<size_t>(address - base_)], &value,
                    sizeof(value));
    }

    bool read(uintptr_t address, uint64_t* out) const {
        if (address < base_ ||
            address - base_ + sizeof(uint64_t) > data_.size()) {
            return false;
        }
        std::memcpy(out, &data_[static_cast<size_t>(address - base_)],
                    sizeof(*out));
        return true;
    }

private:
    uintptr_t base_;
    std::vector<uint8_t> data_;
};

// The registry, laid out the way the guest's is: four seeds at +0x00..+0x18
// and four tables at +0x20/+0x48/+0x70/+0x98.
struct Registry {
    static constexpr uintptr_t kBase = 0x10000000ULL;
    Arena arena{kBase};
    uintptr_t base{kBase};
    uint64_t seed0{0x0123456789ABCDEFULL};
    uint64_t seed1{0xFEDCBA9876543210ULL};
    uint64_t seed2{0x0F1E2D3C4B5A6978ULL};
    uint64_t seed3{0x8877665544332211ULL};
    uint64_t count1{5};  // NOT a power of two: the modulo path
    uint64_t count2{8};  // a power of two: the masked path
    uint64_t count3{16}; // a power of two: the masked path
    uint64_t count4{7};  // NOT a power of two: the modulo path
    uintptr_t buckets1{0};
    uintptr_t buckets2{0};
    uintptr_t buckets3{0};
    uintptr_t buckets4{0};
};

uint64_t indexFor(uint64_t count, uint64_t key) {
    uint64_t index = 0;
    rr3::member_value_store::bucketIndex(count, key, &index);
    return index;
}

// Appends a node to the chain of one bucket, the way the guest's insert does.
uintptr_t addNode(Registry& reg, uintptr_t buckets, uint64_t count, uint64_t key,
                  uint64_t cell) {
    const uint64_t index = indexFor(count, key);
    const uintptr_t slot = buckets + index * 8;
    // The chain links NODE to NODE (ELF 0x9920e0: the new node's next word
    // is the previous head NODE), while the SLOT holds the address of a cell
    // whose first word is the head node.  So the previous node has to be read
    // out of that cell, not used as the link itself.
    uint64_t headCell = 0;
    uint64_t head = 0;
    reg.arena.read(slot, &headCell);
    if (headCell != 0) {
        reg.arena.read(headCell, &head);
    }
    const uintptr_t node = reg.arena.alloc(32);
    reg.arena.put(node + rr3::member_value_store::kNodeNextOff, head);
    reg.arena.put(node + rr3::member_value_store::kNodeKeyOff, key);
    reg.arena.put(node + rr3::member_value_store::kNodeHashOff, key);
    const uintptr_t cellPointer = reg.arena.alloc(8);
    reg.arena.put(cellPointer, cell);
    reg.arena.put(node + rr3::member_value_store::kNodeCellOff, cellPointer);
    // THE SLOT HOLDS THE ADDRESS OF A CELL WHOSE FIRST WORD IS THE NODE
    // (ELF 0x992024 takes buckets[index], 0x992070 takes *buckets[index]),
    // not the node itself.
    const uintptr_t slotCell = reg.arena.alloc(8);
    reg.arena.put(slotCell, node);
    reg.arena.put(slot, slotCell);
    return node;
}

// The guest's own write path, for ONE member (ELF 0x992518), modelled on the
// read path's inverse: the value lives in cells 1 and 2, and cells 3 and 4
// carry the integrity check.
//
//     cell1 = ~(minted ^ value)      ; table 1
//     cell2 = minted                  ; table 2
//     => ~(cell1 ^ cell2) == value
//
// `breakIntegrity` writes a cell4 that does NOT match the hash, which is the
// state the guest answers by minting a new id.
void insertMember(Registry& reg, uint64_t memberId, uint64_t minted,
                  int32_t value, bool breakIntegrity = false) {
    const uint64_t key0 = ~(reg.seed0 ^ memberId);
    const uint64_t key1 = ~(reg.seed1 ^ memberId);
    const uint64_t key2 = ~(reg.seed2 ^ memberId);
    const uint64_t key3 = ~(reg.seed3 ^ memberId);
    const uint64_t asU64 = static_cast<uint64_t>(static_cast<int64_t>(value));
    const uint64_t cell1 = ~(minted ^ asU64);
    const uint64_t cell2 = minted;
    const uint64_t cell3 = minted;
    uint64_t cell4 =
        rr3::member_value_store::guestIntegrityHash(cell1, cell2, cell3);
    if (breakIntegrity) {
        cell4 ^= 0x5A5A5A5A5A5A5A5AULL;
    }
    addNode(reg, reg.buckets1, reg.count1, key0, cell1);
    addNode(reg, reg.buckets2, reg.count2, key1, cell2);
    addNode(reg, reg.buckets3, reg.count3, key2, cell3);
    addNode(reg, reg.buckets4, reg.count4, key3, cell4);
}

struct Reader {
    const Registry* reg;
    bool operator()(uintptr_t address, uint64_t* out) const {
        return reg->arena.read(address, out);
    }
};

void build(Registry& reg) {
    // The registry header comes FIRST: the seeds live at its +0x00..+0x18,
    // and the bucket arrays must not be laid over them.
    reg.base = reg.arena.alloc(0x200);
    reg.buckets1 = reg.arena.alloc(8 * reg.count1);
    reg.buckets2 = reg.arena.alloc(8 * reg.count2);
    reg.buckets3 = reg.arena.alloc(8 * reg.count3);
    reg.buckets4 = reg.arena.alloc(8 * reg.count4);
    reg.arena.put(reg.base + rr3::member_value_store::kSeed0Off, reg.seed0);
    reg.arena.put(reg.base + rr3::member_value_store::kSeed1Off, reg.seed1);
    reg.arena.put(reg.base + rr3::member_value_store::kSeed2Off, reg.seed2);
    reg.arena.put(reg.base + rr3::member_value_store::kSeed3Off, reg.seed3);
    reg.arena.put(reg.base + rr3::member_value_store::kTable1Off, reg.buckets1);
    reg.arena.put(reg.base + rr3::member_value_store::kTable1CountOff,
                  reg.count1);
    reg.arena.put(reg.base + rr3::member_value_store::kTable2Off, reg.buckets2);
    reg.arena.put(reg.base + rr3::member_value_store::kTable2CountOff,
                  reg.count2);
    reg.arena.put(reg.base + rr3::member_value_store::kTable3Off, reg.buckets3);
    reg.arena.put(reg.base + rr3::member_value_store::kTable3CountOff,
                  reg.count3);
    reg.arena.put(reg.base + rr3::member_value_store::kTable4Off, reg.buckets4);
    reg.arena.put(reg.base + rr3::member_value_store::kTable4CountOff,
                  reg.count4);
}

}  // namespace

int main() {
    using rr3::member_value_store::Lookup;

    Registry reg;
    build(reg);
    Reader reader{&reg};

    // THE POSITIVE CONTROL: a lap count of 3 comes back as 3.
    const uint64_t lapsId = 0x1122334455667788ULL;
    insertMember(reg, lapsId, 0xAAAAAAAAAAAAAAAAULL, 3);
    Lookup laps{};
    const char* why = rr3::member_value_store::lookup(reader, reg.base, lapsId,
                                                      &laps);
    check(why == nullptr, "the inserted member is found");
    check(laps.ok, "the lookup reports success");
    check(laps.value == 3, "the lap count comes back as 3");
    check(laps.checkRan, "the guest's integrity check ran");
    check(laps.verified, "the guest's integrity check passes");

    // A second member, with a different value and a different id.
    const uint64_t speedId = 0x99AABBCCDDEEFF00ULL;
    insertMember(reg, speedId, 0x5555555555555555ULL, 12345);
    Lookup speed{};
    why = rr3::member_value_store::lookup(reader, reg.base, speedId, &speed);
    check(why == nullptr, "the second member is found");
    check(speed.value == 12345, "the top speed comes back as 12345");

    // A member that shares a bucket with the first one: the chain walk has
    // to find the SECOND one and not stop at the first.
    uint64_t collidingId = 0;
    for (uint64_t candidate = 1; candidate < 100000; ++candidate) {
        const uint64_t key0 = ~(reg.seed0 ^ candidate);
        if (indexFor(reg.count1, key0) == indexFor(reg.count1, ~(reg.seed0 ^ lapsId))) {
            collidingId = candidate;
            break;
        }
    }
    check(collidingId != 0, "a colliding member id was found");
    if (collidingId != 0) {
        insertMember(reg, collidingId, 0x0F0F0F0F0F0F0F0FULL, 42);
        Lookup colliding{};
        why = rr3::member_value_store::lookup(reader, reg.base, collidingId,
                                              &colliding);
        check(why == nullptr, "the colliding member is found");
        check(colliding.value == 42, "the colliding member comes back as 42");
        // And the member it collides with still reads correctly.
        Lookup again{};
        why = rr3::member_value_store::lookup(reader, reg.base, lapsId, &again);
        check(why == nullptr && again.value == 3,
              "the first member still reads 3 after the collision");
    }

    // THE NEGATIVE CONTROL: an id that was never inserted is NOT found.
    Lookup missing{};
    why = rr3::member_value_store::lookup(reader, reg.base, 0xDEADBEEFULL,
                                          &missing);
    check(why != nullptr, "a member that was never inserted is not found");
    check(!missing.ok, "the missing member is not reported as a success");
    // The two ways a walk can fail are now DIFFERENT strings, because the
    // 2026-10-28 audit could not tell them apart and that was the reason the
    // failure stayed unattributed.  Which one this particular case hits
    // depends on the synthetic table's layout, so the test accepts either.
    check(why != nullptr &&
              (std::strcmp(why, "the-chain-ended") == 0 ||
               std::strcmp(why, "the-node-is-in-another-bucket") == 0),
          "the missing member says which way the walk failed");

    // A negative value, because the member is an int32 and a lap count is
    // not the only thing that lives in the registry.
    const uint64_t negativeId = 0xCAFEBABEDEADBEEFULL;
    insertMember(reg, negativeId, 0x1234ULL, -2048);
    Lookup negative{};
    why = rr3::member_value_store::lookup(reader, reg.base, negativeId,
                                          &negative);
    check(why == nullptr && negative.value == -2048,
          "a negative value comes back as it went in");

    // A member whose integrity check does NOT hold: the guest would answer
    // by minting a new id, the port cannot (that would mutate the guest), so
    // the port reports the value AND says the check failed.
    const uint64_t brokenId = 0x1111222233334444ULL;
    insertMember(reg, brokenId, 0x777788889999AAA0ULL, 7, true);
    Lookup broken{};
    why = rr3::member_value_store::lookup(reader, reg.base, brokenId, &broken);
    check(why == nullptr && broken.value == 7,
          "a member with a broken integrity check still reads its value");
    check(broken.checkRan && !broken.verified,
          "the broken integrity check is reported as failed");

    // The key derivation, spelled out: the port's key is the complement of
    // the exclusive-or of the seed and the id.
    check(laps.key0 == ((~((reg.seed0 ^ lapsId))) & kMask),
          "the first key is ~(seed0 ^ id)");
    check(laps.key1 == ((~((reg.seed1 ^ lapsId))) & kMask),
          "the second key is ~(seed1 ^ id)");
    check(laps.key2 == ((~((reg.seed2 ^ lapsId))) & kMask),
          "the third key is ~(seed2 ^ id)");
    check(laps.key3 == ((~((reg.seed3 ^ lapsId))) & kMask),
          "the fourth key is ~(seed3 ^ id)");

    // The FNV-1a basis and prime are the guest's, not a port choice.
    check(rr3::member_value_store::kFnv1a64Basis == 0xcbf29ce484222325ULL,
          "the FNV-1a 64-bit basis is the guest's");
    check(rr3::member_value_store::kFnv1a64Prime == 0x100000001b3ULL,
          "the FNV-1a 64-bit prime is the guest's");

    if (g_failures != 0) {
        std::printf("FAILED: %d case(s) did not hold\n", g_failures);
        return 1;
    }
    std::printf("PASS: the member value store reads back what was written "
                "and refuses what was not\n");
    return 0;
}
