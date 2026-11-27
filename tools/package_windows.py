#!/usr/bin/env python3
"""Package a self-contained Windows x64 release archive for OpenSky VFR."""

from __future__ import annotations

import argparse
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ARAGON_FILES = (
    "es_aragon_pyrenees.nspack",
    "es_aragon_pyrenees.relief.png",
    "es_aragon_cities.nscity",
    "es_aragon_city_sh.nsshd",
    "es_aragon_scenery.nsscn",
    "es_aragon_weather.nswx",
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
        "@echo off\r\n"
        "setlocal\r\n"
        'cd /d "%~dp0"\r\n'
        '"%~dp0sim_engine\\OpenSky.exe" %*\r\n'
        "exit /b %ERRORLEVEL%\r\n",
        encoding="utf-8",
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


def zip_directory(src_dir: Path, zip_path: Path) -> None:
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
        for path in sorted(src_dir.rglob("*")):
            if path.is_dir():
                continue
            rel = path.relative_to(src_dir.parent)
            # Store already-compressed binary packs without re-deflating
            compress = zipfile.ZIP_STORED if path.suffix in (".nspack", ".nscity", ".nsshd", ".nsscn", ".nswx", ".png", ".glb") else zipfile.ZIP_DEFLATED
            zf.write(path, rel.as_posix(), compress_type=compress)


def main() -> int:
    ap = argparse.ArgumentParser(description="Package OpenSky VFR Windows x64 release.")
    ap.add_argument("--version", default="0.3.0")
    ap.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    ap.add_argument("--packs-dir", type=Path, default=ROOT / "packs")
    ap.add_argument("--no-packs", action="store_true", help="Create lightweight binary-only archive without scenery packs.")
    ap.add_argument("--no-zip", action="store_true", help="Stage directory only without writing .zip archive.")
    args = ap.parse_args()

    out_dir = args.output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    bundle_name = f"OpenSky-{args.version}-win64"
    stage_dir = out_dir / bundle_name

    include_packs = not args.no_packs
    stage_release(stage_dir, args.packs_dir.resolve(), include_packs=include_packs)
    print(f"[PACK-WIN] Staged {stage_dir}")

    if not args.no_zip:
        zip_name = f"{bundle_name}{'' if include_packs else '-bin'}.zip"
        zip_path = out_dir / zip_name
        zip_directory(stage_dir, zip_path)
        size_mb = zip_path.stat().st_size / (1024 * 1024)
        print(f"[PACK-WIN] Built {zip_path} ({size_mb:.1f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
