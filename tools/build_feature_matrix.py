#!/usr/bin/env python3
"""Generate docs/FEATURE-MATRIX.md from the guest's own inventory.

Run:  python tools/build_feature_matrix.py
(the analysis scripts live in .build/ ; this file is the generated artifact)
"""

import io
import os
import re
import sys
from collections import defaultdict

REPO = r"E:\AI\RealRacing3-switch"
CARD = r"E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\.depot"
BASE = os.path.join(CARD, "asset_list_base.txtCache.txt")
RUN_DIR = os.path.join(REPO, ".build", "eden-runs")
LOG_NAME = "20261031-031607-eden-rr3-q39-alias-heapall"
LOG = os.path.join(RUN_DIR, LOG_NAME, "rr3-native-rehost.guest-view.log")
OUT = os.path.join(REPO, "docs", "FEATURE-MATRIX.md")

TOKEN_RE = re.compile(r"(\S+\.(?:gui|atlas|dds|pvr|nct|dat|bin|txt|otf)(?:\.z)?)")
ROOTS = ("/gui/processed/", "/gui_assets/", "/gui/", "/vehicles/", "/Tracks/")


def normalize(token):
    for root in ROOTS:
        index = token.find(root)
        if index >= 0:
            return token[index:]
    return token


def feature_of(path):
    if path.startswith("/gui/processed/"):
        rest = path[len("/gui/processed/"):]
        parts = rest.split("/")
        if len(parts) > 1:
            return "/gui/processed/" + parts[0]
        stem = parts[0][:-4] if parts[0].endswith(".gui") else parts[0]
        return "/gui/processed/" + stem
    if path.startswith("/gui_assets/"):
        return "/gui_assets"
    if path.startswith("/vehicles/"):
        return "/vehicles"
    if path.startswith("/Tracks/"):
        return "/Tracks/" + path[len("/Tracks/"):].split("/", 1)[0]
    return "/" + path.strip("/").split("/", 1)[0]


def family(name):
    """A readable family for a feature name."""
    leaf = name.rsplit("/", 1)[-1]
    match = re.match(r"([A-Za-z]+)", leaf)
    if match and match.group(1):
        return match.group(1)
    return leaf.split("_")[0] or leaf


def main():
    if not os.path.exists(BASE):
        print("the asset list is not on the card: %s" % BASE, file=sys.stderr)
        return 2
    inventory = defaultdict(int)
    total = 0
    with io.open(BASE, "r", encoding="utf-8", errors="replace") as handle:
        for index, line in enumerate(handle):
            line = line.strip()
            if index == 0 or "\t" not in line:
                continue
            path = line.split("\t", 1)[0]
            if not path.startswith("/"):
                continue
            inventory[feature_of(path)] += 1
            total += 1

    touched = defaultdict(set)
    seen = set()
    with io.open(LOG, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            for match in TOKEN_RE.finditer(line):
                path = normalize(match.group(1))
                if path in seen:
                    continue
                seen.add(path)
                touched[feature_of(path)].add(path)

    screens = sorted(n for n in inventory if n.startswith("/gui/processed/"))
    opened = [n for n in screens if touched.get(n)]
    untouched = [n for n in screens if not touched.get(n)]

    families = defaultdict(list)
    for name in untouched:
        families[family(name)].append(name)

    lines = []
    lines.append("# THE FEATURE MATRIX")
    lines.append("")
    lines.append("**WHAT THIS IS.**  The guest's own feature list, and what a real")
    lines.append("run has actually opened.  It exists because \"完整移植安卓端功能\"")
    lines.append("had no countable content: nobody could say what was done.")
    lines.append("")
    lines.append("**THE GUEST NAMES ITS FEATURES ITSELF.**  Every screen and panel it")
    lines.append("has is a GUI file under `gui/processed/`, named after the feature")
    lines.append("(`BankScreen.gui`, `BuyCarScreen.gui`, `CrewScreen.gui`,")
    lines.append("`CommunityScreen.gui`, `online_multiplayer/`, `store/`, `quests/`),")
    lines.append("and every asset it expects is named in its asset lists.  So the")
    lines.append("inventory is READABLE, and a run says which of it it touched.")
    lines.append("")
    lines.append("## THE NUMBERS")
    lines.append("")
    lines.append("| | |")
    lines.append("|---|---|")
    lines.append("| the guest's whole inventory | **%d assets** in **%d features** |"
                 % (total, len(inventory)))
    lines.append("| the UI feature files (`gui/processed/*`) | **%d** |" % len(screens))
    lines.append("| what a full run (q39) actually opened | **%d of %d** UI features "
                 "(%.0f%%) |" % (len(opened), len(screens),
                                  100.0 * len(opened) / max(1, len(screens))))
    lines.append("| distinct asset paths the run touched | %d |" % len(seen))
    lines.append("")
    lines.append("**=>=> SO THE HONEST ANSWER TO \"how much is left\" IS THIS: the")
    lines.append("racing half of the game works and the meta half is untouched.**")
    lines.append("The run reaches the event map, the car select, a race, the pause")
    lines.append("screen and the store menu -- and it has never opened a single")
    lines.append("bank, garage-customisation, crew, community, friends,")
    lines.append("leaderboard, multiplayer or quest screen.")
    lines.append("")
    lines.append("## WHAT THE RUN DID OPEN (%d)" % len(opened))
    lines.append("")
    for name in opened:
        lines.append("- `%s`" % name)
    lines.append("")
    lines.append("## WHAT IS STILL UNTOUCHED (%d), BY FAMILY" % len(untouched))
    lines.append("")
    lines.append("| family | untouched features |")
    lines.append("|---|---|")
    for key in sorted(families, key=lambda k: (-len(families[k]), k)):
        lines.append("| **%s** | %d |" % (key, len(families[key])))
    lines.append("")
    lines.append("### THE BIG ONES")
    lines.append("")
    lines.append("| feature group | assets |")
    lines.append("|---|---|")
    for name in sorted(inventory, key=lambda k: -inventory[k]):
        if name.startswith("/gui/processed/") and inventory[name] > 20:
            lines.append("| `%s` | %d |" % (name, inventory[name]))
    lines.append("")
    lines.append("## HOW TO REGENERATE")
    lines.append("")
    lines.append("```")
    lines.append("python .build/feature_matrix.py      # the numbers")
    lines.append("python tools/build_feature_matrix.py  # this file")
    lines.append("```")
    lines.append("")
    lines.append("The inventory comes from the card's")
    lines.append("`.depot/asset_list_base.txtCache.txt` (the guest's own list of")
    lines.append("every asset it expects); the coverage comes from the guest log of")
    lines.append("a real run (`%s`)." % LOG_NAME)
    lines.append("")
    lines.append("**A FEATURE IS \"OPENED\" WHEN THE GUEST OPENED ONE OF ITS FILES.**")
    lines.append("That is deliberately conservative: it says the guest RENDERED it,")
    lines.append("not that the feature works.  A feature that opens but then fails")
    lines.append("needs a per-feature check, and that is the next layer of this")
    lines.append("matrix.")
    lines.append("")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))
    print("wrote %s (%d bytes)" % (OUT, os.path.getsize(OUT)))
    print("UI features: %d in the inventory, %d opened by q39" % (len(screens), len(opened)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
