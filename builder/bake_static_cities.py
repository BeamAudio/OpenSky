#!/usr/bin/env python3
"""bake_static_cities.py (v3) -- Map-Wide Offline Pre-Baked Exact-Polygon Static Settlements &
Fixed-Sun 10-Time-of-Day Ray-Traced Shadow + Hemisphere Sky-AO Radiosity Baker.

Covers 100% of ALL populated zones across Northern Spain (Zaragoza, Huesca, Jaca, Barbastro,
Monzon, Binefar, Sabinanigo, Castejon de Sos, Ainsa, Graus, Benasque, Santa Cilia, ALL towns
& villages in aragon.osm.pbf!) so ZERO overlapping OBB boxes remain anywhere on the map:
  1. packs/es_aragon_cities.nscity (v3)
     - Exact OSM outer & inner-courtyard polygons (Cadastre building:part deduplicated!)
     - 3D raised rooftop parapets, apartment castilletes (stairwell penthouses) & hipped roofs
     - Wall-aligned local roof UVs + clean interior median IGN PNOA roof RGB (zero camo patches!)
     - Per-vertex ray-traced 10-time-of-day solar visibility (20 bits: 10 x 2-bit) + 8-bit Sky AO
       packed into ONE uint32 (exact same 32-byte CityVert footprint!)
     - 500m x 500m static spatial chunks for <0.02ms CPU frustum culling & Early-Z front-to-back draw!
  2. packs/es_aragon_city_sh.nsshd (v2)
     - Sparse zlib-compressed 128x128 uint32 (3.9 m/px) 10-time-of-day ground shadow & Sky-AO tiles
       for EVERY populated 500m cell across the entire 163km x 122km corridor!
     - Loaded on-demand by PackReader background workers into CDLOD terrain, roads & trees!
"""

from __future__ import annotations

import argparse
import io
import math
import multiprocessing as mp
import os
import struct
import sys
import time
import zlib
from collections import OrderedDict, defaultdict
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / ".tools" / "pydeps"))
sys.path.insert(0, str(ROOT))

import numpy as np  # noqa: E402
import osmium  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
from scipy.ndimage import binary_dilation, gaussian_filter  # noqa: E402
from protocol.pack_format import LAYER_ELEVATION, LAYER_IMAGERY, PackReader  # noqa: E402

BBOX = (-1.20, 41.55, 0.65, 42.65)  # Full Northern Spain pack bbox
PBF_PATH = ROOT / "builder" / ".cache" / "osm" / "aragon.osm.pbf"
PACK_PATH = ROOT / "packs" / "es_aragon_pyrenees.nspack"
OUT_CITY_PATH = ROOT / "packs" / "es_aragon_cities.nscity"
OUT_SH_PATH = ROOT / "packs" / "es_aragon_city_sh.nsshd"

# Hero landmark exclusions (excluded from extruded mesh, KEPT in 2.5D DSM so they cast crisp shadows!)
HERO_EXCL = [
    (41.6569, -0.8787,  68.0),  # Basilica del Pilar
    (41.6693, -0.9073,  36.0),  # Torre del Agua
    (42.1404, -0.4086,  44.0),  # Catedral de Huesca
    (42.3257, -0.6119,  55.0),  # Castillo de Loarre
    (42.3175,  0.2037, 130.0),  # Torre Sumergida de Mediano
]

CHUNK_M = 500.0
SH_RES = 128            # 128x128 per 500m populated cell => 3.9 m/px ground shadows EVERYWHERE!
PAD_M = 110.0           # +110m neighbor padding on every DSM => ZERO shadow seams across 500m boundaries!
DSM_RES = 184           # (500 + 2*110) / 184 = 3.91 m/px matching SH_RES cleanly!
SH_PAD_PX = int(round(PAD_M / (CHUNK_M / SH_RES)))  # 28 px border each side -> 128 + 56 = 184!

# Fixed Iberian 10-Time-of-Day solar trajectory anchor vectors: (az_deg_from_north_cw, elev_deg)
# Packed as 10 x 2-bit soft penumbra visibility (bits 0..19) + 8-bit Hemisphere Sky AO (bits 20..27) in ONE uint32!
SUN_PHASES_10 = [
    ( 78.0,  7.0),  # 0: Dawn
    ( 96.0, 13.0),  # 1: Early Morning
    (114.0, 19.0),  # 2: Morning Fair VFR (EXACT match to fair_vfr!)
    (134.0, 30.0),  # 3: Late Morning
    (156.0, 42.0),  # 4: Pre-Noon
    (180.0, 48.0),  # 5: Solar Noon
    (204.0, 42.0),  # 6: Early Afternoon
    (226.0, 30.0),  # 7: Mid Afternoon
    (248.0, 20.0),  # 8: Golden Afternoon
    (274.0,  8.0),  # 9: Sunset
]


def pack_tod10_u32(vis_list: list[np.ndarray], sky_ao: np.ndarray, night_light: np.ndarray | None = None) -> np.ndarray:
    """Pack 10 solar phases (2 bits each -> bits 0..19) + 8-bit Sky AO (20..27) + 4-bit RT night light (28..31)."""
    out = np.clip(np.rint(sky_ao * 255.0), 0.0, 255.0).astype(np.uint32) << np.uint32(20)
    for k, v in enumerate(vis_list):
        q = np.clip(np.rint(v * 3.0), 0.0, 3.0).astype(np.uint32)
        out |= q << np.uint32(k * 2)
    if night_light is not None:
        nl = np.clip(np.rint(night_light * 15.0), 0.0, 15.0).astype(np.uint32)
        out |= nl << np.uint32(28)
    return out


def meters_per_degree(lat_deg: float) -> tuple[float, float]:
    p = math.radians(lat_deg)
    m_lat = 111132.92 - 559.82 * math.cos(2.0 * p) + 1.175 * math.cos(4.0 * p)
    m_lon = 111412.84 * math.cos(p) - 93.5 * math.cos(3.0 * p)
    return m_lat, m_lon


def sun_dir_enu(az_deg: float, el_deg: float) -> tuple[float, float, float]:
    az, el = math.radians(az_deg), math.radians(el_deg)
    ce, se = math.cos(el), math.sin(el)
    return math.sin(az) * ce, se, math.cos(az) * ce


class PackSampler:
    def __init__(self, reader: PackReader) -> None:
        self.reader = reader
        self.elev_zooms = sorted(reader.manifest["elevation_zooms"], reverse=True)
        self.img_zooms = sorted(reader.manifest["imagery_zooms"], reverse=True)
        lon_min, lat_min, lon_max, lat_max = reader.manifest["bbox"]
        self.anchor_lat = 0.5 * (lat_min + lat_max)
        self.anchor_lon = 0.5 * (lon_min + lon_max)
        self.m_lat, self.m_lon = meters_per_degree(self.anchor_lat)
        self.elev_cache: OrderedDict[tuple[int, int, int], np.ndarray | None] = OrderedDict()
        self.img_cache: OrderedDict[tuple[int, int, int], np.ndarray | None] = OrderedDict()

    def ll_to_en(self, lat: float, lon: float) -> tuple[float, float]:
        return (lon - self.anchor_lon) * self.m_lon, (lat - self.anchor_lat) * self.m_lat

    def _get_elev(self, z: int, tx: int, ty: int) -> np.ndarray | None:
        k = (z, tx, ty)
        if k in self.elev_cache:
            self.elev_cache.move_to_end(k)
            return self.elev_cache[k]
        arr = self.reader.read_tile(LAYER_ELEVATION, z, tx, ty, verify_crc=False)
        self.elev_cache[k] = arr
        if len(self.elev_cache) > 640:
            self.elev_cache.popitem(last=False)
        return arr

    def _get_img(self, z: int, tx: int, ty: int) -> np.ndarray | None:
        k = (z, tx, ty)
        if k in self.img_cache:
            self.img_cache.move_to_end(k)
            return self.img_cache[k]
        raw = self.reader.read_tile(LAYER_IMAGERY, z, tx, ty, verify_crc=False)
        arr = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"), dtype=np.uint8) if raw is not None else None
        self.img_cache[k] = arr
        if len(self.img_cache) > 512:
            self.img_cache.popitem(last=False)
        return arr

    def sample_elev_ll(self, lat: float, lon: float) -> float:
        sin_lat = math.sin(math.radians(max(-85.0, min(85.0, lat))))
        my = 0.5 - math.log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * math.pi)
        mx = (lon + 180.0) / 360.0
        for z in self.elev_zooms:
            n = float(1 << z)
            xt, yt = mx * n, my * n
            tx, ty = int(xt), int(yt)
            arr = self._get_elev(z, tx, ty)
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

    def sample_ortho_rgb_en(self, e: float, n_m: float) -> tuple[int, int, int]:
        lat = self.anchor_lat + n_m / self.m_lat
        lon = self.anchor_lon + e / self.m_lon
        sin_lat = math.sin(math.radians(max(-85.0, min(85.0, lat))))
        my = 0.5 - math.log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * math.pi)
        mx = (lon + 180.0) / 360.0
        for z in self.img_zooms:
            n = float(1 << z)
            xt, yt = mx * n, my * n
            tx, ty = int(xt), int(yt)
            arr = self._get_img(z, tx, ty)
            if arr is None:
                continue
            h_res, w_res, _ = arr.shape
            ix = min(w_res - 1, max(0, int((xt - tx) * w_res)))
            iy = min(h_res - 1, max(0, int((yt - ty) * h_res)))
            px = arr[iy, ix]
            return int(px[0]), int(px[1]), int(px[2])
        return 158, 120, 96

    def bake_patch_dem(self, min_e: float, max_e: float, min_n: float, max_n: float, res: int) -> np.ndarray:
        """Fast 25x25 DEM sample bilinearly upsampled to (res, res) float32 (row 0 = min_n)."""
        cn = 25
        coarse = np.empty((cn, cn), dtype=np.float32)
        es = np.linspace(min_e, max_e, cn)
        ns = np.linspace(min_n, max_n, cn)
        for r in range(cn):
            lat = self.anchor_lat + float(ns[r]) / self.m_lat
            for c in range(cn):
                lon = self.anchor_lon + float(es[c]) / self.m_lon
                coarse[r, c] = self.sample_elev_ll(lat, lon)
        return np.asarray(Image.fromarray(coarse, mode="F").resize((res, res), Image.BILINEAR), dtype=np.float32)


# ---------------- Polygon Geometry & Ear-Clipping with Holes ----------------

def signed_area(ring: list[tuple[float, float]]) -> float:
    a = 0.0
    n = len(ring)
    for i in range(n):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % n]
        a += x0 * y1 - x1 * y0
    return 0.5 * a


def clean_ring(pts: list[tuple[float, float]], min_edge: float = 0.45, col_eps: float = 0.10) -> list[tuple[float, float]]:
    if len(pts) < 3:
        return []
    out: list[tuple[float, float]] = []
    min_e2 = min_edge * min_edge
    for p in pts:
        if not out or (p[0] - out[-1][0]) ** 2 + (p[1] - out[-1][1]) ** 2 >= min_e2:
            out.append(p)
    while len(out) > 2 and (out[-1][0] - out[0][0]) ** 2 + (out[-1][1] - out[0][1]) ** 2 < min_e2:
        out.pop()
    if len(out) < 3:
        return []
    res: list[tuple[float, float]] = []
    n = len(out)
    for i in range(n):
        a, b, c = out[(i - 1) % n], out[i], out[(i + 1) % n]
        abx, aby = b[0] - a[0], b[1] - a[1]
        bcx, bcy = c[0] - b[0], c[1] - b[1]
        cross = abs(abx * bcy - aby * bcx)
        ac_len = math.hypot(c[0] - a[0], c[1] - a[1])
        if ac_len > 1e-4 and (cross / ac_len) < col_eps and (abx * bcx + aby * bcy) > 0.0:
            continue
        res.append(b)
    return res if len(res) >= 3 else out


def point_in_tri(px: float, py: float, ax: float, ay: float, bx: float, by: float, cx: float, cy: float) -> bool:
    c1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
    c2 = (cx - bx) * (py - by) - (cy - by) * (px - bx)
    c3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx)
    return c1 >= -1e-7 and c2 >= -1e-7 and c3 >= -1e-7


def point_in_ring(px: float, py: float, ring: list[tuple[float, float]]) -> bool:
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12) + xi):
            inside = not inside
        j = i
    return inside


def bridge_holes(outer: list[tuple[float, float]], holes: list[list[tuple[float, float]]]) -> list[tuple[float, float]]:
    poly = list(outer)
    for hole in sorted(holes, key=lambda h: max(p[0] for p in h), reverse=True):
        if len(hole) < 3:
            continue
        m_idx = max(range(len(hole)), key=lambda i: hole[i][0])
        mx, my = hole[m_idx]
        best_x, best_edge_i = 1e18, -1
        n = len(poly)
        for i in range(n):
            ax, ay = poly[i]
            bx, by = poly[(i + 1) % n]
            if (ay <= my <= by) or (by <= my <= ay):
                if abs(by - ay) < 1e-9:
                    continue
                ix = ax + ((my - ay) / (by - ay)) * (bx - ax)
                if ix >= mx - 1e-6 and ix < best_x:
                    best_x = ix
                    best_edge_i = i
        if best_edge_i < 0:
            p_idx = min(range(n), key=lambda i: (poly[i][0] - mx) ** 2 + (poly[i][1] - my) ** 2)
        else:
            a_i, b_i = best_edge_i, (best_edge_i + 1) % n
            p_idx = a_i if poly[a_i][0] >= poly[b_i][0] else b_i
            px, py = poly[p_idx]
            ix = best_x
            best_ang = 1e18
            for k in range(n):
                if k == p_idx:
                    continue
                kx, ky = poly[k]
                if kx <= mx:
                    continue
                area_mip = (ix - mx) * (py - my)
                if abs(area_mip) < 1e-9:
                    continue
                in_t = point_in_tri(kx, ky, mx, my, ix, my, px, py) if area_mip > 0 else point_in_tri(kx, ky, mx, my, px, py, ix, my)
                if in_t:
                    ang = abs(math.atan2(ky - my, kx - mx))
                    if ang < best_ang:
                        best_ang = ang
                        p_idx = k
        hole_rot = hole[m_idx:] + hole[: m_idx + 1]
        poly = poly[: p_idx + 1] + hole_rot + [poly[p_idx]] + poly[p_idx + 1 :]
    return poly


def triangulate_polygon(outer: list[tuple[float, float]], inners: list[list[tuple[float, float]]]) -> tuple[list[tuple[float, float]], list[tuple[int, int, int]]]:
    poly = bridge_holes(outer, inners) if inners else list(outer)
    n = len(poly)
    if n < 3:
        return poly, []
    idx = list(range(n))
    tris: list[tuple[int, int, int]] = []
    guard = n * n + 16
    i = 0
    while len(idx) > 3 and guard > 0:
        guard -= 1
        m = len(idx)
        i %= m
        ia, ib, ic = idx[(i - 1) % m], idx[i], idx[(i + 1) % m]
        ax, ay = poly[ia]
        bx, by = poly[ib]
        cx, cy = poly[ic]
        if (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) > 1e-7:
            is_ear = True
            for j in range(m):
                ik = idx[j]
                if ik == ia or ik == ib or ik == ic:
                    continue
                kx, ky = poly[ik]
                if (kx == ax and ky == ay) or (kx == bx and ky == by) or (kx == cx and ky == cy):
                    continue
                if point_in_tri(kx, ky, ax, ay, bx, by, cx, cy):
                    is_ear = False
                    break
            if is_ear:
                tris.append((ia, ib, ic))
                idx.pop(i)
                continue
        i += 1
    if len(idx) == 3:
        ia, ib, ic = idx
        ax, ay = poly[ia]
        bx, by = poly[ib]
        cx, cy = poly[ic]
        if (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) > 1e-7:
            tris.append((ia, ib, ic))
    return poly, tris


def longest_edge_axis(ring: list[tuple[float, float]]) -> tuple[float, float]:
    best_l2 = -1.0
    ux, uy = 1.0, 0.0
    n = len(ring)
    for i in range(n):
        dx = ring[(i + 1) % n][0] - ring[i][0]
        dy = ring[(i + 1) % n][1] - ring[i][1]
        l2 = dx * dx + dy * dy
        if l2 > best_l2:
            best_l2 = l2
            inv = 1.0 / math.sqrt(max(1e-6, l2))
            ux, uy = dx * inv, dy * inv
    return ux, uy


# ---------------- Map-Wide OSM Extraction & Cadastre Deduplication ----------------

@dataclass
class PolyBldg:
    obj_id: int
    outer: list[tuple[float, float]]
    inners: list[list[tuple[float, float]]]
    height_m: float
    arch: int
    style: int
    bhash: int
    area_m2: float
    bbox: tuple[float, float, float, float]
    clat: float
    is_part: bool
    exclude_mesh: bool
    roof_rgb: tuple[int, int, int] = (158, 118, 92)
    roof_kind: int = 0


def _parse_float(val: str | None, default: float) -> float:
    if not val:
        return default
    s = val.strip().lower().replace("m", "").replace(",", ".").split()[0]
    try:
        return float(s)
    except ValueError:
        return default


def _classify_bldg(tags: osmium.osm.TagList, area_m2: float, has_courtyard: bool, lat: float, obj_id: int) -> tuple[float, int, int, int]:
    btag = (tags.get("building") or tags.get("building:part") or "yes").lower()
    levels = _parse_float(tags.get("building:levels"), 0.0)
    height = _parse_float(tags.get("height"), 0.0)
    roof_mat = (tags.get("roof:material") or "").lower()

    if btag in ("church", "chapel", "cathedral", "bell_tower") or tags.get("amenity") == "place_of_worship":
        arch = 3
        if height <= 0.0:
            height = max(14.0, levels * 3.8 if levels > 0 else 17.5)
    elif btag in ("industrial", "warehouse", "hangar", "commercial", "retail", "supermarket", "manufacture", "depot", "sports_hall") or (
        area_m2 > 750.0 and not has_courtyard and levels <= 2.0 and btag not in ("apartments", "residential")
    ):
        arch = 4
        if height <= 0.0:
            height = max(6.5, min(13.0, levels * 3.6 if levels > 0 else 8.2))
    elif btag in ("barn", "farm_auxiliary", "shed", "cowshed", "stable", "farm", "greenhouse", "garage", "garages", "roof"):
        arch = 5
        if height <= 0.0:
            height = max(3.6, levels * 3.0 if levels > 0 else 4.6)
    elif has_courtyard or btag in ("apartments", "hotel", "office", "university", "hospital", "public", "civic", "school") or levels >= 3.0 or height >= 10.0:
        arch = 2
        if height <= 0.0:
            lv = levels if levels > 0.0 else (5.2 if has_courtyard else (4.0 if area_m2 > 180.0 else 3.0))
            height = max(9.5, min(42.0, lv * 3.15))
    else:
        arch = 0 if (lat > 42.32 or "slate" in roof_mat) else 1
        if height <= 0.0:
            lv = levels if levels > 0.0 else (3.0 if area_m2 > 180.0 else 2.1)
            height = max(4.6, min(15.0, lv * 3.1 + 0.8))

    height = max(3.8, min(58.0, height))
    h_u32 = ((int(obj_id) * 2654435761) ^ 0x9E3779B9) & 0xFFFFFFFF
    bhash = (h_u32 >> 16) & 0xFF
    hf = bhash / 255.0

    if arch == 4:
        style = 7
    elif arch in (0, 3, 5):
        style = 3 if (lat > 42.30 or hf < 0.48) else (5 if hf < 0.80 else 1)
    elif arch == 2:
        if hf < 0.30: style = 0
        elif hf < 0.54: style = 2
        elif hf < 0.72: style = 4
        elif hf < 0.86: style = 6
        else: style = 1
    else:
        if hf < 0.28: style = 1
        elif hf < 0.52: style = 5
        elif hf < 0.72: style = 4
        elif hf < 0.88: style = 0
        else: style = 3
    return height, arch, style, bhash


class AllAragonPolyExtractor(osmium.SimpleHandler):
    def __init__(self, sampler: PackSampler) -> None:
        super().__init__()
        self.sampler = sampler
        self.areas: list[PolyBldg] = []
        self.parts: list[PolyBldg] = []
        self.hero_en = [(*sampler.ll_to_en(la, lo), rad) for (la, lo, rad) in HERO_EXCL]

    def _in_bbox(self, lat: float, lon: float) -> bool:
        return BBOX[0] <= lon <= BBOX[2] and BBOX[1] <= lat <= BBOX[3]

    def _is_hero(self, ce: float, cn: float) -> bool:
        for he, hn, hr in self.hero_en:
            if (ce - he) ** 2 + (cn - hn) ** 2 < hr * hr:
                return True
        return False

    def _make_poly(
        self, obj_id: int,
        outer_ll: list[tuple[float, float]],
        inners_ll: list[list[tuple[float, float]]],
        tags: osmium.osm.TagList,
        is_part: bool,
    ) -> PolyBldg | None:
        outer = clean_ring([self.sampler.ll_to_en(la, lo) for (la, lo) in outer_ll])
        if len(outer) < 3:
            return None
        sa = signed_area(outer)
        if abs(sa) < 18.0:
            return None
        if sa < 0.0:
            outer.reverse()
            sa = -sa
        inners: list[list[tuple[float, float]]] = []
        inner_area = 0.0
        for ir_ll in inners_ll:
            ir = clean_ring([self.sampler.ll_to_en(la, lo) for (la, lo) in ir_ll])
            if len(ir) < 3:
                continue
            isa = signed_area(ir)
            if abs(isa) < 12.0:
                continue
            if isa > 0.0:
                ir.reverse()
                isa = -isa
            inner_area += abs(isa)
            inners.append(ir)
        net_area = max(12.0, sa - inner_area)
        es = [p[0] for p in outer]
        ns = [p[1] for p in outer]
        bbox = (min(es), min(ns), max(es), max(ns))
        if (bbox[2] - bbox[0]) > 260.0 or (bbox[3] - bbox[1]) > 260.0:
            return None
        ce = 0.5 * (bbox[0] + bbox[2])
        cn = 0.5 * (bbox[1] + bbox[3])
        clat = self.sampler.anchor_lat + cn / self.sampler.m_lat
        height, arch, style, bhash = _classify_bldg(tags, net_area, len(inners) > 0, clat, obj_id)
        return PolyBldg(
            obj_id=obj_id, outer=outer, inners=inners,
            height_m=height, arch=arch, style=style, bhash=bhash,
            area_m2=net_area, bbox=bbox, clat=clat, is_part=is_part,
            exclude_mesh=self._is_hero(ce, cn),
        )

    def area(self, a: osmium.osm.Area) -> None:
        bt = a.tags.get("building")
        if bt is None or bt == "no":
            return
        for outer in a.outer_rings():
            nds = list(outer)
            if len(nds) < 4 or not self._in_bbox(nds[0].lat, nds[0].lon):
                continue
            outer_ll = [(nd.lat, nd.lon) for nd in nds[:-1]]
            inners_ll = [[(nd.lat, nd.lon) for nd in list(ir)[:-1]] for ir in a.inner_rings(outer) if len(list(ir)) >= 4]
            pb = self._make_poly(a.id, outer_ll, inners_ll, a.tags, is_part=False)
            if pb is not None:
                self.areas.append(pb)

    def way(self, w: osmium.osm.Way) -> None:
        bp = w.tags.get("building:part")
        if bp is None or bp == "no" or w.tags.get("building") is not None:
            return
        if _parse_float(w.tags.get("building:levels"), 1.0) <= 0.0:
            return
        if _parse_float(w.tags.get("min_height"), 0.0) > 1.0:
            return
        nds = w.nodes
        if len(nds) < 4 or not nds[0].location.valid():
            return
        if not self._in_bbox(nds[0].location.lat, nds[0].location.lon):
            return
        outer_ll: list[tuple[float, float]] = []
        for nd in nds:
            if not nd.location.valid():
                return
            outer_ll.append((nd.location.lat, nd.location.lon))
        if len(outer_ll) > 3 and abs(outer_ll[-1][0] - outer_ll[0][0]) < 1e-7 and abs(outer_ll[-1][1] - outer_ll[0][1]) < 1e-7:
            outer_ll.pop()
        pb = self._make_poly(w.id, outer_ll, [], w.tags, is_part=True)
        if pb is not None:
            self.parts.append(pb)


def deduplicate_cadastre(areas: list[PolyBldg], parts: list[PolyBldg]) -> list[PolyBldg]:
    grid: dict[tuple[int, int], list[int]] = defaultdict(list)
    for idx, p in enumerate(parts):
        cx = int(math.floor(0.5 * (p.bbox[0] + p.bbox[2]) / 40.0))
        cy = int(math.floor(0.5 * (p.bbox[1] + p.bbox[3]) / 40.0))
        grid[(cx, cy)].append(idx)

    part_used = [False] * len(parts)
    result: list[PolyBldg] = []
    for a in areas:
        ax0, ay0, ax1, ay1 = a.bbox
        gx0, gx1 = int(math.floor(ax0 / 40.0)) - 1, int(math.floor(ax1 / 40.0)) + 1
        gy0, gy1 = int(math.floor(ay0 / 40.0)) - 1, int(math.floor(ay1 / 40.0)) + 1
        child_idxs: list[int] = []
        for gy in range(gy0, gy1 + 1):
            for gx in range(gx0, gx1 + 1):
                for pi in grid.get((gx, gy), ()):
                    p = parts[pi]
                    pcx = 0.5 * (p.bbox[0] + p.bbox[2])
                    pcy = 0.5 * (p.bbox[1] + p.bbox[3])
                    if ax0 <= pcx <= ax1 and ay0 <= pcy <= ay1 and point_in_ring(pcx, pcy, a.outer):
                        child_idxs.append(pi)
        if not child_idxs:
            result.append(a)
            continue
        child_area = sum(parts[pi].area_m2 for pi in child_idxs)
        if child_area >= 0.58 * a.area_m2:
            for pi in child_idxs:
                if not part_used[pi]:
                    part_used[pi] = True
                    p = parts[pi]
                    p.exclude_mesh = p.exclude_mesh or a.exclude_mesh
                    result.append(p)
        else:
            result.append(a)
            for pi in child_idxs:
                part_used[pi] = True
                if parts[pi].height_m > a.height_m + 1.8:
                    p = parts[pi]
                    p.exclude_mesh = p.exclude_mesh or a.exclude_mesh
                    result.append(p)
    for pi, used in enumerate(part_used):
        if not used:
            result.append(parts[pi])
    return result


# ---------------- Fast Per-Chunk Padded 2.5D DSM + Fixed-Sun Ray-Tracer ----------------

def place_chunk_streetlamps(
    cx: int, cy: int,
    dem: np.ndarray, bldg_h: np.ndarray, is_urban: bool,
    min_e: float, max_e: float, min_n: float, max_n: float,
) -> np.ndarray:
    """Synthesize world-locked street/plaza/courtyard night emitters beside real OSM buildings (ZERO chunk seams!)."""
    res = dem.shape[0]
    bmask = bldg_h > 1.2
    if not np.any(bmask):
        return np.zeros((0, 6), dtype=np.float32)  # (px_x, px_y, east_m, elev_m, north_m, power)
    # Keep open streets down to 4m wide (~1 px) by masking ONLY actual building footprints (~bmask), NOT dilated footprints!
    near_street = binary_dilation(bmask, iterations=3) & (~bmask)
    stride = 4 if is_urban else 6
    # World-locked global pixel coordinates so adjacent 500m chunks place IDENTICAL lamps across seams!
    gx = (cx * SH_RES - SH_PAD_PX) + np.arange(res, dtype=np.int32)[None, :]
    gy = (cy * SH_RES - SH_PAD_PX) + np.arange(res, dtype=np.int32)[:, None]
    row_ok = (gy % stride) == 0
    stagger = ((gy // stride) & 1) * (stride // 2)
    col_ok = ((gx + stagger) % stride) == 0
    grid_mask = row_ok & col_ok
    grid_mask[:2, :] = False; grid_mask[-2:, :] = False
    grid_mask[:, :2] = False; grid_mask[:, -2:] = False

    ys, xs = np.nonzero(near_street & grid_mask)
    if xs.size == 0:
        ys, xs = np.nonzero(near_street)
        if xs.size == 0:
            return np.zeros((0, 6), dtype=np.float32)
        step = max(1, xs.size // 4)
        xs, ys = xs[::step][:4], ys[::step][:4]
    if xs.size > 420:
        step = max(1, xs.size // 420)
        xs, ys = xs[::step][:420], ys[::step][:420]
    scale_e = (max_e - min_e) / (res - 1)
    scale_n = (max_n - min_n) / (res - 1)
    lamps = np.empty((xs.size, 6), dtype=np.float32)
    lamps[:, 0] = xs.astype(np.float32)
    lamps[:, 1] = ys.astype(np.float32)
    lamps[:, 2] = min_e + xs * scale_e
    lamps[:, 3] = dem[ys, xs] + (7.6 if is_urban else 5.8)
    lamps[:, 4] = min_n + ys * scale_n
    # World-locked deterministic lamp brightness variation (exact match across chunk edges!)
    gxs = (cx * SH_RES - SH_PAD_PX + xs).astype(np.int64) & 0xFFFFFFFF
    gys = (cy * SH_RES - SH_PAD_PX + ys).astype(np.int64) & 0xFFFFFFFF
    hvar = (((gxs * 374761393) ^ (gys * 668265263)) >> 16) & 255
    lamps[:, 5] = (1.12 if is_urban else 0.78) * (0.76 + 0.48 * (hvar.astype(np.float32) / 255.0))
    return lamps


def bake_chunk_dsm_and_shadows(
    cx: int, cy: int,
    chunk_bldgs: list[PolyBldg],
    nbr_bldgs: list[PolyBldg],
    sampler: PackSampler,
    bake_night_lights: bool = True,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, float, float, float, float]:
    """Build padded 184x184 2.5D DSM (3.9m/px) around 500m chunk (cx,cy), ray-trace 10-TOD shadows + RT night lights."""
    min_e0, min_n0 = cx * CHUNK_M, cy * CHUNK_M
    min_e, max_e = min_e0 - PAD_M, min_e0 + CHUNK_M + PAD_M
    min_n, max_n = min_n0 - PAD_M, min_n0 + CHUNK_M + PAD_M

    dem = sampler.bake_patch_dem(min_e, max_e, min_n, max_n, DSM_RES)
    sx = (DSM_RES - 1) / (max_e - min_e)
    sy = (DSM_RES - 1) / (max_n - min_n)

    order = sorted(range(len(nbr_bldgs)), key=lambda i: nbr_bldgs[i].height_m)
    bh_img = Image.new("F", (DSM_RES, DSM_RES), 0.0)
    draw = ImageDraw.Draw(bh_img)
    hole_bldgs: list[PolyBldg] = []
    for i in order:
        b = nbr_bldgs[i]
        if b.bbox[2] < min_e or b.bbox[0] > max_e or b.bbox[3] < min_n or b.bbox[1] > max_n:
            continue
        if b.inners:
            hole_bldgs.append(b)
            continue
        draw.polygon([((e - min_e) * sx, (n - min_n) * sy) for (e, n) in b.outer], fill=float(b.height_m))

    bldg_h = np.asarray(bh_img, dtype=np.float32).copy()
    for b in hole_bldgs:
        x0 = max(0, int(math.floor((b.bbox[0] - min_e) * sx)) - 1)
        x1 = min(DSM_RES - 1, int(math.ceil((b.bbox[2] - min_e) * sx)) + 1)
        y0 = max(0, int(math.floor((b.bbox[1] - min_n) * sy)) - 1)
        y1 = min(DSM_RES - 1, int(math.ceil((b.bbox[3] - min_n) * sy)) + 1)
        w, h = x1 - x0 + 1, y1 - y0 + 1
        if w <= 1 or h <= 1:
            continue
        m_img = Image.new("L", (w, h), 0)
        md = ImageDraw.Draw(m_img)
        md.polygon([((e - min_e) * sx - x0, (n - min_n) * sy - y0) for (e, n) in b.outer], fill=255)
        for ir in b.inners:
            md.polygon([((e - min_e) * sx - x0, (n - min_n) * sy - y0) for (e, n) in ir], fill=0)
        mask = np.asarray(m_img, dtype=np.uint8) > 127
        sub = bldg_h[y0 : y1 + 1, x0 : x1 + 1]
        sub[mask] = np.maximum(sub[mask], b.height_m)

    dsm = dem + bldg_h

    # Ray-trace inner SH_RES x SH_RES (128x128) ground shadow + Sky-AO tile cleanly across ALL 10 times of day!
    px_m = (max_e - min_e) / (DSM_RES - 1)
    i0 = SH_PAD_PX
    i1 = SH_PAD_PX + SH_RES
    yy, xx = np.meshgrid(np.arange(i0, i1, dtype=np.float32), np.arange(i0, i1, dtype=np.float32), indexing="ij")
    dem_in = dem[i0:i1, i0:i1]
    vis_10: list[np.ndarray] = []

    dists = np.array([2.5, 5.2, 8.2, 11.8, 16.0, 21.0, 27.0, 34.0, 43.0, 54.0, 68.0, 86.0, 105.0], dtype=np.float32)
    for az_deg, el_deg in SUN_PHASES_10:
        se, su, sn = sun_dir_enu(az_deg, el_deg)
        hl = math.hypot(se, sn)
        de_u, dn_u, tan_el = se / hl, sn / hl, su / hl
        max_occ = np.zeros((SH_RES, SH_RES), dtype=np.float32)
        for d in dists:
            sx_i = np.clip(np.rint(xx + (de_u * d) / px_m).astype(np.int32), 0, DSM_RES - 1)
            sy_i = np.clip(np.rint(yy + (dn_u * d) / px_m).astype(np.int32), 0, DSM_RES - 1)
            occ = np.clip((dsm[sy_i, sx_i] - dem_in - (d * tan_el + 0.55)) / max(1.3, d * 0.095), 0.0, 1.0)
            max_occ = np.maximum(max_occ, occ)
        vis_10.append(gaussian_filter(1.0 - max_occ, sigma=0.70))

    # 8-Direction Horizon-Based Hemisphere Sky AO + Wall Contact Crevice (bits 20..27)
    ao_dists = [3.5, 7.5, 13.5, 22.0, 35.0, 52.0]
    ao_acc = np.zeros((SH_RES, SH_RES), dtype=np.float32)
    for k in range(8):
        ang = (2.0 * math.pi * k) / 8.0
        ce, cn = math.cos(ang), math.sin(ang)
        max_sin = np.zeros((SH_RES, SH_RES), dtype=np.float32)
        for d in ao_dists:
            sx_i = np.clip(np.rint(xx + (ce * d) / px_m).astype(np.int32), 0, DSM_RES - 1)
            sy_i = np.clip(np.rint(yy + (cn * d) / px_m).astype(np.int32), 0, DSM_RES - 1)
            dh = np.maximum(0.0, dsm[sy_i, sx_i] - dem_in)
            max_sin = np.maximum(max_sin, dh / np.sqrt(dh * dh + d * d))
        ao_acc += 1.0 - np.power(max_sin, 1.25)
    foot_blur = gaussian_filter((bldg_h[i0:i1, i0:i1] > 1.0).astype(np.float32), sigma=0.95)
    sky_ao = np.clip(gaussian_filter(ao_acc / 8.0, sigma=0.55) * (1.0 - np.clip(foot_blur * 0.42, 0.0, 0.36)), 0.14, 1.0)

    # Pre-Baked 3-Layer (0m Ground, 5m Mid-Canyon, 10m Upper-Canyon) 2.5D DSM Ray-Traced Volumetric Night Lightmaps!
    lamps = place_chunk_streetlamps(cx, cy, dem, bldg_h, len(chunk_bldgs) >= 25, min_e, max_e, min_n, max_n) if bake_night_lights else np.zeros((0, 6), dtype=np.float32)
    night_0m = np.zeros((SH_RES, SH_RES), dtype=np.float32)
    night_5m = np.zeros((SH_RES, SH_RES), dtype=np.float32)
    night_10m = np.zeros((SH_RES, SH_RES), dtype=np.float32)
    if lamps.shape[0] > 0:
        rad_px = 8  # ~31m streetlamp reach across 0m / 5m / 10m layers
        ts = (0.20, 0.40, 0.60, 0.80, 0.94)
        for li in range(lamps.shape[0]):
            lx, ly, _le, lz, _ln, pwr = lamps[li]
            lxi, lyi = int(round(float(lx))), int(round(float(ly)))
            x0 = max(i0, lxi - rad_px)
            x1 = min(i1 - 1, lxi + rad_px)
            y0 = max(i0, lyi - rad_px)
            y1 = min(i1 - 1, lyi + rad_px)
            if x0 > x1 or y0 > y1:
                continue
            gy, gx = np.meshgrid(np.arange(y0, y1 + 1, dtype=np.float32), np.arange(x0, x1 + 1, dtype=np.float32), indexing="ij")
            dx_m = (gx - lx) * px_m
            dy_m = (gy - ly) * px_m
            d2_xy = dx_m * dx_m + dy_m * dy_m
            dem_sub = dem[y0 : y1 + 1, x0 : x1 + 1]
            z0 = dem_sub + 0.35
            z5 = dem_sub + 5.00
            z10 = dem_sub + 10.00
            vis0 = np.ones_like(d2_xy)
            vis5 = np.ones_like(d2_xy)
            vis10 = np.ones_like(d2_xy)
            for t in ts:
                rx = np.clip(np.rint(gx + (lx - gx) * t).astype(np.int32), 0, DSM_RES - 1)
                ry = np.clip(np.rint(gy + (ly - gy) * t).astype(np.int32), 0, DSM_RES - 1)
                dsm_r = dsm[ry, rx]
                rz0 = z0 + (lz - z0) * t
                rz5 = z5 + (lz - z5) * t
                rz10 = z10 + (lz - z10) * t
                vis0 = np.minimum(vis0, np.clip((rz0 + 0.50 - dsm_r) / 0.85, 0.0, 1.0))
                vis5 = np.minimum(vis5, np.clip((rz5 + 0.60 - dsm_r) / 0.95, 0.0, 1.0))
                vis10 = np.minimum(vis10, np.clip((rz10 + 0.70 - dsm_r) / 1.05, 0.0, 1.0))
            fall0 = np.maximum(0.0, 1.0 - d2_xy / (27.0 * 27.0))
            fall5 = np.maximum(0.0, 1.0 - (d2_xy + 4.0) / (30.0 * 30.0))
            fall10 = np.maximum(0.0, 1.0 - (d2_xy + 12.0) / (32.0 * 32.0))
            sl_y = slice(y0 - i0, y1 - i0 + 1)
            sl_x = slice(x0 - i0, x1 - i0 + 1)
            night_0m[sl_y, sl_x] += pwr * vis0 * (fall0 * fall0) / (1.0 + d2_xy / 46.0)
            night_5m[sl_y, sl_x] += pwr * vis5 * (fall5 * fall5) / (1.0 + (d2_xy + 4.0) / 58.0)
            night_10m[sl_y, sl_x] += pwr * vis10 * (fall10 * fall10) / (1.0 + (d2_xy + 14.0) / 72.0)

        bh_in = bldg_h[i0:i1, i0:i1]
        night_0m = np.clip(np.where(bh_in > 1.2, night_0m * 0.10, night_0m) * 1.15, 0.0, 1.0)
        night_5m = np.clip(np.where(bh_in > 5.2, night_5m * 0.14, night_5m) * 1.10, 0.0, 1.0)
        night_10m = np.clip(np.where(bh_in > 10.2, night_10m * 0.18, night_10m) * 1.05, 0.0, 1.0)
        # Combine 0m razor-sharp ground lightmap + 5m mid-canyon lightmap + 10m upper-canyon lightmap into volumetric night field!
        night_gnd = np.clip(
            0.54 * night_0m
            + 0.28 * gaussian_filter(night_5m, sigma=0.60)
            + 0.18 * gaussian_filter(night_10m, sigma=1.10),
            0.0, 1.0,
        )
    else:
        night_gnd = night_0m

    sh_u32 = pack_tod10_u32(vis_10, sky_ao, night_gnd)
    return dem, dsm, sh_u32, lamps, (night_0m, night_5m, night_10m), min_e, max_e, min_n, max_n


def raytrace_chunk_vertices(
    pos_arr: np.ndarray, nrm_arr: np.ndarray, dem: np.ndarray, dsm: np.ndarray, lamps: np.ndarray,
    night_layers: tuple[np.ndarray, np.ndarray, np.ndarray],
    min_e: float, max_e: float, min_n: float, max_n: float,
) -> np.ndarray:
    res = dsm.shape[0]
    inv_e = (res - 1) / (max_e - min_e)
    inv_n = (res - 1) / (max_n - min_n)
    ve, vy, vn = pos_arr[:, 0], pos_arr[:, 1], pos_arr[:, 2]
    nx, ny, nz = nrm_arr[:, 0], nrm_arr[:, 1], nrm_arr[:, 2]
    ne, nn = nx, -nz
    orig_e = ve + ne * 1.8
    orig_n = vn + nn * 1.8
    orig_y = vy + np.where(ny > 0.5, 0.85, 0.55)

    n_v = ve.shape[0]
    vis_10: list[np.ndarray] = []
    dists = np.array([2.8, 6.0, 10.0, 15.5, 22.5, 32.0, 44.0, 60.0, 82.0, 106.0], dtype=np.float32)

    for az_deg, el_deg in SUN_PHASES_10:
        se, su, sn = sun_dir_enu(az_deg, el_deg)
        hl = math.hypot(se, sn)
        de_u, dn_u, tan_el = se / hl, sn / hl, su / hl
        ndotl = nx * se + ny * su + (-nz) * sn
        max_occ = np.zeros(n_v, dtype=np.float32)
        for d in dists:
            ix = np.clip(np.rint((orig_e + de_u * d - min_e) * inv_e).astype(np.int32), 0, res - 1)
            iy = np.clip(np.rint((orig_n + dn_u * d - min_n) * inv_n).astype(np.int32), 0, res - 1)
            occ = np.clip((dsm[iy, ix] - orig_y - d * tan_el) / max(1.5, d * 0.10), 0.0, 1.0)
            max_occ = np.maximum(max_occ, occ)
        vis_10.append(np.where(ndotl < -0.05, 0.0, 1.0 - max_occ).astype(np.float32))

    ao_dists = [4.0, 9.0, 17.0, 30.0]
    ao_acc = np.zeros(n_v, dtype=np.float32)
    ao_w = np.zeros(n_v, dtype=np.float32)
    for k in range(8):
        ang = (2.0 * math.pi * k) / 8.0
        ce, cn = math.cos(ang), math.sin(ang)
        dw = np.where(ny > 0.5, 1.0, np.clip(ne * ce + nn * cn + 0.25, 0.0, 1.0))
        max_sin = np.zeros(n_v, dtype=np.float32)
        for d in ao_dists:
            ix = np.clip(np.rint((orig_e + ce * d - min_e) * inv_e).astype(np.int32), 0, res - 1)
            iy = np.clip(np.rint((orig_n + cn * d - min_n) * inv_n).astype(np.int32), 0, res - 1)
            dh = np.maximum(0.0, dsm[iy, ix] - orig_y)
            max_sin = np.maximum(max_sin, dh / np.sqrt(dh * dh + d * d))
        ao_acc += dw * (1.0 - np.power(max_sin, 1.25))
        ao_w += dw
    sky_ao = np.clip(ao_acc / np.maximum(0.5, ao_w), 0.165, 1.0)

    # Ray-traced SHARP local night streetlamp irradiance + 3-Layer (0m / 5m / 10m) Volumetric Street-Canyon Light Column!
    night_v = np.zeros(n_v, dtype=np.float32)
    if lamps.shape[0] > 0:
        ts = (0.20, 0.40, 0.60, 0.80, 0.94)
        for li in range(lamps.shape[0]):
            _lx, _ly, le, lz, ln, pwr = lamps[li]
            de = le - orig_e
            dn = ln - orig_n
            dh = lz - orig_y
            d2 = de * de + dn * dn + dh * dh
            idx = np.flatnonzero(d2 < (29.0 * 29.0))
            if idx.size == 0:
                continue
            de_s, dn_s, dh_s = de[idx], dn[idx], dh[idx]
            d2_s = d2[idx]
            inv_d = 1.0 / np.sqrt(np.maximum(1.0, d2_s))
            ndotl = np.clip((ne[idx] * de_s + ny[idx] * dh_s + nn[idx] * dn_s) * inv_d, 0.0, 1.0)
            facing = np.where(ny[idx] > 0.5, ndotl * 0.24, 0.12 + 0.88 * ndotl)
            oe_s, on_s, oy_s = orig_e[idx], orig_n[idx], orig_y[idx]
            vis = np.ones(idx.size, dtype=np.float32)
            for t in ts:
                ix = np.clip(np.rint((oe_s + de_s * t - min_e) * inv_e).astype(np.int32), 0, res - 1)
                iy = np.clip(np.rint((on_s + dn_s * t - min_n) * inv_n).astype(np.int32), 0, res - 1)
                rz = oy_s + dh_s * t
                vis = np.minimum(vis, np.clip((rz + 0.60 - dsm[iy, ix]) / 0.90, 0.0, 1.0))
            falloff = np.maximum(0.0, 1.0 - d2_s / (29.0 * 29.0))
            night_v[idx] += pwr * vis * facing * (falloff * falloff) / (1.0 + d2_s / 62.0)

        # Sample the 3 ray-traced height lightmaps (0m, 5m, 10m) at each vertex & combine vertically for volumetric light!
        n0m, n5m, n10m = night_layers
        vx_dsm = np.clip(np.rint((orig_e - min_e) * inv_e).astype(np.int32), 0, res - 1)
        vy_dsm = np.clip(np.rint((orig_n - min_n) * inv_n).astype(np.int32), 0, res - 1)
        v_dem = dem[vy_dsm, vx_dsm]
        h_agl = np.maximum(0.0, vy - v_dem)
        sx_sh = np.clip(vx_dsm - SH_PAD_PX, 0, SH_RES - 1)
        sy_sh = np.clip(vy_dsm - SH_PAD_PX, 0, SH_RES - 1)
        l0 = n0m[sy_sh, sx_sh]
        l5 = n5m[sy_sh, sx_sh]
        l10 = n10m[sy_sh, sx_sh]
        t05 = np.clip(h_agl / 5.0, 0.0, 1.0)
        t510 = np.clip((h_agl - 5.0) / 5.0, 0.0, 1.0)
        layer_interp = np.where(
            h_agl <= 5.0,
            l0 * (1.0 - t05) + l5 * t05,
            np.where(h_agl <= 10.0, l5 * (1.0 - t510) + l10 * t510, l10 * np.exp(-(h_agl - 10.0) * 0.14)),
        )
        vol_col = 0.48 * l0 + 0.34 * l5 + 0.18 * l10
        wall_w = np.where(ny > 0.5, 0.18, 1.0)
        night_v = np.clip((night_v * 0.68 + (0.65 * layer_interp + 0.35 * vol_col) * 0.52 * wall_w) * 1.15, 0.0, 1.0)

    return pack_tod10_u32(vis_10, sky_ao, night_v)


def classify_roof_per_building(b: PolyBldg, g_elev_m: float, sampler: PackSampler) -> tuple[int, tuple[int, int, int]]:
    """Sample clean interior median IGN PNOA RGB -> ONE coherent roof_kind per building (ZERO camo patches!)."""
    ce = 0.5 * (b.bbox[0] + b.bbox[2])
    cn = 0.5 * (b.bbox[1] + b.bbox[3])
    # Sample 3 interior points (or ring-interior point for courtyard blocks!)
    if b.inners:
        # Midpoint between first outer vertex and centroid-toward-ring
        e0, n0 = b.outer[0]
        e1, n1 = b.outer[len(b.outer) // 2]
        pts = [
            (e0 * 0.72 + ce * 0.28, n0 * 0.72 + cn * 0.28),
            (e1 * 0.72 + ce * 0.28, n1 * 0.72 + cn * 0.28),
            (b.outer[len(b.outer) // 4][0] * 0.75 + ce * 0.25, b.outer[len(b.outer) // 4][1] * 0.75 + cn * 0.25),
        ]
    else:
        pts = [
            (ce, cn),
            (ce * 0.65 + b.outer[0][0] * 0.35, cn * 0.65 + b.outer[0][1] * 0.35),
            (ce * 0.65 + b.outer[len(b.outer) // 2][0] * 0.35, cn * 0.65 + b.outer[len(b.outer) // 2][1] * 0.35),
        ]
    cols = [sampler.sample_ortho_rgb_en(pe, pn) for (pe, pn) in pts]
    rr = sorted(c[0] for c in cols)[1]
    rg = sorted(c[1] for c in cols)[1]
    rb = sorted(c[2] for c in cols)[1]
    # Lift extreme shadow-clipped samples cleanly into realistic sunlit roof range
    luma = 0.2126 * rr + 0.7152 * rg + 0.0722 * rb
    if luma < 68.0:
        sc = 68.0 / max(12.0, luma)
        rr, rg, rb = min(225, int(rr * sc)), min(215, int(rg * sc)), min(210, int(rb * sc))
    elif luma > 205.0:
        sc = 205.0 / luma
        rr, rg, rb = int(rr * sc), int(rg * sc), int(rb * sc)

    warm = (rr - rb) / max(30.0, float(rr + rg + rb))
    if b.arch == 4:
        rk = 3  # Industrial galvanized ribbed steel
    elif g_elev_m > 820.0 and b.clat > 42.34 and (b.arch in (0, 1, 5) or warm < 0.09):
        rk = 3  # Authentic dark Pyrenean slate roofs (Castejon de Sos, Benasque, Jaca!)
        rr, rg, rb = min(115, int(rr * 0.75)), min(118, int(rg * 0.78)), min(126, int(rb * 0.85))
    elif (b.arch == 2 and b.height_m >= 13.5 and warm < 0.085) or (b.area_m2 > 520.0 and warm < 0.075):
        rk = 2  # Modern Spanish flat terrace roof (pavers, skylights, HVAC, solar!)
    elif warm >= 0.075 or b.arch in (0, 1, 3, 5):
        rk = 0 if (b.bhash < 148) else 1  # Warm Spanish Teja Arabe terracotta (0) vs weathered ochre clay (1)!
    else:
        rk = 2
    return rk, (rr, rg, rb)


def build_chunk_geometry(
    cbldgs: list[PolyBldg],
    dem: np.ndarray, dsm: np.ndarray, lamps: np.ndarray,
    night_layers: tuple[np.ndarray, np.ndarray, np.ndarray],
    min_e: float, max_e: float, min_n: float, max_n: float,
    sampler: PackSampler,
) -> tuple[np.ndarray, np.ndarray, tuple[float, float, float, float]] | None:
    res = dem.shape[0]
    inv_e = (res - 1) / (max_e - min_e)
    inv_n = (res - 1) / (max_n - min_n)

    def sample_dem_en(e: float, n: float) -> float:
        fx = max(0.0, min(res - 1.001, (e - min_e) * inv_e))
        fy = max(0.0, min(res - 1.001, (n - min_n) * inv_n))
        x0, y0 = int(fx), int(fy)
        dx, dy = fx - x0, fy - y0
        a = dem[y0, x0] * (1.0 - dx) + dem[y0, x0 + 1] * dx
        b = dem[y0 + 1, x0] * (1.0 - dx) + dem[y0 + 1, x0 + 1] * dx
        return float(a * (1.0 - dy) + b * dy)

    pos_list: list[tuple[float, float, float]] = []
    nrm_list: list[tuple[float, float, float]] = []
    uv_list: list[tuple[float, float]] = []
    flag_list: list[int] = []
    meta_list: list[tuple[int, int, int, int]] = []
    idx_list: list[int] = []

    c_min_e = c_min_y = c_min_n = 1e18
    c_max_e = c_max_y = c_max_n = -1e18

    def push_v(e: float, y: float, n: float, nx: float, ny: float, nz: float, u: float, v: float,
               flags: int, m0: int, m1: int, m2: int, m3: int) -> int:
        vi = len(pos_list)
        pos_list.append((e, y, n))
        nrm_list.append((nx, ny, nz))
        uv_list.append((u, v))
        flag_list.append(flags)
        meta_list.append((m0, m1, m2, m3))
        return vi

    for b in cbldgs:
        if b.exclude_mesh:
            continue
        g_vals = [sample_dem_en(e, n) for (e, n) in b.outer]
        g_min, g_max = min(g_vals), max(g_vals)
        y_bot = g_min - 2.6
        y_vis_base = g_min
        y_top = g_max + b.height_m
        vis_h = y_top - y_vis_base

        roof_kind, (rr, rg, rb) = classify_roof_per_building(b, g_min, sampler)
        is_flat = (roof_kind == 2) or (b.arch == 2 and b.height_m >= 12.0) or bool(b.inners)
        parapet_h = 0.65 if (is_flat and b.arch != 4 and b.height_m >= 7.5) else 0.0
        eave_h = max(3.2, vis_h - parapet_h)
        eave_b = max(4, min(255, int(round((eave_h / 64.0) * 255.0))))
        base_flags = ((b.style & 7) << 2) | ((roof_kind & 3) << 5)

        c_min_e = min(c_min_e, b.bbox[0]); c_max_e = max(c_max_e, b.bbox[2])
        c_min_n = min(c_min_n, b.bbox[1]); c_max_n = max(c_max_n, b.bbox[3])
        c_min_y = min(c_min_y, y_bot);     c_max_y = max(c_max_y, y_top + 3.0)

        # 1. Outer & inner-courtyard walls (mid-split on tall >=14.5m blocks for crisp neighbor shadow cutoff!)
        y_levels = [y_bot, y_top]
        if vis_h >= 14.5:
            y_levels = [y_bot, y_vis_base + min(4.8, vis_h * 0.42), y_top]

        for ring in [b.outer, *b.inners]:
            nr = len(ring)
            for ei in range(nr):
                e0, n0 = ring[ei]
                e1, n1 = ring[(ei + 1) % nr]
                de, dn = e1 - e0, n1 - n0
                elen = math.hypot(de, dn)
                if elen < 0.25:
                    continue
                nx_gl, nz_gl = dn / elen, de / elen
                u1 = max(1.0, round(elen / 3.4)) * 0.25
                for li in range(len(y_levels) - 1):
                    ya, yb = y_levels[li], y_levels[li + 1]
                    va, vb = max(0.0, ya - y_vis_base), max(0.0, yb - y_vis_base)
                    v0 = push_v(e0, ya, n0, nx_gl, 0.0, nz_gl, 0.0, va, base_flags, b.bhash, eave_b, b.arch, 0)
                    v1 = push_v(e1, ya, n1, nx_gl, 0.0, nz_gl, u1,  va, base_flags, b.bhash, eave_b, b.arch, 0)
                    v2 = push_v(e1, yb, n1, nx_gl, 0.0, nz_gl, u1,  vb, base_flags, b.bhash, eave_b, b.arch, 0)
                    v3 = push_v(e0, yb, n0, nx_gl, 0.0, nz_gl, 0.0, vb, base_flags, b.bhash, eave_b, b.arch, 0)
                    idx_list.extend((v0, v1, v2, v0, v2, v3))

        # 2. Wall-aligned local roof UVs (tiles & terrace pavers align cleanly to each building's own walls!)
        ux, uy = longest_edge_axis(b.outer)
        vx, vy = -uy, ux
        ce = 0.5 * (b.bbox[0] + b.bbox[2])
        cn = 0.5 * (b.bbox[1] + b.bbox[3])
        roof_flags = base_flags | 0x01

        def roof_uv(pe: float, pn: float) -> tuple[float, float]:
            de, dn = pe - ce, pn - cn
            return de * ux + dn * uy, de * vx + dn * vy

        # 3D hipped pitched roofs for traditional houses, historic blocks & Pyrenean village homes!
        pitched = False
        if (not b.inners) and (roof_kind != 2) and b.arch != 4 and b.height_m < 13.5 and 30.0 <= b.area_m2 <= 300.0 and len(b.outer) <= 10:
            pc_e = sum(p[0] for p in b.outer) / len(b.outer)
            pc_n = sum(p[1] for p in b.outer) / len(b.outer)
            nr = len(b.outer)
            if all(
                (b.outer[(k + 1) % nr][0] - b.outer[k][0]) * (pc_n - b.outer[k][1])
                - (b.outer[(k + 1) % nr][1] - b.outer[k][1]) * (pc_e - b.outer[k][0]) > 0.65
                for k in range(nr)
            ):
                pitched = True
                ridge_h = min(2.4, max(1.15, math.sqrt(b.area_m2) * 0.16))
                uc, vc = roof_uv(pc_e, pc_n)
                for k in range(nr):
                    e0, n0 = b.outer[k]
                    e1, n1 = b.outer[(k + 1) % nr]
                    ax, ay, az = e1 - e0, 0.0, -(n1 - n0)
                    bx, by, bz = pc_e - e0, ridge_h, -(pc_n - n0)
                    fnx, fny, fnz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
                    fl = max(1e-5, math.sqrt(fnx * fnx + fny * fny + fnz * fnz))
                    fnx, fny, fnz = fnx / fl, fny / fl, fnz / fl
                    u0, v0 = roof_uv(e0, n0)
                    u1, v1 = roof_uv(e1, n1)
                    i0 = push_v(e0, y_top, n0, fnx, fny, fnz, u0, v0, roof_flags, rr, rg, rb, b.bhash)
                    i1 = push_v(e1, y_top, n1, fnx, fny, fnz, u1, v1, roof_flags, rr, rg, rb, b.bhash)
                    i2 = push_v(pc_e, y_top + ridge_h, pc_n, fnx, fny, fnz, uc, vc, roof_flags, rr, rg, rb, b.bhash)
                    idx_list.extend((i0, i1, i2))

        if not pitched:
            poly2d, tris = triangulate_polygon(b.outer, b.inners)
            if tris:
                r_base = len(pos_list)
                # Slightly inset flat terrace floor (-0.35m below parapet rim!) on apartment roofs for crisp 3D depth!
                r_y = y_top - (0.28 if parapet_h > 0.1 else 0.0)
                for (pe, pn) in poly2d:
                    ru, rv = roof_uv(pe, pn)
                    push_v(pe, r_y, pn, 0.0, 1.0, 0.0, ru, rv, roof_flags, rr, rg, rb, b.bhash)
                for (ia, ib, ic) in tris:
                    idx_list.extend((r_base + ia, r_base + ib, r_base + ic))

                # Add authentic Spanish rooftop stairwell bulkhead ("castillete") on large flat apartment roofs!
                if is_flat and b.arch == 2 and b.area_m2 >= 150.0 and (not b.inners) and point_in_ring(ce, cn, b.outer):
                    bw_c = 2.6 + (b.bhash & 3) * 0.45
                    bd_c = 2.0 + ((b.bhash >> 2) & 3) * 0.35
                    bh_c = 2.1 + ((b.bhash >> 4) & 3) * 0.25
                    # Verify all 4 castillete corners stay strictly inside the roof polygon!
                    cc = [
                        (ce + (-bw_c) * ux + (-bd_c) * vx, cn + (-bw_c) * uy + (-bd_c) * vy),
                        (ce + (+bw_c) * ux + (-bd_c) * vx, cn + (+bw_c) * uy + (-bd_c) * vy),
                        (ce + (+bw_c) * ux + (+bd_c) * vx, cn + (+bw_c) * uy + (+bd_c) * vy),
                        (ce + (-bw_c) * ux + (+bd_c) * vx, cn + (-bw_c) * uy + (+bd_c) * vy),
                    ]
                    if all(point_in_ring(pe, pn, b.outer) for (pe, pn) in cc):
                        par_flags = base_flags | 0x02  # Clean masonry parapet/penthouse material (no windows!)
                        for k in range(4):
                            e0, n0 = cc[k]
                            e1, n1 = cc[(k + 1) % 4]
                            de, dn = e1 - e0, n1 - n0
                            el = max(0.1, math.hypot(de, dn))
                            v0 = push_v(e0, r_y, n0, dn / el, 0.0, de / el, 0.0, 0.0, par_flags, b.bhash, 255, b.arch, 0)
                            v1 = push_v(e1, r_y, n1, dn / el, 0.0, de / el, 1.0, 0.0, par_flags, b.bhash, 255, b.arch, 0)
                            v2 = push_v(e1, r_y + bh_c, n1, dn / el, 0.0, de / el, 1.0, bh_c, par_flags, b.bhash, 255, b.arch, 0)
                            v3 = push_v(e0, r_y + bh_c, n0, dn / el, 0.0, de / el, 0.0, bh_c, par_flags, b.bhash, 255, b.arch, 0)
                            idx_list.extend((v0, v1, v2, v0, v2, v3))
                        # Castillete cap
                        r0 = push_v(cc[0][0], r_y + bh_c, cc[0][1], 0.0, 1.0, 0.0, -bw_c, -bd_c, roof_flags, rr, rg, rb, b.bhash)
                        r1 = push_v(cc[1][0], r_y + bh_c, cc[1][1], 0.0, 1.0, 0.0,  bw_c, -bd_c, roof_flags, rr, rg, rb, b.bhash)
                        r2 = push_v(cc[2][0], r_y + bh_c, cc[2][1], 0.0, 1.0, 0.0,  bw_c,  bd_c, roof_flags, rr, rg, rb, b.bhash)
                        r3 = push_v(cc[3][0], r_y + bh_c, cc[3][1], 0.0, 1.0, 0.0, -bw_c,  bd_c, roof_flags, rr, rg, rb, b.bhash)
                        idx_list.extend((r0, r1, r2, r0, r2, r3))

    if not idx_list:
        return None

    pos_arr = np.asarray(pos_list, dtype=np.float32)
    nrm_arr = np.asarray(nrm_list, dtype=np.float32)
    uv_arr = np.asarray(uv_list, dtype=np.float32)
    bake_arr = raytrace_chunk_vertices(pos_arr, nrm_arr, dem, dsm, lamps, night_layers, min_e, max_e, min_n, max_n)

    dt = np.dtype([
        ("pos", "<f4", 3), ("nrm", "i1", 3), ("flags", "u1"),
        ("uv", "<f4", 2), ("bake", "<u4"), ("meta", "u1", 4),
    ])
    assert dt.itemsize == 32
    verts = np.empty(len(pos_list), dtype=dt)
    verts["pos"] = pos_arr
    verts["nrm"] = np.clip(np.rint(nrm_arr * 127.0), -127, 127).astype(np.int8)
    verts["flags"] = np.asarray(flag_list, dtype=np.uint8)
    verts["uv"] = uv_arr
    verts["bake"] = bake_arr
    verts["meta"] = np.asarray(meta_list, dtype=np.uint8)

    ce = 0.5 * (c_min_e + c_max_e)
    cy = 0.5 * (c_min_y + c_max_y)
    cn = 0.5 * (c_min_n + c_max_n)
    rad = math.sqrt((c_max_e - ce) ** 2 + (c_max_y - cy) ** 2 + (c_max_n - cn) ** 2) + 8.0
    return verts, np.asarray(idx_list, dtype=np.uint32), (ce, cy, cn, rad)


def run_unit_checks() -> None:
    outer = [(0.0, 0.0), (20.0, 0.0), (20.0, 20.0), (0.0, 20.0)]
    hole = [(6.0, 6.0), (6.0, 14.0), (14.0, 14.0), (14.0, 6.0)]
    p2, tris = triangulate_polygon(outer, [hole])
    area = sum(0.5 * ((p2[b][0] - p2[a][0]) * (p2[c][1] - p2[a][1]) - (p2[b][1] - p2[a][1]) * (p2[c][0] - p2[a][0])) for a, b, c in tris)
    assert abs(area - 336.0) < 1e-4, f"Courtyard area check failed: {area}"
    assert len(SUN_PHASES_10) == 10 and SUN_PHASES_10[2] == (114.0, 19.0)
    se, su, sn = sun_dir_enu(114.0, 19.0)
    assert se > 0.85 and su > 0.30 and sn < -0.35
    assert SH_RES + 2 * SH_PAD_PX == DSM_RES
    # Verify 10-time-of-day 2-bit + 8-bit Sky AO + 4-bit RT Night Light uint32 pack/unpack round-trip!
    test_v = [np.array([(k % 4) / 3.0], dtype=np.float32) for k in range(10)]
    test_ao = np.array([173.0 / 255.0], dtype=np.float32)
    test_nl = np.array([11.0 / 15.0], dtype=np.float32)
    packed = int(pack_tod10_u32(test_v, test_ao, test_nl)[0])
    for k in range(10):
        assert ((packed >> (k * 2)) & 3) == (k % 4), f"TOD slot {k} mismatch"
    assert ((packed >> 20) & 255) == 173, "Sky AO pack mismatch"
    assert ((packed >> 28) & 15) == 11, "RT Night Light pack mismatch"


_G_CHUNKS_MAP: dict[tuple[int, int], list[PolyBldg]] = {}
_G_PACK_PATH: Path = PACK_PATH
_G_BAKE_NIGHT: bool = True
_G_WORKER_READER: PackReader | None = None
_G_WORKER_SAMPLER: PackSampler | None = None


def _worker_init() -> None:
    global _G_WORKER_READER, _G_WORKER_SAMPLER
    _G_WORKER_READER = PackReader(_G_PACK_PATH)
    _G_WORKER_SAMPLER = PackSampler(_G_WORKER_READER)


def _bake_batch(keys: list[tuple[int, int]]) -> list[tuple[int, int, bytes, int, np.ndarray | None, np.ndarray | None, tuple[float, float, float, float] | None]]:
    assert _G_WORKER_SAMPLER is not None
    out = []
    for cx, cy in keys:
        cbldgs = _G_CHUNKS_MAP[(cx, cy)]
        nbr_bldgs: list[PolyBldg] = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nb = _G_CHUNKS_MAP.get((cx + dx, cy + dy))
                if nb:
                    nbr_bldgs.extend(nb)
        dem, dsm, sh_u32, lamps, night_layers, min_e, max_e, min_n, max_n = bake_chunk_dsm_and_shadows(
            cx, cy, cbldgs, nbr_bldgs, _G_WORKER_SAMPLER, bake_night_lights=_G_BAKE_NIGHT
        )
        sh_comp = zlib.compress(sh_u32.tobytes(), level=6)
        built = build_chunk_geometry(cbldgs, dem, dsm, lamps, night_layers, min_e, max_e, min_n, max_n, _G_WORKER_SAMPLER)
        is_urban = 1 if len(cbldgs) >= 25 else 0
        if built is not None:
            verts, inds, bounds = built
            out.append((cx, cy, sh_comp, is_urban, verts, inds, bounds))
        else:
            out.append((cx, cy, sh_comp, is_urban, None, None, None))
    return out


def main(argv: list[str] | None = None) -> None:
    global _G_CHUNKS_MAP, _G_PACK_PATH, _G_BAKE_NIGHT, BBOX, SH_RES, DSM_RES, SH_PAD_PX
    ap = argparse.ArgumentParser(description="Pre-bake 10-TOD exact-polygon settlements, ground shadows & RT night lights.")
    ap.add_argument("--pack", type=Path, default=PACK_PATH)
    ap.add_argument("--pbf", type=Path, nargs="+", default=[PBF_PATH])
    ap.add_argument("--out-city", type=Path, default=None)
    ap.add_argument("--out-sh", type=Path, default=None)
    ap.add_argument("--sh-res", type=int, default=128, choices=[64, 96, 128, 160])
    ap.add_argument("--no-night-lights", action="store_true")
    ap.add_argument("--workers", type=int, default=max(1, min(14, (os.cpu_count() or 4) - 2)))
    args = ap.parse_args(argv)

    SH_RES = int(args.sh_res)
    SH_PAD_PX = int(round(PAD_M / (CHUNK_M / SH_RES)))
    DSM_RES = SH_RES + 2 * SH_PAD_PX
    run_unit_checks()

    pack_path: Path = args.pack
    pbf_paths: list[Path] = [Path(x) for x in (args.pbf if isinstance(args.pbf, list) else [args.pbf])]
    out_city = args.out_city or (pack_path.parent / (pack_path.stem.replace("_pyrenees", "") + "_cities.nscity"))
    out_sh = args.out_sh or (pack_path.parent / (pack_path.stem.replace("_pyrenees", "") + "_city_sh.nsshd"))
    if args.pack == PACK_PATH and args.out_city is None:
        out_city = OUT_CITY_PATH
    if args.pack == PACK_PATH and args.out_sh is None:
        out_sh = OUT_SH_PATH
    _G_PACK_PATH = pack_path
    _G_BAKE_NIGHT = not args.no_night_lights

    t0 = time.time()
    missing_pbf = [str(p) for p in pbf_paths if not p.exists()]
    if missing_pbf or not pack_path.exists():
        raise SystemExit(f"Missing OSM PBF ({missing_pbf}) or region pack ({pack_path})")

    with PackReader(pack_path) as reader:
        BBOX = tuple(reader.manifest["bbox"])
        sampler = PackSampler(reader)
        print(f"Extracting ALL exact OSM polygons across bbox={BBOX} from {len(pbf_paths)} PBF(s) (sh_res={SH_RES}, night_rt={_G_BAKE_NIGHT})...")
        ext = AllAragonPolyExtractor(sampler)
        for pbf_path in pbf_paths:
            ext.apply_file(str(pbf_path), locations=True)
        print(f"  Raw OSM: {len(ext.areas):,} areas, {len(ext.parts):,} parts ({time.time() - t0:.1f}s)")

    clean = deduplicate_cadastre(ext.areas, ext.parts)
    print(f"  Deduplicated zero-overlap buildings: {len(clean):,} (courtyards: {sum(1 for b in clean if b.inners):,})")

    chunks_map: dict[tuple[int, int], list[PolyBldg]] = defaultdict(list)
    for b in clean:
        ce = 0.5 * (b.bbox[0] + b.bbox[2])
        cn = 0.5 * (b.bbox[1] + b.bbox[3])
        chunks_map[(int(math.floor(ce / CHUNK_M)), int(math.floor(cn / CHUNK_M)))].append(b)
    sorted_keys = sorted(chunks_map.keys())
    _G_CHUNKS_MAP = chunks_map

    n_workers = max(1, min(16, int(args.workers)))
    batch_sz = 48
    batches = [sorted_keys[i : i + batch_sz] for i in range(0, len(sorted_keys), batch_sz)]
    print(f"  Baking {len(sorted_keys):,} populated 500m chunks x 10 TOD + RT Night Lights across {n_workers} workers ({len(batches)} batches)...")

    all_chunks_meta: list[tuple[int, int, int, int, float, float, float, float]] = []
    all_verts: list[np.ndarray] = []
    all_idx: list[np.ndarray] = []
    sh_tiles: list[tuple[int, int, bytes]] = []
    vert_off = 0
    idx_off = 0
    done_chunks = 0

    ctx = mp.get_context("fork")
    with ctx.Pool(processes=n_workers, initializer=_worker_init) as pool:
        for batch_res in pool.imap(_bake_batch, batches):
            for cx, cy, sh_comp, is_urban, verts, inds, bounds in batch_res:
                sh_tiles.append((cx, cy, sh_comp))
                if verts is not None and inds is not None and bounds is not None:
                    ce, cy_m, cn, rad = bounds
                    all_chunks_meta.append((is_urban, idx_off, len(inds), vert_off, ce, cy_m, cn, rad))
                    all_verts.append(verts)
                    all_idx.append(inds)
                    vert_off += len(verts)
                    idx_off += len(inds)
                done_chunks += 1
            if done_chunks % 600 < batch_sz or done_chunks == len(sorted_keys):
                print(f"  Baked {done_chunks}/{len(sorted_keys)} chunks | {vert_off:,} verts | {idx_off // 3:,} tris ({time.time() - t0:.1f}s)", flush=True)

    # Write .nscity (version = 3: map-wide exact polygons + 10-TOD + Sky AO + RT Night Local Lights!)
    verts_cat = np.concatenate(all_verts) if all_verts else np.empty((0,), dtype=np.uint8)
    idx_cat = np.concatenate(all_idx) if all_idx else np.empty((0,), dtype=np.uint32)
    assert len(verts_cat) == vert_off and len(idx_cat) == idx_off
    out_city.parent.mkdir(parents=True, exist_ok=True)
    with open(out_city, "wb") as f:
        f.write(struct.pack("<4sIIIII", b"NSCT", 3, len(clean), len(all_chunks_meta), len(verts_cat), len(idx_cat)))
        for ch in all_chunks_meta:
            f.write(struct.pack("<IIIiffff", *ch))
        f.write(verts_cat.tobytes())
        f.write(idx_cat.tobytes())

    # Write .nsshd (version = 2: sparse zlib-compressed SH_RES^2 uint32 10-TOD shadows + Sky AO + RT Night Lights!)
    with open(out_sh, "wb") as f:
        f.write(struct.pack("<4sIIfI", b"NSSH", 2, SH_RES, float(CHUNK_M), len(sh_tiles)))
        data_off = 20 + len(sh_tiles) * 16
        cur_off = data_off
        for cx, cy, cbytes in sh_tiles:
            f.write(struct.pack("<iiII", cx, cy, cur_off, len(cbytes)))
            cur_off += len(cbytes)
        for _cx, _cy, cbytes in sh_tiles:
            f.write(cbytes)

    print(f"\n[DONE] {out_city.name} v3: {out_city.stat().st_size / 1e6:.2f} MB ({len(all_chunks_meta):,} chunks, {len(verts_cat):,} verts, {len(idx_cat)//3:,} tris)")
    print(f"[DONE] {out_sh.name} v2: {out_sh.stat().st_size / 1e6:.2f} MB ({len(sh_tiles):,} populated 500m 10-TOD+NightRT tiles @ {CHUNK_M/SH_RES:.1f}m/px) in {time.time() - t0:.1f}s!")


if __name__ == "__main__":
    main()
