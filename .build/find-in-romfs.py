"""Are the track detail maps in the volumes at all?

WHY: the guest shows "error loading track data, please reinstall" and the
port answers ENOENT for

    Tracks/common/detail/detailmap_road_surface_norm_a.dxt.dds.z
    Tracks/common/detail/detailmap_road_surface_norm_a.dxt.dds
    Tracks/common/detail/detailmap_road_surface_norm_a.etc.dds.z
    Tracks/common/detail/detailmap_road_surface_norm_a.etc.dds

with the primary candidate rr3track:/files/.depot/Tracks/... .  So either the
volumes do not ship those files (a DATA gap) or the port does not route them
(a RESOLUTION gap), and the two have opposite fixes.  The romfs images store
the path names as plain bytes, so a byte search answers it.
"""
import io
import mmap
import os
import sys

NAMES = [
    b"detailmap_road_surface_norm_a.dxt.dds.z",
    b"detailmap_road_surface_norm_a.dxt.dds",
    b"detailmap_road_surface_norm_a.etc.dds.z",
    b"detailmap_road_surface_norm_a.etc.dds",
    b"Tracks/common/detail",
    b"common/detail",
    b"detailmap_road_surface",
]


def search(path):
    hits = {n: 0 for n in NAMES}
    with io.open(path, "rb") as handle:
        size = os.path.getsize(path)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
            for name in NAMES:
                start = 0
                while True:
                    found = mapped.find(name, start)
                    if found < 0:
                        break
                    hits[name] += 1
                    start = found + 1
                    if hits[name] > 5000:
                        break
    return hits


def main():
    for target in sys.argv[1:]:
        if not os.path.exists(target):
            print("%s: MISSING" % target)
            continue
        print("%s  (%.2f GiB)" % (os.path.basename(target),
                                  os.path.getsize(target) / 1073741824.0))
        hits = search(target)
        for name in NAMES:
            print("   %-42s %d" % (name.decode("ascii", "replace"), hits[name]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
