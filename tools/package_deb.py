#!/usr/bin/env python3
"""Build the desktop app and optional Catalonia region Debian/Ubuntu packages."""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_ROOT = Path("opt/opensky-vfr")
ARAGON_FILES = (
    "es_aragon_pyrenees.nspack",
    "es_aragon_pyrenees.relief.png",
    "es_aragon_cities.nscity",
    "es_aragon_city_sh.nsshd",
    "es_aragon_scenery.nsscn",
    "es_aragon_weather.nswx",
)
CATALONIA_FILES = (
    "es_catalonia_coast.nspack",
    "es_catalonia_coast.relief.png",
    "es_catalonia_coast_cities.nscity",
    "es_catalonia_coast_city_sh.nsshd",
    "es_catalonia_coast_scenery.nsscn",
    "es_catalonia_coast_weather.nswx",
    "es_catalonia_pyrenees.nspack",
    "es_catalonia_pyrenees.relief.png",
    "es_catalonia_pyrenees_cities.nscity",
    "es_catalonia_pyrenees_city_sh.nsshd",
    "es_catalonia_pyrenees_scenery.nsscn",
    "es_catalonia_pyrenees_weather.nswx",
)
MAX_RELEASE_ASSET_BYTES = 2 * 1024**3


def run(args: list[str], *, cwd: Path = ROOT) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def copy_runtime(stage: Path, packs_dir: Path) -> None:
    app = stage / APP_ROOT
    (app / "sim_engine").mkdir(parents=True)
    (app / "game").mkdir()
    shutil.copy2(ROOT / "sim_engine/nspain_sim", app / "sim_engine/nspain_sim")
    shutil.copytree(ROOT / "sim_engine/shaders", app / "sim_engine/shaders")
    shutil.copytree(ROOT / "game/assets", app / "game/assets",
                    ignore=shutil.ignore_patterns("*.blend", "*.blend1"))
    shutil.copytree(ROOT / "game/aircraft", app / "game/aircraft")
    (app / "game/bin").mkdir(parents=True)
    shutil.copy2(ROOT / "game/bin/libJSBSim.so.1", app / "game/bin/libJSBSim.so.1")

    destination = app / "packs"
    destination.mkdir()
    missing = [name for name in ARAGON_FILES if not (packs_dir / name).is_file()]
    if missing:
        raise SystemExit("Missing required Aragón release data: " + ", ".join(missing))
    for name in ARAGON_FILES:
        shutil.copy2(packs_dir / name, destination / name)

    doc = stage / "usr/share/doc/opensky-vfr"
    doc.mkdir(parents=True)
    for name in ("README.md", "LICENSE", "THIRD_PARTY_LICENSES.md"):
        shutil.copy2(ROOT / name, doc / name)
    shutil.copy2(ROOT / "docs/MODDING.md", doc / "MODDING.md")
    shutil.copy2(ROOT / "engine/thirdparty/jsbsim/COPYING.LGPL-2.1", doc / "JSBSim-LGPL-2.1")
    shutil.copytree(ROOT / "engine/thirdparty/jsbsim", doc / "jsbsim-source",
                    ignore=shutil.ignore_patterns(".git", "build", "cmake-build-*"))

    desktop = stage / "usr/share/applications/opensky-vfr.desktop"
    desktop.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "packaging/linux/opensky-vfr.desktop", desktop)
    icon = stage / "usr/share/icons/hicolor/scalable/apps/opensky-vfr.svg"
    icon.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "packaging/linux/opensky-vfr.svg", icon)


def copy_catalonia(stage: Path, packs_dir: Path) -> None:
    destination = stage / APP_ROOT / "packs"
    destination.mkdir(parents=True)
    missing = [name for name in CATALONIA_FILES if not (packs_dir / name).is_file()]
    if missing:
        raise SystemExit("Missing required Catalonia release data: " + ", ".join(missing))
    for name in CATALONIA_FILES:
        shutil.copy2(packs_dir / name, destination / name)
    doc = stage / "usr/share/doc/opensky-vfr-catalonia"
    doc.mkdir(parents=True)
    for name in ("README.md", "THIRD_PARTY_LICENSES.md"):
        shutil.copy2(ROOT / name, doc / name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="release version, for example 0.3.0")
    parser.add_argument("--packs-dir", type=Path, default=ROOT / "packs",
                        help="folder containing the reviewed Aragón region pack files")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--maintainer", required=True,
                        help='package contact in "Name <email>" format')
    parser.add_argument("--content-approved", action="store_true",
                        help="confirm that every included model and data file is cleared for redistribution")
    args = parser.parse_args()

    if not args.content_approved:
        parser.error("commercial/open-source content rights must be reviewed before packaging; pass --content-approved only after that review")
    if platform.system() != "Linux" or platform.machine().lower() not in {"x86_64", "amd64"}:
        parser.error("build this amd64 Debian package on an x86_64 Linux host")
    os_release = {}
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            key, sep, value = line.partition("=")
            if sep:
                os_release[key] = value.strip().strip('"')
    except OSError:
        pass
    if os_release.get("ID") != "ubuntu" or os_release.get("VERSION_ID") != "22.04":
        parser.error("build release packages on Ubuntu 22.04 to keep the glibc and libstdc++ baseline compatible with Debian/Ubuntu targets")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*(?:[-+~][A-Za-z0-9.+~:-]+)?", args.version):
        parser.error("--version contains characters that are not valid in a Debian version")
    if not re.fullmatch(r"[^<>\n]+ <[^<>\s@]+@[^<>\s@]+>", args.maintainer):
        parser.error('--maintainer must look like "OpenSky VFR maintainers <email@example.org>"')
    if shutil.which("dpkg-deb") is None:
        parser.error("dpkg-deb is required on the release machine")
    packs_dir = args.packs_dir.resolve()
    if not packs_dir.is_dir():
        parser.error(f"regional pack directory does not exist: {packs_dir}")
    missing = [name for name in ARAGON_FILES if not (packs_dir / name).is_file()]
    if missing:
        parser.error("required Aragón release data is missing: " + ", ".join(missing))
    missing = [name for name in CATALONIA_FILES if not (packs_dir / name).is_file()]
    if missing:
        parser.error("required Catalonia release data is missing: " + ", ".join(missing))

    # A redistributable build avoids host-specific CPU tuning and auto-detected AVX2.
    run(["make", "-C", "sim_engine", "clean"])
    run(["make", "-C", "sim_engine", f"-j{os.cpu_count() or 2}", "PORTABLE=1", "PLAYER_PACKAGE=1"])
    if not (ROOT / "game/bin/libJSBSim.so.1").is_file():
        parser.error("the build did not produce game/bin/libJSBSim.so.1")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = args.output_dir.resolve()
    app_output = output_dir / f"opensky-vfr_{args.version}_amd64.deb"
    catalonia_output = output_dir / f"opensky-vfr-catalonia_{args.version}_all.deb"
    with tempfile.TemporaryDirectory(prefix="opensky-vfr-deb-") as tmp:
        temporary = Path(tmp)
        stage = Path(tmp) / "app"
        copy_runtime(stage, packs_dir)
        control = stage / "DEBIAN/control"
        control.parent.mkdir(parents=True)
        control.write_text(
            "Package: opensky-vfr\n"
            f"Version: {args.version}\n"
            "Section: games\n"
            "Priority: optional\n"
            "Architecture: amd64\n"
            "Depends: libc6 (>= 2.35), libstdc++6 (>= 11), libgcc-s1, libx11-6, libgl1, curl, libasound2 | libasound2t64\n"
            f"Maintainer: {args.maintainer}\n"
            "Description: OpenSky VFR flight exploration simulator\n"
            " Explore the Pyrenees in a Cessna 172S. Includes Aragón.\n"
            " Launch it from the Games menu after installation.\n",
            encoding="utf-8",
        )
        run(["dpkg-deb", "--root-owner-group", "--uniform-compression", "--compression=xz",
             "--build", str(stage), str(temporary / app_output.name)], cwd=ROOT)

        stage = Path(tmp) / "catalonia"
        copy_catalonia(stage, packs_dir)
        control = stage / "DEBIAN/control"
        control.parent.mkdir(parents=True)
        control.write_text(
            "Package: opensky-vfr-catalonia\n"
            f"Version: {args.version}\n"
            "Section: games\n"
            "Priority: optional\n"
            "Architecture: all\n"
            f"Depends: opensky-vfr (= {args.version})\n"
            f"Maintainer: {args.maintainer}\n"
            "Description: Catalonia region data for OpenSky VFR\n"
            " Adds the Catalonia coast and Pyrenees region packs.\n",
            encoding="utf-8",
        )
        run(["dpkg-deb", "--root-owner-group", "--uniform-compression", "--compression=xz",
             "--build", str(stage), str(temporary / catalonia_output.name)], cwd=ROOT)

        temporary_assets = (temporary / app_output.name, temporary / catalonia_output.name)
        for artifact in temporary_assets:
            size = artifact.stat().st_size
            if size >= MAX_RELEASE_ASSET_BYTES:
                parser.error(f"{artifact.name} is {size} bytes; split every GitHub release asset below 2 GiB")
        for source, destination in zip(temporary_assets, (app_output, catalonia_output)):
            shutil.move(source, destination)

    for artifact in (app_output, catalonia_output):
        size = artifact.stat().st_size
        print(f"Built {artifact} ({size / 1024**3:.2f} GiB)")
    print("Install both packages with Software, or run:")
    print(f"sudo apt install ./{app_output.name} ./{catalonia_output.name}")
    print("Then launch OpenSky VFR from the desktop application menu.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
