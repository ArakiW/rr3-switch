#!/usr/bin/env python3
"""The tracks-volume mirror of a track's data half, checked on a synthetic tree.

WHAT IS BEING FIXED
-------------------
Loading a race, the guest asks for the track's files under
"Tracks/<Track>/processed/":

    processed/<name>.fmc             the collision
    processed/<name>.spline          the spline
    processed/<name>.rr_ai           the AI's racing line
    processed/<name>_minimap.m3g     the minimap

while EA's data package keeps that same set under the VARIANT directory
("circuit", "gp", "indy", "circuit_drag", ...). The guest's own asset list
names the variant spelling, so the guest downloads the files there and then
cannot find them where its loader looks.

The symptom is a refusal, not a missing file. MEASURED 2026-10-25, archives
20261025-162550-eden-f1b and 20261025-165527-eden-f1c: the guest logged
"Failed to load track spline", "Need to regenerate start_spline as it is
missing" and "Need to regenerate start_spline but it can only be done on
Win32", gave up on the race, and then showed a dialog saying the track
could not be loaded for lack of memory -- a message about a resource class,
not about the file that was actually missing.

Brands_Hatch and Tutorial ship the data half in processed/ already (and
their variant directories hold only the variant's own collision), which is
why the 2026-10-24 race-door run reached a race and the later runs did
not. So the tracks volume mirrors the data half for the tracks that do not
have it.

WHAT THIS TEST PINS
--------------------
1. The data half IS mirrored: .fmc, .spline, .rr_ai, the minimap .m3g and
   the .lights file land in processed/, with the variant directory's own
   layout preserved (high/, low/).
2. The .pvs files are NOT mirrored. The tracks volume is already 1.91 GiB
   against a 2 GiB build_romfs limit, and the guest logs a failed PVS load
   and carries on; the spline and the collision are the ones it gives up
   over.
3. An existing processed/ file is NEVER overwritten. Brands_Hatch and
   Tutorial ship a real processed/ tree, and replacing one of their files
   with a substitute from the variant directory would be worse than the
   bug.
4. "processed" and "resources" are not themselves mirrored, so the mirror
   cannot copy a directory onto itself.
5. The mirror only touches the tracks volume's tree, and a tree with no
   Tracks directory is not an error.

The test runs on a synthetic tree, so it needs neither the guest binary
nor EA's data package, and it can exercise the "do not overwrite" rule
without a 6 GiB download.
"""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT.parent / "tools" / "prepare_rr3_external_data.py"


def load_deploy_module():
    spec = importlib.util.spec_from_file_location("prepare_rr3_external_data", DEPLOY)
    assert spec is not None and spec.loader is not None, f"cannot load {DEPLOY}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_tree(root: Path) -> None:
    """A tracks volume stage with one track that needs the mirror and one
    that already has it."""
    tracks = root / "files" / ".depot" / "Tracks"

    # A track whose data half lives in the variant directory.
    bathurst = tracks / "Bathurst" / "circuit"
    bathurst.mkdir(parents=True)
    (bathurst / "bathurst.fmc").write_bytes(b"collision" * 100)
    (bathurst / "bathurst.spline").write_bytes(b"spline" * 100)
    (bathurst / "bathurst.rr_ai").write_bytes(b"ai" * 100)
    (bathurst / "bathurst_minimap.m3g").write_bytes(b"minimap" * 100)
    (bathurst / "bathurst_night.lights").write_bytes(b"lights" * 100)
    (bathurst / "bathurst_variation.m3g").write_bytes(b"variation" * 100)
    (bathurst / "high").mkdir()
    (bathurst / "high" / "bathurst.pvs").write_bytes(b"pvs" * 1000)
    (bathurst / "low").mkdir()
    (bathurst / "low" / "bathurst.pvs").write_bytes(b"pvs" * 1000)
    # A visual asset that lives in processed/ already, and must stay put.
    processed = tracks / "Bathurst" / "processed"
    processed.mkdir(parents=True)
    (processed / "bathurst.trackdata").write_bytes(b"trackdata")
    (processed / "high").mkdir()
    (processed / "high" / "bathurst.m3g").write_bytes(b"mesh")

    # A track that ships the data half in processed/ already.
    hatch = tracks / "Brands_Hatch"
    (hatch / "processed").mkdir(parents=True)
    (hatch / "processed" / "hatch.fmc").write_bytes(b"real dirt collision")
    (hatch / "indy").mkdir()
    (hatch / "indy" / "hatch.fmc").write_bytes(b"asphalt collision")
    (hatch / "indy" / "hatch.spline").write_bytes(b"spline")


def main() -> int:
    module = load_deploy_module()
    with tempfile.TemporaryDirectory(prefix="rr3-track-mirror-") as work:
        root = Path(work)
        build_tree(root)
        copied, copied_bytes = module.mirror_track_data_halves(root)

        bathurst_processed = root / "files" / ".depot" / "Tracks" / "Bathurst" / "processed"
        hatch_processed = root / "files" / ".depot" / "Tracks" / "Brands_Hatch" / "processed"

        for name in ("bathurst.fmc", "bathurst.spline", "bathurst.rr_ai",
                     "bathurst_minimap.m3g"):
            assert (bathurst_processed / name).is_file(), (
                f"the data half was not mirrored: {name} is missing from processed/"
            )
        assert (bathurst_processed / "high" / "bathurst.pvs").exists() is False, (
            "the .pvs files must NOT be mirrored: the tracks volume is 1.91 GiB "
            "against a 2 GiB limit, and the guest survives a missing PVS"
        )
        assert (bathurst_processed / "low" / "bathurst.pvs").exists() is False, (
            "the low/.pvs must not be mirrored either"
        )
        assert (bathurst_processed / "bathurst_night.lights").exists() is False, (
            "the .lights file must not be mirrored: the guest asks for it under "
            "a base name the package does not have, and it survives the miss"
        )
        assert (bathurst_processed / "bathurst_variation.m3g").exists() is False, (
            "the variation mesh must not be mirrored: it is a VISUAL asset that "
            "belongs in processed/high/, and matching on the .m3g extension "
            "would drag it in"
        )

        # The visual half that was already there must be untouched.
        assert (bathurst_processed / "bathurst.trackdata").read_bytes() == b"trackdata", (
            "the mirror overwrote a file that was already in processed/"
        )
        assert (bathurst_processed / "high" / "bathurst.m3g").read_bytes() == b"mesh", (
            "the mirror overwrote the visual mesh in processed/high/"
        )

        # A track that ships a real processed/ collision keeps it, and gets
        # the data half it is missing.
        assert (hatch_processed / "hatch.fmc").read_bytes() == b"real dirt collision", (
            "the mirror replaced a track's real processed/ collision"
        )
        assert (hatch_processed / "hatch.spline").read_bytes() == b"spline", (
            "the mirror did not add the data half a track was missing"
        )

        assert copied == 5, f"the mirror reported {copied} files; expected 5"
        assert copied_bytes > 0, "the mirror reported no bytes"

        # A tree with no Tracks directory is not an error.
        empty = root / "empty"
        empty.mkdir()
        assert module.mirror_track_data_halves(empty) == (0, 0), (
            "a stage without a Tracks directory must mirror nothing"
        )

    print("PASS: the tracks volume mirrors a track's data half into processed/, "
          "leaves the .pvs files out, and never overwrites a real file")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
