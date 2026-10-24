#!/usr/bin/env python3
"""Static contract for the touch control file's SCRIPT FORMAT.

The port's touch control file is a script, and the script's format is the
thing a user has to get right: a line the parser silently rejects is a line
that never fires, and nothing on screen says so.

The bug this guards against is MEASURED, not hypothetical. The header
(native-switch/include/rr3/guest_touch.hpp) documents the prefix as

    [at <frame>] <x> <y> <action>

but guest_touch.cpp's parseLine() tested `line[0] == 'a'`, so a leading '['
sent the whole line down the coordinate path, where it failed with
"expected '<x> <y> <down|move|up>'". On Eden (archive
20261024-102025-eden-race-door) a 48-line bracketed script produced 36
badlines and 0 queued events, and the run that was supposed to reach a race
reached the front end and sat there. The same script written without the
brackets queued every line.

So: the parser must accept BOTH forms, and the header must say so. The test
below checks the SOURCE, because the source is the thing that decides; a
comment that agrees with the code proves nothing about either, so the source
is read and the actual parser shape is required to be present in it.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "rr3" / "guest_touch.hpp"
SOURCE = ROOT / "source" / "guest_touch.cpp"


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")

    # The header must document the bracketed form AND say that the bare form
    # works, because a script writer has to know which one to type.
    assert "[at <frame>]" in header, "the header must document '[at <frame>]'"
    assert "at <frame> <x> <y> <action>" in header, (
        "the header must document the bare 'at <frame> <x> <y> <action>' form")

    # The parser must handle the bracketed form. The check is on the source
    # text because that IS the parser: there is no C++ unit test harness in
    # this tree, and a test that recompiles the port would not run in CI.
    assert "parseLine" in source, "guest_touch.cpp has no parseLine"
    body = source[source.index("bool parseLine("):]
    body = body[:body.index("\n}\n")]

    # The bracketed branch: a '[' first character, then "at", then a frame
    # number, then the closing ']'.
    assert "line[0] == '['" in body, (
        "the parser must accept the documented bracketed '[at <frame>]' form")
    assert "'['" in body and "']'" in body, (
        "the bracketed branch must look for the closing ']'")
    assert "strncmp(line + 1, \"at\", 2)" in body, (
        "the bracketed branch must require the 'at' keyword after '['")

    # And the bare form must still work.
    assert re.search(r"line\[0\] == 'a' && line\[1\] == 't'", body), (
        "the bare 'at <frame>' form must still be accepted")

    # The frame number must be parsed in both branches, and a negative frame
    # must be refused in both: a negative due-frame is meaningless and would
    # make the event wait forever.
    assert body.count("parsed < 0") >= 2, (
        "both the bracketed and the bare branch must reject a negative frame")

    print("PASS: touch control file script format contracts")


if __name__ == "__main__":
    main()
