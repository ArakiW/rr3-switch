#!/usr/bin/env python3
"""The census counters must measure the GUEST, not the port's own log.

WHAT THIS EXISTS FOR.  The round census used to count "start lights" and
"race screen" by REGEXING the guest-view log for the strings
"StartLightsScreen" and "InGameScreen".  The only occurrences of those
strings left in the log are the port's OWN path-trace lines, and their
number is decided by the trace's policy -- the gui/processed/ thinning and a
2048-slot dedup with no eviction -- and by the guest-log quota of 4000
lines.  The number therefore measured the LOG'S POLICY, not the game: it
fell from 27 to 2 while the game did not change.

The port now keeps the two markers as counters, and these checks keep the
measurement independent of what the port writes.

WHY THE COUNTERS ARE COUNTED IN resolvePath().  An earlier version of this
change counted them in logResolvedPathOnce(), which ONE resolver branch
calls; the guest reaches the same files through six other branches that
resolved them without counting.  A counter there measured "requests that
arrived in the app-root spelling".  resolvePath() is the ONE place every
path takes (resolvePathImpl() is called from nowhere else), so that is
where the counters live.

WHY THE TEST IS THIS STRICT.  The first version of this test was checked by
MUTATION: five ways of breaking the property were written, and the test
caught ONE of them.  So each assertion below is aimed at one of those five,
and the extractor is checked against its own output so that a broken
extractor fails the test instead of passing it vacuously.
"""

import re
from pathlib import Path


def mask(text):
    """Blank out comments and literals, keeping every other character.

    The result has the same length as the input, so a slice of the masked
    text is the same slice of the real text.  Brace matching on the masked
    text cannot be confused by a brace inside a comment or a string.
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        ch = text[index]
        if ch == "/" and index + 1 < length and text[index + 1] == "/":
            while index < length and text[index] != "\n":
                out[index] = " "
                index += 1
            continue
        if ch == "/" and index + 1 < length and text[index + 1] == "*":
            while index < length and not (text[index] == "*" and index + 1 < length
                                          and text[index + 1] == "/"):
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            for _ in range(2):
                if index < length:
                    out[index] = " "
                    index += 1
            continue
        if ch in ('"', "'"):
            quote = ch
            index += 1
            while index < length:
                if text[index] == "\\":
                    out[index] = " "
                    if index + 1 < length:
                        out[index + 1] = " "
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                if text[index] != "\n":
                    out[index] = " "
                index += 1
            continue
        index += 1
    return "".join(out)


def function_slice(text, masked, signature):
    """The raw text of the function whose definition starts at signature."""
    start = masked.find(signature)
    assert start >= 0, "no definition of " + signature
    open_brace = masked.find("{", start)
    assert open_brace >= 0, "no body for " + signature
    depth = 0
    for index in range(open_brace, len(masked)):
        ch = masked[index]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1], masked[start:index + 1]
    raise AssertionError("unbalanced braces after " + signature)


def main():
    root = Path(__file__).resolve().parents[1]
    fs = (root / "source" / "guest_filesystem.cpp").read_text(encoding="utf-8")
    hpp = (root / "include" / "rr3" / "guest_filesystem.hpp").read_text(encoding="utf-8")
    view = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    census = (root / ".." / "tools" / "Test-OnEden.ps1").resolve().read_text(encoding="utf-8")

    fs_masked = mask(fs)

    # 0. THE EXTRACTOR ITSELF.  A test whose extractor silently returns the
    #    wrong slice passes everything, so its output is checked: the
    #    resolver is a big function and a brace-counting mistake would make
    #    it thousands of characters too long or too short.
    resolve_raw, resolve_masked = function_slice(fs, fs_masked, "bool resolvePath(")
    assert 2000 < len(resolve_raw) < 20000, \
        "the extractor returned %d chars for resolvePath()" % len(resolve_raw)
    assert resolve_masked.count("{") == resolve_masked.count("}"), \
        "the extracted body is not balanced"

    # 1. The counters exist and are exposed.
    assert "unsigned long long g_startLightsOpens = 0;" in fs
    assert "unsigned long long g_raceScreenOpens = 0;" in fs
    assert "unsigned long long guestStartLightsOpens();" in hpp
    assert "unsigned long long guestRaceScreenOpens();" in hpp

    # 2. THE COUNTERS ARE COUNTED IN resolvePath(), THE ONE PLACE EVERY PATH
    #    TAKES.  MUTANT E: moving them back into the trace function (or into
    #    any single branch) must fail here.
    for counter in ("g_startLightsOpens", "g_raceScreenOpens",
                    "g_guiLayoutOpens", "g_guiTwosquareOpens",
                    "g_resolvedPathCount"):
        assert ("__atomic_add_fetch(&" + counter) in resolve_masked, \
            "the counter " + counter + " is not counted in resolvePath()"
    trace_raw, trace_masked = function_slice(fs, fs_masked, "void logResolvedPathOnce(")
    for counter in ("g_startLightsOpens", "g_raceScreenOpens",
                    "g_guiLayoutOpens", "g_guiTwosquareOpens",
                    "g_resolvedPathCount"):
        assert ("__atomic_add_fetch(&" + counter) not in trace_masked, \
            "the counter " + counter + " is counted in the trace function again"

    # 3. THE MARKERS ARE THE RIGHT STRINGS, AND EACH INCREMENT IS INSIDE ITS
    #    GUARD.  MUTANT A (the guard removed) and MUTANT B (a typo in the
    #    marker) must both fail here.
    for marker, counter in (
            ("gui/processed/StartLightsScreen.gui", "g_startLightsOpens"),
            ("gui/processed/InGameScreen.gui", "g_raceScreenOpens"),
            ("gui/processed/main_menu/", "g_guiLayoutOpens"),
            ("main_menu_twosquare.gui", "g_guiTwosquareOpens")):
        marker_at = resolve_raw.find(marker)
        assert marker_at >= 0, "the marker " + marker + " is not in resolvePath()"
        increment_at = resolve_masked.find("__atomic_add_fetch(&" + counter)
        assert increment_at >= 0, "no increment for " + counter
        between = resolve_masked[marker_at:increment_at]
        assert "{" in between, \
            "the increment of " + counter + " is not inside a guard"
        assert ";" not in between, \
            "the increment of " + counter + " is outside its guard"

    # 4. THE CENSUS LINE CARRIES THE TWO NUMBERS AT ITS HEAD.  The log is
    #    written by two threads into one file and a line can be SPLIT; the
    #    fields at the END of a line are the ones a split can separate from
    #    their line.
    assert '"frame=%llu lights=%llu race=%llu census bound=%d' in view, \
        "the race numbers are not at the head of the census line"
    assert '" gui=%llu twosq=%llu",' in view, \
        "the census line's tail still holds the race numbers"
    assert "rr3::guestStartLightsOpens()" in view
    assert "rr3::guestRaceScreenOpens()" in view

    # 5. THE ROUND CENSUS READS THE PORT'S COUNTERS.  MUTANT C (the census
    #    assigns the markers from the text count again) and MUTANT D (it
    #    takes the FIRST match instead of the last) must fail here.
    assert "' gui=(\\d+)(?: |\\r?$)'" in census
    assert "' lights=(\\d+)(?: |\\r?$)'" in census
    assert "' race=(\\d+)(?: |\\r?$)'" in census
    assert "$summary.msStartLights = [regex]::Matches(" not in census
    assert "$summary.msRaceScreen  = [regex]::Matches(" not in census
    assert "$summary.msStartLights = [int]$portStartLights[" in census
    assert "$portStartLights[$portStartLights.Count - 1]" in census
    assert "$portRaceScreen[$portRaceScreen.Count - 1]" in census
    assert "$portStartLights[0]" not in census
    assert "$portRaceScreen[0]" not in census

    # 6. THE CENSUS READS THE COMPLETE CENSUS LINES ONLY.  A spliced line's
    #    tail belongs to no line at all, and the numbers at the end of a
    #    line are the ones a split can take away.
    assert "(?m)^phase=guest-frame-census" in census, \
        "the census does not select the complete census lines"

    # 7. THE SUMMARY SAYS WHERE EACH NUMBER CAME FROM, and says WHEN it was
    #    sampled.  A number must never wear the wrong label, and the age of
    #    a sample must be visible.
    assert "$summary.msRaceMarkerSource = 'port'" in census
    assert "$summary.msRaceMarkerSource = 'text'" in census
    assert "$summary.msFrontEndSource = 'port'" in census
    assert "$summary.msFrontEndSource = 'text'" in census
    assert "$summary.msCensusFrame" in census
    assert "counted by the {1}" in census
    # The old counts survive only as a labelled cross-check.
    assert "msStartLightsText" in census
    assert "msRaceScreenText" in census

    print("PASS: the race markers are counted by the port, at the resolver, "
          "and the census reads them off the complete census lines")


if __name__ == "__main__":
    main()
