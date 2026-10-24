#!/usr/bin/env python3
"""Measure the car's motion between framebuffer captures.

The race HUD's numbers are not drawn in this build, so the framebuffer is the
only place the car's motion shows. The camera is locked to the car, so the CAR
barely moves on screen while the WORLD slides past: how much the ground region
changes between two captures is how far the car travelled, and the same change
divided by the frame gap is a speed proxy.

The measurement region is the ground on BOTH SIDES of the car and BELOW the
horizon, because the car occupies the bottom centre of the frame and the sky
is a flat field that carries no translation signal at all.

Usage: motion.py <frames-dir>
"""

import os
import re
import sys

import numpy as np
from PIL import Image

FRAME_RE = re.compile(r"frame-(\d+)\.png$")

# The region that carries the world's motion: the road and the barriers, to
# the left and to the right of the car. Rows 380..720, columns 0..430 and
# 980..1280. The car's silhouette sits in the middle of the frame and would
# otherwise dominate the difference.
BANDS = [(380, 720, 0, 430), (380, 720, 980, 1280)]


def load_gray(path):
    with Image.open(path) as im:
        return np.asarray(im.convert("L"), dtype=np.float64)


def region(img):
    return np.concatenate([img[r0:r1, c0:c1].ravel() for (r0, r1, c0, c1) in BANDS])


def shift_between(a, b):
    """Best integer translation of a onto b, and how much of the image changed."""
    A = np.concatenate([a[r0:r1, c0:c1] for (r0, r1, c0, c1) in BANDS], axis=1)
    B = np.concatenate([b[r0:r1, c0:c1] for (r0, r1, c0, c1) in BANDS], axis=1)
    win = np.hanning(A.shape[0])[:, None] * np.hanning(A.shape[1])[None, :]
    A = (A - A.mean()) * win
    B = (B - B.mean()) * win
    FA = np.fft.fft2(A)
    FB = np.fft.fft2(B)
    R = FA * np.conj(FB)
    mag = np.abs(R)
    mag[mag == 0] = 1.0
    corr = np.fft.ifft2(R / mag).real
    peak = np.unravel_index(np.argmax(corr), corr.shape)
    dy = peak[0] if peak[0] <= corr.shape[0] // 2 else peak[0] - corr.shape[0]
    dx = peak[1] if peak[1] <= corr.shape[1] // 2 else peak[1] - corr.shape[1]
    # Sharpness: the peak against the mean of the correlation map. A real
    # match stands well above the noise floor; a flat map means "no
    # correspondence", and the shift it reports is meaningless.
    sharp = float(corr[peak] / (corr.mean() + 1e-9))
    return dx, dy, sharp


def main() -> int:
    folder = sys.argv[1]
    names = []
    for n in os.listdir(folder):
        m = FRAME_RE.search(n)
        if m:
            names.append((int(m.group(1)), os.path.join(folder, n)))
    names.sort()

    prev = None
    print("frame      dx     dy    |d|   sharp   groundMAD%")
    for frame, path in names:
        img = load_gray(path)
        if prev is None:
            print(f"{frame:<9} (first)")
            prev = (frame, img)
            continue
        pf, pimg = prev
        dx, dy, sharp = shift_between(pimg, img)
        mad = float(np.abs(region(img) - region(pimg)).mean())
        gap = max(1, frame - pf)
        print(f"{frame:<9} {dx:<6} {dy:<5} {(dx*dx+dy*dy) ** 0.5:<5.1f} "
              f"{sharp:<7.2f} {mad / 255 * 100:<8.3f}  gap={gap}")
        prev = (frame, img)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
