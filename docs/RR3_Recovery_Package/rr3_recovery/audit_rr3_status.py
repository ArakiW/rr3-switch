#!/usr/bin/env python3
"""Read-only RR3 report audit. Python 3.10+, standard library only.

Does NOT load/execute/patch a guest DSO or modify PROJECT_STATUS.md.
Exit codes: 0 = requested checks passed; 1 = audit errors; 2 = input/tool error.
A passing report check does not prove that any runtime address is correct.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any


def hex_signed(value: int) -> str:
    return ("-" if value < 0 else "+") + hex(abs(value))


def parse_int(text: str) -> int:
    text = text.strip().strip("`").replace("−", "-")
    if not re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)", text):
        raise ValueError(f"Not an integer: {text!r}")
    sign = -1 if text.startswith("-") else 1
    unsigned = text.lstrip("+-")
    return sign * int(unsigned, 16 if unsigned.lower().startswith("0x") else 10)


def audit_text(text: str, source: str = "<text>") -> dict[str, Any]:
    # Only parse the address table. Do not interpret unrelated hex values.
    heading = re.search(r"^###\s+地址映射\s*$", text, re.MULTILINE)
    if not heading:
        raise ValueError("Missing '### 地址映射' section; no checks were performed.")
    remainder = text[heading.end():]
    next_heading = re.search(r"^#{1,3}\s", remainder, re.MULTILINE)
    section = remainder[:next_heading.start()] if next_heading else remainder
    rows: list[dict[str, Any]] = []
    for line in section.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or not cells[1].strip("`").lower().startswith("0x"):
            continue
        rows.append({"symbol": cells[0].strip("`"),
                     "elf_va": parse_int(cells[1]), "reported_delta": parse_int(cells[2])})
    anchors = [r for r in rows if r["symbol"] == "onViewRenderJNI"]
    if len(anchors) != 1 or len(rows) < 2:
        raise ValueError("Address table must contain one onViewRenderJNI anchor and target rows.")
    names = [r["symbol"] for r in rows]
    if len(set(names)) != len(names):
        raise ValueError("Duplicate symbol labels in address table; resolve ambiguity first.")
    anchor = anchors[0]["elf_va"]
    checks: list[dict[str, Any]] = []
    findings: list[dict[str, Any]] = []
    for row in rows:
        calculated = row["elf_va"] - anchor
        ok = calculated == row["reported_delta"]
        item = {
            "symbol": row["symbol"], "elf_va": hex(row["elf_va"]),
            "reported_delta": hex_signed(row["reported_delta"]),
            "calculated_delta": hex_signed(calculated),
            "reported_delta_resolves_to_va": hex(anchor + row["reported_delta"]),
            "status": "PASS_ARITHMETIC_ONLY" if ok else "ERROR_DELTA_MISMATCH",
        }
        checks.append(item)
        if not ok:
            findings.append({"severity": "ERROR", "code": "DELTA_MISMATCH", **item,
                             "runtime_cause_proven": False})
        name_match = re.fullmatch(r"FUN_([0-9a-fA-F]+)", row["symbol"])
        if name_match:
            label_va = int(name_match.group(1), 16)
            if label_va != row["elf_va"]:
                findings.append({"severity": "REVIEW", "code": "LABEL_VA_NAMESPACE_DIFFERENCE",
                                 "symbol": row["symbol"], "label_va": hex(label_va),
                                 "elf_va": hex(row["elf_va"]),
                                 "difference": hex_signed(label_va - row["elf_va"]),
                                 "note": "May be a documented Ghidra rebase, NOT proof of a wrong runtime address."})
    sha_match = re.search(r"`libRealRacing3\.so`\s*SHA-256:\s*`([^`]+)`", text)
    expected_sha = sha_match.group(1).lower() if sha_match else None
    if expected_sha is None or not re.fullmatch(r"[a-f0-9]{64}", expected_sha):
        findings.append({"severity": "ERROR", "code": "MISSING_OR_INVALID_DSO_SHA256"})
        expected_sha = None
    findings.append({"severity": "REVIEW", "code": "VTABLE_SLOT_272_REVIEW",
                     "byte_offset": 272, "assumed_entry_bytes": 8,
                     "index_relative_to_vptr_address_point": 34,
                     "fits_40_function_entries": True,
                     "note": "Arithmetic only. Actual object/vptr/table population and crash provenance are unverified."})
    return {"schema_version": 1, "source": source,
            "scope": "Report arithmetic and labels; optional read-only ELF structural checks.",
            "anchor_elf_va": hex(anchor), "expected_dso_sha256": expected_sha,
            "address_checks": checks, "findings": findings,
            "elf_check": {"status": "NOT_RUN_NO_ELF_PROVIDED"},
            "runtime_checks": {"status": "NOT_RUN",
                               "reason": "No project source build, Switch execution, or actual guest trace is performed by this tool."}}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_elf64(path: Path) -> dict[str, Any]:
    size = path.stat().st_size
    with path.open("rb") as f:
        header = f.read(64)
        if len(header) != 64 or header[:4] != b"\x7fELF":
            raise ValueError("Not a complete ELF header.")
        if header[4] != 2 or header[5] != 1 or header[6] != 1:
            raise ValueError("Expected ELF64 little-endian version 1.")
        fields = struct.unpack("<16sHHIQQQIHHHHHH", header)
        (_, e_type, machine, version, _, phoff, _, _, ehsize,
         phentsize, phnum, _, _, _) = fields
        if e_type != 3 or machine != 183 or version != 1 or ehsize != 64:
            raise ValueError("Expected an AArch64 ET_DYN ELF64 object with a standard header.")
        if phnum in (0, 0xffff):
            raise ValueError("No program headers, or extended numbering (not supported by this small auditor).")
        if phentsize < 56 or phoff < 64 or phoff + phnum * phentsize > size:
            raise ValueError("Program header table is truncated or invalid.")
        segments = []
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            data = f.read(56)
            if len(data) != 56:
                raise ValueError(f"Truncated program header {i}.")
            kind, flags, offset, va, _, filesz, memsz, align = struct.unpack("<IIQQQQQQ", data)
            if kind != 1:  # PT_LOAD only
                continue
            if filesz > memsz or offset + filesz > size or va + memsz > (1 << 64):
                raise ValueError(f"Invalid PT_LOAD range {i}.")
            if align > 1 and ((align & (align - 1)) != 0 or (va - offset) % align != 0):
                raise ValueError(f"Invalid PT_LOAD alignment {i}.")
            segments.append({"program_header_index": i, "offset": offset,
                             "va": va, "filesz": filesz, "memsz": memsz,
                             "flags": flags, "align": align})
    if not segments:
        raise ValueError("ELF has no PT_LOAD segments.")
    return {"path": str(path.resolve()), "bytes": size, "segments": segments}


def audit_elf(report: dict[str, Any], path: Path) -> None:
    elf = read_elf64(path)
    actual_sha = sha256_file(path)
    expected = report["expected_dso_sha256"]
    match = expected is not None and actual_sha == expected
    if not match:
        report["findings"].append({"severity": "ERROR", "code": "DSO_IDENTITY_NOT_CONFIRMED",
                                   "actual_sha256": actual_sha, "expected_sha256": expected})
    addresses = []
    for item in report["address_checks"]:
        va = int(item["elf_va"], 16)
        segments = [s for s in elf["segments"] if s["va"] <= va < s["va"] + s["memsz"]]
        result: dict[str, Any] = {"symbol": item["symbol"], "elf_va": item["elf_va"]}
        if len(segments) != 1:
            result["status"] = "ERROR_NOT_IN_ONE_LOAD_SEGMENT"
            report["findings"].append({"severity": "ERROR", "code": result["status"], **result})
        else:
            seg = segments[0]
            displacement = va - seg["va"]
            result.update({"status": "IN_PT_LOAD_ONLY_NOT_SEMANTICALLY_VERIFIED",
                           "segment_index": seg["program_header_index"],
                           "flags": "".join(c for bit, c in [(4, "R"), (2, "W"), (1, "X")] if seg["flags"] & bit),
                           "file_offset": hex(seg["offset"] + displacement) if displacement < seg["filesz"] else None,
                           "zero_fill_region": displacement >= seg["filesz"]})
        addresses.append(result)
    report["elf_check"] = {"status": "CHECKED_READ_ONLY", "path": elf["path"],
                           "bytes": elf["bytes"], "sha256": actual_sha,
                           "identity_matches_report": match, "addresses": addresses,
                           "limitations": ["No disassembly or symbol ownership verification.",
                                           "No relocation interpretation or live load_bias validation.",
                                           "No guest object, callback, or runtime verification."]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("status", type=Path, help="Path to PROJECT_STATUS.md")
    parser.add_argument("--elf", type=Path, help="Optional original libRealRacing3.so; never executed or edited")
    parser.add_argument("--out", type=Path, help="Optional JSON result path; must not overwrite an input")
    args = parser.parse_args(argv)
    try:
        if args.out:
            inputs = {args.status.resolve()}
            if args.elf:
                inputs.add(args.elf.resolve())
            if args.out.resolve() in inputs:
                raise ValueError("Output path must not overwrite an input file.")
        report = audit_text(args.status.read_text(encoding="utf-8-sig"), str(args.status.resolve()))
        report["source_sha256"] = sha256_file(args.status)
        if args.elf:
            audit_elf(report, args.elf)
        errors = sum(item["severity"] == "ERROR" for item in report["findings"])
        report["summary"] = {"audit_errors": errors,
                             "overall": "FAIL" if errors else "PASS_REQUESTED_STATIC_CHECKS_ONLY"}
        encoded = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(encoded, encoding="utf-8")
        # ensure_ascii avoids errors on Windows consoles without UTF-8.
        print(json.dumps(report, ensure_ascii=True, indent=2))
        return 1 if errors else 0
    except (OSError, ValueError, struct.error) as exc:
        print(f"Input/tool error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
