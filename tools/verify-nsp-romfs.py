#!/usr/bin/env python3
"""Open an NSP and check that its Program NCA really carries the RomFS tree.

WHY THIS TOOL EXISTS.  The first NSP this project shipped was 5,411,032
bytes and it did not run.  It was not "a bit small": it was missing the
whole RomFS (44,948,780 bytes over 35 files), including the guest's own
libraries -- libRealRacing3.so is 33,110,472 bytes of that.  A size
comparison catches the symptom; this tool catches the cause.

WHAT IT DOES.  It takes the NSP apart the way the installer does, and then
takes the Program NCA apart the way the loader does:

    PFS0 container
      -> three NCAs (Program, Control, Meta)
      -> the Program NCA's header is AES-256-XTS encrypted
      -> the key area is AES-128-ECB encrypted
      -> each section is AES-128-CTR encrypted
      -> section 1 is the RomFS, wrapped in IVFC
      -> IVFC level 5 holds the raw RomFS image

Every step mirrors hacbrewpack's own code (nca.c, aes.c), so the tool reads
the artifact that is about to be published rather than a stand-in built
next to it.

THE CHECKS.  The RomFS file list has to match the tree on disk, name for
name and size for size.  With --nro, the same list has to match the RomFS
the NRO embeds, which is the artifact that is known to run on hardware.

Usage:
    python tools/verify-nsp-romfs.py <nsp> [--romfs native-switch/romfs]
                                       [--nro native-switch/rr3-native-rehost.nro]
                                       [--keys <prod.keys>]
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

INVALID = 0xFFFFFFFF

# --- the layout hacbrewpack writes -------------------------------------------
NCA_HEADER_SIZE = 0xC00
NCA_SECTION_ENTRY_SIZE = 0x10
NCA_FS_HEADER_SIZE = 0x200
NCA_SECTION_ENTRY_OFFSET = 0x240
NCA_ENCRYPTED_KEYS_OFFSET = 0x300
NCA_FS_HEADERS_OFFSET = 0x400
FS_HEADER_SUPERBLOCK_OFFSET = 0x08
FS_HEADER_SECTION_CTR_OFFSET = 0x140
IVFC_LEVEL_HEADER_OFFSET = 0x10
IVFC_LEVEL_HEADER_SIZE = 0x18
IVFC_MAX_LEVEL = 6
CONTENT_TYPE_PROGRAM = 0x00
HASH_TYPE_ROMFS = 0x03


class RomFS:
    """The reader for a raw RomFS image (the same one the data volumes use)."""

    def __init__(self, blob: bytes) -> None:
        if len(blob) < 80:
            raise ValueError("short RomFS image")
        fields = struct.unpack("<10Q", blob[:80])
        if fields[0] != 80:
            raise ValueError("unexpected RomFS header size %d" % fields[0])
        self.blob = blob
        self.dir_table = fields[3]
        self.file_table = fields[7]
        self.data_offset = fields[5]
        self.data_size = fields[6]

    def directory(self, offset: int):
        base = self.dir_table + offset
        _, sibling, child_dir, child_file, _, name_len = struct.unpack(
            "<6I", self.blob[base:base + 24])
        name = self.blob[base + 24:base + 24 + name_len].decode("utf-8", "replace")
        return sibling, child_dir, child_file, name

    def file(self, offset: int):
        base = self.file_table + offset
        _, sibling, _, size, _, name_len = struct.unpack(
            "<IIQQII", self.blob[base:base + 32])
        name = self.blob[base + 32:base + 32 + name_len].decode("utf-8", "replace")
        return sibling, size, name

    def walk(self, node: int = 0, prefix: str = ""):
        _, child_dir, child_file, _ = self.directory(node)
        path = prefix
        while child_file != INVALID:
            _, size, name = self.file(child_file)
            yield path + name, size
            child_file = self.file(child_file)[0]
        while child_dir != INVALID:
            sibling, _, _, name = self.directory(child_dir)
            yield from self.walk(child_dir, path + name + "/")
            child_dir = sibling

    def listing(self):
        return {path: size for path, size in self.walk()}


def read_keys(path: Path) -> dict:
    keys = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        keys[name.strip()] = bytes.fromhex(value.strip())
    return keys


def xts_decrypt(key32: bytes, blob: bytes, sector_size: int = 0x200) -> bytes:
    """AES-256-XTS with Nintendo's tweak (see hacbrewpack's aes.c).

    The tweak for sector N is the sector number written big-endian into the
    16-byte tweak block, and the sector size is 0x200.
    """
    out = bytearray()
    for index, offset in enumerate(range(0, len(blob), sector_size)):
        tweak = index.to_bytes(16, "big")
        decryptor = Cipher(algorithms.AES(key32), modes.XTS(tweak)).decryptor()
        out += decryptor.update(blob[offset:offset + sector_size]) + decryptor.finalize()
    return bytes(out)


def parse_pfs0(blob: bytes):
    """Read a PFS0 container.

    The header carries the string table size directly (pfs0.c:125), and the
    string table is padded to 0x20, so the data region starts at
    0x10 + entries + string_table_size.  The entry offsets are relative to
    that data region, NOT to the start of the file.
    """
    if blob[:4] != b"PFS0":
        raise ValueError("not a PFS0 container")
    count = struct.unpack("<I", blob[4:8])[0]
    string_table_size = struct.unpack("<I", blob[8:12])[0]
    string_base = 0x10 + count * 0x18
    entries = []
    for index in range(count):
        base = 0x10 + index * 0x18
        data_offset, data_size = struct.unpack("<QQ", blob[base:base + 16])
        name_offset = struct.unpack("<I", blob[base + 0x10:base + 0x14])[0]
        end = string_base + name_offset
        while end < len(blob) and blob[end] != 0:
            end += 1
        name = blob[string_base + name_offset:end].decode("utf-8", "replace")
        entries.append({"name": name, "offset": data_offset, "size": data_size})
    data_base = string_base + string_table_size
    return entries, data_base


def decrypt_nca_header(nca: bytes, header_key: bytes) -> bytes:
    return xts_decrypt(header_key, nca[:NCA_HEADER_SIZE])


def parse_nca_header(header: bytes):
    magic = header[0x200:0x204]
    if magic != b"NCA3":
        raise ValueError("unexpected NCA magic %r" % magic)
    content_type = header[0x205]
    sections = []
    for index in range(4):
        base = NCA_SECTION_ENTRY_OFFSET + index * NCA_SECTION_ENTRY_SIZE
        start, end = struct.unpack("<II", header[base:base + 8])
        sections.append((start, end))
    fs_headers = []
    for index in range(4):
        base = NCA_FS_HEADERS_OFFSET + index * NCA_FS_HEADER_SIZE
        fs_headers.append(header[base:base + NCA_FS_HEADER_SIZE])
    return {
        "content_type": content_type,
        "sections": sections,
        "fs_headers": fs_headers,
        "encrypted_keys": header[NCA_ENCRYPTED_KEYS_OFFSET:
                                NCA_ENCRYPTED_KEYS_OFFSET + 0x40],
    }


def decrypt_section(nca: bytes, header: dict, index: int, key_area_key: bytes) -> bytes:
    """Decrypt one NCA section the way hacbrewpack encrypts it."""
    start, end = header["sections"][index]
    start_offset = start * 0x200
    end_offset = end * 0x200
    blob = nca[start_offset:end_offset]

    fs_header = header["fs_headers"][index]
    crypt_type = fs_header[0x04]
    if crypt_type == 0x01:      # plaintext
        return blob
    if crypt_type != 0x03:      # regular crypto (AES-CTR)
        raise ValueError("section %d has crypt type %d" % (index, crypt_type))

    # The key area's first 0x40 bytes are AES-ECB encrypted with the key
    # area key; key 2 (offset 0x20) is the section key.
    decryptor = Cipher(algorithms.AES(key_area_key), modes.ECB()).decryptor()
    key_area = decryptor.update(header["encrypted_keys"]) + decryptor.finalize()
    section_key = key_area[0x20:0x30]

    section_ctr = fs_header[FS_HEADER_SECTION_CTR_OFFSET:
                            FS_HEADER_SECTION_CTR_OFFSET + 8]
    counter = bytearray(16)
    for index_byte in range(8):
        counter[index_byte] = section_ctr[8 - index_byte - 1]
    block_offset = start_offset >> 4
    for index_byte in range(8):
        counter[16 - index_byte - 1] = (block_offset >> (8 * index_byte)) & 0xFF

    decryptor = Cipher(algorithms.AES(section_key),
                       modes.CTR(bytes(counter))).decryptor()
    return decryptor.update(blob) + decryptor.finalize()


def ivfc_level6(section: bytes, fs_header: bytes):
    """Return the raw RomFS image: IVFC level 5's hash data."""
    superblock = fs_header[FS_HEADER_SUPERBLOCK_OFFSET:
                           FS_HEADER_SUPERBLOCK_OFFSET + 0x138]
    magic, _id, master_hash_size, num_levels = struct.unpack(
        "<IIII", superblock[0:16])
    if magic != 0x43465649:       # "IVFC"
        raise ValueError("unexpected IVFC magic %08X" % magic)
    levels = []
    for index in range(IVFC_MAX_LEVEL):
        base = IVFC_LEVEL_HEADER_OFFSET + index * IVFC_LEVEL_HEADER_SIZE
        logical_offset, hash_data_size = struct.unpack(
            "<QQ", superblock[base:base + 16])
        levels.append((logical_offset, hash_data_size))
    offset, size = levels[IVFC_MAX_LEVEL - 1]
    return section[offset:offset + size], levels, num_levels


def tree_listing(root: Path):
    listing = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            listing[path.relative_to(root).as_posix()] = path.stat().st_size
    return listing


def nro_romfs_listing(nro: bytes):
    """The RomFS the NRO embeds: its asset section holds the raw image."""
    if nro[:4] != b"NRO0":
        raise ValueError("not an NRO")
    # NroHeader: the asset section header sits right after the fixed header.
    asset_offset, asset_size = struct.unpack("<QQ", nro[0x28:0x38])
    base = 0x80 + asset_offset
    # The asset section starts with an ASET header (0x20 bytes) that wraps
    # the RomFS image.
    romfs_offset, romfs_size = struct.unpack("<QQ", nro[base + 0x08:base + 0x18])
    blob = nro[base + romfs_offset:base + romfs_offset + romfs_size]
    return RomFS(blob).listing()


def report(label: str, listing, reference, failures):
    missing = sorted(set(reference) - set(listing))
    extra = sorted(set(listing) - set(reference))
    resized = sorted(path for path in set(reference) & set(listing)
                     if reference[path] != listing[path])
    total = sum(listing.values())
    print("%-12s %4d files  %13d bytes" % (label, len(listing), total))
    if not missing and not extra and not resized:
        print("%-12s identical to the reference" % "")
        return
    for path in missing[:20]:
        print("%-12s MISSING %s" % ("", path))
    for path in extra[:20]:
        print("%-12s EXTRA   %s" % ("", path))
    for path in resized[:20]:
        print("%-12s RESIZED %s (%d -> %d)" % (
            "", path, reference[path], listing[path]))
    failures.append(label)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("nsp", type=Path)
    parser.add_argument("--romfs", type=Path,
                        default=Path("native-switch/romfs"))
    parser.add_argument("--nro", type=Path, default=None)
    parser.add_argument("--keys", type=Path,
                        default=Path(r"E:\AI\FH1_SWITCH\tools\Eden-Windows-v0.2.1-pgo\keys\prod.keys"))
    parser.add_argument("--romfs-image", type=Path, default=None,
                        help="a raw RomFS image to compare against (the NRO's)")
    args = parser.parse_args()

    keys = read_keys(args.keys)
    header_key = keys["header_key"]
    key_area_key = keys["key_area_key_application_00"]

    container = args.nsp.read_bytes()
    entries, data_base = parse_pfs0(container)
    print("NSP            %13d bytes" % len(container))
    for entry in entries:
        print("  %-40s %13d bytes" % (entry["name"], entry["size"]))

    program = None
    for entry in entries:
        start = data_base + entry["offset"]
        blob = container[start:start + entry["size"]]
        header = parse_nca_header(decrypt_nca_header(blob, header_key))
        if header["content_type"] == CONTENT_TYPE_PROGRAM:
            program = (blob, header, entry["name"])
            break
    if program is None:
        print("no Program NCA in the container")
        return 1

    blob, header, name = program
    print("program NCA    %s (%d bytes)" % (name, len(blob)))
    for index, (start, end) in enumerate(header["sections"]):
        if end > start:
            fs_header = header["fs_headers"][index]
            print("  section %d    %13d bytes  hash_type=%d crypt_type=%d" % (
                index, (end - start) * 0x200, fs_header[0x03], fs_header[0x04]))

    romfs_index = None
    for index, fs_header in enumerate(header["fs_headers"]):
        if fs_header[0x03] == HASH_TYPE_ROMFS and fs_header[0x02] == 0x00:
            romfs_index = index
            break
    if romfs_index is None:
        print("the Program NCA has no RomFS section")
        return 1

    section = decrypt_section(blob, header, romfs_index, key_area_key)
    image, levels, num_levels = ivfc_level6(section, header["fs_headers"][romfs_index])
    listing = RomFS(image).listing()
    print("romfs image    %13d bytes  (IVFC levels=%d)" % (len(image), num_levels))

    failures = []
    reference = tree_listing(args.romfs)
    report("in the NSP", listing, reference, failures)

    if args.romfs_image:
        report("romfs image", RomFS(args.romfs_image.read_bytes()).listing(),
               reference, failures)
    if args.nro:
        report("in the NRO", nro_romfs_listing(args.nro.read_bytes()),
               reference, failures)

    if failures:
        print("FAILED: %s" % ", ".join(failures))
        return 1
    print("OK: the NSP carries the whole RomFS tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
