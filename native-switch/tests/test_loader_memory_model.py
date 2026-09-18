#!/usr/bin/env python3
"""State-model regression test for the one-pass Horizon ModuleCode loader."""

from __future__ import annotations

import sys
from pathlib import Path

from reference_cross_linker import Image, PT_LOAD, align_down, align_up


PF_X = 1
PF_W = 2
PERM_NONE = 0
PERM_R = 1
PERM_RW = 3
PERM_RX = 5


def permission(flags: int) -> int:
    if flags & PF_X:
        return PERM_RX
    if flags & PF_W:
        return PERM_RW
    return PERM_R


def validate_image(path: Path) -> int:
    image = Image(path)
    pages = image.image_bytes // 0x1000
    state = [PERM_NONE] * pages

    for header in image.headers:
        if header[0] != PT_LOAD:
            continue
        start = (align_down(header[3]) - image.minimum) // 0x1000
        end = (align_up(header[3] + header[6]) - image.minimum) // 0x1000
        target = permission(header[1])
        assert 0 <= start < end <= pages
        assert target != PERM_NONE
        for page in range(start, end):
            assert state[page] == PERM_NONE
            state[page] = target

    if image.relro_size:
        start = (align_down(image.relro_address) - image.minimum) // 0x1000
        end = (align_up(image.relro_address + image.relro_size) - image.minimum) // 0x1000
        assert 0 <= start < end <= pages
        for page in range(start, end):
            assert state[page] in (PERM_R, PERM_RW)
            state[page] = PERM_R

    operations = 0
    page = 0
    while page < pages:
        target = state[page]
        end = page + 1
        while end < pages and state[end] == target:
            end += 1
        if target != PERM_NONE:
            operations += 1
        page = end
    return operations


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    names = [
        "libRealRacing3.so",
        "libNimble.so",
        "libfmodex.so",
        "libc++_shared.so",
    ]
    operations = sum(
        validate_image(root / "reverse" / "native" / "arm64" / name)
        for name in names
    )
    assert operations == 11, operations
    print(f"PASS: {len(names)} images, {operations} permission operations, no None->None transition")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
