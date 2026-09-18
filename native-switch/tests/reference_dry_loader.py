#!/usr/bin/env python3
"""Host reference for the NRO's read-only/dry ELF mapping result."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path


PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_PLTRELSZ = 2
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_JMPREL = 23
DT_VERSYM = 0x6FFFFFF0
R_AARCH64_RELATIVE = 1027
PAGE = 0x1000


def align_down(value: int) -> int:
    return value & ~(PAGE - 1)


def align_up(value: int) -> int:
    return (value + PAGE - 1) & ~(PAGE - 1)


def fnv1a64(data: bytes | bytearray) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def fnv1a64_append(value: int, name: str) -> int:
    for byte in name.encode("utf-8") + b"\0":
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def inspect(path: Path, resolver_source: Path | None = None) -> dict[str, object]:
    raw = path.read_bytes()
    if raw[:4] != b"\x7fELF" or raw[4] != 2 or raw[5] != 1:
        raise ValueError("expected ELF64 little-endian input")
    elf_type, machine = struct.unpack_from("<HH", raw, 0x10)
    if elf_type != 3 or machine != 183:
        raise ValueError("expected AArch64 ET_DYN input")
    phoff = struct.unpack_from("<Q", raw, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", raw, 0x36)
    if phentsize != 56:
        raise ValueError("unexpected program-header size")

    headers = [
        struct.unpack_from("<IIQQQQQQ", raw, phoff + index * phentsize)
        for index in range(phnum)
    ]
    loads = [header for header in headers if header[0] == PT_LOAD]
    dynamic = next((header for header in headers if header[0] == PT_DYNAMIC), None)
    minimum = min(align_down(header[3]) for header in loads)
    maximum = max(align_up(header[3] + header[6]) for header in loads)
    image = bytearray(maximum - minimum)
    copied = 0
    zeroed = 0
    for header in loads:
        _type, _flags, offset, vaddr, _paddr, file_size, memory_size, _align = header
        relative = vaddr - minimum
        image[relative : relative + file_size] = raw[offset : offset + file_size]
        copied += file_size
        zeroed += memory_size - file_size
    pre_reloc_hash = fnv1a64(image)

    if dynamic is None:
        raise ValueError("missing PT_DYNAMIC")
    dynamic_offset = dynamic[3] - minimum
    dynamic_count = dynamic[6] // 16
    values: dict[int, int] = {}
    for index in range(dynamic_count):
        tag, value = struct.unpack_from("<qQ", image, dynamic_offset + index * 16)
        if tag == DT_NULL:
            break
        values[tag] = value

    relative_count = 0
    pending = 0
    invalid = 0
    internal_symbol_relocations = 0
    external_names: set[str] = set()
    seen_external_indices: set[int] = set()
    external_relocations = 0
    weak_external_symbols: set[str] = set()
    versioned_external_symbols: set[str] = set()
    maximum_symbol_index = 0
    symbol_name_hash = 1469598103934665603
    resolver_names: list[str] = []
    if resolver_source is not None:
        source = resolver_source.read_text(encoding="utf-8")
        resolver_names = [
            name
            for name in re.findall(r"RR3_BIND\(([^)]+)\)", source)
            if name != "symbol"
        ]
        resolver_names.extend(re.findall(r'RR3_SHIM\("([^"]+)"', source))
        if len(resolver_names) != len(set(resolver_names)):
            raise ValueError("duplicate RR3_BIND entries")
    resolver_set = set(resolver_names)
    resolver_table_hash = 1469598103934665603
    for resolver_name in resolver_names:
        resolver_table_hash = fnv1a64_append(resolver_table_hash, resolver_name)
    lookup_matched_symbols = 0
    lookup_missing_symbols = 0
    lookup_matched_relocations = 0
    lookup_missing_relocations = 0
    lookup_matched_name_hash = 1469598103934665603
    categories = {
        "gles": 0,
        "egl": 0,
        "fmod": 0,
        "arcore": 0,
        "pthread": 0,
        "android": 0,
        "cxx": 0,
        "other": 0,
    }

    def classify(name: str) -> None:
        if name.startswith("FMOD_") or name.startswith("_ZN4FMOD"):
            categories["fmod"] += 1
        elif name.startswith("egl"):
            categories["egl"] += 1
        elif len(name) > 2 and name.startswith("gl") and name[2].isupper():
            categories["gles"] += 1
        elif len(name) > 2 and name.startswith("Ar") and name[2].isupper():
            categories["arcore"] += 1
        elif name.startswith("pthread"):
            categories["pthread"] += 1
        elif name.startswith("__android_"):
            categories["android"] += 1
        elif name.startswith(("_Z", "__cxa", "__gxx")):
            categories["cxx"] += 1
        else:
            categories["other"] += 1

    symbol_table = values[DT_SYMTAB]
    symbol_entry_size = values[DT_SYMENT]
    string_table = values[DT_STRTAB]
    string_table_size = values[DT_STRSZ]
    version_table = values.get(DT_VERSYM)
    if symbol_entry_size != 24:
        raise ValueError("unexpected DT_SYMENT")

    def read_symbol(symbol_index: int) -> tuple[str, int, int]:
        nonlocal maximum_symbol_index
        maximum_symbol_index = max(maximum_symbol_index, symbol_index)
        symbol_offset = symbol_table - minimum + symbol_index * symbol_entry_size
        name_offset, info, _other, section_index, _value, _size = struct.unpack_from(
            "<IBBHQQ", image, symbol_offset
        )
        if name_offset >= string_table_size:
            raise ValueError("symbol name offset outside DT_STRTAB")
        name_start = string_table - minimum + name_offset
        name_end_limit = string_table - minimum + string_table_size
        name_end = image.find(0, name_start, name_end_limit)
        if name_end < 0:
            raise ValueError("unterminated dynamic symbol name")
        name = bytes(image[name_start:name_end]).decode("utf-8", errors="strict")
        return name, info, section_index

    def process(address: int, size: int) -> None:
        nonlocal relative_count, pending, invalid, internal_symbol_relocations
        nonlocal external_relocations
        nonlocal symbol_name_hash
        nonlocal lookup_matched_symbols, lookup_missing_symbols
        nonlocal lookup_matched_relocations, lookup_missing_relocations
        nonlocal lookup_matched_name_hash
        if address < minimum or address + size > maximum or size % 24:
            invalid += 1
            return
        offset = address - minimum
        for item in range(size // 24):
            relocation_offset, info, _addend = struct.unpack_from(
                "<QQq", image, offset + item * 24
            )
            relocation_type = info & 0xFFFFFFFF
            symbol_index = info >> 32
            if relocation_type == R_AARCH64_RELATIVE:
                if not (minimum <= relocation_offset <= maximum - 8):
                    invalid += 1
                else:
                    relative_count += 1
            else:
                name, symbol_info, section_index = read_symbol(symbol_index)
                if section_index != 0:
                    internal_symbol_relocations += 1
                    continue
                pending += 1
                external_relocations += 1
                external_names.add(name)
                if name in resolver_set:
                    lookup_matched_relocations += 1
                else:
                    lookup_missing_relocations += 1
                if symbol_index not in seen_external_indices:
                    seen_external_indices.add(symbol_index)
                    symbol_name_hash = fnv1a64_append(symbol_name_hash, name)
                    classify(name)
                    if name in resolver_set:
                        lookup_matched_symbols += 1
                        lookup_matched_name_hash = fnv1a64_append(
                            lookup_matched_name_hash, name
                        )
                    else:
                        lookup_missing_symbols += 1
                    if symbol_info >> 4 == 2:
                        weak_external_symbols.add(name)
                    if version_table is not None:
                        version_offset = version_table - minimum + symbol_index * 2
                        version = struct.unpack_from("<H", image, version_offset)[0] & 0x7FFF
                        if version > 1:
                            versioned_external_symbols.add(name)

    process(values[DT_RELA], values[DT_RELASZ])
    if values.get(DT_JMPREL) and values.get(DT_PLTRELSZ):
        process(values[DT_JMPREL], values[DT_PLTRELSZ])

    return {
        "path": str(path),
        "image_bytes": len(image),
        "copied_bytes": copied,
        "zero_filled_bytes": zeroed,
        "relative": relative_count,
        "pending": pending,
        "internal_symbol_relocations": internal_symbol_relocations,
        "external_relocations": external_relocations,
        "unique_external_symbols": len(external_names),
        "weak_external_symbols": len(weak_external_symbols),
        "versioned_external_symbols": len(versioned_external_symbols),
        "maximum_symbol_index": maximum_symbol_index,
        "symbol_name_fnv1a64": f"{symbol_name_hash:016x}",
        "resolver_table_entries": len(resolver_names),
        "resolver_table_fnv1a64": f"{resolver_table_hash:016x}",
        "lookup_matched_symbols": lookup_matched_symbols,
        "lookup_missing_symbols": lookup_missing_symbols,
        "lookup_matched_relocations": lookup_matched_relocations,
        "lookup_missing_relocations": lookup_missing_relocations,
        "lookup_matched_name_fnv1a64": f"{lookup_matched_name_hash:016x}",
        "categories": categories,
        "invalid": invalid,
        "pre_reloc_fnv1a64": f"{pre_reloc_hash:016x}",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--resolver-source", type=Path)
    args = parser.parse_args()
    resolver = args.resolver_source.resolve() if args.resolver_source else None
    print(json.dumps(inspect(args.elf.resolve(), resolver), indent=2))


if __name__ == "__main__":
    main()
