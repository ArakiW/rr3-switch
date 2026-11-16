#!/usr/bin/env python3
"""The race markers must be a number the port keeps, not a string it writes.

The round census counted "start lights" and "race screen" by regexing the
guest-view log's text.  The only occurrences of those strings left in the log
are the port's own path-trace lines, and the path trace's content is decided
by the trace's own policy -- the gui/processed/ thinning and a 2048-slot FIFO
dedup with no eviction.  The race screens are requested late, so their paths
lost the slots and the count collapsed from 27 to 2 while the game did not
change.  The number measured the log's policy.

These checks keep the measurement independent of what the port writes.
"""

from pathlib import Path


def body_of(text: str, marker: str) -> str:
    """The text of the function whose definition starts at marker."""
    cursor = 0
    while True:
        found = text.find(marker, cursor)
        if found < 0:
            raise AssertionError("no definition of " + marker)
        open_paren = found + len(marker) - 1
        depth = 0
        close = -1
        for index in range(open_paren, len(text)):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    close = index
                    break
        if close >= 0 and text[close + 1:].lstrip().startswith("{"):
            brace = text.index("{", close)
            level = 0
            for index in range(brace, len(text)):
                if text[index] == "{":
                    level += 1
                elif text[index] == "}":
                    level -= 1
                    if level == 0:
                        return text[found:index + 1]
            raise AssertionError("unbalanced braces after " + marker)
        cursor = found + len(marker)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fs = (root / "source" / "guest_filesystem.cpp").read_text(encoding="utf-8")
    hpp = (root / "include" / "rr3" / "guest_filesystem.hpp").read_text(encoding="utf-8")
    view = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    census = (root / ".." / "tools" / "Test-OnEden.ps1").resolve().read_text(encoding="utf-8")

    # 1. The counters exist and are exposed.
    assert "unsigned long long g_startLightsOpens = 0;" in fs
    assert "unsigned long long g_raceScreenOpens = 0;" in fs
    assert "unsigned long long guestStartLightsOpens();" in hpp
    assert "unsigned long long guestRaceScreenOpens();" in hpp
    assert "guestStartLightsOpens()" in fs
    assert "guestRaceScreenOpens()" in fs

    # 2. They are incremented as RESOLUTION EVENTS, BEFORE the trace is
    #    thinned and before the dedup can drop the path.  Counting them after
    #    the thinning is exactly the bug this test exists to prevent.
    trace = body_of(fs, "void logResolvedPathOnce(")
    lights_at = trace.index("g_startLightsOpens, 1ULL")
    race_at = trace.index("g_raceScreenOpens, 1ULL")
    thinning_at = trace.index('std::strstr(path, "gui/processed/") == nullptr')
    assert lights_at < thinning_at, "the start-lights counter is counted after the thinning"
    assert race_at < thinning_at, "the race-screen counter is counted after the thinning"

    # 3. The census line carries both numbers.
    assert "gui=%llu twosq=%llu lights=%llu race=%llu" in view
    assert "rr3::guestStartLightsOpens()" in view
    assert "rr3::guestRaceScreenOpens()" in view

    # 4. The round census reads the port's counters, and no longer assigns
    #    the race markers straight from a regex over the log's text.
    assert "' lights=(\\d+)(?:\\s|$)'" in census
    assert "' race=(\\d+)(?:\\s|$)'" in census
    assert "$summary.msStartLights = [regex]::Matches(" not in census
    assert "$summary.msRaceScreen  = [regex]::Matches(" not in census
    # The old counts survive only as a labelled cross-check.
    assert "msStartLightsText" in census
    assert "msRaceScreenText" in census

    print("PASS: the race markers are counted by the port, before the trace is thinned")


if __name__ == "__main__":
    main()
