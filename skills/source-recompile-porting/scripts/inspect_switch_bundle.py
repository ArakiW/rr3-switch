#!/usr/bin/env python3
"""Read-only structural inventory for Nintendo Switch port bundles.

This script never decrypts NCA content and never searches for keys.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


TITLE_ID_RE = re.compile(r"^[0-9a-fA-F]{16}$")
PFS0_ENTRY = struct.Struct("<QQII")


def parse_pfs0(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path), "is_pfs0": False, "files": []}
    try:
        with path.open("rb") as stream:
            header = stream.read(16)
            if len(header) != 16 or header[:4] != b"PFS0":
                return result
            file_count, string_size, _ = struct.unpack_from("<III", header, 4)
            if file_count > 100_000 or string_size > 64 * 1024 * 1024:
                result["error"] = "unreasonable PFS0 header"
                return result
            entries = []
            for _index in range(file_count):
                raw = stream.read(PFS0_ENTRY.size)
                if len(raw) != PFS0_ENTRY.size:
                    result["error"] = "truncated PFS0 entry table"
                    return result
                entries.append(PFS0_ENTRY.unpack(raw))
            strings = stream.read(string_size)
            if len(strings) != string_size:
                result["error"] = "truncated PFS0 string table"
                return result
            header_size = 16 + file_count * PFS0_ENTRY.size + string_size
            data_offset = (header_size + 15) & ~15
            listing = []
            for offset, size, name_offset, _reserved in entries:
                if name_offset >= len(strings):
                    name = "<invalid-name-offset>"
                else:
                    end = strings.find(b"\0", name_offset)
                    if end < 0:
                        end = len(strings)
                    name = strings[name_offset:end].decode("utf-8", errors="replace")
                listing.append(
                    {
                        "name": name,
                        "size": size,
                        "container_offset": data_offset + offset,
                    }
                )
            result.update(
                {
                    "is_pfs0": True,
                    "file_count": file_count,
                    "string_table_size": string_size,
                    "data_offset": data_offset,
                    "files": listing,
                }
            )
    except OSError as exc:
        result["error"] = str(exc)
    return result


def parse_settings(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path)}
    try:
        root = ET.parse(path).getroot()
        values: dict[str, str] = {}
        interesting = {
            "DX_Version",
            "ScreenWidth",
            "ScreenHeight",
            "RefreshRate",
            "TextureQuality",
            "ShaderQuality",
        }
        for element in root.iter():
            if element.tag in interesting and "value" in element.attrib:
                values[element.tag] = element.attrib["value"]
        card = root.find("VideoCardDescription")
        if card is not None:
            values["VideoCardDescription"] = card.text or ""
        result["values"] = values
    except (OSError, ET.ParseError) as exc:
        result["error"] = str(exc)
    return result


def inspect(root: Path) -> dict[str, Any]:
    extensions: collections.Counter[str] = collections.Counter()
    total_size = 0
    file_count = 0
    nsp_files: list[Path] = []
    rpf_headers: collections.Counter[str] = collections.Counter()
    rpf_platform_names: list[str] = []
    settings: list[dict[str, Any]] = []
    layered: dict[str, dict[str, Any]] = {}

    for current, _directories, filenames in os.walk(root):
        current_path = Path(current)
        parts_lower = [part.lower() for part in current_path.parts]
        title_id = None
        try:
            atmosphere_index = parts_lower.index("atmosphere")
            if parts_lower[atmosphere_index + 1] == "contents":
                candidate = current_path.parts[atmosphere_index + 2]
                if TITLE_ID_RE.fullmatch(candidate):
                    title_id = candidate.lower()
        except (ValueError, IndexError):
            pass

        for filename in filenames:
            path = current_path / filename
            try:
                size = path.stat().st_size
            except OSError:
                size = 0
            file_count += 1
            total_size += size
            extension = path.suffix.lower() or "<none>"
            extensions[extension] += 1

            if title_id is not None:
                item = layered.setdefault(
                    title_id,
                    {"title_id": title_id, "files": 0, "bytes": 0, "has_romfs": False},
                )
                item["files"] += 1
                item["bytes"] += size
                if "romfs" in parts_lower:
                    item["has_romfs"] = True

            if path.suffix.lower() == ".nsp":
                nsp_files.append(path)
            elif path.suffix.lower() == ".rpf":
                try:
                    with path.open("rb") as stream:
                        header = stream.read(16)
                    magic = header[:4].decode("ascii", errors="replace")
                    marker = header[12:16].decode("ascii", errors="replace")
                    rpf_headers[f"{magic}:{marker}"] += 1
                    if re.search(r"(?i)(?:^|[\\/])switch[a-z]?\.rpf$", str(path)):
                        rpf_platform_names.append(path.name)
                except OSError:
                    rpf_headers["<read-error>"] += 1
            elif path.name.lower() == "settings.xml":
                settings.append(parse_settings(path))

    return {
        "root": str(root),
        "file_count": file_count,
        "total_bytes": total_size,
        "extensions": dict(extensions.most_common()),
        "nsp": [parse_pfs0(path) for path in nsp_files],
        "layeredfs_titles": sorted(layered.values(), key=lambda item: item["title_id"]),
        "rpf_headers": dict(rpf_headers),
        "switch_named_rpf": sorted(set(rpf_platform_names)),
        "settings": settings,
        "limitations": [
            "NCA content is not decrypted or inspected.",
            "Packaging does not identify the graphics API or prove runtime behavior.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="Switch port bundle directory")
    parser.add_argument("--json", type=Path, dest="json_path", help="write JSON report")
    args = parser.parse_args()

    root = args.path.resolve()
    if not root.is_dir():
        parser.error(f"not a directory: {root}")
    report = inspect(root)
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
