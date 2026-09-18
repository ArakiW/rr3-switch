#!/usr/bin/env python3
"""Host reference for the executable multi-DSO loader session."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from reference_cross_linker import (
    Image,
    R_AARCH64_ABS64,
    R_AARCH64_GLOB_DAT,
    R_AARCH64_JUMP_SLOT,
    R_AARCH64_RELATIVE,
    SHN_UNDEF,
    STB_WEAK,
    fnv_append,
    resolver_names,
)


VIRTUALIZED = {
    "libandroid.so",
    "libc.so",
    "libdl.so",
    "libEGL.so",
    "libGLESv2.so",
    "liblog.so",
    "libm.so",
    "libz.so",
    "libarcore_sdk_c.so",
    "libarcore_sdk_jni.so",
    "libfuelmetrics.so",
    # FMOD Android audio backends: silent/error stubs, not real devices.
    "libOpenSLES.so",
    "libaaudio.so",
}


def inspect(paths: list[Path], resolver_source: Path) -> dict[str, object]:
    images = [Image(path) for path in paths]
    host = resolver_names(resolver_source)
    plan_hash = 1469598103934665603
    counters = {
        "relative_applied": 0,
        "internal_applied": 0,
        "host_relocations": 0,
        "cross_relocations": 0,
        "weak_zero_relocations": 0,
        "missing_relocations": 0,
        "patched_relocations": 0,
        "verified_writes": 0,
        "invalid_relocations": 0,
    }
    host_names: set[str] = set()
    cross_names: set[str] = set()
    missing_names: set[str] = set()
    per_image = [
        {
            "soname": image.soname,
            "host_relocations": 0,
            "cross_relocations": 0,
            "weak_zero_relocations": 0,
            "missing_relocations": 0,
            "missing_names": set(),
        }
        for image in images
    ]

    for image_index, image in enumerate(images):
        for relocation in image.relocations:
            provider = b"M"
            name = ""
            symbol_value = 0
            patch = False
            if relocation.type == R_AARCH64_RELATIVE:
                if relocation.symbol_index != 0:
                    counters["invalid_relocations"] += 1
                    continue
                provider = b"R"
                counters["relative_applied"] += 1
                patch = True
            elif relocation.type in (
                R_AARCH64_ABS64,
                R_AARCH64_GLOB_DAT,
                R_AARCH64_JUMP_SLOT,
            ):
                if relocation.symbol_index == 0:
                    counters["invalid_relocations"] += 1
                    continue
                symbol = image.read_symbol(relocation.symbol_index)
                name = symbol.name
                symbol_value = symbol.value
                if not name:
                    counters["invalid_relocations"] += 1
                    continue
                if symbol.section_index != SHN_UNDEF:
                    provider = b"I"
                    counters["internal_applied"] += 1
                    patch = True
                else:
                    exported = None
                    for other_index, other in enumerate(images):
                        if other_index != image_index and name in other.exports:
                            exported = other.exports[name]
                            break
                    if exported is not None:
                        provider = b"X"
                        symbol_value = exported.value
                        counters["cross_relocations"] += 1
                        per_image[image_index]["cross_relocations"] += 1
                        cross_names.add(name)
                        patch = True
                    elif name in host:
                        provider = b"H"
                        counters["host_relocations"] += 1
                        per_image[image_index]["host_relocations"] += 1
                        host_names.add(name)
                        patch = True
                    elif symbol.info >> 4 == STB_WEAK:
                        provider = b"W"
                        counters["weak_zero_relocations"] += 1
                        per_image[image_index]["weak_zero_relocations"] += 1
                        patch = True
                    else:
                        counters["missing_relocations"] += 1
                        per_image[image_index]["missing_relocations"] += 1
                        per_image[image_index]["missing_names"].add(name)
                        missing_names.add(name)
            else:
                counters["invalid_relocations"] += 1
                continue

            plan_hash = fnv_append(plan_hash, bytes([image_index]))
            plan_hash = fnv_append(plan_hash, provider)
            plan_hash = fnv_append(plan_hash, struct.pack("<I", relocation.type))
            plan_hash = fnv_append(plan_hash, struct.pack("<q", relocation.addend))
            plan_hash = fnv_append(plan_hash, struct.pack("<Q", symbol_value))
            plan_hash = fnv_append(plan_hash, name.encode() + b"\0")
            if patch:
                counters["patched_relocations"] += 1
                counters["verified_writes"] += 1

    sonames = {image.soname for image in images}
    needed_entries = sum(len(image.needed_names) for image in images)
    needed_embedded = sum(
        needed in sonames for image in images for needed in image.needed_names
    )
    needed_virtualized = sum(
        needed not in sonames and needed in VIRTUALIZED
        for image in images
        for needed in image.needed_names
    )
    needed_missing = needed_entries - needed_embedded - needed_virtualized

    constructors = 0
    finalizers = 0
    lifecycle_verified = 0
    relro_ranges = 0
    relro_bytes = 0
    relro_targets = 0
    for image in images:
        init_count, init_verified = image.lifecycle_entries(
            image.init_array, image.init_array_size
        )
        fini_count, fini_verified = image.lifecycle_entries(
            image.fini_array, image.fini_array_size
        )
        constructors += init_count
        finalizers += fini_count
        lifecycle_verified += init_verified + fini_verified
        if image.relro_size:
            relro_ranges += 1
            relro_bytes += image.relro_size
            relro_end = image.relro_address + image.relro_size
            relro_targets += sum(
                image.relro_address <= relocation.offset <= relro_end - 8
                for relocation in image.relocations
            )

    activation_ready = (
        counters["missing_relocations"] == 0
        and counters["invalid_relocations"] == 0
        and needed_missing == 0
        and sum(image.writable_executable_segments for image in images) == 0
        and sum(image.tls_segments for image in images) == 0
    )

    soname_to_index = {image.soname: index for index, image in enumerate(images)}
    states = [0] * len(images)
    order: list[int] = []
    dependency_cycles = 0

    def visit(index: int) -> bool:
        nonlocal dependency_cycles
        if states[index] == 2:
            return True
        if states[index] == 1:
            dependency_cycles += 1
            return False
        states[index] = 1
        for needed in images[index].needed_names:
            dependency = soname_to_index.get(needed)
            if dependency is not None and not visit(dependency):
                return False
        states[index] = 2
        order.append(index)
        return True

    for image_index in range(len(images)):
        visit(image_index)

    constructor_order_entries = 0
    constructor_order_hash = 1469598103934665603
    for image_index in order:
        image = images[image_index]
        entries = image.init_array_size // 8
        constructor_order_entries += entries
        constructor_order_hash = fnv_append(constructor_order_hash, image.soname.encode() + b"\0")
        constructor_order_hash = fnv_append(constructor_order_hash, struct.pack("<Q", entries))

    finalizer_order_entries = 0
    finalizer_order_hash = 1469598103934665603
    for image_index in reversed(order):
        image = images[image_index]
        entries = image.fini_array_size // 8
        finalizer_order_entries += entries
        finalizer_order_hash = fnv_append(finalizer_order_hash, image.soname.encode() + b"\0")
        finalizer_order_hash = fnv_append(finalizer_order_hash, struct.pack("<Q", entries))
    return {
        "paths": [str(path.resolve()) for path in paths],
        "image_count": len(images),
        "total_image_bytes": sum(image.image_bytes for image in images),
        "load_segments": sum(image.load_segments for image in images),
        "executable_segments": sum(image.executable_segments for image in images),
        "writable_segments": sum(image.writable_segments for image in images),
        "read_only_segments": sum(image.read_only_segments for image in images),
        "writable_executable_segments": sum(
            image.writable_executable_segments for image in images
        ),
        **counters,
        "unique_host_symbols": len(host_names),
        "unique_cross_symbols": len(cross_names),
        "unique_missing_symbols": len(missing_names),
        "needed_entries": needed_entries,
        "needed_embedded": needed_embedded,
        "needed_virtualized": needed_virtualized,
        "needed_missing": needed_missing,
        "relro_ranges": relro_ranges,
        "relro_bytes": relro_bytes,
        "relro_targets_verified": relro_targets,
        "tls_segments": sum(image.tls_segments for image in images),
        "tls_bytes": sum(image.tls_bytes for image in images),
        "constructors": constructors,
        "finalizers": finalizers,
        "lifecycle_targets_verified": lifecycle_verified,
        "constructor_order_entries": constructor_order_entries,
        "finalizer_order_entries": finalizer_order_entries,
        "constructor_order_fnv1a64": f"{constructor_order_hash:016x}",
        "finalizer_order_fnv1a64": f"{finalizer_order_hash:016x}",
        "dependency_cycles": dependency_cycles,
        "resolution_plan_fnv1a64": f"{plan_hash:016x}",
        "activation_ready": activation_ready,
        "per_image": [
            {
                **{key: value for key, value in item.items() if key != "missing_names"},
                "unique_missing_symbols": len(item["missing_names"]),
                "missing_symbols": sorted(item["missing_names"]),
            }
            for item in per_image
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--resolver-source", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.images, args.resolver_source), indent=2))


if __name__ == "__main__":
    main()
