#!/usr/bin/env python3
"""The race-metrics value probe's claims, checked against the guest binary.

The port reads the guest's lap count and top speed out of the guest's own
member VALUE store: a four-table hash registry in the guest's .bss, reached
through the accessor 0xb5741c. That mechanism was decoded from the guest's
own code, so THIS TEST is the oracle check: every address and every offset
the probe uses is re-derived here, from
reverse/native/arm64/libRealRacing3.so, and compared with the constants the
port actually ships.

What is checked:
  1. The registry's address, its two seeds, its two tables and the chain
     node layout, re-derived from the guest's own accessor (0xb5741c), its
     own read path (0x991d1c) and its own bucket lookup (0x991fd8).
  2. The RuleSet_RaceMetrics member slots (+0x60 and +0xa8), re-derived from
     the guest's own telemetry function (0xb35800), and the class's vtable
     address point, through the guest's own Itanium RTTI.
  3. The port's source carries those SAME values, so the source and the
     binary cannot drift apart silently.
  4. The probe never CALLS the guest's member accessor (0x991c8c), which
     re-keys the member and would mutate the guest from a read-only hook.
  5. Every guest read goes through the port's safe-read helper: there is no
     direct dereference of a guest pointer anywhere in the new code.
  6. The lookup itself, against a synthetic registry built HERE: a value
     that goes in comes out (the POSITIVE control) and a member that was
     never inserted is NOT found (the NEGATIVE control). The same header is
     also run as a compiled self-test when a host compiler is available.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"
PROBE = ROOT / "source" / "race_metrics_probe.cpp"
PROBE_HEADER = ROOT / "include" / "rr3" / "race_metrics_probe.hpp"
STORE = ROOT / "include" / "rr3" / "member_value_store.hpp"
STATE = ROOT / "source" / "race_state_probe.cpp"
STATE_HEADER = ROOT / "include" / "rr3" / "race_state_probe.hpp"
HOST_SELFTEST = ROOT / "tests" / "host" / "member_value_store_selftest.cpp"

R_AARCH64_RELATIVE = 1027

# The guest's member accessor. Calling it MUTATES the guest: it re-keys the
# member on every read. The probe must not call it.
GUEST_MEMBER_ACCESSOR = 0x991C8C


class Elf:
    def __init__(self, path):
        self.blob = path.read_bytes()
        self.progs = []
        self._parse()

    def _parse(self):
        b = self.blob
        e_phoff, = struct.unpack_from("<Q", b, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, _fl, p_offset, p_vaddr, _pa, p_filesz, _ms, _ = \
                struct.unpack_from("<IIQQQQQQ", b, off)
            self.progs.append((p_type, p_offset, p_vaddr, p_filesz))

    def va_to_off(self, va):
        for (p_type, p_offset, p_vaddr, p_filesz) in self.progs:
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
        return None

    def section(self, name):
        b = self.blob
        e_shoff, = struct.unpack_from("<Q", b, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", b, 0x3A)
        sh = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, _fl, sh_addr, sh_offset, sh_size,
             _lk, _inf, _al, _es) = struct.unpack_from("<IIQQQQIIQQ", b, off)
            sh.append((sh_name, sh_type, sh_addr, sh_offset, sh_size))
        strtab_off = sh[e_shstrndx][3]
        for (sh_name, sh_type, sh_addr, sh_offset, sh_size) in sh:
            nm = b[strtab_off + sh_name:].split(b"\x00")[0].decode("ascii", "replace")
            if nm == name:
                return (sh_addr, sh_offset, sh_size)
        return None


def relative_relocs(elf):
    rela = elf.section(".rela.dyn")
    out = {}
    if rela is None:
        return out
    _addr, offset, size = rela
    blob = elf.blob
    for i in range(size // 24):
        off = offset + i * 24
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
        if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
            out[r_offset] = r_addend
    return out


def cstring(elf, va):
    off = elf.va_to_off(va)
    if off is None:
        return None
    end = elf.blob.find(b"\x00", off)
    if end < 0:
        return None
    try:
        return elf.blob[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def class_name_for_vptr(elf, relocs, vptr):
    """The mangled class name of the vtable whose address point is vptr."""
    typeinfo = relocs.get(vptr - 8)
    if typeinfo is None:
        return None
    name_va = relocs.get(typeinfo + 8)
    if name_va is None:
        return None
    return cstring(elf, name_va)


def disasm(elf, lo, hi):
    """[(address, mnemonic, op_str)] for the guest's code in [lo, hi)."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

    off = elf.va_to_off(lo)
    code = elf.blob[off:off + (hi - lo)]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    return [(ins.address, ins.mnemonic, ins.op_str)
            for ins in md.disasm(code, lo)]


# ---------------------------------------------------------------------------
# 1. THE REGISTRY, re-derived from the guest's own code.
# ---------------------------------------------------------------------------
def derive_registry_va(elf):
    """The .bss singleton the accessor 0xb5741c hands out.

    The accessor computes it as an adrp/add pair and RETURNS it, so the pair
    that is followed by the return is the one, not the first one (the
    function also computes a guard byte at the same page).
    """
    insns = disasm(elf, 0xB5741C, 0xB57460)
    pairs = []
    pending = None
    for address, mnemonic, ops in insns:
        if mnemonic == "adrp":
            match = re.match(r"(x\d+), #(0x[0-9a-fA-F]+|[0-9]+)$", ops)
            pending = (match.group(1), int(match.group(2), 0)) if match else None
        elif mnemonic == "add" and pending is not None:
            match = re.match(r"(x\d+), (x\d+), #(0x[0-9a-fA-F]+|[0-9]+)$", ops)
            if match and match.group(1) == match.group(2) == pending[0]:
                pairs.append((address, pending[1] + int(match.group(3), 0)))
                pending = None
    for address, value in reversed(pairs):
        index = next(i for i, ins in enumerate(insns) if ins[0] == address)
        if any(ins[1] == "ret" for ins in insns[index + 1:index + 4]):
            return value
    return None


def derive_registry_layout(elf):
    """(base register, seed offsets, [(table offset, cell offset)]).

    From the guest's own read path (0x991d1c): the registry base is the
    argument, the seeds are the words at the base, and each table is found
    by an "add x0, base, #imm" right before the bucket lookup, whose result
    is read at [x0 + cell offset].
    """
    insns = disasm(elf, 0x991D1C, 0x991E00)
    base = None
    for _address, mnemonic, ops in insns:
        match = re.match(r"(x\d+), x0$", ops)
        if mnemonic == "mov" and match:
            base = match.group(1)
            break
    assert base is not None, "the read path does not copy the registry base"

    seeds = set()
    tables = []
    pending_table = None
    for index, (_address, mnemonic, ops) in enumerate(insns):
        if mnemonic == "ldr":
            if re.match(r"x\d+, \[" + base + r"\]$", ops):
                seeds.add(0)
            match = re.match(r"x\d+, \[" + base + r", #(0x[0-9a-fA-F]+|\d+)\]$", ops)
            if match:
                seeds.add(int(match.group(1), 16))
        elif mnemonic == "add":
            match = re.match(r"x0, " + base + r", #(0x[0-9a-fA-F]+|[0-9]+)$", ops)
            if match:
                pending_table = int(match.group(1), 16)
        elif mnemonic == "bl" and "0x991fd8" in ops:
            follow = insns[index + 1]
            match = re.match(r"x\d+, \[x0, #(0x[0-9a-fA-F]+|\d+)\]$", follow[2])
            assert match, f"unexpected node load after the lookup: {follow!r}"
            tables.append((pending_table, int(match.group(1), 16)))
    return base, seeds, tables


def derive_bucket_geometry(elf):
    """(the offset of the bucket count, the offset of the bucket array).

    From the guest's own bucket lookup (0x991fd8): the count is the word at
    table+8 and the bucket array is the word at table+0.
    """
    insns = disasm(elf, 0x991FD8, 0x992020)
    count = None
    array = None
    for _address, mnemonic, ops in insns:
        if mnemonic != "ldr":
            continue
        match = re.match(r"x\d+, \[x0, #(0x[0-9a-fA-F]+|\d+)\]$", ops)
        if match and count is None:
            count = int(match.group(1), 16)
        if re.match(r"x\d+, \[x19\]$", ops) and array is None:
            array = 0
    return count, array


def derive_node_layout(elf):
    """(the offsets the chain node is read at).

    From the guest's own bucket lookup: the chain is walked with "ldr x20,
    [x20]" (the next pointer is the whole word) and the node's fields are
    read at [x20 + imm].
    """
    insns = disasm(elf, 0x991FD8, 0x992130)
    fields = set()
    next_is_whole_word = False
    for _address, mnemonic, ops in insns:
        if mnemonic != "ldr":
            continue
        if re.match(r"x20, \[x20\]$", ops):
            next_is_whole_word = True
        match = re.match(r"x\d+, \[x20, #(0x[0-9a-fA-F]+|\d+)\]$", ops)
        if match:
            fields.add(int(match.group(1), 16))
    return fields, next_is_whole_word


def derive_accessor_members(elf, lo, hi, accessor):
    """The member offsets the guest reads through its accessor.

    A tiny data-flow over the function: "this" is tracked per register, so
    the offset that reaches the accessor call is the member's offset.
    """
    insns = disasm(elf, lo, hi)
    offsets = {"x0": 0, "x19": 0}
    members = []
    for _address, mnemonic, ops in insns:
        if mnemonic == "adrp":
            destination = re.match(r"(x\d+), ", ops)
            if destination:
                offsets.pop(destination.group(1), None)
        elif mnemonic == "mov":
            match = re.match(r"(x\d+), (x\d+)$", ops)
            if match:
                destination, source = match.groups()
                if source in offsets:
                    offsets[destination] = offsets[source]
                else:
                    offsets.pop(destination, None)
        elif mnemonic == "add":
            match = re.match(r"(x\d+), (x\d+), #(0x[0-9a-fA-F]+|[0-9]+)$", ops)
            if match:
                destination, source, imm = match.group(1), match.group(2), \
                    int(match.group(3), 16)
                if source in offsets:
                    offsets[destination] = offsets[source] + imm
                else:
                    offsets.pop(destination, None)
        elif mnemonic == "bl" and accessor in ops:
            members.append(offsets.get("x0"))
    return members


# ---------------------------------------------------------------------------
# 2. THE PORT'S SOURCE.
# ---------------------------------------------------------------------------
def source_constant(text, name):
    """The value of a named uintptr_t constant in the port's source."""
    pattern = (r"constexpr\s+uintptr_t\s+" + re.escape(name) +
               r"\s*=\s*0x([0-9a-fA-F]+)ULL")
    match = re.search(pattern, text)
    assert match is not None, f"the source does not carry the constant {name}"
    return int(match.group(1), 16)


def strip_comments(text):
    """The source without its comments (string literals are kept)."""
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char == '"':
            out.append(char)
            index += 1
            while index < length:
                out.append(text[index])
                if text[index] == "\\":
                    index += 1
                    if index < length:
                        out.append(text[index])
                        index += 1
                    continue
                if text[index] == '"':
                    index += 1
                    break
                index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "*":
            index += 2
            while index + 1 < length and not (text[index] == "*" and
                                              text[index + 1] == "/"):
                index += 1
            index += 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# 3. THE CONTROL: a synthetic registry, looked up with the same algorithm.
# ---------------------------------------------------------------------------
MASK = (1 << 64) - 1


def to_int32(value):
    value &= 0xFFFFFFFF
    return value - (1 << 32) if value & 0x80000000 else value


def bucket_index(count, key):
    """The guest's own bucket index (ELF 0x991fd8)."""
    if count == 0:
        return None
    if (count & (count - 1)) == 0:
        return key & (count - 1)
    return key % count


class SyntheticRegistry:
    """A registry that looks like the guest's.

    The same seeds, the same two tables, the same chain nodes (a key and a
    value cell each) and the same insert, which is the guest's own write
    path (ELF 0x992518):

        table 1 (key0 = ~(seed0 ^ id)) gets the cell  ~(minted ^ value)
        table 2 (key1 = ~(seed1 ^ id)) gets the cell  minted
    """

    def __init__(self, seed0, seed1, count1, count2):
        self.seed0 = seed0
        self.seed1 = seed1
        self.count1 = count1
        self.count2 = count2
        self.table1 = [[] for _ in range(count1)]
        self.table2 = [[] for _ in range(count2)]

    def insert(self, member_id, minted, value):
        key0 = (~(self.seed0 ^ member_id)) & MASK
        key1 = (~(self.seed1 ^ member_id)) & MASK
        cell1 = (~(minted ^ (value & MASK))) & MASK
        cell2 = minted & MASK
        self.table1[bucket_index(self.count1, key0)].append((key0, cell1))
        self.table2[bucket_index(self.count2, key1)].append((key1, cell2))

    def find(self, table, count, key):
        index = bucket_index(count, key)
        if index is None:
            return None, "the-table-has-no-buckets"
        for node_key, cell in table[index]:
            if node_key == key:
                return (node_key, cell), None
            node_index = bucket_index(count, node_key)
            if node_index is not None and node_index != index:
                return None, "the-key-is-not-in-the-table"
        return None, "the-key-is-not-in-the-table"

    def lookup(self, member_id):
        key0 = (~(self.seed0 ^ member_id)) & MASK
        key1 = (~(self.seed1 ^ member_id)) & MASK
        node1, why = self.find(self.table1, self.count1, key0)
        if why is not None:
            return None, why
        node2, why = self.find(self.table2, self.count2, key1)
        if why is not None:
            return None, why
        cell1 = node1[1]
        cell2 = node2[1]
        return to_int32((~(cell1 ^ cell2)) & MASK), None


def run_host_selftest():
    """The same header, compiled and run, when a host compiler is there."""
    compiler = (shutil.which("g++") or shutil.which("c++") or
                shutil.which("clang++"))
    if compiler is None:
        print("  note  : no host C++ compiler found, the compiled "
              "self-test was skipped")
        return None
    with tempfile.TemporaryDirectory() as work:
        binary = os.path.join(work, "mvs" + (".exe" if os.name == "nt" else ""))
        command = [compiler, "-std=c++20", "-O1",
                   "-I", str(ROOT / "include"), str(HOST_SELFTEST),
                   "-o", binary]
        compiled = subprocess.run(command, capture_output=True, text=True)
        assert compiled.returncode == 0, (
            "the host self-test did not compile:\n" + compiled.stderr)
        run = subprocess.run([binary], capture_output=True, text=True)
        assert run.returncode == 0, (
            "the host self-test failed:\n" + run.stdout + run.stderr)
        return run.stdout


def main() -> int:
    if not SO.exists():
        print(f"SKIP: the guest binary is not at {SO}")
        return 0

    elf = Elf(SO)
    relocs = relative_relocs(elf)

    # ---- 1. THE REGISTRY, from the guest's own code ----------------------
    registry_va = derive_registry_va(elf)
    assert registry_va == 0x1F97980, (
        f"the registry accessor hands out {registry_va:#x}, expected "
        "0x1f97980")

    base, seeds, tables = derive_registry_layout(elf)
    assert 0x00 in seeds and 0x08 in seeds, (
        f"the read path does not read the two seeds it is expected to: "
        f"{sorted(seeds)}")
    table_offsets = [offset for offset, _cell in tables]
    assert 0x20 in table_offsets and 0x48 in table_offsets, (
        f"the read path does not use the two tables it is expected to: "
        f"{sorted(table_offsets)}")
    cell_offsets = {cell for _offset, cell in tables}
    assert cell_offsets == {0x18}, (
        f"the node's value cell is not at +0x18: {sorted(cell_offsets)}")

    count_offset, array_offset = derive_bucket_geometry(elf)
    assert count_offset == 8 and array_offset == 0, (
        f"the bucket geometry is count at +{count_offset} and the array at "
        f"+{array_offset}, expected count at +8 and the array at +0")

    node_fields, node_next_is_whole = derive_node_layout(elf)
    assert node_next_is_whole, "the chain's next pointer is not the whole word"
    assert 0x08 in node_fields and 0x10 in node_fields, (
        f"the chain node's key fields are not at +0x08/+0x10: "
        f"{sorted(node_fields)}")

    # ---- 2. THE RuleSet_RaceMetrics MEMBERS, from the guest's telemetry ---
    members = derive_accessor_members(elf, 0xB35800, 0xB358F0,
                                      "0x991c8c")
    unique_members = sorted({offset for offset in members if offset is not None})
    assert 0x60 in unique_members, (
        f"the telemetry does not read the lap count at +0x60: "
        f"{unique_members}")
    assert 0xA8 in unique_members, (
        f"the telemetry does not read the top speed at +0xa8: "
        f"{unique_members}")

    # The class the object has to be, through the guest's own RTTI.
    rs_vptr = 0x1E4B378
    assert class_name_for_vptr(elf, relocs, rs_vptr) == "19RuleSet_RaceMetrics", (
        "the vtable the probe matches is not the RuleSet_RaceMetrics one")

    # ---- 3. THE PORT'S SOURCE CARRIES THE SAME VALUES --------------------
    store = STORE.read_text(encoding="utf-8")
    probe = PROBE.read_text(encoding="utf-8")
    probe_header = PROBE_HEADER.read_text(encoding="utf-8")
    state = STATE.read_text(encoding="utf-8")
    state_header = STATE_HEADER.read_text(encoding="utf-8")

    assert source_constant(store, "kRegistryBssVa") == registry_va, (
        "the port's registry address does not match the guest's")
    assert source_constant(store, "kSeed0Off") == 0x00
    assert source_constant(store, "kSeed1Off") == 0x08
    assert source_constant(store, "kTable1Off") == 0x20
    assert source_constant(store, "kTable1CountOff") == 0x20 + count_offset
    assert source_constant(store, "kTable2Off") == 0x48
    assert source_constant(store, "kTable2CountOff") == 0x48 + count_offset
    assert source_constant(store, "kNodeNextOff") == 0x00
    assert source_constant(store, "kNodeKeyOff") == 0x08
    assert source_constant(store, "kNodeCellOff") == 0x18
    assert source_constant(probe, "kRuleSetRaceMetricsVptrElfVa") == rs_vptr
    assert source_constant(probe, "kRuleSetTotalLapsOff") == 0x60
    assert source_constant(probe, "kRuleSetTopSpeedOff") == 0xA8

    # The algorithm itself, spelled the way the port spells it, so the
    # control below cannot drift away from the shipped code.
    assert "(count & (count - 1)) == 0" in store, (
        "the port does not mask the key when the count is a power of two")
    assert "key % count" in store, (
        "the port does not take the key modulo the count")
    assert "~(seed0 ^ id)" in store and "~(seed1 ^ id)" in store, (
        "the port does not derive its keys as ~(seed ^ id)")
    assert "cell1 ^ cell2" in store and "~mixed" in store, (
        "the port does not decode the value as ~(cell1 ^ cell2)")

    # ---- 4. THE PROBE NEVER CALLS THE MUTATING ACCESSOR ------------------
    accessor = "%x" % GUEST_MEMBER_ACCESSOR
    for name, text in (("race_metrics_probe.cpp", probe),
                       ("member_value_store.hpp", store)):
        code = strip_comments(text)
        assert accessor not in code.lower(), (
            f"{name} mentions the guest's member accessor 0x{accessor} in "
            "code; that call re-keys the member and mutates the guest")

    # ---- 5. EVERY GUEST READ GOES THROUGH THE SAFE-READ HELPER -----------
    forbidden = ("reinterpret_cast", "*(uint64_t", "*(u64", "*(uint32_t",
                 "*(const volatile", "*(volatile", "volatile ")
    for name, text in (("race_metrics_probe.cpp", probe),
                       ("member_value_store.hpp", store)):
        code = strip_comments(text)
        for pattern in forbidden:
            assert pattern not in code, (
                f"{name} dereferences memory directly ({pattern!r}); every "
                "guest read has to go through raceStateReadWord()")
    probe_code = strip_comments(probe)
    # The reader the value store uses, plus the three reads the probe makes
    # itself: the object's vptr and the two member descriptors.
    assert probe_code.count("raceStateReadWord(") >= 4, (
        "the probe does not read guest memory through the safe-read helper")

    # The probe has to be pumped, and it has to be pumped from the race
    # state's own call site.
    assert "pumpRaceMetricsProbe" in state, (
        "the race-state probe does not pump the race-metrics probe")
    assert "pumpRaceMetricsProbe" in probe
    assert "race-metrics" in probe, "the probe does not name its own phase"
    assert "sdmc:/switch/rr3-native-rehost.guest-view.log" in probe, (
        "the probe does not write to the guest-view log")
    assert "raceStateResolveRuleSetRaceMetrics" in state and \
        "raceStateResolveRuleSetRaceMetrics" in state_header, (
        "the port does not expose the resolved RuleSet to the probe")
    assert "raceStateReadWord" in state and "raceStateReadWord" in state_header, (
        "the port does not expose its safe read to the probe")

    # ---- 6. THE CONTROL --------------------------------------------------
    # The registry the control builds has a table whose count is NOT a power
    # of two (the modulo path) and a table whose count is (the masked path),
    # so both of the guest's index rules are exercised.
    registry = SyntheticRegistry(0x0123456789ABCDEF, 0xFEDCBA9876543210, 5, 8)
    laps_id = 0x1122334455667788
    registry.insert(laps_id, 0xAAAAAAAAAAAAAAAA, 3)
    value, why = registry.lookup(laps_id)
    assert why is None and value == 3, (
        f"the positive control failed: value={value!r} why={why!r}")

    # A second member, and a negative value (the registry holds int32s).
    speed_id = 0x99AABBCCDDEEFF00
    registry.insert(speed_id, 0x5555555555555555, -2048)
    value, why = registry.lookup(speed_id)
    assert why is None and value == -2048, (
        f"the negative-value control failed: value={value!r} why={why!r}")

    # THE NEGATIVE CONTROL: a member that was never inserted is NOT found.
    value, why = registry.lookup(0xDEADBEEF)
    assert value is None, "a member that was never inserted produced a value"
    assert why == "the-key-is-not-in-the-table", (
        f"the negative control did not say why: {why!r}")

    # The same header, compiled and run.
    host_output = run_host_selftest()

    print("PASS: the race-metrics probe agrees with the guest binary "
          f"(registry {registry_va:#x}, seeds +0x00/+0x08, tables "
          "+0x20/+0x28 and +0x48/+0x50, node key +0x08, cell +0x18, "
          f"members {unique_members}, vtable {rs_vptr:#x}); "
          "the probe never calls the guest accessor 0x991c8c and reads "
          "guest memory only through the safe-read helper; the synthetic "
          "registry returns what was written and refuses what was not")
    if host_output:
        for line in host_output.strip().splitlines():
            print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
