#!/usr/bin/env python3
"""The Tutorial track's asset list, under the name the guest asks for.

WHAT IS BEING FIXED
-------------------
The guest asks the port for

    rr3core:/files/.depot/asset_list_Tutorial.txt

and, when that misses,

    rr3core:/files/apk/res/asset_list_Tutorial.txt

and then says, in its own words (MEASURED 2026-10-25, archive
20261025-183814-eden-f1e):

    CC: AssetManager::LoadAssetList() : Asset List not found - asset_list_Tutorial.txt

EA's data package ships that list SPLIT BY QUALITY and under a different name:

    files/apk/res/asset_list_track_tutorial_common.txt   (3 rows)
    files/apk/res/asset_list_track_tutorial_high.txt     (2 rows)
    files/apk/res/asset_list_track_tutorial_low.txt      (2 rows)

The seven rows are disjoint, and they are exactly the list the game ends up
with on a card: the card's own .depot/asset_list_track_tutorial.txt is those
same seven rows. So the merged file is the list -- not a new one, and not a
guess about what the Tutorial track needs.

The guest LOGS a missing list and CARRIES ON, so this is not a crash. It is a
hole in what the guest can load for the Tutorial track.

WHAT THIS TEST PINS
--------------------
1. The merged list lands at files/.depot/asset_list_Tutorial.txt -- the path
   the guest asks for FIRST.
2. It holds every row of the three source lists, in a stable order, one per
   line, and no row twice.
3. Its line ending is LF. The list is a tab-separated format the guest parses
   line by line, and a stray CR would end up inside a path.
4. An existing asset_list_Tutorial.txt is NEVER overwritten. A card that
   already ships the list keeps it.
5. A stage with no tutorial lists writes nothing and is not an error.

The test runs on a synthetic tree, so it needs neither the guest binary nor
EA's data package.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT.parent / "tools" / "prepare_rr3_external_data.py"

COMMON = (
    "/Tracks/Tutorial/processed/tutorial.fmc\tad4d5b836ced774d9962e704710d3279\t108509\t279380\n"
    "/Tracks/Tutorial/processed/tutorial_sky.m3g\t5291e37eb78f3511b4abc9333f2525f9\t2596\t4326\n"
    "/Tracks/Tutorial/processed/tutorial_sky.pvr\ted4135d4ed92212a53b31cbc8c9b251e\t51\t244\n"
)
HIGH = (
    "/Tracks/Tutorial/processed/high/tutorial.m3g\t7b7419422a8ef2f7718ef30c36abe7f7\t129332\t175124\n"
    "/Tracks/Tutorial/processed/high/tutorial.pvs\t11af6e1a7456c6e1973a112a9131f264\t7869\t25748\n"
)
LOW = (
    "/Tracks/Tutorial/processed/low/tutorial.m3g\t7b7419422a8ef2f7718ef30c36abe7f7\t129332\t175124\n"
    "/Tracks/Tutorial/processed/low/tutorial.pvs\t11af6e1a7456c6e1973a112a9131f264\t7869\t25748\n"
)


def load_deploy_module():
    spec = importlib.util.spec_from_file_location("prepare_rr3_external_data", DEPLOY)
    assert spec is not None and spec.loader is not None, f"cannot load {DEPLOY}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_tree(root: Path) -> None:
    """A core-volume stage holding the three tutorial lists."""
    res = root / "files" / "apk" / "res"
    res.mkdir(parents=True)
    (res / "asset_list_track_tutorial_common.txt").write_bytes(COMMON.encode("utf-8"))
    (res / "asset_list_track_tutorial_high.txt").write_bytes(HIGH.encode("utf-8"))
    (res / "asset_list_track_tutorial_low.txt").write_bytes(LOW.encode("utf-8"))


def main() -> int:
    module = load_deploy_module()
    with tempfile.TemporaryDirectory(prefix="rr3-tutorial-list-") as work:
        root = Path(work)
        build_tree(root)

        written, written_bytes = module.merge_tutorial_asset_list(root)
        target = root / "files" / ".depot" / "asset_list_Tutorial.txt"

        assert target.is_file(), (
            "the merged Tutorial asset list was not written to files/.depot/, "
            "which is the path the guest asks for first"
        )
        assert written == 1, f"the merge reported {written} files; expected 1"
        assert written_bytes == target.stat().st_size, (
            "the merge reported a byte count that does not match the file"
        )

        raw = target.read_bytes()
        assert b"\r" not in raw, (
            "the merged asset list contains a CR: the guest parses it line by "
            "line and the CR would end up inside a path"
        )
        text = raw.decode("utf-8")
        rows = [line for line in text.split("\n") if line]
        assert len(rows) == 7, f"the merged list has {len(rows)} rows; expected 7"
        assert len(set(rows)) == 7, "the merged list repeats a row"

        for expected in (COMMON + HIGH + LOW).splitlines():
            assert expected in rows, f"the merged list lost the row {expected!r}"
        for row in rows:
            assert row.count("\t") == 3, (
                f"the row {row!r} is not a four-column tab-separated entry"
            )

        # An existing list is never overwritten.
        again, again_bytes = module.merge_tutorial_asset_list(root)
        assert again == 0 and again_bytes == 0, (
            "the merge overwrote an asset list that was already there"
        )

        # A stage with no tutorial lists writes nothing, and is not an error.
        empty = root / "empty"
        (empty / "files" / "apk" / "res").mkdir(parents=True)
        assert module.merge_tutorial_asset_list(empty) == (0, 0), (
            "a stage without the tutorial lists must write nothing"
        )

    print("PASS: the Tutorial track's asset list is written under the name the "
          "guest asks for, merged from the three shipped lists, LF terminated, "
          "and never overwrites an existing file")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
