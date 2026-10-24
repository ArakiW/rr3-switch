#!/usr/bin/env python3
"""Build a frame timeline of guest events from an archived guest-view.log.

The guest-view.log is written by several threads at once, so a line's position
in the file is NOT its time. The port's own input-door line carries the frame
counter ("f=NNN"), so the timeline is built by walking the file once and
attaching every event to the nearest input-door frame at or before it.

Usage: race_timeline.py <guest-view.log>
"""

import re
import sys

FRAME_RE = re.compile(r'phase=input-door .*?\bf=(\d+)\b')
EVENTS = [
    ("start-lights", re.compile(r'StartLightsScreen\.gui')),
    ("in-game-screen", re.compile(r'InGameScreen\.gui')),
    ("race-start", re.compile(r'RaceStartScreen\.gui')),
    ("pre-race", re.compile(r'PreRaceScreen\.gui')),
    ("race-complete", re.compile(r'RaceCompleteScreen\.gui')),
    ("results", re.compile(r'(SoloResults\.gui|ResultsContainer\.gui|StandardRaceResultRow\.gui)')),
    ("postrace-ad", re.compile(r'interstitial_postrace')),
    ("lap-saved", re.compile(r'(?i)laptime|lap count|CURRENT_LAP')),
    ("car-finished", re.compile(r'(?i)finished in %|finished in [0-9]')),
    ("false-start", re.compile(r'FalseStartScreen\.gui')),
    ("pause", re.compile(r'PauseScreen\.gui')),
    ("hud-reload", re.compile(r'Reload HUD planes')),
    ("objectives", re.compile(r'Hud_Objective\.gui')),
    ("localstyles-fail", re.compile(r"unable to load file: 'gui/processed/LocalStyles")),
    ("race-position", re.compile(r'RacePositionScreen\.gui')),
]


def main() -> int:
    path = sys.argv[1]
    frame = 0
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            m = FRAME_RE.search(line)
            if m:
                frame = int(m.group(1))
                continue
            for name, rx in EVENTS:
                if rx.search(line):
                    rows.append((frame, lineno, name, line.strip()[:150]))
                    break
    print(f"# events: {len(rows)}")
    for frame, lineno, name, text in rows:
        safe = text.encode("unicode_escape").decode("ascii")
        print(f"f={frame:<7} line={lineno:<8} {name:<18} {safe}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
