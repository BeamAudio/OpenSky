#!/usr/bin/env python3
"""Package a self-contained Windows x64 release archive for OpenSky VFR."""

from __future__ import annotations

import argparse
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_RELEASE_ASSET_BYTES = 2 * 1024**3

ARAGON_FILES = (
    "es_aragon_pyrenees.nspack",
    "es_aragon_pyrenees.relief.png",
    "es_aragon_cities.nscity",
    "es_aragon_city_sh.nsshd",
    "es_aragon_scenery.nsscn",
    "es_aragon_weather.nswx",
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
        "catalonia-coast",
        "Barcelona, Costa Brava, Girona & Tarragona",
        region_file_tuple("es_catalonia_coast"),
        "Adds Barcelona, Tarragona, Girona, Montserrat, and the Costa Brava.",
    ),
    (
        "catalonia-pyrenees",
        "Catalan Pyrenees, Lleida & Andorra",
        region_file_tuple("es_catalonia_pyrenees"),
        "Adds Lleida, Seu d'Urgell, Cerdanya, Andorra, and the Catalan Pyrenees.",
    ),
    (
        "navarra-basque",
        "Navarra, Rioja & Basque Coast",
        region_file_tuple("es_navarra_euskadi"),
        "Adds Navarra, La Rioja, Bilbao, San Sebastián, and the eastern Cantabrian coast.",
    ),
    (
        "asturias-cantabria",
        "Asturias, Cantabria & Picos de Europa",
        region_file_tuple("es_picos_europa"),
        "Adds Asturias, Cantabria, Santander, Oviedo/Gijón, and Picos de Europa.",
    ),
    (
        "galicia",
        "Galicia & Atlantic Coast",
        region_file_tuple("es_galicia"),
        "Adds Galicia, Santiago, A Coruña, Vigo, and the Atlantic coast.",
    ),
    (
        "aragon-south",
        "Southern Aragón & Teruel",
        region_file_tuple("es_aragon_south"),
        "Adds Southern Aragón, Teruel, and the Iberian System.",
    ),
    (
        "castilla-y-leon",
        "Castilla y León",
        region_file_tuple("es_castilla_y_leon"),
        "Adds the Meseta Norte, Duero basin, Valladolid, León, Burgos, and Salamanca.",
    ),
    (
        "madrid-central",
        "Madrid & Sierra de Guadarrama",
        region_file_tuple("es_madrid_central"),
        "Adds Madrid and the Sierra de Guadarrama / Gredos corridor.",
    ),
)

RUNTIME_DLLS = (
    "libJSBSim.dll",
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
)


def copy_tree(src: Path, dst: Path, ignore=None) -> None:
    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


def stage_release(stage_dir: Path, packs_dir: Path | None, include_packs: bool) -> None:
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True)

    exe_src = ROOT / "sim_engine" / "OpenSky.exe"
    if not exe_src.is_file():
        raise FileNotFoundError(f"Missing Windows executable: {exe_src}")

    sim_dst = stage_dir / "sim_engine"
    sim_dst.mkdir(parents=True)
    shutil.copy2(exe_src, sim_dst / "OpenSky.exe")

    for dll in RUNTIME_DLLS:
        cand = ROOT / "sim_engine" / dll
        if not cand.is_file():
            cand = ROOT / "game" / "bin" / dll
        if cand.is_file():
            shutil.copy2(cand, sim_dst / dll)
        elif dll == "libJSBSim.dll":
            raise FileNotFoundError("Missing dynamically linked libJSBSim.dll")

    copy_tree(ROOT / "sim_engine" / "shaders", sim_dst / "shaders")
    copy_tree(
        ROOT / "game" / "assets",
        stage_dir / "game" / "assets",
        ignore=shutil.ignore_patterns("*.blend", "*.blend1"),
    )
    copy_tree(ROOT / "game" / "aircraft", stage_dir / "game" / "aircraft")

    # Root launcher so double-clicking OpenSky.bat starts cleanly from the package root
    launcher = stage_dir / "OpenSky.bat"
    launcher.write_text(
        "@echo off\n"
        "setlocal\n"
        'cd /d "%~dp0"\n'
        '"%~dp0sim_engine\\OpenSky.exe" %*\n'
        "exit /b %ERRORLEVEL%\n",
        encoding="utf-8",
        newline="\r\n",
    )

    # Scenery packs (baseline Aragon when available and requested)
    (stage_dir / "packs").mkdir(parents=True, exist_ok=True)
    if include_packs and packs_dir is not None:
        missing = [f for f in ARAGON_FILES if not (packs_dir / f).is_file()]
        if missing:
            raise FileNotFoundError("Missing baseline Aragon pack files: " + ", ".join(missing))
        for fname in ARAGON_FILES:
            shutil.copy2(packs_dir / fname, stage_dir / "packs" / fname)

    # Licenses, attributions, and LGPL-2.1 JSBSim source compliance bundle
    for name in ("LICENSE", "THIRD_PARTY_LICENSES.md", "README.md"):
        src = ROOT / name
        if src.is_file():
            shutil.copy2(src, stage_dir / name)
    lic_dir = ROOT / "LICENSES"
    if lic_dir.is_dir():
        copy_tree(lic_dir, stage_dir / "LICENSES")

    doc_dir = stage_dir / "doc"
    doc_dir.mkdir(parents=True, exist_ok=True)
    if (ROOT / "docs" / "MODDING.md").is_file():
        shutil.copy2(ROOT / "docs" / "MODDING.md", doc_dir / "MODDING.md")
    shutil.copy2(ROOT / "engine" / "thirdparty" / "jsbsim" / "COPYING.LGPL-2.1", doc_dir / "JSBSim-LGPL-2.1")
    copy_tree(
        ROOT / "engine" / "thirdparty" / "jsbsim",
        doc_dir / "jsbsim-source",
        ignore=shutil.ignore_patterns(".git", "build", "cmake-build-*"),
    )


def zip_directory(src_dir: Path, zip_path: Path, *, include_root_dir: bool = True) -> None:
    if zip_path.exists():
        zip_path.unlink()
    base_dir = src_dir.parent if include_root_dir else src_dir
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as zf:
        for path in sorted(src_dir.rglob("*")):
            if path.is_dir():
                continue
            rel = path.relative_to(base_dir)
            # Store already-compressed binary assets; deflate uncompressed city/scenery databases (.nscity/.nsscn).
            compress = zipfile.ZIP_STORED if path.suffix in (".nspack", ".nsshd", ".nswx", ".png", ".glb") else zipfile.ZIP_DEFLATED
            zf.write(path, rel.as_posix(), compress_type=compress)


def stage_region_addon(
    stage_dir: Path,
    slug: str,
    title: str,
    details: str,
    files: tuple[str, ...],
    packs_dir: Path,
    version: str,
) -> None:
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    packs_dst = stage_dir / "packs"
    packs_dst.mkdir(parents=True)

    missing = [f for f in files if not (packs_dir / f).is_file()]
    if missing:
        raise FileNotFoundError(f"Missing {slug} pack files: " + ", ".join(missing))
    for fname in files:
        shutil.copy2(packs_dir / fname, packs_dst / fname)

    for name in ("LICENSE", "THIRD_PARTY_LICENSES.md"):
        src = ROOT / name
        if src.is_file():
            shutil.copy2(src, stage_dir / name)
    lic_dir = ROOT / "LICENSES"
    if lic_dir.is_dir():
        copy_tree(lic_dir, stage_dir / "LICENSES")

    install_txt = stage_dir / "INSTALL.txt"
    install_txt.write_text(
        f"OpenSky VFR {version} - Regional Scenery Pack: {title}\n"
        f"{details}\n\n"
        "Installation (Windows):\n"
        f"  1. Extract this archive into your OpenSky-{version}-win64 folder (next to OpenSky.bat)\n"
        "     so the files inside packs\\ merge into OpenSky's packs\\ folder.\n"
        "     Alternatively, copy the files inside packs\\ into:\n"
        "       %LOCALAPPDATA%\\opensky-vfr\\packs\\\n"
        "  2. Launch OpenSky.bat. The new region is discovered automatically.\n\n"
        "Data attribution & licenses:\n"
        "  - Terrain/imagery (.nspack, .relief.png): IGN Spain PNOA / MDT05 / MDT25 (CC BY 4.0),\n"
        "    OurAirports (Public Domain). See LICENSES\\CC-BY-4.0.txt and THIRD_PARTY_LICENSES.md.\n"
        "  - Cities/scenery (.nscity, .nsscn): OpenStreetMap contributors (ODbL 1.0 Share-Alike),\n"
        "    Poly Haven (CC0). See LICENSES\\ODbL-1.0.txt and THIRD_PARTY_LICENSES.md.\n",
        encoding="utf-8",
        newline="\r\n",
    )


def build_region_addon_zip(
    out_dir: Path,
    slug: str,
    title: str,
    details: str,
    files: tuple[str, ...],
    packs_dir: Path,
    version: str,
    no_zip: bool,
) -> list[Path]:
    stage_dir = out_dir / f"OpenSky-{version}-win64-{slug}"
    stage_region_addon(stage_dir, slug, title, details, files, packs_dir, version)
    print(f"[PACK-WIN] Staged addon {stage_dir}")
    if no_zip:
        return [stage_dir]

    zip_path = out_dir / f"OpenSky-{version}-win64-{slug}.zip"
    zip_directory(stage_dir, zip_path, include_root_dir=False)
    shutil.rmtree(stage_dir)
    size = zip_path.stat().st_size
    if size < MAX_RELEASE_ASSET_BYTES:
        print(f"[PACK-WIN] Built {zip_path} ({size / (1024 * 1024):.1f} MiB)")
        return [zip_path]

    # Split _cities.nscity into a companion archive if a region ever exceeds the 2 GiB cap.
    zip_path.unlink()
    city_files = tuple(f for f in files if f.endswith("_cities.nscity"))
    base_files = tuple(f for f in files if not f.endswith("_cities.nscity"))
    if not city_files:
        raise RuntimeError(f"{zip_path.name} exceeds 2 GiB GitHub release asset cap and cannot be split")

    outputs: list[Path] = []
    for sub_slug, sub_title, sub_files in (
        (slug, title, base_files),
        (f"{slug}-cities", f"{title} (3D Cities)", city_files),
    ):
        sub_stage = out_dir / f"OpenSky-{version}-win64-{sub_slug}"
        stage_region_addon(sub_stage, sub_slug, sub_title, details, sub_files, packs_dir, version)
        sub_zip = out_dir / f"OpenSky-{version}-win64-{sub_slug}.zip"
        zip_directory(sub_stage, sub_zip, include_root_dir=False)
        shutil.rmtree(sub_stage)
        sub_size = sub_zip.stat().st_size
        if sub_size >= MAX_RELEASE_ASSET_BYTES:
            raise RuntimeError(f"{sub_zip.name} still exceeds 2 GiB GitHub release asset cap")
        print(f"[PACK-WIN] Built {sub_zip} ({sub_size / (1024 * 1024):.1f} MiB)")
        outputs.append(sub_zip)
    return outputs


def main() -> int:
    ap = argparse.ArgumentParser(description="Package OpenSky VFR Windows x64 release.")
    ap.add_argument("--version", default="0.3.2")
    ap.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    ap.add_argument("--packs-dir", type=Path, default=ROOT / "packs")
    ap.add_argument("--no-packs", action="store_true", help="Create lightweight binary-only archive without scenery packs.")
    ap.add_argument("--no-zip", action="store_true", help="Stage directory only without writing .zip archive.")
    ap.add_argument(
        "--addons",
        default="",
        help="Optional comma-separated list of regional add-on slugs to package (or 'all' for every region present in --packs-dir).",
    )
    ap.add_argument(
        "--addons-only",
        action="store_true",
        help="Package only the selected regional add-on .zip archives without rebuilding the main Windows bundle.",
    )
    args = ap.parse_args()

    out_dir = args.output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    packs_dir = args.packs_dir.resolve()
    bundle_name = f"OpenSky-{args.version}-win64"

    if not args.addons_only:
        stage_dir = out_dir / bundle_name
        include_packs = not args.no_packs
        stage_release(stage_dir, packs_dir, include_packs=include_packs)
        print(f"[PACK-WIN] Staged {stage_dir}")

        if not args.no_zip:
            zip_name = f"{bundle_name}{'' if include_packs else '-bin'}.zip"
            zip_path = out_dir / zip_name
            zip_directory(stage_dir, zip_path, include_root_dir=True)
            size = zip_path.stat().st_size
            if size >= MAX_RELEASE_ASSET_BYTES:
                raise RuntimeError(f"{zip_path.name} exceeds 2 GiB GitHub release asset cap")
            print(f"[PACK-WIN] Built {zip_path} ({size / (1024 * 1024):.1f} MiB)")

    if args.addons:
        req = {s.strip() for s in args.addons.split(",") if s.strip()}
        select_all = "all" in req
        known = {a[0] for a in OPTIONAL_ADDONS}
        if not select_all:
            unknown = sorted(req - known)
            if unknown:
                ap.error("Unknown --addons slug(s): " + ", ".join(unknown))
        selected = [
            addon for addon in OPTIONAL_ADDONS
            if (select_all or addon[0] in req)
            and all((packs_dir / fn).is_file() for fn in addon[2])
        ]
        if not select_all:
            missing_slugs = [s for s in sorted(req) if s not in {a[0] for a in selected}]
            if missing_slugs:
                ap.error("Requested add-on pack files missing from --packs-dir: " + ", ".join(missing_slugs))
        for slug, title, files, details in selected:
            build_region_addon_zip(out_dir, slug, title, details, files, packs_dir, args.version, args.no_zip)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
