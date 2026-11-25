#!/usr/bin/env python3
"""Render quick visual previews of .nspack and .nswx for the GUI/compiler."""

from __future__ import annotations

import io
import json
import math
import re
import struct
import zlib
from pathlib import Path
from typing import Any, Optional

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

# Zoom-level quality palette (RGBA / hex aligned cleanly across PIL, Tkinter & HTML5)
ZOOM_COLORS = {
    10: ("#4b6b94", "Cruise (~113 m/px)"),
    11: ("#4d88b5", "Cruise (~57 m/px)"),
    12: ("#38a3c7", "Enroute (~28 m/px)"),
    13: ("#22c1c3", "Valley (~14 m/px / MDT05)"),
    14: ("#2cd49a", "Midslope (~7.1 m/px / 5m DEM)"),
    15: ("#4ade80", "Approach HD (~3.5 m/px)"),
    16: ("#a3e635", "City/Strip HD (~1.8 m/px)"),
    17: ("#facc15", "Sub-meter (~0.89 m/px)"),
    18: ("#fb923c", "Ultra-HD (~0.44 m/px)"),
    19: ("#f472b6", "Runway Detail (~0.22 m/px)"),
    20: ("#e879f9", "Hyper-HD (~0.11 m/px)"),
}


def tile_to_bbox(z: int, x: int, y: int) -> tuple[float, float, float, float]:
    """Return (lon_min, lat_min, lon_max, lat_max) for Web Mercator XYZ tile."""
    n = float(1 << z)
    lon0 = x / n * 360.0 - 180.0
    lon1 = (x + 1) / n * 360.0 - 180.0
    lat1 = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n))))
    lat0 = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y + 1) / n))))
    return lon0, lat0, lon1, lat1


def latlon_to_tile_xy(lat_deg: float, lon_deg: float, z: int) -> tuple[int, int]:
    n = float(1 << z)
    lat_r = math.radians(max(-85.0, min(85.0, lat_deg)))
    tx = int((lon_deg + 180.0) / 360.0 * n)
    ty = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    lim = (1 << z) - 1
    return max(0, min(lim, tx)), max(0, min(lim, ty))


def gsd_meters_per_px(z: int, lat_deg: float = 42.1) -> float:
    return (156543.033928 * math.cos(math.radians(lat_deg))) / float(1 << z)


def _merge_tile_spans(coords_by_z: dict[int, list[tuple[int, int]]]) -> list[dict[str, Any]]:
    """Collapse contiguous XYZ tiles per (z, ty) into bounding boxes so 16k+ tiles draw in <5ms."""
    spans: list[dict[str, Any]] = []
    for z, pairs in sorted(coords_by_z.items()):
        by_y: dict[int, list[int]] = {}
        for tx, ty in pairs:
            by_y.setdefault(ty, []).append(tx)
        for ty, xs in sorted(by_y.items()):
            xs.sort()
            _, lat0, _, lat1 = tile_to_bbox(z, xs[0], ty)
            run0 = xs[0]
            prev = xs[0]
            for x in xs[1:]:
                if x == prev + 1:
                    prev = x
                else:
                    lon0, _, lon1, _ = tile_to_bbox(z, run0, ty)
                    lon1 = tile_to_bbox(z, prev, ty)[2]
                    spans.append({"z": z, "bbox": [round(lon0, 5), round(lat0, 5), round(lon1, 5), round(lat1, 5)], "n": prev - run0 + 1})
                    run0 = prev = x
            lon0 = tile_to_bbox(z, run0, ty)[0]
            lon1 = tile_to_bbox(z, prev, ty)[2]
            spans.append({"z": z, "bbox": [round(lon0, 5), round(lat0, 5), round(lon1, 5), round(lat1, 5)], "n": prev - run0 + 1})
    return spans


def _infer_quality_label(max_img_z: int, max_elev_z: int, sh_res: int | None) -> str:
    if max_img_z >= 17 and (sh_res or 0) >= 160:
        return "ultra"
    if max_img_z >= 17 or max_elev_z >= 14:
        return "high"
    if max_img_z >= 15:
        return "medium"
    return "low"


def _meters_per_deg(lat_deg: float) -> tuple[float, float]:
    p = math.radians(lat_deg)
    return (
        111132.92 - 559.82 * math.cos(2.0 * p) + 1.175 * math.cos(4.0 * p),
        111412.84 * math.cos(p) - 93.5 * math.cos(3.0 * p),
    )


def _read_nsshd_meta(path: Path, bbox: list[float]) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        with path.open("rb") as f:
            hdr = f.read(20)
            if len(hdr) < 20:
                return None
            magic, ver, res, chunk_m, count = struct.unpack("<4sIIfI", hdr)
            if magic != b"NSSH" or ver != 2:
                return None
            idx_raw = f.read(count * 16)
        lon_min, lat_min, lon_max, lat_max = bbox
        alat = 0.5 * (lat_min + lat_max)
        alon = 0.5 * (lon_min + lon_max)
        m_lat, m_lon = _meters_per_deg(alat)
        cells: dict[int, list[int]] = {}
        for i in range(min(count, len(idx_raw) // 16)):
            cx, cy, _off, _csz = struct.unpack_from("<iiII", idx_raw, i * 16)
            cells.setdefault(cy, []).append(cx)
        # Merge consecutive 500m city chunks horizontally
        spans: list[list[float]] = []
        for cy, cxs in cells.items():
            cxs.sort()
            lat0 = alat + (cy * chunk_m) / m_lat
            lat1 = alat + ((cy + 1) * chunk_m) / m_lat
            r0 = p = cxs[0]
            for c in cxs[1:]:
                if c == p + 1:
                    p = c
                else:
                    lon0 = alon + (r0 * chunk_m) / m_lon
                    lon1 = alon + ((p + 1) * chunk_m) / m_lon
                    spans.append([round(lon0, 5), round(lat0, 5), round(lon1, 5), round(lat1, 5)])
                    r0 = p = c
            lon0 = alon + (r0 * chunk_m) / m_lon
            lon1 = alon + ((p + 1) * chunk_m) / m_lon
            spans.append([round(lon0, 5), round(lat0, 5), round(lon1, 5), round(lat1, 5)])
        return {
            "path": path.name,
            "size_mb": round(path.stat().st_size / 1e6, 2),
            "version": ver,
            "res": res,
            "chunk_m": chunk_m,
            "gsd_m": round(chunk_m / max(1, res), 2),
            "chunks": count,
            "spans": spans,
        }
    except Exception:
        return None


def inspect_installed_modules(packs_dir: Path | None = None, cache_dir: Path | None = None) -> dict[str, Any]:
    """Return full inventory of installed regions, layers, zoom quality histograms, and online cache."""
    from protocol.pack_format import LAYER_ELEVATION, LAYER_IMAGERY, LAYER_SURFACE, PackReader

    pdir = packs_dir or (ROOT / "packs")
    cdir = cache_dir or (ROOT / "cache" / "online_tiles")
    reg_file = pdir / "registry.json"
    registry: dict[str, Any] = {}
    if reg_file.is_file():
        try:
            registry = json.loads(reg_file.read_text(encoding="utf-8")).get("regions", {})
        except Exception:
            registry = {}

    regions: list[dict[str, Any]] = []
    if pdir.is_dir():
        for p in sorted(pdir.glob("*.nspack")):
            rid = p.stem
            reg_e = dict(registry.get(rid, {}))
            with PackReader(p) as r:
                m = r.manifest
                bbox = list(m.get("bbox", r.bbox))
                mid_lat = 0.5 * (bbox[1] + bbox[3])
                by_lz: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
                for k, meta in r.index.items():
                    layer = (k >> 56) & 0xFF
                    z = (k >> 48) & 0xFF
                    tx = (k >> 24) & 0xFFFFFF
                    ty = k & 0xFFFFFF
                    by_lz.setdefault((layer, z), []).append((tx, ty, meta[1]))

                layer_names = {LAYER_ELEVATION: "elevation", LAYER_IMAGERY: "imagery", LAYER_SURFACE: "surface"}
                layers_out: dict[str, Any] = {}
                for lid, lname in layer_names.items():
                    z_stats: list[dict[str, Any]] = []
                    coords_z: dict[int, list[tuple[int, int]]] = {}
                    for (ll, z), lst in sorted(by_lz.items()):
                        if ll != lid:
                            continue
                        coords_z[z] = [(a, b) for a, b, _ in lst]
                        z_stats.append({
                            "z": z,
                            "tiles": len(lst),
                            "mb": round(sum(sz for _, _, sz in lst) / 1e6, 2),
                            "gsd_m": round(gsd_meters_per_px(z, mid_lat), 2),
                            "color": ZOOM_COLORS.get(z, ("#94a3b8", ""))[0],
                        })
                    layers_out[lname] = {
                        "zooms": z_stats,
                        "total_tiles": sum(s["tiles"] for s in z_stats),
                        "total_mb": round(sum(s["mb"] for s in z_stats), 2),
                        "spans": _merge_tile_spans(coords_z),
                    }

                stem_base = rid.replace("_pyrenees", "")
                def _find_comp(ext: str) -> Path | None:
                    for c in (pdir / f"{stem_base}{ext}", pdir / f"{rid}{ext}"):
                        if c.is_file():
                            return c
                    return None

                sh_meta = None
                sh_p = _find_comp("_city_sh.nsshd")
                if sh_p:
                    sh_meta = _read_nsshd_meta(sh_p, bbox)

                city_meta = None
                ct_p = _find_comp("_cities.nscity")
                if ct_p:
                    with ct_p.open("rb") as cf:
                        ch = cf.read(16)
                    cver, ccnt = (struct.unpack("<II", ch[4:12]) if len(ch) >= 12 and ch[:4] == b"NSCT" else (0, 0))
                    city_meta = {"path": ct_p.name, "size_mb": round(ct_p.stat().st_size / 1e6, 2), "version": cver, "chunks": ccnt}

                scn_meta = None
                scn_p = _find_comp("_scenery.nsscn")
                if scn_p:
                    scn_meta = {"path": scn_p.name, "size_mb": round(scn_p.stat().st_size / 1e6, 2)}

                wx_meta = None
                wx_p = _find_comp("_weather.nswx")
                if wx_p:
                    wx_meta = {"path": wx_p.name, "size_mb": round(wx_p.stat().st_size / 1e6, 2), "states": 220}

                max_img_z = max((s["z"] for s in layers_out["imagery"]["zooms"]), default=10)
                max_elev_z = max((s["z"] for s in layers_out["elevation"]["zooms"]), default=10)
                qual = reg_e.get("quality") or _infer_quality_label(max_img_z, max_elev_z, sh_meta["res"] if sh_meta else None)
                total_mb = p.stat().st_size / 1e6 + sum(
                    float(x["size_mb"]) for x in (sh_meta, city_meta, scn_meta, wx_meta) if x
                )

                airports = [
                    {"id": a.get("id", ""), "name": a.get("name", ""), "lat": a.get("lat", 0.0), "lon": a.get("lon", 0.0), "kind": a.get("kind", "")}
                    for a in m.get("airports", [])
                ]

                regions.append({
                    "region_id": rid,
                    "name": m.get("name", rid),
                    "pack_file": p.name,
                    "pack_mb": round(p.stat().st_size / 1e6, 2),
                    "total_mb": round(total_mb, 2),
                    "quality": qual,
                    "bbox": bbox,
                    "max_ortho_z": max_img_z,
                    "max_ortho_gsd_m": round(gsd_meters_per_px(max_img_z, mid_lat), 2),
                    "max_dem_z": max_elev_z,
                    "max_dem_gsd_m": round(gsd_meters_per_px(max_elev_z, mid_lat), 2),
                    "tile_count": r.index_count,
                    "airports_count": len(airports),
                    "airports": airports,
                    "layers": layers_out,
                    "city_shadows": sh_meta,
                    "cities": city_meta,
                    "scenery": scn_meta,
                    "weather": wx_meta,
                })

    # Inspect Live Online Tile Cache (cache/online_tiles/{prov}_z{z}_{x}_{y}.jpg)
    online_pat = re.compile(r"^(pnoa|bing|google)_z(\d+)_(\d+)_(\d+)\.jpg$")
    onl_coords: dict[int, list[tuple[int, int]]] = {}
    onl_by_pz: dict[tuple[str, int], list[int]] = {}
    onl_total_bytes = 0
    if cdir.is_dir():
        for entry in cdir.iterdir():
            m = online_pat.match(entry.name)
            if not m or not entry.is_file():
                continue
            prov = m.group(1)
            z, tx, ty = int(m.group(2)), int(m.group(3)), int(m.group(4))
            sz = entry.stat().st_size
            onl_total_bytes += sz
            onl_coords.setdefault(z, []).append((tx, ty))
            onl_by_pz.setdefault((prov, z), []).append(sz)

    online_stats = [
        {
            "provider": prov,
            "z": z,
            "tiles": len(szs),
            "mb": round(sum(szs) / 1e6, 2),
            "gsd_m": round(gsd_meters_per_px(z, 42.1), 2),
            "color": ZOOM_COLORS.get(z, ("#e879f9", ""))[0],
        }
        for (prov, z), szs in sorted(onl_by_pz.items())
    ]

    return {
        "regions": regions,
        "online_cache": {
            "total_tiles": sum(len(v) for v in onl_coords.values()),
            "total_mb": round(onl_total_bytes / 1e6, 2),
            "by_provider_zoom": online_stats,
            "spans": _merge_tile_spans(onl_coords),
        },
        "zoom_legend": [
            {"z": z, "color": col, "label": lbl}
            for z, (col, lbl) in sorted(ZOOM_COLORS.items())
        ],
    }


def _hex_rgba(hex_c: str, alpha: int) -> tuple[int, int, int, int]:
    h = hex_c.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), alpha


def render_tile_quality_map(
    out_png: Path,
    packs_dir: Path | None = None,
    cache_dir: Path | None = None,
    region_id: str | None = None,
    show_layers: tuple[str, ...] = ("imagery", "cities", "online"),
    zoom_filter: int | None = None,
    highlight_bbox: tuple[float, float, float, float] | None = None,
) -> tuple[Path, dict[str, Any]]:
    """Render high-res satellite/relief mosaic + color-coded tile quality heatmap PNG."""
    from protocol.pack_format import LAYER_IMAGERY, PackReader

    pdir = packs_dir or (ROOT / "packs")
    cdir = cache_dir or (ROOT / "cache" / "online_tiles")
    inv = inspect_installed_modules(pdir, cdir)
    regs = inv["regions"]
    all_mode = (region_id == "__all__")
    reg = None if all_mode else next((x for x in regs if x["region_id"] == region_id), regs[0] if regs else None)
    target_regs = regs if all_mode else ([reg] if reg else [])

    if all_mode or not reg:
        lon0, lat0, lon1, lat1 = -9.90, 35.15, 4.75, 44.55
        pad_lon = pad_lat = 0.0
    else:
        lon0, lat0, lon1, lat1 = reg["bbox"]
        pad_lon = (lon1 - lon0) * 0.04
        pad_lat = (lat1 - lat0) * 0.04
    vx0, vy0, vx1, vy1 = lon0 - pad_lon, lat0 - pad_lat, lon1 + pad_lon, lat1 + pad_lat

    W, H = 1280, 860
    base = Image.new("RGBA", (W, H), (8, 18, 30, 255))

    def to_px(lon: float, lat: float) -> tuple[float, float]:
        px = (lon - vx0) / (vx1 - vx0) * W
        py = (1.0 - (lat - vy0) / (vy1 - vy0)) * H
        return px, py

    # 1. Paint authentic ortho mosaic basemap from the .nspack z11/z12 imagery tiles!
    for r_item in target_regs:
        pack_path = pdir / r_item["pack_file"]
        rl0, rlat0, rl1, rlat1 = r_item["bbox"]
        with PackReader(pack_path) as r:
            iz = [s["z"] for s in r_item["layers"]["imagery"]["zooms"]]
            bz = 10 if (all_mode and 10 in iz) else (11 if 11 in iz else (12 if 12 in iz else (min(iz) if iz else 10)))
            x_a, y_b = latlon_to_tile_xy(rlat0, rl0, bz)
            x_b, y_a = latlon_to_tile_xy(rlat1, rl1, bz)
            for ty in range(y_a, y_b + 1):
                for tx in range(x_a, x_b + 1):
                    raw = r.read_tile(LAYER_IMAGERY, bz, tx, ty, verify_crc=False)
                    if not raw:
                        continue
                    tl0, tlat0, tl1, tlat1 = tile_to_bbox(bz, tx, ty)
                    px0, py0 = to_px(tl0, tlat1)
                    px1, py1 = to_px(tl1, tlat0)
                    iw = max(1, int(round(px1 - px0)))
                    ih = max(1, int(round(py1 - py0)))
                    tile_im = Image.open(io.BytesIO(raw)).convert("RGB").resize((iw, ih), Image.Resampling.BILINEAR)
                    arr = (np.asarray(tile_im, dtype=np.float32) * 0.68).astype(np.uint8)
                    rgba = Image.fromarray(arr).convert("RGBA")
                    base.alpha_composite(rgba, (int(round(px0)), int(round(py0))))

    overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    odraw = ImageDraw.Draw(overlay)

    # In All-Spain overview mode, outline every available Spain preset so users see installed vs buildable areas.
    if all_mode or not target_regs:
        try:
            from builder.build_region import ALL_SPAIN_PRESETS, PRESETS
            inst_ids = {x["region_id"] for x in regs}
            for pk in ALL_SPAIN_PRESETS:
                pr = PRESETS[pk]
                bx0, by0, bx1, by1 = pr["bbox"]
                px0, py0 = to_px(bx0, by1)
                px1, py1 = to_px(bx1, by0)
                is_inst = pr["region_id"] in inst_ids
                col = (56, 189, 248, 220) if is_inst else (42, 106, 138, 125)
                odraw.rectangle([px0, py0, px1, py1], outline=col, width=2 if is_inst else 1)
                odraw.text((px0 + 4, py0 + 3), pk, fill=(210, 240, 255, 220) if is_inst else (110, 155, 180, 185))
        except Exception:
            pass

    # 2. Draw per-zoom installed tile coverage cleanly (no horizontal scan-line artifacts!)
    for lkey in ("elevation", "surface", "imagery"):
        if not target_regs or lkey not in show_layers:
            continue
        z_groups: dict[int, list[dict[str, Any]]] = {}
        for r_item in target_regs:
            for sp in r_item["layers"][lkey]["spans"]:
                z = sp["z"]
                if zoom_filter is not None and z != zoom_filter:
                    continue
                z_groups.setdefault(z, []).append(sp)
        for z, sps in sorted(z_groups.items()):
            layer_ovr = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ld = ImageDraw.Draw(layer_ovr)
            col_hex = ZOOM_COLORS.get(z, ("#94a3b8", ""))[0]
            alpha_fill = 55 if lkey != "imagery" else (32 if z <= 12 else (72 if z <= 14 else (105 if z <= 16 else 155)))
            fill_c = _hex_rgba(col_hex, alpha_fill)
            for sp in sps:
                bx0, by0, bx1, by1 = sp["bbox"]
                px0, py0 = to_px(bx0, by1)
                px1, py1 = to_px(bx1, by0)
                ld.rectangle([int(math.floor(px0)), int(math.floor(py0)), int(math.ceil(px1)), int(math.ceil(py1))], fill=fill_c)
            overlay = Image.alpha_composite(overlay, layer_ovr)
    odraw = ImageDraw.Draw(overlay)

    # 3. Baked 500m 10-TOD Cities & 3-Layer Night Light chunks (.nsshd)
    if "cities" in show_layers:
        for r_item in target_regs:
            if not r_item.get("city_shadows"):
                continue
            for bx0, by0, bx1, by1 in r_item["city_shadows"]["spans"]:
                px0, py0 = to_px(bx0, by1)
                px1, py1 = to_px(bx1, by0)
                odraw.rectangle([px0, py0, max(px0 + 2, px1), max(py0 + 2, py1)], fill=(251, 146, 60, 115))

    # 4. Online Cached Tiles (cache/online_tiles)
    if "online" in show_layers:
        for sp in inv["online_cache"]["spans"]:
            z = sp["z"]
            if zoom_filter is not None and z != zoom_filter:
                continue
            bx0, by0, bx1, by1 = sp["bbox"]
            if bx1 < vx0 or bx0 > vx1 or by1 < vy0 or by0 > vy1:
                continue
            px0, py0 = to_px(bx0, by1)
            px1, py1 = to_px(bx1, by0)
            odraw.rectangle([int(math.floor(px0)), int(math.floor(py0)), int(math.ceil(max(px0 + 3, px1))), int(math.ceil(max(py0 + 3, py1)))], fill=(232, 121, 249, 155))

    if highlight_bbox:
        hx0, hy0, hx1, hy1 = highlight_bbox
        px0, py0 = to_px(hx0, hy1)
        px1, py1 = to_px(hx1, hy0)
        odraw.rectangle([px0, py0, px1, py1], outline=(255, 224, 102, 240), width=3)

    base = Image.alpha_composite(base, overlay)
    draw = ImageDraw.Draw(base)

    # 5. Airports & Crisp Legend Banner
    for r_item in target_regs:
        for ap in r_item["airports"]:
            if all_mode and "large" not in ap["kind"] and "medium" not in ap["kind"]:
                continue
            px, py = to_px(ap["lon"], ap["lat"])
            r_dot = 4 if "large" in ap["kind"] or "medium" in ap["kind"] else 3
            draw.ellipse([px - r_dot, py - r_dot, px + r_dot, py + r_dot], fill=(255, 255, 255, 255), outline=(10, 22, 40, 255))
            draw.text((px + 6, py - 5), ap["id"], fill=(255, 240, 170, 255))

    draw.rectangle([10, 10, W - 10, 52], fill=(6, 16, 27, 215), outline=(42, 106, 138, 255))
    if all_mode and target_regs:
        tot_mb = round(sum(float(x["total_mb"]) for x in target_regs), 2)
        tot_ap = sum(int(x["airports_count"]) for x in target_regs)
        hdr_txt = (
            f"SPAIN OVERVIEW: {len(target_regs)} installed module(s) | {tot_mb} MB | {tot_ap} airports | "
            f"Online Cache: {inv['online_cache']['total_tiles']} tiles ({inv['online_cache']['total_mb']} MB)"
        )
    elif reg:
        hdr_txt = (
            f"MODULE: {reg['region_id']} [{reg['quality'].upper()}] | {reg['total_mb']} MB | "
            f"Ortho max Z{reg['max_ortho_z']} ({reg['max_ortho_gsd_m']} m/px) | "
            f"DEM max Z{reg['max_dem_z']} ({reg['max_dem_gsd_m']} m/px) | "
            f"Online Cache: {inv['online_cache']['total_tiles']} tiles ({inv['online_cache']['total_mb']} MB)"
        )
    else:
        hdr_txt = (
            f"SPAIN MAP (NO MODULES INSTALLED YET) | "
            f"Online Cache: {inv['online_cache']['total_tiles']} tiles ({inv['online_cache']['total_mb']} MB)"
        )
    draw.text((22, 23), hdr_txt, fill=(220, 240, 255, 255))

    # Bottom legend strip
    draw.rectangle([10, H - 46, W - 10, H - 10], fill=(6, 16, 27, 220), outline=(42, 106, 138, 255))
    lx = 20
    for z, lbl in ((12, "Z12 28m"), (14, "Z14 7m DEM"), (15, "Z15 3.5m"), (16, "Z16 1.8m"), (17, "Z17 0.89m")):
        col = _hex_rgba(ZOOM_COLORS[z][0], 235)
        draw.rectangle([lx, H - 35, lx + 14, H - 21], fill=col, outline=(255, 255, 255, 180))
        draw.text((lx + 19, H - 34), lbl, fill=(210, 230, 245, 255))
        lx += 135
    draw.rectangle([lx, H - 35, lx + 14, H - 21], fill=(251, 146, 60, 220))
    draw.text((lx + 19, H - 34), "500m Baked Cities/RT-Night", fill=(210, 230, 245, 255))
    lx += 225
    draw.rectangle([lx, H - 35, lx + 14, H - 21], fill=(232, 121, 249, 220), outline=(250, 204, 21, 230))
    draw.text((lx + 19, H - 34), "Online Cache", fill=(210, 230, 245, 255))

    out_png.parent.mkdir(parents=True, exist_ok=True)
    base.convert("RGB").save(out_png)
    inv["view_bbox"] = [round(vx0, 5), round(vy0, 5), round(vx1, 5), round(vy1, 5)]
    return out_png, inv


def inspect_point_tiles(
    lat: float, lon: float, out_png: Path,
    packs_dir: Path | None = None, cache_dir: Path | None = None,
    region_id: str | None = None,
) -> dict[str, Any]:
    """Extract highest-res installed Ortho, DEM, Surface & Online cached tiles at (lat, lon) and save side-by-side PNG."""
    from protocol.pack_format import LAYER_ELEVATION, LAYER_IMAGERY, PackReader

    pdir = packs_dir or (ROOT / "packs")
    cdir = cache_dir or (ROOT / "cache" / "online_tiles")
    packs = sorted(pdir.glob("*.nspack"))
    if region_id:
        packs = [p for p in packs if p.stem == region_id] or packs
    if not packs:
        raise FileNotFoundError("No .nspack found")

    chosen = None
    for p in packs:
        with PackReader(p) as r:
            b = r.bbox
            if b[0] <= lon <= b[2] and b[1] <= lat <= b[3]:
                chosen = p
                break
    if chosen is None and region_id and region_id != "__all__":
        chosen = packs[0]

    panels: list[tuple[str, Image.Image]] = []
    info: dict[str, Any] = {"lat": round(lat, 5), "lon": round(lon, 5), "pack": chosen.name if chosen else None}

    if chosen is None:
        try:
            from builder.build_region import ALL_SPAIN_PRESETS, PRESETS
            cand = sorted(
                (pk for pk in ALL_SPAIN_PRESETS if PRESETS[pk]["bbox"][0] <= lon <= PRESETS[pk]["bbox"][2] and PRESETS[pk]["bbox"][1] <= lat <= PRESETS[pk]["bbox"][3]),
                key=lambda k: (PRESETS[k]["bbox"][2] - PRESETS[k]["bbox"][0]) * (PRESETS[k]["bbox"][3] - PRESETS[k]["bbox"][1]),
            )
            if cand:
                info["covering_preset"] = cand[0]
        except Exception:
            pass
    else:
      with PackReader(chosen) as r:
        # Highest installed imagery tile at point
        izs = sorted(set(r.manifest.get("imagery_zooms", [])), reverse=True)
        for z in izs:
            tx, ty = latlon_to_tile_xy(lat, lon, z)
            raw = r.read_tile(LAYER_IMAGERY, z, tx, ty, verify_crc=False)
            if raw:
                im = Image.open(io.BytesIO(raw)).convert("RGB").resize((256, 256), Image.Resampling.BILINEAR)
                gsd = round(gsd_meters_per_px(z, lat), 2)
                panels.append((f"PACK ORTHO Z{z}\n({gsd} m/px | {len(raw)/1024:.0f} KB)", im))
                info.update({"ortho_z": z, "ortho_tx": tx, "ortho_ty": ty, "ortho_gsd_m": gsd, "ortho_kb": round(len(raw) / 1024.0, 1)})
                break

        # Highest installed DEM tile at point (shaded topographic relief preview!)
        ezs = sorted(set(r.manifest.get("elevation_zooms", [])), reverse=True)
        for z in ezs:
            tx, ty = latlon_to_tile_xy(lat, lon, z)
            arr = r.read_tile(LAYER_ELEVATION, z, tx, ty, verify_crc=False)
            if arr is not None:
                a = np.asarray(arr, dtype=np.float32)
                lo, hi = float(np.min(a)), float(np.max(a))
                norm = (a - lo) / max(1.0, hi - lo)
                gy, gx = np.gradient(a)
                shade = np.clip(0.75 + (-gx * 0.4 - gy * 0.4) / max(4.0, (hi - lo) * 0.08), 0.35, 1.25)
                r_ch = np.clip((48 + norm * 175) * shade, 0, 255).astype(np.uint8)
                g_ch = np.clip((88 + norm * 140) * shade, 0, 255).astype(np.uint8)
                b_ch = np.clip((45 + np.power(norm, 1.8) * 195) * shade, 0, 255).astype(np.uint8)
                rgb = np.stack([r_ch, g_ch, b_ch], axis=-1)
                im = Image.fromarray(rgb, mode="RGB").resize((256, 256), Image.Resampling.BILINEAR)
                gsd = round(gsd_meters_per_px(z, lat), 2)
                panels.append((f"DEM Z{z} ({gsd} m/px)\n{lo:.0f}..{hi:.0f} m MSL", im))
                info.update({"dem_z": z, "dem_gsd_m": gsd, "elev_range_m": [round(lo, 1), round(hi, 1)]})
                break

        # Companion 500m 10-TOD Shadow + 3-Layer RT Night Lightmap tile (.nsshd v2) if populated at (lat, lon)
        stem_base = chosen.stem.replace("_pyrenees", "")
        for cand_sh in (pdir / f"{stem_base}_city_sh.nsshd", pdir / f"{chosen.stem}_city_sh.nsshd"):
            if not cand_sh.is_file():
                continue
            b = r.bbox
            alat, alon = 0.5 * (b[1] + b[3]), 0.5 * (b[0] + b[2])
            m_lat, m_lon = _meters_per_deg(alat)
            em, nm = (lon - alon) * m_lon, (lat - alat) * m_lat
            with cand_sh.open("rb") as sf:
                shdr = sf.read(20)
                if len(shdr) == 20 and shdr[:4] == b"NSSH":
                    _mg, _vr, sres, ch_m, cnt = struct.unpack("<4sIIfI", shdr)
                    tcx, tcy = int(math.floor(em / ch_m)), int(math.floor(nm / ch_m))
                    idx_buf = sf.read(cnt * 16)
                    for ii in range(cnt):
                        cx0, cy0, off0, csz0 = struct.unpack_from("<iiII", idx_buf, ii * 16)
                        if cx0 == tcx and cy0 == tcy:
                            sf.seek(off0)
                            u32 = np.frombuffer(zlib.decompress(sf.read(csz0)), dtype="<u4").reshape((sres, sres))
                            # Decode TOD2 morning shadow (R/B) + 3L RT Night light (amber G/R)!
                            vis2 = ((u32 >> 4) & 3).astype(np.float32) / 3.0
                            night = ((u32 >> 28) & 15).astype(np.float32) / 15.0
                            cr = np.clip(vis2 * 140 + night * 255, 0, 255).astype(np.uint8)
                            cg = np.clip(vis2 * 170 + night * 145, 0, 255).astype(np.uint8)
                            cb = np.clip(vis2 * 210 + night * 30, 0, 255).astype(np.uint8)
                            shim = Image.fromarray(np.flipud(np.stack([cr, cg, cb], axis=-1)), mode="RGB").resize((256, 256), Image.Resampling.NEAREST)
                            panels.append((f"BAKED CITY SH/NIGHT ({ch_m/sres:.1f}m)\nChunk ({tcx},{tcy}) 10-TOD+3L-RT", shim))
                            info["city_chunk"] = [tcx, tcy]
                            break
            break

    # Online cached tile at point (highest zoom available)
    if cdir.is_dir():
        for z in range(20, 12, -1):
            tx, ty = latlon_to_tile_xy(lat, lon, z)
            found = None
            for prov in ("pnoa", "bing", "google"):
                cand = cdir / f"{prov}_z{z}_{tx}_{ty}.jpg"
                if cand.is_file() and cand.stat().st_size > 400:
                    found = (prov, cand)
                    break
            if found:
                prov, cp = found
                im = Image.open(cp).convert("RGB").resize((256, 256), Image.Resampling.BILINEAR)
                gsd = round(gsd_meters_per_px(z, lat), 2)
                panels.append((f"ONLINE {prov.upper()} Z{z}\n({gsd} m/px cached)", im))
                info.update({"online_provider": prov, "online_z": z, "online_gsd_m": gsd})
                break

    if not panels:
        panels.append(("No tile at cursor", Image.new("RGB", (256, 256), (18, 30, 48))))

    pad = 12
    cw = len(panels) * (256 + pad) + pad
    ch = 256 + 60
    sheet = Image.new("RGB", (cw, ch), (8, 18, 30))
    dr = ImageDraw.Draw(sheet)
    for idx, (cap, im) in enumerate(panels):
        x = pad + idx * (256 + pad)
        sheet.paste(im, (x, 12))
        dr.rectangle([x, 12, x + 256, 268], outline=(60, 130, 170))
        dr.text((x + 4, 274), cap, fill=(210, 235, 255))
    out_png.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_png)
    info["preview_png"] = str(out_png)
    return info



def render_pack_elevation_heatmap(pack_path: Path, out_png: Path, max_dim: int = 1400) -> Optional[Path]:
    from protocol.pack_format import LAYER_ELEVATION, PackReader

    with PackReader(pack_path) as r:
        min_lon, min_lat, max_lon, max_lat = r.bbox
        # Pick a mid elevation zoom level available
        ez = sorted(r.manifest.get("elevation_zooms") or r.manifest.get("elev_zooms") or [10, 11, 12])
        z = ez[min(2, len(ez) - 1)] if ez else 12
        n = 2 ** z
        x0 = int(math.floor((min_lon + 180.0) / 360.0 * n))
        x1 = int(math.floor((max_lon + 180.0) / 360.0 * n))
        y0 = int(math.floor((1.0 - math.asinh(math.tan(math.radians(max_lat))) / math.pi) / 2.0 * n))
        y1 = int(math.floor((1.0 - math.asinh(math.tan(math.radians(min_lat))) / math.pi) / 2.0 * n))
        x0, x1 = max(0, x0), min(n - 1, max(x0, x1))
        y0, y1 = max(0, y0), min(n - 1, max(y0, y1))
        tiles_x = x1 - x0 + 1
        tiles_y = y1 - y0 + 1
        if tiles_x <= 0 or tiles_y <= 0:
            return None
        # Limit total tiles by downsampling fetch grid
        step = max(1, int(math.ceil(math.sqrt((tiles_x * tiles_y) / 64.0))))
        xs = list(range(x0, x1 + 1, step))
        ys = list(range(y0, y1 + 1, step))
        cols, rows = len(xs), len(ys)
        H_px = rows * 256
        W_px = cols * 256
        dem = np.full((H_px, W_px), np.nan, dtype=np.float32)
        for iy, ty in enumerate(ys):
            for ix, tx in enumerate(xs):
                arr = r.read_tile(LAYER_ELEVATION, z, tx, ty, verify_crc=False)
                if arr is None:
                    continue
                a = np.asarray(arr, dtype=np.float32)
                y1p, y2p = iy * 256, min((iy + 1) * 256, H_px)
                x1p, x2p = ix * 256, min((ix + 1) * 256, W_px)
                ah = y2p - y1p
                aw = x2p - x1p
                dem[y1p:y2p, x1p:x2p] = a[:ah, :aw]
        valid = ~np.isnan(dem)
        if not np.any(valid):
            return None
        dmin, dmax = float(np.nanmin(dem)), float(np.nanmax(dem))
        if dmax <= dmin:
            dmax = dmin + 1.0
        norm = (np.clip(dem, dmin, dmax) - dmin) / (dmax - dmin)
        # Terrain-like colormap via numpy (no matplotlib required for basic PNG)
        try:
            import matplotlib.pyplot as plt
            from matplotlib.colors import LightSource
            ls = LightSource(azdeg=315, altdeg=45)
            rgb = ls.shade(norm, cmap=plt.get_cmap("terrain"), vert_exag=0.6, blend_mode="soft")
            img = (np.clip(rgb[:, :, :3], 0, 1) * 255).astype(np.uint8)
        except Exception:
            img = (norm * 255).astype(np.uint8)
            img = np.stack([img, img, img], axis=-1)
        if img.shape[0] > max_dim or img.shape[1] > max_dim:
            scale = max_dim / max(img.shape[0], img.shape[1])
            img = np.array(Image.fromarray(img).resize((int(img.shape[1] * scale), int(img.shape[0] * scale)), Image.BILINEAR))
        out_png.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(img).save(out_png)
        return out_png


def render_nswx_previews(nswx_path: Path, out_dir: Path, season: int = 2, slot: int = 5, wtype: int = 0) -> list[Path]:
    """Generate heatmaps for u,v,w,cloud per level and a wind vector map (level 1)."""
    from protocol.weather_format import (
        GRID_H, GRID_W, LEVELS, LEVEL_AGL_M,
        HEADER_SIZE, STATE_SIZE, CHAN_HI, CHAN_LO,
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    data = nswx_path.read_bytes()
    if len(data) < HEADER_SIZE + STATE_SIZE * 220:
        return []
    entries_off = HEADER_SIZE
    idx = (season * 11 + slot) * 5 + wtype
    entry_base = entries_off + idx * STATE_SIZE
    fields = struct.unpack_from("<4IQ16f32s", data, entry_base)
    comp_size = int(fields[3])
    offset = int(fields[4])
    comp = data[offset: offset + comp_size]
    raw_sz = LEVELS * GRID_H * GRID_W * 4
    raw = zlib.decompress(comp, wbits=15)
    if len(raw) != raw_sz:
        return []
    arr = np.frombuffer(raw, dtype=np.uint8).reshape((LEVELS, GRID_H, GRID_W, 4)).astype(np.float32)
    for ch in range(4):
        lo, hi = CHAN_LO[ch], CHAN_HI[ch]
        arr[:, :, :, ch] = lo + arr[:, :, :, ch] / 255.0 * (hi - lo)

    import matplotlib.pyplot as plt
    written: list[Path] = []
    ch_names = ["u_mps (east+)", "v_mps (north+)", "w_mps (up+)", "cloud_delta"]
    for lev in range(LEVELS):
        for ch in range(4):
            fig, ax = plt.subplots(figsize=(8, 6))
            d = arr[lev, :, :, ch]
            if ch == 2:
                vmax = max(0.5, float(np.percentile(np.abs(d), 99)))
                im = ax.imshow(d, origin="lower", cmap="RdBu_r", vmin=-vmax, vmax=vmax)
            elif ch == 3:
                im = ax.imshow(d, origin="lower", cmap="BrBG", vmin=-1, vmax=1)
            else:
                mx = max(0.5, float(np.percentile(np.abs(d), 99)))
                im = ax.imshow(d, origin="lower", cmap="RdBu_r", vmin=-mx, vmax=mx)
            ax.set_title(f"NSWX {nswx_path.stem}\nS{season} T{slot} W{wtype} | L{lev} {LEVEL_AGL_M[lev]}m AGL — {ch_names[ch]}")
            plt.colorbar(im, ax=ax, shrink=0.8)
            out = out_dir / f"nswx_L{lev}_{['u','v','w','cloud'][ch]}.png"
            fig.tight_layout()
            fig.savefig(out, dpi=140)
            plt.close(fig)
            written.append(out)

    levm = 1
    u = arr[levm, :, :, 0]
    v = arr[levm, :, :, 1]
    step = max(1, GRID_H // 24)
    ys, xs = np.mgrid[0:GRID_H:step, 0:GRID_W:step]
    uu, vv = u[::step, ::step], v[::step, ::step]
    fig, ax = plt.subplots(figsize=(9, 7))
    spd = np.hypot(u, v)
    im = ax.imshow(spd, origin="lower", cmap="viridis")
    ax.quiver(xs, ys, uu, vv, color="white", alpha=0.85, scale=None, width=0.0035)
    ax.set_title(f"Wind speed + vectors (L{levm}, {LEVEL_AGL_M[levm]}m AGL) — baked blended state")
    plt.colorbar(im, ax=ax, label="speed (m/s)", shrink=0.8)
    outv = out_dir / "nswx_wind_vectors.png"
    fig.tight_layout()
    fig.savefig(outv, dpi=140)
    plt.close(fig)
    written.append(outv)
    return written
