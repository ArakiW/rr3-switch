#!/usr/bin/env python3
"""The guest-action command table must hold a whole experiment, and a command
that does not fit must be reported.

THE BUG
-------
The control file is read ONCE, at the start of the run, into a table of
EIGHT pending commands. A ninth line was skipped SILENTLY: the parser did
`if (name[0] == '\0' || g_pendingCount >= kMaxPendingActions) { continue; }`
and said nothing.

Two runs were wasted on it. The control file had eleven commands, the log
said "pending=8", and the four commands that were supposed to START A RACE
never ran at all - which read exactly like "the guest's race commands do not
work", and sent the next two experiments after the wrong thing.

WHAT THIS PINS
--------------
  1. the table is large enough for a whole experiment (32);
  2. a command that does not fit is LOGGED with a reason, so a dropped
     command is never indistinguishable from a command that was never
     written;
  3. the log line that reports the table's contents says how many commands
     were read, so the number in the log can be compared with the number of
     lines in the control file.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source" / "guest_action.cpp"

MIN_COMMANDS = 32


def body_of(source, signature):
    start = source.find(signature)
    assert start >= 0, f"{signature} is not in the source"
    depth = 0
    seen = False
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
            seen = True
        elif source[i] == "}":
            depth -= 1
            if seen and depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"{signature} has no closing brace")


def main() -> int:
    if not SOURCE.exists():
        print(f"SKIP: {SOURCE} is not there")
        return 0
    source = SOURCE.read_text(encoding="utf-8")

    # 1. THE TABLE IS BIG ENOUGH.
    m = re.search(r"constexpr\s+size_t\s+kMaxPendingActions\s*=\s*(\d+)\s*;", source)
    assert m, "kMaxPendingActions is not defined"
    capacity = int(m.group(1))
    assert capacity >= MIN_COMMANDS, (
        f"the command table holds {capacity} commands; the control file is "
        f"read once, so anything past that is lost for the whole run")

    # 2. A COMMAND THAT DOES NOT FIT IS LOGGED.
    read = body_of(source, "void readActionConf(")
    check = re.search(r"if\s*\(\s*g_pendingCount\s*>=\s*kMaxPendingActions\s*\)"
                      r"\s*\{(.*?)\}", read, re.S)
    assert check, (
        "the parser does not check the table's capacity at all")
    assert "logAction" in check.group(1), (
        "a command that does not fit is dropped without a word; a dropped "
        "command is indistinguishable from a command that was never written")
    assert "dropped=1" in read, (
        "the drop is not marked as a drop in the log")
    assert re.search(r"reason=\S*the-command-table-is-full|"
                      r"the-command-table-is-full", read), (
        "the drop does not say WHY the command was dropped")

    # 3. THE LOG SAYS HOW MANY COMMANDS WERE READ.
    assert re.search(r'pending=%zu', source), (
        "the log does not report how many commands were read, so a truncated "
        "control file cannot be spotted")

    print(f"PASS: the command table holds {capacity} commands, a command that "
          f"does not fit is logged with a reason, and the log reports how "
          f"many commands were read")
    return 0


if __name__ == "__main__":
    sys.exit(main())
