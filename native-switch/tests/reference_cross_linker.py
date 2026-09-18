#!/usr/bin/env python3
"""Deterministic host reference for the retained two-DSO dry linker."""

from __future__ import annotations

import argparse
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path


PT_LOAD = 1
PT_DYNAMIC = 2
PT_TLS = 7
PT_GNU_RELRO = 0x6474E552
PF_X = 1
PF_W = 2
PF_R = 4
DT_NULL = 0
DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_PLTREL = 20
DT_JMPREL = 23
DT_INIT_ARRAY = 25
DT_FINI_ARRAY = 26
DT_INIT_ARRAYSZ = 27
DT_FINI_ARRAYSZ = 28
DT_SONAME = 14
DT_GNU_HASH = 0x6FFFFEF5
R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
STB_GLOBAL = 1
STB_WEAK = 2
PAGE = 0x1000


def align_down(value: int) -> int:
    return value & ~(PAGE - 1)


def align_up(value: int) -> int:
    return (value + PAGE - 1) & ~(PAGE - 1)


def fnv_append(value: int, data: bytes) -> int:
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


@dataclass(frozen=True)
class Symbol:
    name: str
    info: int
    other: int
    section_index: int
    value: int


@dataclass(frozen=True)
class Relocation:
    offset: int
    type: int
    symbol_index: int
    addend: int


class Image:
    def __init__(self, path: Path):
        self.path = path
        raw = path.read_bytes()
        if raw[:4] != b"\x7fELF" or raw[4] != 2 or raw[5] != 1:
            raise ValueError(f"{path}: expected ELF64 little-endian input")
        elf_type, machine = struct.unpack_from("<HH", raw, 0x10)
        if elf_type != 3 or machine != 183:
            raise ValueError(f"{path}: expected AArch64 ET_DYN input")
        phoff = struct.unpack_from("<Q", raw, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", raw, 0x36)
        if phentsize != 56:
            raise ValueError(f"{path}: unexpected program-header size")
        self.headers = [
            struct.unpack_from("<IIQQQQQQ", raw, phoff + index * phentsize)
            for index in range(phnum)
        ]
        loads = [header for header in self.headers if header[0] == PT_LOAD]
        dynamic = next((header for header in self.headers if header[0] == PT_DYNAMIC), None)
        if not loads or dynamic is None:
            raise ValueError(f"{path}: missing LOAD or DYNAMIC segment")
        self.minimum = min(align_down(header[3]) for header in loads)
        self.maximum = max(align_up(header[3] + header[6]) for header in loads)
        self.image = bytearray(self.maximum - self.minimum)
        for header in loads:
            _type, _flags, offset, vaddr, _paddr, file_size, memory_size, _align = header
            if file_size > memory_size or offset + file_size > len(raw):
                raise ValueError(f"{path}: invalid PT_LOAD")
            relative = vaddr - self.minimum
            self.image[relative : relative + file_size] = raw[offset : offset + file_size]

        values: dict[int, int] = {}
        needed_offsets: list[int] = []
        dynamic_offset = dynamic[3] - self.minimum
        for index in range(dynamic[6] // 16):
            tag, value = struct.unpack_from("<qQ", self.image, dynamic_offset + index * 16)
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed_offsets.append(value)
            values[tag] = value
        else:
            raise ValueError(f"{path}: unterminated dynamic table")

        self.symbol_table = values[DT_SYMTAB]
        self.string_table = values[DT_STRTAB]
        self.string_size = values[DT_STRSZ]
        if values[DT_SYMENT] != 24:
            raise ValueError(f"{path}: unexpected symbol entry size")
        if DT_HASH in values:
            _buckets, self.symbol_count = struct.unpack_from(
                "<II", self.image, values[DT_HASH] - self.minimum
            )
        elif DT_GNU_HASH in values:
            self.symbol_count = self._gnu_symbol_count(values[DT_GNU_HASH])
        else:
            raise ValueError(f"{path}: missing symbol hash table")

        rela_entry_size = values.get(DT_RELAENT, 24)
        if rela_entry_size != 24 or values.get(DT_PLTREL, DT_RELA) != DT_RELA:
            raise ValueError(f"{path}: unsupported relocation format")
        self.relocations: list[Relocation] = []
        self._append_relocations(values.get(DT_RELA, 0), values.get(DT_RELASZ, 0))
        self._append_relocations(values.get(DT_JMPREL, 0), values.get(DT_PLTRELSZ, 0))
        self.init_array = values.get(DT_INIT_ARRAY, 0)
        self.init_array_size = values.get(DT_INIT_ARRAYSZ, 0)
        self.fini_array = values.get(DT_FINI_ARRAY, 0)
        self.fini_array_size = values.get(DT_FINI_ARRAYSZ, 0)
        self.exports: dict[str, Symbol] = {}
        for index in range(1, self.symbol_count):
            symbol = self.read_symbol(index)
            binding = symbol.info >> 4
            visibility = symbol.other & 3
            if (
                symbol.section_index != SHN_UNDEF
                and binding in (STB_GLOBAL, STB_WEAK)
                and visibility in (0, 3)
                and symbol.name
            ):
                self.exports.setdefault(symbol.name, symbol)

        def dynamic_string(offset: int) -> str:
            if offset >= self.string_size:
                raise ValueError(f"{path}: dynamic string outside table")
            start = self.string_table - self.minimum + offset
            end = self.image.find(0, start, self.string_table - self.minimum + self.string_size)
            if end < 0:
                raise ValueError(f"{path}: unterminated dynamic string")
            return bytes(self.image[start:end]).decode()

        self.soname = dynamic_string(values[DT_SONAME]) if DT_SONAME in values else path.name
        self.needed_names = [dynamic_string(offset) for offset in needed_offsets]
        loads = [header for header in self.headers if header[0] == PT_LOAD]
        self.load_segments = len(loads)
        self.executable_segments = sum(bool(header[1] & PF_X) for header in loads)
        self.writable_segments = sum(bool(header[1] & PF_W) for header in loads)
        self.read_only_segments = sum(
            bool(header[1] & PF_R) and not bool(header[1] & (PF_W | PF_X))
            for header in loads
        )
        self.writable_executable_segments = sum(
            (header[1] & (PF_W | PF_X)) == (PF_W | PF_X) for header in loads
        )
        tls_headers = [header for header in self.headers if header[0] == PT_TLS]
        self.tls_segments = len(tls_headers)
        self.tls_bytes = sum(header[6] for header in tls_headers)
        relro_headers = [header for header in self.headers if header[0] == PT_GNU_RELRO]
        if len(relro_headers) > 1:
            raise ValueError(f"{path}: multiple GNU_RELRO segments")
        self.relro_address = relro_headers[0][3] if relro_headers else 0
        self.relro_size = relro_headers[0][6] if relro_headers else 0

    @property
    def image_bytes(self) -> int:
        return len(self.image)

    def _u32(self, address: int) -> int:
        offset = address - self.minimum
        if offset < 0 or offset + 4 > len(self.image):
            raise ValueError(f"{self.path}: hash entry outside image")
        return struct.unpack_from("<I", self.image, offset)[0]

    def _gnu_symbol_count(self, address: int) -> int:
        bucket_count = self._u32(address)
        symbol_offset = self._u32(address + 4)
        bloom_size = self._u32(address + 8)
        if bucket_count == 0 or bloom_size == 0:
            raise ValueError(f"{self.path}: invalid GNU hash header")
        bucket_address = address + 16 + bloom_size * 8
        chain_address = bucket_address + bucket_count * 4
        maximum = symbol_offset
        saw_symbol = False
        for bucket_index in range(bucket_count):
            symbol_index = self._u32(bucket_address + bucket_index * 4)
            if symbol_index < symbol_offset:
                continue
            saw_symbol = True
            for _guard in range(1_000_000):
                chain = self._u32(chain_address + (symbol_index - symbol_offset) * 4)
                maximum = max(maximum, symbol_index)
                if chain & 1:
                    break
                symbol_index += 1
            else:
                raise ValueError(f"{self.path}: unterminated GNU hash chain")
        return maximum + 1 if saw_symbol else symbol_offset

    def _append_relocations(self, address: int, size: int) -> None:
        if size == 0:
            return
        if size % 24:
            raise ValueError(f"{self.path}: invalid relocation table size")
        start = address - self.minimum
        if start < 0 or start + size > len(self.image):
            raise ValueError(f"{self.path}: relocation table outside image")
        for index in range(size // 24):
            offset, info, addend = struct.unpack_from("<QQq", self.image, start + index * 24)
            self.relocations.append(
                Relocation(offset, info & 0xFFFFFFFF, info >> 32, addend)
            )

    def read_symbol(self, index: int) -> Symbol:
        if index >= self.symbol_count:
            raise ValueError(f"{self.path}: symbol index outside table")
        offset = self.symbol_table - self.minimum + index * 24
        name_offset, info, other, section_index, value, _size = struct.unpack_from(
            "<IBBHQQ", self.image, offset
        )
        if name_offset >= self.string_size:
            raise ValueError(f"{self.path}: symbol name outside string table")
        start = self.string_table - self.minimum + name_offset
        end = self.image.find(0, start, self.string_table - self.minimum + self.string_size)
        if end < 0:
            raise ValueError(f"{self.path}: unterminated symbol name")
        return Symbol(bytes(self.image[start:end]).decode(), info, other, section_index, value)

    def lifecycle_entries(self, address: int, size: int) -> tuple[int, int]:
        if size == 0:
            return 0, 0
        if size % 8:
            raise ValueError(f"{self.path}: invalid lifecycle array size")
        relocations = {relocation.offset: relocation for relocation in self.relocations}
        verified = 0
        for slot in range(address, address + size, 8):
            relocation = relocations.get(slot)
            if relocation is None:
                continue
            if relocation.type == R_AARCH64_RELATIVE:
                target = relocation.addend
            elif relocation.type in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
                symbol = self.read_symbol(relocation.symbol_index)
                if symbol.section_index == SHN_UNDEF:
                    continue
                target = symbol.value + (relocation.addend if relocation.type == R_AARCH64_ABS64 else 0)
            else:
                continue
            if self.minimum <= target < self.maximum and target % 4 == 0:
                verified += 1
        return size // 8, verified


def resolver_names(source: Path) -> set[str]:
    text = source.read_text(encoding="utf-8")
    names = {
        name
        for name in re.findall(r"RR3_BIND\(([^)]+)\)", text)
        if name != "symbol"
    }
    names.update(re.findall(r'RR3_SHIM\("([^"]+)"', text))
    return names


def inspect(primary_path: Path, dependency_path: Path, resolver_source: Path) -> dict[str, object]:
    primary = Image(primary_path)
    dependency = Image(dependency_path)
    host = resolver_names(resolver_source)
    plan_hash = 1469598103934665603
    relative = 0
    internal = 0
    patched = 0
    invalid = 0
    host_names: set[str] = set()
    cross_names: set[str] = set()
    missing_names: set[str] = set()
    counters = {
        "primary_host_relocations": 0,
        "primary_cross_relocations": 0,
        "primary_weak_zero_relocations": 0,
        "primary_missing_relocations": 0,
        "dependency_host_relocations": 0,
        "dependency_cross_relocations": 0,
        "dependency_weak_zero_relocations": 0,
        "dependency_missing_relocations": 0,
    }

    primary_constructors, primary_constructor_verified = primary.lifecycle_entries(
        primary.init_array, primary.init_array_size
    )
    dependency_constructors, dependency_constructor_verified = dependency.lifecycle_entries(
        dependency.init_array, dependency.init_array_size
    )
    primary_finalizers, primary_finalizer_verified = primary.lifecycle_entries(
        primary.fini_array, primary.fini_array_size
    )
    dependency_finalizers, dependency_finalizer_verified = dependency.lifecycle_entries(
        dependency.fini_array, dependency.fini_array_size
    )

    for image_tag, image, other, prefix in (
        (b"P", primary, dependency, "primary"),
        (b"D", dependency, primary, "dependency"),
    ):
        for relocation in image.relocations:
            provider = b"M"
            name = ""
            symbol_value = 0
            should_patch = False
            if relocation.type == R_AARCH64_RELATIVE:
                if relocation.symbol_index != 0:
                    invalid += 1
                    continue
                provider = b"R"
                relative += 1
                should_patch = True
            elif relocation.type in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
                if relocation.symbol_index == 0:
                    invalid += 1
                    continue
                symbol = image.read_symbol(relocation.symbol_index)
                name = symbol.name
                symbol_value = symbol.value
                if not name:
                    invalid += 1
                    continue
                if symbol.section_index != SHN_UNDEF:
                    provider = b"I"
                    internal += 1
                    should_patch = True
                elif name in other.exports:
                    provider = b"X"
                    symbol_value = other.exports[name].value
                    counters[f"{prefix}_cross_relocations"] += 1
                    cross_names.add(name)
                    should_patch = True
                elif name in host:
                    provider = b"H"
                    counters[f"{prefix}_host_relocations"] += 1
                    host_names.add(name)
                    should_patch = True
                elif symbol.info >> 4 == STB_WEAK:
                    provider = b"W"
                    counters[f"{prefix}_weak_zero_relocations"] += 1
                    should_patch = True
                else:
                    counters[f"{prefix}_missing_relocations"] += 1
                    missing_names.add(name)
            else:
                invalid += 1
                continue

            plan_hash = fnv_append(plan_hash, image_tag)
            plan_hash = fnv_append(plan_hash, provider)
            plan_hash = fnv_append(plan_hash, struct.pack("<I", relocation.type))
            plan_hash = fnv_append(plan_hash, struct.pack("<q", relocation.addend))
            plan_hash = fnv_append(plan_hash, struct.pack("<Q", symbol_value))
            plan_hash = fnv_append(plan_hash, name.encode() + b"\0")
            if should_patch:
                patched += 1

    return {
        "primary_path": str(primary_path.resolve()),
        "dependency_path": str(dependency_path.resolve()),
        "primary_image_bytes": primary.image_bytes,
        "dependency_image_bytes": dependency.image_bytes,
        "relative_applied": relative,
        "internal_applied": internal,
        **counters,
        "patched_relocations": patched,
        "verified_writes": patched,
        "unique_host_symbols": len(host_names),
        "unique_cross_symbols": len(cross_names),
        "unique_missing_symbols": len(missing_names),
        "primary_constructors": primary_constructors,
        "dependency_constructors": dependency_constructors,
        "finalizers": primary_finalizers + dependency_finalizers,
        "lifecycle_targets_verified": primary_constructor_verified
        + dependency_constructor_verified
        + primary_finalizer_verified
        + dependency_finalizer_verified,
        "resolution_plan_fnv1a64": f"{plan_hash:016x}",
        "invalid_relocations": invalid,
        "primary_symbol_count": primary.symbol_count,
        "dependency_symbol_count": dependency.symbol_count,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("primary", type=Path)
    parser.add_argument("dependency", type=Path)
    parser.add_argument("--resolver-source", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.primary, args.dependency, args.resolver_source), indent=2))


if __name__ == "__main__":
    main()
