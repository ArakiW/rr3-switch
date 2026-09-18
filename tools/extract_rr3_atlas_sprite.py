#!/usr/bin/env python3
"""Extract an exact named sprite from a decoded RR3 atlas image."""

from __future__ import annotations

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("atlas", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("sprite")
    parser.add_argument("output", type=Path)
    parser.add_argument("--origin", choices=("bottom-left", "top-left"),
                        default="bottom-left")
    args = parser.parse_args()

    root = ET.parse(args.atlas).getroot()
    entry = next((node for node in root.findall("image")
                  if node.attrib.get("name") == args.sprite), None)
    if entry is None:
        raise SystemExit(f"sprite not found: {args.sprite}")
    position = entry.find("pos")
    if position is None:
        raise SystemExit("sprite has no atlas position")
    x = int(position.attrib["x"])
    y = int(position.attrib["y"])
    width = int(entry.attrib["width"])
    height = int(entry.attrib["height"])

    with Image.open(args.image) as atlas_image:
        top = y if args.origin == "top-left" else atlas_image.height - y - height
        sprite = atlas_image.crop((x, top, x + width, top + height))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        sprite.save(args.output)
    print(f"extracted {args.sprite}: {width}x{height} at {x},{y}")


if __name__ == "__main__":
    main()
