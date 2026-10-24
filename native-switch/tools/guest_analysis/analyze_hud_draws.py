#!/usr/bin/env python3
"""Mine a guest-view.log for the HUD's 2D-program draws.

The text probe reports, per draw:
  phase=hud-draw ordinal=<texture> target=<count> ... tex=<id> texWH=<w>x<h>
      prog=<program> fbo=<n> vp=... u_CurrentColour(...) attr0=a_Position ...
  phase=text-quad ordinal=<texture> target=<drawOrdinal> ...
      ndc= ... rect=<x>,<y>,<w>,<h> onScreen=<0|1>
      readback=<w>x<h> at=<x>,<y> whiteBefore=<n> whiteAfter=<n>
      maxRBefore=<n> maxRAfter=<n> uniqAfter=<n> first=r,g,b,a

The two lines of one draw are adjacent: the probe logs the "before" state
first and the "after" state from the post-draw hook.

The GL readback origin is BOTTOM-LEFT. The captured PNG is TOP-LEFT. Every
rectangle comparison has to flip Y.

Usage:
    python analyze_hud_draws.py <archive-dir> [--png-frame N] [--window from,to]
"""

import argparse
import json
import os
import re
import sys

KV = re.compile(r"(\w+)=(-?\d+(?:,-?\d+)*|-?[\w.+\-]+|\[[^\]]*\]|not-probed|none|null)")


def parse_kv(details):
    out = {}
    for m in KV.finditer(details):
        out[m.group(1)] = m.group(2)
    return out


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for index, line in enumerate(handle):
            line = line.rstrip("\n")
            if not line.startswith("phase="):
                continue
            head, _, rest = line.partition(" ")
            phase = head.split("=", 1)[1]
            m = re.match(r"ordinal=(\d+) target=([0-9a-fx]+)\s*(.*)", rest)
            if not m:
                rows.append((index, phase, None, rest))
                continue
            rows.append((index, phase, (int(m.group(1)), int(m.group(2), 16),
                                        m.group(3)), None))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--png-frame", type=int, default=None)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log in %s" % args.archive)
        return 1
    rows = load(log)

    # --- the race window, measured from the guest's own markers -------------
    markers = {}
    for index, phase, parsed, _ in rows:
        if parsed is None:
            continue
        details = parsed[2]
        if "InGameScreen" in details or "Reload HUD planes" in details:
            markers.setdefault(phase, []).append(index)
    for phase, hits in markers.items():
        print("marker %-16s lines %s" % (phase, hits[:12]))

    # --- pair hud-draw with its text-quad ----------------------------------
    draws = []
    for pos, (index, phase, parsed, _) in enumerate(rows):
        if phase != "hud-draw" or parsed is None:
            continue
        ordinal, target, details = parsed
        kv = parse_kv(details)
        quad = None
        quad_line = None
        for j in range(pos + 1, min(pos + 4, len(rows))):
            if rows[j][1] == "text-quad":
                qp = rows[j][2]
                quad = parse_kv(qp[2])
                quad["_drawOrdinal"] = qp[1]
                quad_line = rows[j][0]
                break
        draws.append({
            "line": index,
            "quadLine": quad_line,
            "drawOrdinal": quad.get("_drawOrdinal") if quad else None,
            "tex": int(kv.get("tex", "0")),
            "texWH": kv.get("texWH", "?"),
            "prog": kv.get("prog", "?"),
            "fbo": kv.get("fbo", "?"),
            "vp": kv.get("vp", "?"),
            "count": kv.get("count", "?"),
            "colour": None,
            "rect": quad.get("rect") if quad else None,
            "onScreen": quad.get("onScreen") if quad else None,
            "rb": None,
            "raw": details,
        })
        # u_CurrentColour
        m = re.search(r"u_CurrentColour\(type=0x[0-9a-f]+ \[([^\]]*)\]", details)
        if m:
            draws[-1]["colour"] = m.group(1)
        if quad:
            m = re.search(r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) "
                          r"whiteAfter=(\d+) maxRBefore=(\d+) maxRAfter=(\d+) "
                          r"uniqAfter=(\d+)", quad.get("_", "") or "")
            if not m:
                m = re.search(r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) "
                              r"whiteAfter=(\d+) maxRBefore=(\d+) maxRAfter=(\d+) "
                              r"uniqAfter=(\d+)", rows[pos + 1][2][2])
            if m:
                draws[-1]["rb"] = {
                    "w": int(m.group(1)), "h": int(m.group(2)),
                    "x": int(m.group(3)), "y": int(m.group(4)),
                    "whiteBefore": int(m.group(5)), "whiteAfter": int(m.group(6)),
                    "maxRBefore": int(m.group(7)), "maxRAfter": int(m.group(8)),
                    "uniqAfter": int(m.group(9)),
                }

    print("\n== %d hud-draw lines ==" % len(draws))
    for d in draws:
        rb = d["rb"]
        changed = ""
        if rb:
            changed = "PAINTED" if (rb["whiteAfter"] != rb["whiteBefore"] or
                                    rb["maxRAfter"] != rb["maxRBefore"] or
                                    rb["uniqAfter"] > 1) else "no-change"
        print("line=%-7s draw#=%-7s tex=%-5s %-9s prog=%-4s fbo=%-3s count=%-3s "
              "rect=%-16s rb=%-14s %s colour=%s"
              % (d["line"], d["drawOrdinal"], d["tex"], d["texWH"], d["prog"],
                 d["fbo"], d["count"], d["rect"],
                 ("%sx%s@%s,%s" % (rb["w"], rb["h"], rb["x"], rb["y"])) if rb else "-",
                 changed, d["colour"]))

    if args.json:
        print(json.dumps(draws, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
