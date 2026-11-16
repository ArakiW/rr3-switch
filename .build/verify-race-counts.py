#!/usr/bin/env python3
"""Read the race-marker numbers straight out of the archived Eden rounds."""
import io, json, os, re, sys

ROOT = r"E:\AI\RealRacing3-switch"
RUNS = os.path.join(ROOT, ".build", "eden-runs")

def scan(tag):
    base = os.path.join(RUNS, tag)
    log = None
    for name in os.listdir(base):
        if name.endswith("guest-view.log"):
            log = os.path.join(base, name)
    if log is None:
        return {"tag": tag, "error": "no guest-view.log"}
    with io.open(log, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    lines = text.split("\n")
    sl = [l for l in lines if "StartLightsScreen" in l]
    rg = [l for l in lines if "InGameScreen" in l]
    lights = re.findall(r" lights=(\d+)(?:\s|$)", text)
    race = re.findall(r" race=(\d+)(?:\s|$)", text)
    ordinal = [l for l in lines if "guest-log ordinal" in l]
    return {
        "tag": tag,
        "log_bytes": os.path.getsize(log),
        "log_name": os.path.basename(log),
        "StartLightsScreen_occurrences": len(sl),
        "InGameScreen_occurrences": len(rg),
        "sl_are_port_path_traces": all((".depot/gui/processed/" in l and "phase=map-app" in l) for l in sl) if sl else None,
        "rg_are_port_path_traces": all((".depot/gui/processed/" in l and "phase=map-app" in l) for l in rg) if rg else None,
        "sl_sample": sl[0][:220] if sl else None,
        "rg_sample": rg[0][:220] if rg else None,
        "lights_values_first5": lights[:5],
        "lights_last": lights[-1] if lights else None,
        "lights_samples": len(lights),
        "race_last": race[-1] if race else None,
        "race_samples": len(race),
        "ordinal_tail": ordinal[-1][:200] if ordinal else None,
    }

out = []
for tag in sys.argv[1:]:
    out.append(scan(tag))
print(json.dumps(out, indent=2, ensure_ascii=False))
