#!/usr/bin/env python3
"""Deterministic constructor/finalizer inventory for staged guest startup."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from reference_cross_linker import (
    Image,
    R_AARCH64_ABS64,
    R_AARCH64_GLOB_DAT,
    R_AARCH64_JUMP_SLOT,
    R_AARCH64_RELATIVE,
    SHN_UNDEF,
)


def lifecycle_targets(image: Image, address: int, size: int) -> list[dict[str, object]]:
    relocations = {relocation.offset: relocation for relocation in image.relocations}
    exports = sorted(
        (symbol.value, name)
        for name, symbol in image.exports.items()
        if symbol.value
    )
    targets: list[dict[str, object]] = []
    for ordinal, slot in enumerate(range(address, address + size, 8)):
        relocation = relocations.get(slot)
        target = None
        kind = "missing"
        if relocation is not None:
            if relocation.type == R_AARCH64_RELATIVE:
                target = relocation.addend
                kind = "relative"
            elif relocation.type in (
                R_AARCH64_ABS64,
                R_AARCH64_GLOB_DAT,
                R_AARCH64_JUMP_SLOT,
            ):
                symbol = image.read_symbol(relocation.symbol_index)
                if symbol.section_index != SHN_UNDEF:
                    target = symbol.value
                    if relocation.type == R_AARCH64_ABS64:
                        target += relocation.addend
                    kind = "internal-symbol"
        nearest_name = None
        nearest_delta = None
        if target is not None:
            for value, name in exports:
                if value > target:
                    break
                nearest_name = name
                nearest_delta = target - value
        targets.append(
            {
                "ordinal": ordinal,
                "slot": f"0x{slot:x}",
                "target": None if target is None else f"0x{target:x}",
                "kind": kind,
                "valid": target is not None
                and image.minimum <= target < image.maximum
                and target % 4 == 0,
                "nearest_export": nearest_name,
                "nearest_export_delta": nearest_delta,
            }
        )
    return targets


def inspect(paths: list[Path]) -> dict[str, object]:
    images = [Image(path) for path in paths]
    by_soname = {image.soname: index for index, image in enumerate(images)}
    states = [0] * len(images)
    order: list[int] = []
    cycles = 0

    def visit(index: int) -> None:
        nonlocal cycles
        if states[index] == 2:
            return
        if states[index] == 1:
            cycles += 1
            return
        states[index] = 1
        for needed in images[index].needed_names:
            dependency = by_soname.get(needed)
            if dependency is not None:
                visit(dependency)
        states[index] = 2
        order.append(index)

    for index in range(len(images)):
        visit(index)

    records = []
    for index in order:
        image = images[index]
        constructors = lifecycle_targets(
            image, image.init_array, image.init_array_size
        )
        finalizers = lifecycle_targets(
            image, image.fini_array, image.fini_array_size
        )
        records.append(
            {
                "image_index": index,
                "soname": image.soname,
                "minimum": f"0x{image.minimum:x}",
                "maximum": f"0x{image.maximum:x}",
                "constructor_count": len(constructors),
                "valid_constructors": sum(item["valid"] for item in constructors),
                "constructors": constructors,
                "finalizer_count": len(finalizers),
                "valid_finalizers": sum(item["valid"] for item in finalizers),
                "finalizers": finalizers,
            }
        )
    return {
        "dependency_order": [images[index].soname for index in order],
        "dependency_cycles": cycles,
        "dependency_constructor_count": sum(
            record["constructor_count"]
            for record in records
            if record["soname"] != "libRealRacing3.so"
        ),
        "main_constructor_count": next(
            record["constructor_count"]
            for record in records
            if record["soname"] == "libRealRacing3.so"
        ),
        "images": records,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("images", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.images), indent=2))


if __name__ == "__main__":
    main()
