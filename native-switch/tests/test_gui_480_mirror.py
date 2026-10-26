#!/usr/bin/env python3
"""The 480x320 copy of the GUI tree, mirrored from 960x640.

WHAT IS BEING FIXED
-------------------
The guest builds its resolution chain by enumerating gui_assets/ and keeps the
folders in readdir order; the guest's own "Res order:" line in the run log says
"960x640, then 480x320". For every image the guest looks for the folder in each
chain entry in turn, and inside it tries the three atlas spellings
(sprites.android.atlas, sprites.ios.atlas, sprites.atlas).

MEASURED 2026-10-25, archive 20261025-183814-eden-f1e and its disk-space
control 20261025-222948-eden-f1f: 125 failures of the form

    Failed to load image from atlas: 'button/standard.png'
    Failed to load image from atlas: './avatars/ai_avatar_25.png'

For "button" the port's stat answers 1268 bytes for
960x640/button/sprites.android.atlas -- the guest DOES find that file -- and
still does not take the sprite from it, so the chain falls through to 480x320.
There every candidate misses: the volumes ship 16 files and 3 sprite atlases
under gui_assets/480x320/ (only events/ and splash/) against 3,929 files and
1,928 atlases under 960x640/. The "avatars" failures degrade further, to a
whole-tree name search ("./avatars/ai_avatar_25.png").

So the second link of the chain has to be a real tree, not a stub.

WHAT THIS TEST PINS
--------------------
1. Every file under 960x640/ is mirrored into 480x320/.
2. An existing 480x320 file is NEVER overwritten: the content EA really ships
   at 480x320 (events/, splash/) keeps its own bytes.
3. A stage with no 960x640 tree mirrors nothing and is not an error.
4. The mirror reports the file count and the byte count it wrote.

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


def load_deploy_module():
    spec = importlib.util.spec_from_file_location("prepare_rr3_external_data", DEPLOY)
    assert spec is not None and spec.loader is not None, f"cannot load {DEPLOY}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_tree(root: Path) -> None:
    """A gui-sd volume stage with a 960x640 tree and a sparse 480x320 tree."""
    gui = root / "files" / ".depot" / "gui_assets"

    sd = gui / "960x640" / "button"
    sd.mkdir(parents=True)
    (sd / "sprites.android.atlas").write_bytes(b"<atlas_images/>" * 40)
    (sd / "sprites_0.rgb.pvr.z").write_bytes(b"\x00\x01" * 5000)

    av = gui / "960x640" / "avatars"
    av.mkdir(parents=True)
    (av / "sprites.android.atlas").write_bytes(b"<atlas_images/>" * 10)

    # EA's own 480x320 content: it must survive untouched.
    own = gui / "480x320" / "splash"
    own.mkdir(parents=True)
    (own / "sprites.android.atlas").write_bytes(b"EA-OWN-480")


def main() -> int:
    module = load_deploy_module()
    with tempfile.TemporaryDirectory(prefix="rr3-gui-480-") as work:
        root = Path(work)
        build_tree(root)

        copied, copied_bytes = module.mirror_gui_assets_to_480x320(root)
        gui = root / "files" / ".depot" / "gui_assets"

        assert (gui / "480x320" / "button" / "sprites.android.atlas").is_file(), (
            "the 960x640 button atlas was not mirrored into 480x320/"
        )
        assert (gui / "480x320" / "button" / "sprites_0.rgb.pvr.z").is_file(), (
            "the 960x640 button texture was not mirrored into 480x320/"
        )
        assert (gui / "480x320" / "avatars" / "sprites.android.atlas").is_file(), (
            "the 960x640 avatars atlas was not mirrored into 480x320/"
        )

        assert (gui / "480x320" / "splash" / "sprites.android.atlas").read_bytes() \
            == b"EA-OWN-480", (
            "the mirror overwrote the 480x320 content EA really ships"
        )

        assert copied == 3, f"the mirror reported {copied} files; expected 3"
        assert copied_bytes > 0, "the mirror reported no bytes"

        # A second run is a no-op: everything is already in place.
        again, again_bytes = module.mirror_gui_assets_to_480x320(root)
        assert again == 0 and again_bytes == 0, (
            "the mirror was not idempotent: a second run copied files again"
        )

        # A stage with no 960x640 tree mirrors nothing, and is not an error.
        empty = root / "empty"
        empty.mkdir()
        assert module.mirror_gui_assets_to_480x320(empty) == (0, 0), (
            "a stage without a 960x640 tree must mirror nothing"
        )

    print("PASS: the 480x320 GUI tree is mirrored from 960x640, the content EA "
          "really ships at 480x320 is never overwritten, and the mirror is "
          "idempotent")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
