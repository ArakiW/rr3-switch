#!/usr/bin/env python3
"""Build FAT32-safe read-only RR3 RomFS volumes from an authorized DXT package."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import zipfile


VERSION = "0.21.0"
FAT32_MAX_FILE = 0xFFFFFFFF
BUILD_ROMFS_MAX_FILE = 0x7FFFFFFF
VOLUMES = (
    "core",
    "gui-hd",
    "gui-sd",
    "tracks",
    "vehicles-classic",
    "vehicles-mid",
    "vehicles-modern",
)
SENTINELS = {
    "core": "files/.depot/langs.utf8",
    "gui-hd": "files/.depot/gui_assets/2048x1536/",
    "gui-sd": "files/.depot/gui_assets/960x640/",
    "tracks": "files/.depot/Tracks/Menu/",
    "vehicles-classic": "files/.depot/vehicles/2013_bentley_cgtspeed/",
    "vehicles-mid": "files/.depot/vehicles/2018_porsche_911_gt2_rs/",
    "vehicles-modern": "files/.depot/vehicles/2024_toyota_yaris_wrc/",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def classify(name: str) -> str:
    normalized = name.replace("\\", "/")
    if normalized.startswith("files/.depot/gui_assets/2048x1536/"):
        return "gui-hd"
    if normalized.startswith("files/.depot/gui_assets/"):
        return "gui-sd"
    if normalized.startswith("files/.depot/Tracks/"):
        return "tracks"
    vehicle_prefix = "files/.depot/vehicles/"
    if normalized.startswith(vehicle_prefix):
        vehicle = normalized[len(vehicle_prefix) :].split("/", 1)[0]
        year = int(vehicle[:4]) if vehicle[:4].isdigit() else 0
        if year == 0 or year <= 2015:
            return "vehicles-classic"
        if year <= 2020:
            return "vehicles-mid"
        return "vehicles-modern"
    return "core"


def safe_relative(name: str) -> Path:
    pure = PurePosixPath(name.replace("\\", "/"))
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        raise ValueError(f"unsafe archive path: {name}")
    return Path(*pure.parts)


def extract_volume(archive: zipfile.ZipFile, infos: list[zipfile.ZipInfo],
                   volume: str, staging: Path) -> tuple[int, int]:
    selected = [item for item in infos if not item.is_dir() and classify(item.filename) == volume]
    total = sum(item.file_size for item in selected)
    written = 0
    print(f"[{volume}] extracting {len(selected)} files ({total / 1024**3:.2f} GiB)", flush=True)
    for index, item in enumerate(selected, 1):
        destination = staging / safe_relative(item.filename)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(item) as source, destination.open("wb") as output:
            shutil.copyfileobj(source, output, length=4 * 1024 * 1024)
        written += item.file_size
        if index % 2000 == 0:
            print(f"[{volume}] {index}/{len(selected)} files, {written / 1024**3:.2f} GiB", flush=True)
    return len(selected), total


# The track data the guest's loader looks for under "processed/".
#
# MEASURED 2026-10-25, archives 20261025-162550-eden-f1b and
# 20261025-165527-eden-f1c: loading a race, the guest asks for the track's
# files under "Tracks/<Track>/processed/" -- the collision ("<name>.fmc"),
# the spline ("<name>.spline"), the AI racing line ("<name>.rr_ai") and the
# minimap ("<name>_minimap.m3g") -- while EA's package keeps that same set
# under the VARIANT directory ("circuit", "gp", "indy", ...). The guest's
# own asset list names the variant spelling, so the guest downloads the
# files there and then cannot find them where its loader looks.
#
# The symptom is not "a file is missing" but a refusal: the guest logged
# "Failed to load track spline", "Need to regenerate start_spline as it is
# missing" and "Need to regenerate start_spline but it can only be done on
# Win32", and gave up on the race. The dialog it then shows says "not
# enough memory to load the track", which is why this took three runs to
# find: the message names a resource class, not the thing that is missing.
#
# WHICH OF THEM ARE FATAL, MEASURED IN THE SAME RUN:
#   .fmc     fatal -- "Failed to load collision file" aborts the load;
#   .spline  fatal -- the guest would have to regenerate it, and says it can
#                    only do that on Win32;
#   .rr_ai   not fatal -- the guest logs "Unable to load track ai data" and
#                    carries on with "Using default AI values for ...";
#   the minimap and the .pvs and the .lights files are not fatal either.
# The three cheap ones are mirrored as well, because they are kilobytes and
# they save a run each; the .pvs files (2-4 MB each) are NOT mirrored: the
# tracks volume is already 1.91 GiB against a 2 GiB build_romfs limit.
#
# An existing file is never overwritten, so a track that ships a real
# processed/ tree (Brands_Hatch and Tutorial do) keeps it.
TRACK_DATA_SUFFIXES = (".fmc", ".spline", ".rr_ai")
TRACK_DATA_MINIMAP_SUFFIX = "_minimap.m3g"
TRACK_DATA_SKIP_DIRS = {"processed", "resources"}


def is_track_data_file(name: str) -> bool:
    """True for the files the guest's track loader needs under processed/.

    The minimap is matched by its own name suffix and not by its extension:
    the extension would also match "<name>_variation.m3g", which is a VISUAL
    asset that belongs in processed/high/ and must not be copied.
    """
    return name.endswith(TRACK_DATA_SUFFIXES) or name.endswith(TRACK_DATA_MINIMAP_SUFFIX)


def mirror_track_data_halves(volume_stage: Path) -> tuple[int, int]:
    """Copy each track's data half into its processed/ directory."""
    tracks_root = volume_stage / "files" / ".depot" / "Tracks"
    if not tracks_root.is_dir():
        return 0, 0
    copied = 0
    copied_bytes = 0
    for track in sorted(tracks_root.iterdir()):
        if not track.is_dir():
            continue
        processed = track / "processed"
        for variant in sorted(track.iterdir()):
            if not variant.is_dir() or variant.name in TRACK_DATA_SKIP_DIRS:
                continue
            for source in sorted(variant.rglob("*")):
                if not source.is_file():
                    continue
                if not is_track_data_file(source.name):
                    continue
                relative = source.relative_to(variant)
                destination = processed / relative
                if destination.exists():
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                copied += 1
                copied_bytes += destination.stat().st_size
                print(f"  + {track.name}/processed/{relative}", flush=True)
    return copied, copied_bytes


def build_volume(builder: Path, staging: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(builder), str(staging), str(destination)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise RuntimeError(f"build_romfs failed for {destination.name}: {completed.stderr}")
    if not destination.is_file() or destination.stat().st_size == 0:
        raise RuntimeError(f"build_romfs did not create {destination}")
    if destination.stat().st_size > FAT32_MAX_FILE:
        raise RuntimeError(f"{destination.name} exceeds FAT32 maximum file size")
    if destination.stat().st_size > BUILD_ROMFS_MAX_FILE:
        raise RuntimeError(f"{destination.name} exceeds build_romfs safe 2 GiB range")


def checked_remove_staging(path: Path, project: Path) -> None:
    resolved = path.resolve()
    allowed = (project / ".build" / "rr3-data-volumes").resolve()
    if resolved != allowed:
        raise RuntimeError(f"refusing to remove unexpected staging path: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path("dist") / f"rr3-native-rehost-complete-{VERSION}")
    parser.add_argument("--builder", type=Path,
                        default=Path(".tools/devkitPro/msys64/opt/devkitpro/tools/bin/build_romfs.exe"))
    parser.add_argument("--program-only", action="store_true",
                        help="replace the NRO and refresh its manifest entry without rebuilding data volumes")
    args = parser.parse_args()

    project = Path.cwd().resolve()
    archive_path = args.archive.resolve()
    output = args.output.resolve()
    builder = args.builder.resolve()
    staging = project / ".build" / "rr3-data-volumes"
    data_output = output / "switch" / "rr3-native-rehost" / "data"
    nro_source = project / "native-switch" / "rr3-native-rehost.nro"

    if not archive_path.is_file():
        raise FileNotFoundError(archive_path)
    if not builder.is_file():
        raise FileNotFoundError(builder)
    if not nro_source.is_file():
        raise FileNotFoundError(nro_source)
    output.mkdir(parents=True, exist_ok=True)
    data_output.mkdir(parents=True, exist_ok=True)
    manifest_path = data_output / "rr3-data-manifest.json"
    if args.program_only:
        if not manifest_path.is_file():
            raise FileNotFoundError(manifest_path)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        program_dir = output / "switch" / "rr3-native-rehost"
        shutil.copy2(nro_source, program_dir / "rr3-native-rehost.nro")
        manifest["program"] = {
            "file": "rr3-native-rehost.nro",
            "bytes": nro_source.stat().st_size,
            "sha256": sha256(nro_source),
        }
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"PROGRAM UPDATED {program_dir / 'rr3-native-rehost.nro'}", flush=True)
        return 0
    for stale_image in data_output.glob("*.romfs"):
        stale_image.unlink()
    checked_remove_staging(staging, project)
    staging.mkdir(parents=True)

    archive_hash = sha256(archive_path)
    manifest: dict[str, object] = {
        "format": "rr3-native-rehost-external-romfs-v1",
        "version": VERSION,
        "source_archive": archive_path.name,
        "source_sha256": archive_hash,
        "vehicle_split": "year <= 2015 classic; 2016-2020 mid; year >= 2021 modern; non-year classic",
        "volumes": {},
    }

    try:
        with zipfile.ZipFile(archive_path) as archive:
            bad = archive.testzip()
            if bad is not None:
                raise RuntimeError(f"ZIP CRC failure: {bad}")
            infos = archive.infolist()
            for volume in VOLUMES:
                volume_stage = staging / volume
                volume_stage.mkdir(parents=True)
                count, source_bytes = extract_volume(archive, infos, volume, volume_stage)
                if volume == "tracks":
                    mirrored, mirrored_bytes = mirror_track_data_halves(volume_stage)
                    print(f"[{volume}] mirrored {mirrored} track data file(s) into "
                          f"processed/ ({mirrored_bytes} bytes)", flush=True)
                    count += mirrored
                    source_bytes += mirrored_bytes
                destination = data_output / f"{volume}.romfs"
                print(f"[{volume}] building {destination.name}", flush=True)
                build_volume(builder, volume_stage, destination)
                volume_hash = sha256(destination)
                manifest["volumes"][volume] = {
                    "file": destination.name,
                    "files": count,
                    "source_bytes": source_bytes,
                    "romfs_bytes": destination.stat().st_size,
                    "sha256": volume_hash,
                    "sentinel": SENTINELS[volume],
                }
                print(
                    f"[{volume}] ready {destination.stat().st_size / 1024**3:.2f} GiB "
                    f"SHA256={volume_hash}", flush=True
                )
                shutil.rmtree(volume_stage)
    finally:
        checked_remove_staging(staging, project)

    program_dir = output / "switch" / "rr3-native-rehost"
    shutil.copy2(nro_source, program_dir / "rr3-native-rehost.nro")
    manifest["program"] = {
        "file": "rr3-native-rehost.nro",
        "bytes": nro_source.stat().st_size,
        "sha256": sha256(nro_source),
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
    readme = output / "README-一次复制.txt"
    readme.write_text(
        "RR3 Native Rehost 完整外置数据包 0.21.0\n\n"
        "把本目录中的 switch 文件夹合并复制到 TF 卡根目录。\n"
        "最终路径应为：\n"
        "  /switch/rr3-native-rehost/rr3-native-rehost.nro\n"
        "  /switch/rr3-native-rehost/data/core.romfs\n"
        "  /switch/rr3-native-rehost/data/gui-hd.romfs\n"
        "  /switch/rr3-native-rehost/data/gui-sd.romfs\n"
        "  /switch/rr3-native-rehost/data/tracks.romfs\n"
        "  /switch/rr3-native-rehost/data/vehicles-classic.romfs\n"
        "  /switch/rr3-native-rehost/data/vehicles-mid.romfs\n"
        "  /switch/rr3-native-rehost/data/vehicles-modern.romfs\n\n"
        "七个数据卷均小于 2 GiB，并兼容 FAT32 及 build_romfs。以后更新通常只替换 NRO。\n",
        encoding="utf-8",
    )
    print(f"COMPLETE {output}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr, flush=True)
        raise
