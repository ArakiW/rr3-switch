"""Append section 9.37 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the thinning of the path trace was measured and REVERTED (it made the
census worse, because the census counts the log's text), and the warning tail
in guest_filesystem.cpp is a pre-existing class that only a full recompile
surfaces.  Both belong on the record.
"""
import io
import sys

SECTION = """
### 9.37 The thinning was measured, reverted, and the real fix is named

**MEASURED 2026-11-10, round slim1: the path trace thinned to the
`gui/processed/` subtree.**

    round     front-end layout  main menu layouts  EventMapScreen  race  lights
    yield2            15                75               300          36     27
    slim1              1                 5               152           1      1

**⇒ ⇒ THE THINNING CUT THE LOG AND BLINDED THE CENSUS**, because the census
COUNTS THE LOG'S TEXT (`tools/Test-OnEden.ps1` counts
'gui/processed/main_menu/main_menu_twosquare.gui' in the guest-view log), and
the thinned trace no longer contains the repeated main-menu opens the count
depends on.  **⇒ ⇒ SO THE THINNING IS REVERTED.**

**⇒ ⇒ AND THE FIX IT NAMED IS THE ONE THIS PROJECT KEEPS RE-ARRIVING AT: a
measurement that reads a STRING out of the log is only as good as the log it
reads.**  Every census counter in `Test-OnEden.ps1` is a regex over the
guest-view log, so any change to what the port writes turns the counter into a
lie.  **⇒ ⇒ THE CENSUS MUST READ A COUNTER THE PORT KEEPS.**  The port
already counts path resolutions (`paths=` on the race-state line, added with
this round); the counters the census wants are the next step, and then the
trace can be thinned for real.

**⇒ ⇒ AND THE WARNING TAIL IS A PRE-EXISTING CLASS.**  MEASURED 2026-11-10:
`guest_filesystem.cpp` had not been fully recompiled since the incremental
build was introduced, so its `-Wformat-truncation=` warnings were invisible --
they are all the same thing (a guest-supplied path or error string formatted
into a fixed buffer that is a little too small) and they surface one by one as
the file is touched.  Twenty-four are fixed, with explicit precisions and one
`ino_t` truncation (a value written into a 16-bit field, which the compiler
had been reporting since before this session).  **⇒ ⇒ A TAIL REMAINS, AND IT IS
DIAGNOSTIC-ONLY**: a truncated log line, not a byte the guest receives.  It is
listed here rather than hidden, because a build that is clean for the files you
touched and dirty for the file you did not is exactly how the "the build is
warning free" claim got wrong in the first place.

**⇒ ⇒ AND THE RULE THAT PRODUCED ALL OF IT:** a diagnostic that is
load-bearing must be made CHEAP, not REMOVED; and a measurement that reads a
string out of a log must be moved onto a counter before the log is allowed to
change.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.37 The thinning was measured, reverted" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
