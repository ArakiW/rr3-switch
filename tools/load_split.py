# Split a run's frames into "loading" and "after loading".
#
# WHY: comparing a round's average frame rate against another round's is
# misleading when the two rounds are in different phases.  The October round
# (q11) spent most of its 1206 s IN THE RACE; the November rounds spend it
# LOADING (the game downloads the F1 2024 car and Silverstone asset lists
# first).  The honest comparison is the frame rate AFTER the load finishes.
#
# The guest prints "Total load time to end of load scene_DoIncrementalLoad"
# when the load is done, and the frame census lines carry the frame number.
# So the frame at the milestone is the census line just before it.
#
# Usage: python tools\load_split.py <guest-view.log> [more logs...]

import re
import sys

CENSUS = re.compile(r"phase=guest-frame-census.*?\bframe=(\d+)")
MARK = "Total load time to end of load"


def analyse(path):
    last_frame = -1
    frame_at_load = None
    frames_total = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "phase=guest-frame-census" in line:
                m = CENSUS.search(line)
                if m:
                    last_frame = int(m.group(1))
                    frames_total += 1
            if MARK in line and frame_at_load is None:
                frame_at_load = last_frame
    print(f"{path}")
    print(f"   census lines {frames_total}   last frame {last_frame}")
    if frame_at_load is None:
        print("   the load-complete milestone is not in this log")
        print()
        return
    print(f"   frame at load-complete : {frame_at_load}")
    print(f"   frames after the load  : {last_frame - frame_at_load}")
    print()


for path in sys.argv[1:]:
    analyse(path)
