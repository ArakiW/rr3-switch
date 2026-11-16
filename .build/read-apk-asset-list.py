#!/usr/bin/env python3
"""The guest reads files/apk/res/asset_list_audio.txt -- from the APK."""
import io, os, zipfile
apk = r"E:\AI\RealRacing3-switch\带鉴别包的14.0.1_row_RealRacingArchive.apk"
with zipfile.ZipFile(apk) as z:
    names = [n for n in z.namelist() if "asset_list" in n]
    print("asset list entries in the APK:", len(names))
    for n in names[:12]:
        print("   ", n, z.getinfo(n).file_size)
    # the audio one
    cand = [n for n in names if n.endswith("asset_list_audio.txt")]
    for n in cand:
        data = z.read(n).decode("utf-8", "replace")
        lines = [l for l in data.split("\n") if l.strip()]
        print("\n---", n, "lines:", len(lines), "---")
        for l in lines[:4]:
            print("   ", l[:160])
        dep = [l for l in lines if ".decomp" in l]
        print("   .decomp entries:", len(dep))
        for l in dep[:4]:
            print("     ", l[:200])
        v8 = [l for l in lines if "v8_" in l]
        print("   entries mentioning v8_:", len(v8))
        for l in v8[:4]:
            print("     ", l[:200])
