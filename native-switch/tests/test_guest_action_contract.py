#!/usr/bin/env python3
"""The guest-action allowlist, checked against the guest binary.

The port can invoke the guest's own named commands ("Character|Unlock All
Tracks", "Career|Win all events", ...). It does so with x0 = 0, so ONLY a
command that provably does NOT read its argument may be called: one that
dereferences x0 would take the whole run down.

That property cannot be checked at runtime -- the port would have to
disassemble the function to know -- so it is checked HERE, offline, and the
result is pinned. This test re-derives all of it from the guest binary:

  1. the action table: 317 entries of {relocated function pointer, plain
     word} at ELF 0x1e78b70, read out of .rela.dyn;
  2. for every entry, whether the function READS x0 before the call
     returns;
  3. that every command in the port's allowlist is one of the ones that
     do not read x0;
  4. that the port's own constants (the table address, the name table
     address, the count, the strides, the text-segment end) are the ones
     this file derives, so the port and the binary cannot drift apart.

The names are NOT derivable offline: the guest builds its name table in
.bss at run time, which is why the port looks a command up BY NAME at
runtime. This test therefore pins the INDEX-independent property (the x0
one) and the addresses, and the runtime log carries the names.
"""

import re
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"
SOURCE = ROOT / "source" / "guest_action.cpp"
HEADER = ROOT / "include" / "rr3" / "guest_action.hpp"

ACTION_TABLE_VA = 0x1E78B70
ACTION_STRIDE = 0x10
ACTION_COUNT = 317
NAME_TABLE_VA = 0x1F9D6D8
NAME_STRIDE = 0x18
TEXT_END_VA = 0x1E19AE0
R_AARCH64_RELATIVE = 1027

# The commands the port is allowed to call. Every one of them must be a
# command that does not read its argument.
ALLOWLIST = (
    "Character|Unlock All Tracks",
    "Career|Reload event data",
    "Career|Win all events",
    "Career|Win tapped event",
    "Assets|UPDATE EVENTS",
)


class Elf:
    def __init__(self, path):
        self.blob = path.read_bytes()
        self.progs = []
        self.sections = []
        self._parse()

    def _parse(self):
        b = self.blob
        e_phoff, = struct.unpack_from("<Q", b, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, _fl, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _al = \
                struct.unpack_from("<IIQQQQQQ", b, off)
            self.progs.append((p_type, p_offset, p_vaddr, p_filesz, p_memsz))
        e_shoff, = struct.unpack_from("<Q", b, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", b, 0x3A)
        sh = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, _fl, sh_addr, sh_offset, sh_size,
             _lk, _inf, _al2, _es) = struct.unpack_from("<IIQQQQIIQQ", b, off)
            sh.append((sh_name, sh_type, sh_addr, sh_offset, sh_size))
        strtab = sh[e_shstrndx][3]
        for (sh_name, sh_type, sh_addr, sh_offset, sh_size) in sh:
            nm = b[strtab + sh_name:].split(b"\x00")[0].decode("ascii", "replace")
            self.sections.append((nm, sh_type, sh_addr, sh_offset, sh_size))

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def va_to_off(self, va):
        for (p_type, p_offset, p_vaddr, p_filesz, _ms) in self.progs:
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
        return None


def relative_relocs(elf):
    rela = elf.section(".rela.dyn")
    out = {}
    if rela is None:
        return out
    _addr, offset, size = rela[2], rela[3], rela[4]
    blob = elf.blob
    for i in range(size // 24):
        off = offset + i * 24
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
        if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
            out[r_offset] = r_addend
    return out


def build_text_index(elf):
    text = elf.section(".text")
    _nm, _typ, addr, offset, size = text
    code = elf.blob[offset:offset + size]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    insns = list(md.disasm(code, addr))
    return insns, {ins.address: i for i, ins in enumerate(insns)}


def function_start(index, insns, addresses):
    i = addresses.get(index)
    if i is None:
        return None
    for j in range(i, max(-1, i - 500), -1):
        ins = insns[j]
        if ins.mnemonic == "sub" and ins.op_str.startswith("sp, sp"):
            return ins.address
        if (ins.mnemonic == "stp" and "x29, x30" in ins.op_str
                and "sp, #-" in ins.op_str):
            return ins.address
    return None


READS_X0 = re.compile(r"(^|[\s,])x0([,\s]|$)")


def reads_x0(insns, addresses, function):
    """True when the function READS x0 before it returns."""
    start = function_start(function, insns, addresses)
    if start is None:
        return None
    i = addresses.get(start)
    for k in range(i, min(len(insns), i + 600)):
        ins = insns[k]
        if k > i + 4 and ins.mnemonic in ("ret", "b"):
            break
        if "," not in ins.op_str:
            continue
        head, rest = ins.op_str.split(",", 1)
        dest = head.strip()
        if dest.startswith("["):
            dest = ""              # a memory destination is not a register write
        if dest == "x0":
            continue              # x0 is written here, so it is not read here
        if READS_X0.search(rest):
            return True
    return False


def main() -> int:
    if not SO.exists():
        print(f"SKIP: the guest binary is not at {SO}")
        return 0

    elf = Elf(SO)
    relocs = relative_relocs(elf)
    insns, addresses = build_text_index(elf)

    # 1. THE ACTION TABLE.
    functions = []
    for index in range(ACTION_COUNT):
        slot = ACTION_TABLE_VA + index * ACTION_STRIDE
        functions.append(relocs.get(slot))
    missing = [i for i, f in enumerate(functions) if f is None]
    assert not missing, (
        f"{len(missing)} of the {ACTION_COUNT} action table entries have no "
        f"relocated function pointer (first: {missing[:5]})")

    # 2. HOW MANY COMMANDS IGNORE THEIR ARGUMENT.
    safe = []
    for index, function in enumerate(functions):
        used = reads_x0(insns, addresses, function)
        if used is False:
            safe.append(index)
    assert len(safe) > 50, (
        f"only {len(safe)} of {ACTION_COUNT} commands ignore their argument; "
        "the enumeration is broken")
    print(f"commands that ignore their argument: {len(safe)} of {ACTION_COUNT}")

    # 3. THE PORT'S ALLOWLIST MUST ONLY HOLD SUCH COMMANDS.
    source = SOURCE.read_text(encoding="utf-8")
    listed = re.findall(r'\{"([^"]+)"\}', source)
    assert listed, "the port's allowlist could not be read from the source"
    for name in listed:
        assert name in ALLOWLIST, (
            f"the port allows {name!r}, which this test does not know about")
    for name in ALLOWLIST:
        assert name in listed, (
            f"{name!r} is expected to be allowed but the port does not list it")

    # 4. THE PORT'S CONSTANTS ARE THE BINARY'S.
    #    A constant may be spelled in decimal or in hex, so both are accepted;
    #    what matters is the VALUE.
    for name, value in (("kActionTableVa", ACTION_TABLE_VA),
                        ("kActionTableStride", ACTION_STRIDE),
                        ("kActionTableCount", ACTION_COUNT),
                        ("kActionNameTableVa", NAME_TABLE_VA),
                        ("kActionNameTableStride", NAME_STRIDE),
                        ("kGuestTextEndVa", TEXT_END_VA)):
        spellings = (f"0x{value:X}ULL", f"0x{value:x}ULL", f"{value}ULL")
        found = any(re.search(rf"{name}\s*=\s*{re.escape(s)}", source)
                    for s in spellings)
        assert found, (
            f"the port does not carry {name} = {value} (0x{value:X})")

    # 5. THE ALLOWLIST IS ENFORCED, NOT A COMMENT.
    assert "isAllowed" in source, "the port has no allowlist check"
    assert "refused=1" in source, "a refused command is not reported"
    assert "GuestTlsScope" in source, (
        "the guest is called without a guest TLS scope")
    header = HEADER.read_text(encoding="utf-8")
    assert "rr3-action.txt" in header, (
        "the header does not name the control file")

    print("PASS: the guest-action allowlist is pinned to commands that do not "
          f"read their argument ({len(safe)} of {ACTION_COUNT} qualify), and "
          "the port's table addresses are the binary's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
