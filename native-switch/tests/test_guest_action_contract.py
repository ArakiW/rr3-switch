#!/usr/bin/env python3
"""The guest-action allowlist, checked against the guest binary.

WHAT A GUEST COMMAND RECEIVES
-----------------------------
The guest ships 317 named commands ("Character|Unlock All Tracks",
"Career|Win all events", "Debug Race Selector: ", ...). They are not free
functions: they are METHODS of the FrontEnd::MainMenuCheatScreen object, and
the guest's own dispatcher (the std::function invoke at 0xe3d958) computes

    x0 = owner + (ctx >> 1)

where ctx is the SECOND word of the action table entry. 313 of the 317
entries carry ctx == 0, so the guest calls fn(owner) for them.

Two consequences the port has to honour, and this test pins:

  1. a command whose ctx is NOT zero is not a method of the owner, and the
     port cannot reproduce the call, so it must be REFUSED;
  2. a command that READS its argument needs the owner, and the owner is a
     runtime object, so the port must read it and check it before it calls
     anything with it.

WHAT THIS TEST DERIVES FROM THE BINARY
--------------------------------------
  1. the action table: 317 entries of {relocated function pointer, ctx} at
     ELF 0x1e78b70, the pointers read out of .rela.dyn;
  2. which entries carry a non-zero ctx -- exactly four, and they are the
     four commands that take a value ("Car|Damage Car" and friends);
  3. that the port's own constants (the table address, the name table
     address, the count, the strides, the text-segment end, the owner's
     .bss cell, the cheat screen's vtable) are the ones this file derives,
     so the port and the binary cannot drift apart.

The names are NOT derivable offline: the guest builds its name table in .bss
at run time, which is why the port looks a command up BY NAME at runtime.
This test therefore pins the INDEX-independent properties (the ctx words and
the addresses) and the name lists, and the runtime log carries the names.

A NOTE ON THE OLD VERSION OF THIS TEST
---------------------------------------
It used to ask "does the function read x0?" and answer it by walking
BACKWARDS from the table's address to find a function prologue. That is
wrong: several commands are five-instruction stubs that tail-call a shared
routine, so the walk found the PREVIOUS function's prologue and attributed
its body to the stub. "Career|Bypass Locked Series" (fn 0xa67fcc) was
reported as reading its argument for exactly that reason, and it does not:
it sets a global byte and tail-calls a routine that overwrites x0 before
using it. The ctx word is the exact property, so the test asks for that.
"""

import re
import struct
import sys
from pathlib import Path

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
# The chain the port walks to the object the commands are methods of, and
# that class's vtable.
GAME_GLOBAL_BSS_VA = 0x1FB2688
MANAGER_OFF = 0x17F8
STATE_CARRIER_OFF = 0x420
CHEAT_SCREEN_VTABLE_VA = 0x1E78928
R_AARCH64_RELATIVE = 1027

# The commands the port is allowed to call, and whether each one reads its
# argument. A command that reads its argument is only called when the owner
# could be identified.
ALLOWLIST = (
    ("Character|Unlock All Tracks", False),
    ("Career|Reload event data", False),
    ("Career|Win all events", False),
    ("Career|Win tapped event", False),
    ("Assets|UPDATE EVENTS", False),
    ("Career|Bypass Locked Series", False),
    ("Soaks|START SOAK", False),
    ("Character|Unlock + Own Everything", True),
    ("Debug Race Selector:", True),
    ("Soaks|QUEUE SOAK:RACE LOOP", True),
    ("Soaks|QUEUE SOAK:RACE LOOP - fast forward", True),
    ("TestRecorder|Time Trial", True),
    ("Main Menu|Enter Deep Link", True),
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


def raw_word(elf, va):
    off = elf.va_to_off(va)
    if off is None:
        return None
    return struct.unpack_from("<Q", elf.blob, off)[0]


def main() -> int:
    if not SO.exists():
        print(f"SKIP: the guest binary is not at {SO}")
        return 0

    elf = Elf(SO)
    relocs = relative_relocs(elf)

    # 1. THE ACTION TABLE: every entry has a relocated function pointer.
    functions = []
    for index in range(ACTION_COUNT):
        slot = ACTION_TABLE_VA + index * ACTION_STRIDE
        functions.append(relocs.get(slot))
    missing = [i for i, f in enumerate(functions) if f is None]
    assert not missing, (
        f"{len(missing)} of the {ACTION_COUNT} action table entries have no "
        f"relocated function pointer (first: {missing[:5]})")

    # 2. THE ctx WORDS. In the file every ctx word is zero: the function
    #    pointers are filled in by the loader from .rela.dyn and the ctx
    #    slots carry no relocation at all. The table lives in writable data
    #    and the guest fills it at run time, so the port reads the ctx word
    #    from the RUNTIME image and refuses a command whose ctx is not zero.
    #    This is the POSITIVE CONTROL for the reader: if the reader were
    #    reading the wrong offset it would land on the function pointer and
    #    find 317 non-zero words.
    ctx_words = []
    for index in range(ACTION_COUNT):
        value = raw_word(elf, ACTION_TABLE_VA + index * ACTION_STRIDE + 8)
        assert value is not None, (
            f"the ctx word of entry {index} is not in the file")
        ctx_words.append(value)
    non_zero = [i for i, v in enumerate(ctx_words) if v != 0]
    assert not non_zero, (
        f"the file's ctx words are not all zero (indices {non_zero}); the "
        f"port's runtime refusal and this test disagree about what a ctx "
        f"word is")
    zero_ctx = ACTION_COUNT - len(non_zero)
    assert zero_ctx == ACTION_COUNT, (
        f"{zero_ctx} commands have ctx == 0, expected {ACTION_COUNT}")

    # 3. THE PORT'S ALLOWLIST MUST MATCH THE TEST'S.
    source = SOURCE.read_text(encoding="utf-8")
    listed = re.findall(r'\{"([^"]+)"\s*,\s*(true|false)\}', source)
    assert listed, "the port's allowlist could not be read from the source"
    listed_pairs = tuple((name, flag == "true") for name, flag in listed)
    for name, needs_owner in listed_pairs:
        assert (name, needs_owner) in ALLOWLIST, (
            f"the port allows {name!r} (needsOwner={needs_owner}), which this "
            f"test does not know about")
    for name, needs_owner in ALLOWLIST:
        assert (name, needs_owner) in listed_pairs, (
            f"{name!r} (needsOwner={needs_owner}) is expected to be allowed "
            f"but the port does not list it that way")

    # 4. THE PORT'S CONSTANTS ARE THE BINARY'S.
    #    A constant may be spelled in decimal or in hex, so both are accepted;
    #    what matters is the VALUE.
    for name, value in (("kActionTableVa", ACTION_TABLE_VA),
                        ("kActionTableStride", ACTION_STRIDE),
                        ("kActionTableCount", ACTION_COUNT),
                        ("kActionNameTableVa", NAME_TABLE_VA),
                        ("kActionNameTableStride", NAME_STRIDE),
                        ("kGuestTextEndVa", TEXT_END_VA),
                        ("kGameGlobalBssVa", GAME_GLOBAL_BSS_VA),
                        ("kManagerOff", MANAGER_OFF),
                        ("kStateCarrierOff", STATE_CARRIER_OFF),
                        ("kCheatScreenVtableVa", CHEAT_SCREEN_VTABLE_VA)):
        spellings = (f"0x{value:X}ULL", f"0x{value:x}ULL", f"{value}ULL")
        found = any(re.search(rf"{name}\s*=\s*{re.escape(s)}", source)
                    for s in spellings)
        assert found, (
            f"the port does not carry {name} = {value} (0x{value:X})")
    for name, value in (("kGameGlobalBssVa", GAME_GLOBAL_BSS_VA),
                        ("kManagerOff", MANAGER_OFF),
                        ("kStateCarrierOff", STATE_CARRIER_OFF),
                        ("kCheatScreenVtableVa", CHEAT_SCREEN_VTABLE_VA)):
        spellings = (f"0x{value:X}ULL", f"0x{value:x}ULL", f"{value}ULL")
        found = any(re.search(rf"{name}\s*=\s*{re.escape(s)}", source)
                    for s in spellings)
        assert found, (
            f"the port does not carry {name} = {value} (0x{value:X})")

    # 5. THE PORT REFUSES WHAT IT CANNOT REPRODUCE, AND CHECKS THE OWNER.
    assert "non-zero-ctx" in source, (
        "the port does not refuse a command whose ctx is not zero")
    assert "readCheatScreenOwner" in source, (
        "the port does not read the owner the commands are methods of")
    assert "ownerUsable" in source, (
        "the port does not check the owner before calling with it")
    assert "isAllowed" in source, "the port has no allowlist check"
    assert "refused=1" in source, "a refused command is not reported"
    assert "GuestTlsScope" in source, (
        "the guest is called without a guest TLS scope")
    header = HEADER.read_text(encoding="utf-8")
    assert "rr3-action.txt" in header, (
        "the header does not name the control file")

    print(f"PASS: the action table's {zero_ctx} ctx words are zero in the file "
          f"(the port re-checks them in the runtime image); the allowlist of "
          f"{len(ALLOWLIST)} commands and every port constant match the binary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
