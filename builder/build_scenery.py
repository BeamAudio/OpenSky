#!/usr/bin/env python3
"""build_scenery.py -- Extract & Offline Pre-Bake real OpenStreetMap (ODbL) buildings & roads
from Geofabrik Aragon .osm.pbf + packs/es_aragon_pyrenees.nspack into packs/es_aragon_scenery.nsscn (v2).

Offline Pre-Baked per Building (36 bytes):
  - lat_e6, lon_e6 (int32 x 2)
  - width_m, depth_m, height_m, yaw_rad (float32 x 4)
  - ground_m (float32): exact IGN MDT05/MDT25 elevation pre-baked from .nspack!
  - archetype (uint8): 0=stone_house, 1=terra_house, 2=apartment, 3=church, 4=industrial, 5=barn
  - color_var (uint8): deterministic variation seed
  - roof_r, roof_g, roof_b (uint8 x 3): real-world IGN PNOA orthophoto rooftop RGB sampled at (lat, lon)!
  - canyon_ao (uint8): pre-baked street-canyon ambient occlusion from local 35m building neighborhood density!
  - pad (uint16)

Offline Pre-Baked per Road Vertex (12 bytes):
  - lat_e6, lon_e6 (int32 x 2)
  - ground_m (float32): exact IGN MDT05/MDT25 elevation pre-baked from .nspack!
"""

from __future__ import annotations

import io
import math
import struct
import sys
import time
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / ".tools" / "pydeps"))
sys.path.insert(0, str(ROOT))

import numpy as np  # noqa: E402
import osmium  # noqa: E402
from PIL import Image  # noqa: E402
from protocol.pack_format import LAYER_ELEVATION, LAYER_IMAGERY, PackReader  # noqa: E402

BBOX = (-1.20, 41.55, 0.65, 42.65)  # min_lon, min_lat, max_lon, max_lat
PBF_PATH = ROOT / "builder" / ".cache" / "osm" / "aragon.osm.pbf"
PACK_PATH = ROOT / "packs" / "es_aragon_pyrenees.nspack"
OUT_PATH = ROOT / "packs" / "es_aragon_scenery.nsscn"

ROAD_KINDS = {
    "motorway": 0, "motorway_link": 0, "trunk": 0, "trunk_link": 0,
    "primary": 1, "primary_link": 1, "secondary": 1, "secondary_link": 1,
    "tertiary": 2, "tertiary_link": 2, "residential": 2, "living_street": 2,
    "unclassified": 3, "service": 3,
}


class CachedPackSampler:
    """Fast LRU-cached sampler for offline pre-baking elevation & real PNOA ortho roof colors from .nspack."""

    def __init__(self, reader: PackReader) -> None:
        self.reader = reader
        self.elev_zooms = sorted(reader.manifest["elevation_zooms"], reverse=True)
        self.img_zooms = sorted(reader.manifest["imagery_zooms"], reverse=True)
        self.elev_cache: OrderedDict[tuple[int, int, int], np.ndarray | None] = OrderedDict()
        self.img_cache: OrderedDict[tuple[int, int, int], np.ndarray | None] = OrderedDict()

    def _get_elev_tile(self, z: int, tx: int, ty: int) -> np.ndarray | None:
        key = (z, tx, ty)
        if key in self.elev_cache:
            self.elev_cache.move_to_end(key)
            return self.elev_cache[key]
        arr = self.reader.read_tile(LAYER_ELEVATION, z, tx, ty, verify_crc=False)
        self.elev_cache[key] = arr
        if len(self.elev_cache) > 512:
            self.elev_cache.popitem(last=False)
        return arr

    def _get_img_tile(self, z: int, tx: int, ty: int) -> np.ndarray | None:
        key = (z, tx, ty)
        if key in self.img_cache:
            self.img_cache.move_to_end(key)
            return self.img_cache[key]
        raw = self.reader.read_tile(LAYER_IMAGERY, z, tx, ty, verify_crc=False)
        arr = None
        if raw is not None:
            arr = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"), dtype=np.uint8)
        self.img_cache[key] = arr
        if len(self.img_cache) > 384:
            self.img_cache.popitem(last=False)
        return arr

    def sample_elevation(self, lat: float, lon: float) -> float:
        sin_lat = math.sin(math.radians(max(-85.0, min(85.0, lat))))
        merc_y = 0.5 - math.log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * math.pi)
        merc_x = (lon + 180.0) / 360.0
        for z in self.elev_zooms:
            n = float(1 << z)
            xt = merc_x * n
            yt = merc_y * n
            tx, ty = int(xt), int(yt)
            arr = self._get_elev_tile(z, tx, ty)
            if arr is None:
                continue
            h_res, w_res = arr.shape
            fx = max(0.0, min(1.0, xt - tx)) * (w_res - 1)
            fy = max(0.0, min(1.0, yt - ty)) * (h_res - 1)
            x0, y0 = int(fx), int(fy)
            x1, y1 = min(w_res - 1, x0 + 1), min(h_res - 1, y0 + 1)
            dx, dy = fx - x0, fy - y0
            a = float(arr[y0, x0]) * (1.0 - dx) + float(arr[y0, x1]) * dx
            b = float(arr[y1, x0]) * (1.0 - dx) + float(arr[y1, x1]) * dx
            return a * (1.0 - dy) + b * dy
        return 400.0

    def sample_ortho_rgb(self, lat: float, lon: float) -> tuple[int, int, int]:
        sin_lat = math.sin(math.radians(max(-85.0, min(85.0, lat))))
        merc_y = 0.5 - math.log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * math.pi)
        merc_x = (lon + 180.0) / 360.0
        for z in self.img_zooms:
            n = float(1 << z)
            xt = merc_x * n
            yt = merc_y * n
            tx, ty = int(xt), int(yt)
            arr = self._get_img_tile(z, tx, ty)
            if arr is None:
                continue
            h_res, w_res, _ = arr.shape
            ix = min(w_res - 1, max(0, int((xt - tx) * w_res)))
            iy = min(h_res - 1, max(0, int((yt - ty) * h_res)))
            px = arr[iy, ix]
            return int(px[0]), int(px[1]), int(px[2])
        return 155, 132, 114


def _in_bbox(lon: float, lat: float) -> bool:
    return BBOX[0] <= lon <= BBOX[2] and BBOX[1] <= lat <= BBOX[3]


def _oriented_bbox(pts_m: list[tuple[float, float]]) -> tuple[float, float, float, float, float]:
    """Compute oriented bounding box (cx, cy, width, depth, yaw_rad) aligned to longest polygon edge."""
    n = len(pts_m)
    best_len2 = -1.0
    best_dx, best_dy = 1.0, 0.0
    for i in range(n):
        x0, y0 = pts_m[i]
        x1, y1 = pts_m[(i + 1) % n]
        dx, dy = x1 - x0, y1 - y0
        l2 = dx * dx + dy * dy
        if l2 > best_len2:
            best_len2 = l2
            best_dx, best_dy = dx, dy
    if best_len2 < 1e-4:
        return 0.0, 0.0, 0.0, 0.0, 0.0
    inv_l = 1.0 / math.sqrt(best_len2)
    ux, uy = best_dx * inv_l, best_dy * inv_l
    vx, vy = -uy, ux

    min_u = min_v = 1e9
    max_u = max_v = -1e9
    for x, y in pts_m:
        pu = x * ux + y * uy
        pv = x * vx + y * vy
        if pu < min_u: min_u = pu
        if pu > max_u: max_u = pu
        if pv < min_v: min_v = pv
        if pv > max_v: max_v = pv

    cu = 0.5 * (min_u + max_u)
    cv = 0.5 * (min_v + max_v)
    cx = cu * ux + cv * vx
    cy = cu * uy + cv * vy
    w = max_u - min_u
    d = max_v - min_v
    yaw = math.atan2(uy, ux)
    return cx, cy, w, d, yaw


def _parse_float(val: str | None, default: float) -> float:
    if not val:
        return default
    s = val.strip().lower().replace("m", "").replace(",", ".").split()[0]
    try:
        return float(s)
    except ValueError:
        return default


def _simplify_polyline(pts: list[tuple[float, float]], eps_deg: float) -> list[tuple[float, float]]:
    if len(pts) <= 2:
        return pts
    out = [pts[0]]
    last = pts[0]
    eps2 = eps_deg * eps_deg
    for p in pts[1:-1]:
        dx = p[0] - last[0]
        dy = p[1] - last[1]
        if dx * dx + dy * dy >= eps2:
            out.append(p)
            last = p
    out.append(pts[-1])
    return out


class SceneryExtractor(osmium.SimpleHandler):
    def __init__(self) -> None:
        super().__init__()
        self.buildings: list[tuple[int, int, float, float, float, float, int, int]] = []
        self.roads: list[tuple[int, list[tuple[int, int]]]] = []
        self.counts = [0] * 6

    def _emit_building(
        self,
        lon0: float, lat0: float,
        pts_m: list[tuple[float, float]],
        tags: osmium.osm.TagList,
        obj_id: int,
        has_courtyard: bool = False,
    ) -> None:
        if len(pts_m) < 3:
            return
        cx, cy, width, depth, yaw = _oriented_bbox(pts_m)
        area = width * depth
        if area < 20.0 or width < 3.2 or depth < 3.2 or width > 220.0 or depth > 220.0:
            return
        m_lat = 111132.0
        m_lon = 111320.0 * math.cos(math.radians(lat0))
        clat = lat0 + cy / m_lat
        clon = lon0 + cx / m_lon

        btag = tags.get("building") or tags.get("building:part") or "yes"
        levels = _parse_float(tags.get("building:levels"), 0.0)
        height = _parse_float(tags.get("height"), 0.0)
        bt = btag.lower()
        roof_mat = (tags.get("roof:material") or "").lower()

        if bt in ("church", "chapel", "cathedral", "bell_tower") or tags.get("amenity") == "place_of_worship":
            arch = 3
            if height <= 0.0:
                height = 18.0
        elif bt in ("industrial", "warehouse", "hangar", "commercial", "retail", "supermarket", "manufacture", "depot", "sports_hall") or (max(width, depth) > 32.0 and not has_courtyard and levels <= 2.0 and bt not in ("apartments", "residential")):
            arch = 4
            if height <= 0.0:
                height = max(6.5, min(13.0, 4.5 + min(width, depth) * 0.14))
        elif bt in ("barn", "farm_auxiliary", "shed", "cowshed", "stable", "farm", "greenhouse"):
            arch = 5
            if height <= 0.0:
                height = 5.4
        elif has_courtyard or bt in ("apartments", "hotel", "office", "university", "hospital", "public", "civic", "school") or levels >= 3.0 or height >= 10.5:
            arch = 2
            if height <= 0.0:
                lv = levels if levels > 0.0 else (5.5 if has_courtyard else 4.5)
                height = max(11.0, min(34.0, lv * 3.15))
        else:
            is_pyrenees = (clat > 42.30) or ("slate" in roof_mat)
            arch = 0 if is_pyrenees else 1
            if height <= 0.0:
                lv = levels if levels > 0.0 else (2.4 if area > 85.0 else 1.8)
                height = max(5.2, min(12.0, lv * 3.1 + 1.4))

        # Decompose large Spanish Cadastre multipolygon courtyard blocks (manzanas) into 4 real perimeter wings!
        if has_courtyard and width > 24.0 and depth > 24.0:
            wt = max(9.0, min(16.0, min(width, depth) * 0.25))
            ux, uy = math.cos(yaw), math.sin(yaw)
            vx, vy = -uy, ux
            inner_d = max(4.0, depth - 2.0 * wt)
            wings = [
                (0.0, +(depth * 0.5 - wt * 0.5), width, wt, yaw),
                (0.0, -(depth * 0.5 - wt * 0.5), width, wt, yaw),
                (-(width * 0.5 - wt * 0.5), 0.0, inner_d, wt, yaw + math.pi * 0.5),
                (+(width * 0.5 - wt * 0.5), 0.0, inner_d, wt, yaw + math.pi * 0.5),
            ]
            for wi, (lu, lv_off, ww, wd, wyaw) in enumerate(wings):
                wcx = cx + lu * ux + lv_off * vx
                wcy = cy + lu * uy + lv_off * vy
                wlat = lat0 + wcy / m_lat
                wlon = lon0 + wcx / m_lon
                cvar = ((int(obj_id) * 2654435761) ^ (wi * 374761393)) & 0xFF
                self.buildings.append((
                    int(round(wlat * 1e6)), int(round(wlon * 1e6)),
                    float(ww), float(wd), float(height), float(wyaw), arch, cvar,
                ))
                self.counts[arch] += 1
            return

        cvar = (int(obj_id) * 2654435761) & 0xFF
        self.buildings.append((
            int(round(clat * 1e6)),
            int(round(clon * 1e6)),
            float(width),
            float(depth),
            float(height),
            float(yaw),
            arch,
            cvar,
        ))
        self.counts[arch] += 1

    def area(self, a: osmium.osm.Area) -> None:
        tags = a.tags
        btag = tags.get("building")
        if btag is None or btag == "no":
            return
        for outer in a.outer_rings():
            nodes = list(outer)
            if len(nodes) < 4:
                continue
            lon0, lat0 = nodes[0].lon, nodes[0].lat
            if not _in_bbox(lon0, lat0):
                continue
            m_lat = 111132.0
            m_lon = 111320.0 * math.cos(math.radians(lat0))
            pts_m = [((nd.lon - lon0) * m_lon, (nd.lat - lat0) * m_lat) for nd in nodes[:-1]]
            has_inner = any(True for _ in a.inner_rings(outer))
            self._emit_building(lon0, lat0, pts_m, tags, a.id, has_courtyard=has_inner)

    def way(self, w: osmium.osm.Way) -> None:
        tags = w.tags
        bpart = tags.get("building:part")
        htag = tags.get("highway")
        if bpart is None and htag is None:
            return
        nodes = w.nodes
        if len(nodes) < 2:
            return

        # Also extract multi-height Spanish Cadastre building:part wings!
        if bpart is not None and bpart != "no" and tags.get("building") is None and len(nodes) >= 4:
            n0 = nodes[0]
            if not n0.location.valid():
                return
            lon0, lat0 = n0.location.lon, n0.location.lat
            if not _in_bbox(lon0, lat0):
                return
            m_lat = 111132.0
            m_lon = 111320.0 * math.cos(math.radians(lat0))
            pts_m: list[tuple[float, float]] = []
            for nd in nodes:
                if not nd.location.valid():
                    return
                pts_m.append(((nd.location.lon - lon0) * m_lon, (nd.location.lat - lat0) * m_lat))
            if len(pts_m) > 3 and abs(pts_m[-1][0] - pts_m[0][0]) < 1e-4 and abs(pts_m[-1][1] - pts_m[0][1]) < 1e-4:
                pts_m.pop()
            self._emit_building(lon0, lat0, pts_m, tags, w.id, has_courtyard=False)
            return

        if htag is not None:
            rk = ROAD_KINDS.get(htag)
            if rk is None:
                return
            if rk == 3 and (tags.get("service") in ("parking_aisle", "driveway", "alley")):
                return
            pts: list[tuple[float, float]] = []
            any_in = False
            for nd in nodes:
                if nd.location.valid():
                    lo, la = nd.location.lon, nd.location.lat
                    if _in_bbox(lo, la):
                        any_in = True
                    pts.append((lo, la))
            if not any_in or len(pts) < 2:
                return
            eps = 0.00007 if rk <= 1 else 0.00005
            spts = _simplify_polyline(pts, eps)
            if len(spts) >= 2:
                self.roads.append((rk, [(int(round(la * 1e6)), int(round(lo * 1e6))) for (lo, la) in spts]))


def _prebake_street_canyon_ao(buildings: list[tuple[int, int, float, float, float, float, int, int]]) -> list[int]:
    """Pre-bake street-canyon ambient occlusion (0..255) from local ~35m building neighborhood density."""
    # Bucket into ~35m cells (350 microdegrees)
    grid: dict[tuple[int, int], int] = {}
    for b in buildings:
        gx, gy = b[1] // 350, b[0] // 350
        grid[(gx, gy)] = grid.get((gx, gy), 0) + 1

    ao_bytes: list[int] = []
    for b in buildings:
        gx, gy = b[1] // 350, b[0] // 350
        neighbors = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                neighbors += grid.get((gx + dx, gy + dy), 0)
        # Isolated farmhouse (neighbors=1) -> AO=245 (0.96); dense Huesca/Zaragoza block (neighbors>=14) -> AO=115 (0.45)
        density = min(1.0, max(0.0, (neighbors - 1) / 14.0))
        ao_val = int(round((0.96 - 0.50 * density) * 255.0))
        ao_bytes.append(max(96, min(255, ao_val)))
    return ao_bytes


def main(argv: list[str] | None = None) -> None:
    import argparse
    global BBOX
    ap = argparse.ArgumentParser(description="Extract & bake OSM buildings/roads .nsscn for any .nspack.")
    ap.add_argument("--pack", type=Path, default=PACK_PATH)
    ap.add_argument("--pbf", type=Path, nargs="+", default=[PBF_PATH])
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)

    pack_path: Path = args.pack
    pbf_paths: list[Path] = [Path(x) for x in (args.pbf if isinstance(args.pbf, list) else [args.pbf])]
    out_path: Path = args.out or (pack_path.parent / (pack_path.stem.replace("_pyrenees", "") + "_scenery.nsscn"))

    missing_pbf = [str(p) for p in pbf_paths if not p.exists()]
    if missing_pbf:
        raise SystemExit(f"Missing OSM PBF: {missing_pbf}")
    if not pack_path.exists():
        raise SystemExit(f"Missing terrain pack: {pack_path}")

    with PackReader(pack_path) as r0:
        BBOX = tuple(r0.manifest["bbox"])

    t0 = time.time()
    ext = SceneryExtractor()
    for pbf_path in pbf_paths:
        ext.apply_file(str(pbf_path), locations=True)
    dt = time.time() - t0
    print(f"Parsed {len(pbf_paths)} PBF(s) in {dt:.1f}s -> buildings={len(ext.buildings)}, roads={len(ext.roads)}")
    names = ["stone_house", "terra_house", "apartment", "church", "industrial", "barn"]
    print("  Archetypes:", ", ".join(f"{n}={c}" for n, c in zip(names, ext.counts)))

    # Sort buildings by spatial grid key (0.025 deg cells) for high tile-cache hit rate & fast C++ streaming!
    ext.buildings.sort(key=lambda b: ((b[0] // 25000), (b[1] // 25000)))
    ext.roads.sort(key=lambda r: ((r[1][0][0] // 25000), (r[1][0][1] // 25000)))

    t_bake = time.time()
    canyon_ao_list = _prebake_street_canyon_ao(ext.buildings)

    baked_buildings: list[bytes] = []
    baked_road_pts: list[bytes] = []

    with PackReader(pack_path) as reader:
        sampler = CachedPackSampler(reader)
        for idx, b in enumerate(ext.buildings):
            lat = b[0] * 1e-6
            lon = b[1] * 1e-6
            g_m = sampler.sample_elevation(lat, lon)
            rr, rg, rb = sampler.sample_ortho_rgb(lat, lon)
            # Clamp extreme dark shadow pixels or blown white pixels from ortho into natural Spanish roof albedo range
            luma = 0.2126 * rr + 0.7152 * rg + 0.0722 * rb
            if luma < 52.0:
                scale = 52.0 / max(1.0, luma)
                rr, rg, rb = min(255, int(rr * scale)), min(255, int(rg * scale)), min(255, int(rb * scale))
            elif luma > 205.0:
                scale = 205.0 / luma
                rr, rg, rb = int(rr * scale), int(rg * scale), int(rb * scale)
            ao_b = canyon_ao_list[idx]
            # 36 bytes: <iifffffBBBBBBH
            baked_buildings.append(struct.pack(
                "<iifffffBBBBBBH",
                b[0], b[1], b[2], b[3], b[4], b[5], float(g_m),
                b[6], b[7], rr, rg, rb, ao_b, 0
            ))

        for _rk, pts in ext.roads:
            for la_e6, lo_e6 in pts[:65535]:
                g_m = sampler.sample_elevation(la_e6 * 1e-6, lo_e6 * 1e-6)
                # 12 bytes: <iif
                baked_road_pts.append(struct.pack("<iif", la_e6, lo_e6, float(g_m)))

    print(f"Pre-baked building elevations, PNOA roof colors, street-canyon AO & road elevations in {time.time() - t_bake:.1f}s!")

    total_road_pts = len(baked_road_pts)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        # Header (version = 2): 4s I I I I 4f = 36 bytes
        f.write(struct.pack("<4sIIII4f", b"NSCN", 2, len(baked_buildings), len(ext.roads), total_road_pts, *BBOX))
        for bb in baked_buildings:
            f.write(bb)
        off = 0
        for rk, pts in ext.roads:
            cnt = min(len(pts), 65535)
            f.write(struct.pack("<IHBB", off, cnt, rk, 0))
            off += cnt
        for rp in baked_road_pts:
            f.write(rp)

    sz_mb = out_path.stat().st_size / (1024 * 1024)
    print(f"Wrote {out_path} (v2 pre-baked, {sz_mb:.2f} MB, {len(baked_buildings)} buildings, {len(ext.roads)} roads, {total_road_pts} road vertices)")


if __name__ == "__main__":
    main()
