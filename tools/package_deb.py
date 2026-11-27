#!/usr/bin/env python3
"""Build split Debian/Ubuntu desktop packages (<2 GiB each) for OpenSky VFR."""

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
MAX_RELEASE_ASSET_BYTES = 2 * 1024**3

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

def region_file_tuple(region_id: str) -> tuple[str, ...]:
    return (
        f"{region_id}.nspack",
        f"{region_id}.relief.png",
        f"{region_id}_cities.nscity",
        f"{region_id}_city_sh.nsshd",
        f"{region_id}_scenery.nsscn",
        f"{region_id}_weather.nswx",
    )


OPTIONAL_ADDONS: tuple[tuple[str, str, tuple[str, ...], str], ...] = (
    (
        "opensky-vfr-catalonia-coast",
        "Barcelona, Costa Brava, Girona & Tarragona scenery pack for OpenSky VFR",
        region_file_tuple("es_catalonia_coast"),
        "Adds Barcelona, Tarragona, Girona, Montserrat, and the Costa Brava.",
    ),
    (
        "opensky-vfr-catalonia-pyrenees",
        "Catalan Pyrenees, Lleida & Andorra scenery pack for OpenSky VFR",
        region_file_tuple("es_catalonia_pyrenees"),
        "Adds Lleida, Seu d'Urgell, Cerdanya, Andorra, and the Catalan Pyrenees.",
    ),
    (
        "opensky-vfr-navarra-basque",
        "Navarra, Rioja & Basque Coast scenery pack for OpenSky VFR",
        region_file_tuple("es_navarra_euskadi"),
        "Adds Navarra, La Rioja, Bilbao, San Sebastián, and the eastern Cantabrian coast.",
    ),
    (
        "opensky-vfr-asturias-cantabria",
        "Asturias, Cantabria & Picos de Europa scenery pack for OpenSky VFR",
        region_file_tuple("es_picos_europa"),
        "Adds Asturias, Cantabria, Santander, Oviedo/Gijón, and Picos de Europa.",
    ),
    (
        "opensky-vfr-galicia",
        "Galicia & Atlantic Coast scenery pack for OpenSky VFR",
        region_file_tuple("es_galicia"),
        "Adds Galicia, Santiago, A Coruña, Vigo, and the Atlantic coast.",
    ),
    (
        "opensky-vfr-aragon-south",
        "Southern Aragón & Teruel scenery pack for OpenSky VFR",
        region_file_tuple("es_aragon_south"),
        "Adds Southern Aragón, Teruel, and the Iberian System.",
    ),
    (
        "opensky-vfr-castilla-y-leon",
        "Castilla y León scenery pack for OpenSky VFR",
        region_file_tuple("es_castilla_y_leon"),
        "Adds the Meseta Norte, Duero basin, Valladolid, León, Burgos, and Salamanca.",
    ),
    (
        "opensky-vfr-madrid-central",
        "Madrid & Sierra de Guadarrama scenery pack for OpenSky VFR",
        region_file_tuple("es_madrid_central"),
        "Adds Madrid and the Sierra de Guadarrama / Gredos corridor.",
    ),
)

ALL_BASELINE_FILES = ARAGON_FILES + CATALONIA_FILES

EXTRA_RUNTIME_DEPS = (
    "libgl1",
    "libxrandr2",
    "curl",
    "libasound2 | libasound2t64",
    "libasound2-plugins",
)


def run(args: list[str], *, cwd: Path = ROOT, capture_output: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, check=True, text=True, capture_output=capture_output)


def parse_ver_tuple(raw: str) -> tuple[int, ...]:
    nums = re.findall(r"[0-9]+", raw)
    return tuple(int(x) for x in nums[:2])


def read_os_release() -> dict[str, str]:
    info: dict[str, str] = {}
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            key, sep, value = line.partition("=")
            if sep:
                info[key] = value.strip().strip('"')
    except OSError:
        pass
    return info


def resolve_cmake() -> str:
    env_cmake = os.environ.get("CMAKE")
    if env_cmake and Path(env_cmake).is_file():
        return env_cmake
    in_path = shutil.which("cmake")
    if in_path:
        return in_path
    bundled = ROOT / "gameplay_captures/engine-foundation/build-tools/cmake/data/bin/cmake"
    if bundled.is_file():
        return str(bundled)
    return "cmake"


def copy_pack_files(packs_dir: Path, dest_dir: Path, filenames: tuple[str, ...]) -> None:
    dest_dir.mkdir(parents=True, exist_ok=True)
    missing = [name for name in filenames if not (packs_dir / name).is_file()]
    if missing:
        raise SystemExit("Missing required region data: " + ", ".join(missing))
    for name in filenames:
        shutil.copy2(packs_dir / name, dest_dir / name)


def resolve_runtime_depends(binary_path: Path, jsbsim_lib: Path, temp_dir: Path) -> str:
    deps: list[str] = []
    if shutil.which("dpkg-shlibdeps"):
        shlib_dir = temp_dir / "shlib_probe"
        (shlib_dir / "debian").mkdir(parents=True, exist_ok=True)
        (shlib_dir / "debian/control").write_text(
            "Source: opensky-vfr\n\nPackage: opensky-vfr\nArchitecture: amd64\n",
            encoding="utf-8",
        )
        res = run(
            [
                "dpkg-shlibdeps",
                "--ignore-missing-info",
                "-O",
                f"-l{jsbsim_lib.parent}",
                "-e",
                str(binary_path),
                "-e",
                str(jsbsim_lib),
            ],
            cwd=shlib_dir,
            capture_output=True,
        )
        for line in res.stdout.splitlines():
            if line.startswith("shlibs:Depends="):
                raw = line.partition("=")[2]
                deps = [
                    item.strip()
                    for item in raw.split(",")
                    if item.strip() and "jsbsim" not in item.lower()
                ]
                break
    if not deps:
        deps = ["libc6 (>= 2.35)", "libstdc++6 (>= 11)", "libgcc-s1", "libx11-6"]
    for extra in EXTRA_RUNTIME_DEPS:
        if extra not in deps:
            deps.append(extra)
    return ", ".join(deps)


def write_dep5_copyright(doc_dir: Path, pkg_name: str, is_base: bool) -> None:
    sections = [
        "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/",
        f"Upstream-Name: {pkg_name}",
        "Source: https://github.com/BeamAudio/OpenSky",
        "Disclaimer: Independent open-source VFR flight simulation project.",
        " Not affiliated with or endorsed by Textron Aviation, Cessna, Garmin,",
        " BendixKing/Honeywell, Lycoming, or any other third-party brand.",
        "",
    ]
    if is_base:
        sections.extend([
            "Files: opt/opensky-vfr/sim_engine/* opt/opensky-vfr/game/aircraft/*",
            "Copyright: OpenSky VFR contributors",
            "License: MIT and CC0-1.0",
            "",
            "Files: opt/opensky-vfr/game/bin/libJSBSim.so*",
            "Copyright: JSBSim Development Team",
            "License: LGPL-2.1-or-later",
            " See /usr/share/doc/opensky-vfr/JSBSim-LGPL-2.1 and jsbsim-source.",
            "",
            "Files: opt/opensky-vfr/game/assets/models/aircraft/c172s_exterior.glb",
            "Copyright: e737 (Sketchfab), veyron007/AUREL contributors, OpenSky VFR contributors",
            "License: CC-BY-4.0",
            " See /usr/share/doc/opensky-vfr/LICENSES/CC-BY-4.0.txt and THIRD_PARTY_LICENSES.md.",
            "",
        ])
    sections.extend([
        "Files: opt/opensky-vfr/packs/*.nspack opt/opensky-vfr/packs/*.relief.png",
        "Copyright: Instituto Geografico Nacional de Espana (IGN), OurAirports, Natural Earth",
        "License: CC-BY-4.0 and Public-Domain",
        " Scenery terrain/imagery includes data from IGN Spain PNOA / MDT05 / MDT25",
        " (CC BY 4.0; /usr/share/doc/" + pkg_name + "/LICENSES/CC-BY-4.0.txt).",
        "",
        "Files: opt/opensky-vfr/packs/*.nscity opt/opensky-vfr/packs/*.nsscn",
        "Copyright: OpenStreetMap contributors, Poly Haven",
        "License: ODbL-1.0 and CC0-1.0",
        " Derived building/road databases are distributed under ODbL 1.0 Share-Alike.",
        " Full license text: /usr/share/doc/" + pkg_name + "/LICENSES/ODbL-1.0.txt.",
        "",
        "Files: opt/opensky-vfr/*",
        "Copyright: OpenSky VFR contributors",
        "License: MIT",
        " See /usr/share/doc/" + pkg_name + "/LICENSE.",
        "",
    ])
    (doc_dir / "copyright").write_text("\n".join(sections), encoding="utf-8")


def copy_license_bundle(doc_dir: Path, pkg_name: str, is_base: bool) -> None:
    for name in ("README.md", "LICENSE", "THIRD_PARTY_LICENSES.md"):
        shutil.copy2(ROOT / name, doc_dir / name)
    lic_src = ROOT / "LICENSES"
    if lic_src.is_dir():
        shutil.copytree(lic_src, doc_dir / "LICENSES", dirs_exist_ok=True)
    write_dep5_copyright(doc_dir, pkg_name, is_base)


def copy_app_base(stage: Path, packs_dir: Path) -> None:
    app = stage / APP_ROOT
    (app / "sim_engine").mkdir(parents=True)
    (app / "game/bin").mkdir(parents=True)
    shutil.copy2(ROOT / "sim_engine/OpenSky", app / "sim_engine/OpenSky")
    shutil.copytree(ROOT / "sim_engine/shaders", app / "sim_engine/shaders")
    shutil.copytree(
        ROOT / "game/assets",
        app / "game/assets",
        ignore=shutil.ignore_patterns("*.blend", "*.blend1"),
    )
    shutil.copytree(ROOT / "game/aircraft", app / "game/aircraft")
    shutil.copy2(ROOT / "game/bin/libJSBSim.so.1", app / "game/bin/libJSBSim.so.1")
    (app / "game/bin/libJSBSim.so").symlink_to("libJSBSim.so.1")

    copy_pack_files(packs_dir, app / "packs", ARAGON_FILES)

    launcher = stage / "usr/bin/opensky-vfr"
    launcher.parent.mkdir(parents=True)
    launcher.write_text(
        "#!/bin/sh\n"
        "cd /opt/opensky-vfr || exit 1\n"
        'exec /opt/opensky-vfr/sim_engine/OpenSky "$@"\n',
        encoding="utf-8",
    )
    launcher.chmod(0o755)

    doc = stage / "usr/share/doc/opensky-vfr"
    doc.mkdir(parents=True)
    copy_license_bundle(doc, "opensky-vfr", is_base=True)
    shutil.copy2(ROOT / "docs/MODDING.md", doc / "MODDING.md")
    shutil.copy2(ROOT / "engine/thirdparty/jsbsim/COPYING.LGPL-2.1", doc / "JSBSim-LGPL-2.1")
    shutil.copytree(
        ROOT / "engine/thirdparty/jsbsim",
        doc / "jsbsim-source",
        ignore=shutil.ignore_patterns(".git", "build", "cmake-build-*"),
    )

    desktop = stage / "usr/share/applications/opensky-vfr.desktop"
    desktop.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "packaging/linux/opensky-vfr.desktop", desktop)
    icon = stage / "usr/share/icons/hicolor/scalable/apps/opensky-vfr.svg"
    icon.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "packaging/linux/opensky-vfr.svg", icon)


def copy_region_addon(stage: Path, pkg_name: str, packs_dir: Path, filenames: tuple[str, ...]) -> None:
    copy_pack_files(packs_dir, stage / APP_ROOT / "packs", filenames)
    doc = stage / "usr/share/doc" / pkg_name
    doc.mkdir(parents=True)
    copy_license_bundle(doc, pkg_name, is_base=False)


def build_deb_package(
    stage: Path,
    control_text: str,
    temp_dir: Path,
    output_path: Path,
    threads: int,
) -> Path | None:
    control = stage / "DEBIAN/control"
    control.parent.mkdir(parents=True, exist_ok=True)
    control.write_text(control_text, encoding="utf-8")
    staged_deb = temp_dir / output_path.name
    run(
        [
            "dpkg-deb",
            "--root-owner-group",
            "--uniform-compression",
            "-Zxz",
            "-z1",
            f"--threads-max={threads}",
            "--build",
            str(stage),
            str(staged_deb),
        ],
        cwd=ROOT,
    )
    size = staged_deb.stat().st_size
    if size >= MAX_RELEASE_ASSET_BYTES:
        shutil.move(staged_deb, output_path.with_suffix(".oversize.tmp"))
        output_path.with_suffix(".oversize.tmp").unlink()
        return None
    shutil.move(staged_deb, output_path)
    return output_path


def package_addon(
    pkg_name: str,
    summary: str,
    details: str,
    files: tuple[str, ...],
    version: str,
    maintainer: str,
    packs_dir: Path,
    temp_dir: Path,
    output_dir: Path,
    threads: int,
) -> list[Path]:
    stage = temp_dir / pkg_name
    copy_region_addon(stage, pkg_name, packs_dir, files)
    out_deb = output_dir / f"{pkg_name}_{version}_all.deb"
    built = build_deb_package(
        stage,
        f"Package: {pkg_name}\n"
        f"Version: {version}\n"
        "Section: games\n"
        "Priority: optional\n"
        "Architecture: all\n"
        f"Depends: opensky-vfr (>= {version})\n"
        f"Maintainer: {maintainer}\n"
        f"Description: {summary}\n"
        f" {details}\n",
        temp_dir,
        out_deb,
        threads,
    )
    shutil.rmtree(stage)
    if built is not None:
        return [built]

    # Split _cities.nscity into a companion package so every GitHub release asset stays < 2 GiB.
    city_files = tuple(f for f in files if f.endswith("_cities.nscity"))
    base_files = tuple(f for f in files if not f.endswith("_cities.nscity"))
    if not city_files:
        raise SystemExit(f"{out_deb.name} exceeds 2 GiB release asset cap and has no _cities.nscity split")

    results: list[Path] = []
    for sub_name, sub_files, sub_deps, sub_desc in (
        (pkg_name, base_files, f"opensky-vfr (>= {version})", f"{summary}\n {details}"),
        (f"{pkg_name}-cities", city_files, f"{pkg_name} (>= {version})", f"Pre-baked 3D city polygons for {pkg_name}\n Companion city pack."),
    ):
        sub_stage = temp_dir / sub_name
        copy_region_addon(sub_stage, sub_name, packs_dir, sub_files)
        sub_out = output_dir / f"{sub_name}_{version}_all.deb"
        sub_built = build_deb_package(
            sub_stage,
            f"Package: {sub_name}\n"
            f"Version: {version}\n"
            "Section: games\n"
            "Priority: optional\n"
            "Architecture: all\n"
            f"Depends: {sub_deps}\n"
            f"Maintainer: {maintainer}\n"
            f"Description: {sub_desc}\n",
            temp_dir,
            sub_out,
            threads,
        )
        shutil.rmtree(sub_stage)
        if sub_built is None:
            raise SystemExit(f"{sub_out.name} still exceeds 2 GiB release asset cap")
        results.append(sub_built)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="release version, for example 0.3.0")
    parser.add_argument(
        "--packs-dir",
        type=Path,
        default=ROOT / "packs",
        help="folder containing the reviewed Aragón and Catalunya baseline region files",
    )
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument(
        "--maintainer",
        required=True,
        help='package contact in "Name <email>" format',
    )
    parser.add_argument(
        "--content-approved",
        action="store_true",
        help="confirm that every included model and data file is cleared for redistribution",
    )
    args = parser.parse_args()

    if not args.content_approved:
        parser.error("verify all included model and data redistribution terms before packaging; then pass --content-approved")
    if platform.system() != "Linux" or platform.machine().lower() not in {"x86_64", "amd64"}:
        parser.error("build this amd64 Debian package on an x86_64 Linux host")

    os_release = read_os_release()
    distro_id = os_release.get("ID", "unknown")
    distro_ver = os_release.get("VERSION_ID", "unknown")
    ver_tuple = parse_ver_tuple(distro_ver)
    if distro_id not in {"ubuntu", "debian"}:
        parser.error(f"unsupported host distro '{distro_id}'; build .deb installers on Debian or Ubuntu")
    if distro_id == "ubuntu" and ver_tuple < (22, 4):
        parser.error("Ubuntu 22.04 or newer is required")
    if distro_id == "debian" and ver_tuple < (12,):
        parser.error("Debian 12 or newer is required")

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
        parser.error("required baseline region data is missing: " + ", ".join(missing))

    threads = os.cpu_count() or 2
    cmake_bin = resolve_cmake()
    run(["make", "-C", "sim_engine", "clean"])
    run(["make", "-C", "sim_engine", f"-j{threads}", f"CMAKE={cmake_bin}", "PORTABLE=1", "PLAYER_PACKAGE=1"])
    if not (ROOT / "sim_engine/OpenSky").is_file() or not (ROOT / "game/bin/libJSBSim.so.1").is_file():
        parser.error("missing sim_engine/OpenSky or game/bin/libJSBSim.so.1")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = args.output_dir.resolve()
    outputs: list[Path] = []

    with tempfile.TemporaryDirectory(prefix=".deb-stage-", dir=output_dir) as tmp:
        temp_dir = Path(tmp)
        depends = resolve_runtime_depends(
            ROOT / "sim_engine/OpenSky",
            ROOT / "game/bin/libJSBSim.so.1",
            temp_dir,
        )

        enabled_addons = [
            addon for addon in OPTIONAL_ADDONS
            if all((packs_dir / fn).is_file() for fn in addon[2])
        ]
        recommends = ", ".join(f"{addon[0]} (= {args.version})" for addon in enabled_addons)
        rec_line = f"Recommends: {recommends}\n" if recommends else ""

        app_stage = temp_dir / "opensky-vfr"
        copy_app_base(app_stage, packs_dir)
        base_out = output_dir / f"opensky-vfr_{args.version}_amd64.deb"
        base_deb = build_deb_package(
            app_stage,
            "Package: opensky-vfr\n"
            f"Version: {args.version}\n"
            "Section: games\n"
            "Priority: optional\n"
            "Architecture: amd64\n"
            f"Depends: {depends}\n"
            f"{rec_line}"
            f"Maintainer: {args.maintainer}\n"
            "Description: OpenSky VFR flight exploration simulator\n"
            " Explore the Pyrenees and Northern Spain in the OpenSky 172S trainer. Includes Aragón baseline scenery.\n"
            " Launch it from the Games menu or with opensky-vfr.\n",
            temp_dir,
            base_out,
            threads,
        )
        shutil.rmtree(app_stage)
        if base_deb is None:
            raise SystemExit(f"{base_out.name} exceeds 2 GiB GitHub release asset cap")
        outputs.append(base_deb)

        for pkg_name, summary, files, details in enabled_addons:
            outputs.extend(
                package_addon(
                    pkg_name,
                    summary,
                    details,
                    files,
                    args.version,
                    args.maintainer,
                    packs_dir,
                    temp_dir,
                    output_dir,
                    threads,
                )
            )

    print(f"Built {len(outputs)} Debian/Ubuntu package(s) on {distro_id} {distro_ver}:")
    for p in outputs:
        print(f"  - {p} ({p.stat().st_size / 1024**3:.2f} GiB)")
    print("Install all packages cleanly with:")
    print(f"  sudo apt install ./opensky-vfr*_{args.version}_*.deb")
    return 0


if __name__ == "__main__":
    sys.exit(main())
