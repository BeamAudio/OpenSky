#!/usr/bin/env python3
"""build_region.py -- OpenSky VFR World Region Package Manager & Offline Pack Baker.

Manage regional scenery packages (terrain DEM, IGN PNOA orthophoto, hydrography, OSM roads/buildings,
and pre-baked 10-Time-of-Day + Ray-Traced Night Local Lights static settlements).

Built-in data sources:
  - Orthophoto: IGN Spain PNOA Maxima Actualidad WMTS (CC-BY 4.0)
  - Elevation:  IGN Spain MDT05 (5 m) & MDT25 (25 m) WCS ArcGrid (CC-BY 4.0),
                with AWS Terrarium fallback outside Spain. Imported sources retain the
                attribution supplied by the user; check terms before redistribution.
  - Airports:   OurAirports (Public Domain) + OpenStreetMap (ODbL).

Package Manager Examples:
  python3 builder/build_region.py list
  python3 builder/build_region.py presets
  python3 builder/build_region.py info es_aragon_pyrenees
  python3 builder/build_region.py install zaragoza-pyrenees-quick --quality high --bake-cities
  python3 builder/build_region.py install --region-id es_custom --bbox -0.6 42.0 0.2 42.6 --quality medium --bake-cities
  python3 builder/build_region.py remove es_custom --yes
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any

import numpy as np
import requests
from PIL import Image
from scipy.ndimage import gaussian_filter1d, map_coordinates

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from builder.airports import haversine_m, merge_airports  # noqa: E402
from protocol.pack_format import (  # noqa: E402
    ENC_JPEG,
    ENC_PNG,
    LAYER_ELEVATION,
    LAYER_IMAGERY,
    LAYER_SURFACE,
    TileRecord,
    encode_height_tile,
    write_pack,
)

USER_AGENT = "NorthernSpainVFRSim/0.1 (builder; IGN CC-BY 4.0)"

# Broad service coverage guard to fail fast on accidental global PNOA requests.
PNOA_COVERAGE_BBOX = (-18.2, 27.6, 4.35, 43.8)

PNOA_WMTS_URL = (
    "https://www.ign.es/wmts/pnoa-ma?"
    "SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=OI.OrthoimageCoverage"
    "&STYLE=default&FORMAT=image/jpeg&TILEMATRIXSET=GoogleMapsCompatible"
    "&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}"
)
IGN_WCS_URL = "https://servicios.idee.es/wcs-inspire/mdt"
TERRARIUM_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"

# Geofabrik Spain regional PBF directory + approximate bounds for automatic custom-bbox matching.
GEOFABRIK_SPAIN_URL = "https://download.geofabrik.de/europe/spain/{sub}-latest.osm.pbf"
GEOFABRIK_CANARY_URL = "https://download.geofabrik.de/africa/canary-islands-latest.osm.pbf"
SPAIN_OSM_BOXES: dict[str, tuple[float, float, float, float]] = {
    "galicia": (-9.40, 41.75, -6.65, 43.85),
    "asturias": (-7.25, 42.85, -4.45, 43.70),
    "cantabria": (-4.88, 42.72, -3.10, 43.55),
    "pais-vasco": (-3.50, 42.45, -1.70, 43.50),
    "navarra": (-2.55, 41.90, -0.70, 43.35),
    "la-rioja": (-3.20, 41.90, -1.65, 42.70),
    "aragon": (-2.25, 39.80, 0.85, 42.95),
    "cataluna": (0.10, 40.45, 3.40, 42.95),
    "castilla-y-leon": (-7.15, 40.05, -1.75, 43.25),
    "madrid": (-4.65, 39.85, -3.00, 41.20),
    "extremadura": (-7.60, 37.90, -4.60, 40.55),
    "castilla-la-mancha": (-5.45, 38.00, -0.90, 41.35),
    "valencia": (-1.60, 37.80, 0.70, 40.85),
    "murcia": (-2.45, 37.35, -0.60, 38.80),
    "andalucia": (-7.60, 35.90, -1.55, 38.80),
    "islas-baleares": (1.10, 38.55, 4.40, 40.20),
    "canary-islands": (-18.25, 27.55, -13.30, 29.40),
}

_STD_PRESET_BASE: dict[str, Any] = {
    "cruise_imagery_zooms": (10, 11, 12),
    "cruise_elev_zooms": (10, 11, 12),
    "mid_imagery_zooms": (13, 14),
    "mid_elev_zooms": (13,),
    "mid_radius_km": {"large_airport": 9.0, "medium_airport": 7.5, "small_airport": 6.0, "airstrip": 5.0},
    "inset_imagery_zooms": (15, 16),
    "inset_elev_zooms": (14,),
    "inset_radius_km": {"large_airport": 3.5, "medium_airport": 2.5, "small_airport": 2.0, "airstrip": 1.6},
    "close_imagery_zooms": (17,),
    "close_radius_km": {"large_airport": 1.2, "medium_airport": 1.0, "small_airport": 0.9, "airstrip": 0.75},
}


def _preset(entry: dict[str, Any]) -> dict[str, Any]:
    out = dict(_STD_PRESET_BASE)
    out.update(entry)
    return out


# Complete Spain regional catalog (every corner of Mainland Spain + Balearic & Canary Islands).
PRESETS: dict[str, dict[str, Any]] = {
    "zaragoza-pyrenees-quick": _preset({
        "region_id": "es_aragon_pyrenees",
        "name": "Aragon & Central Pyrenees (HD Corridor)",
        "bbox": (-1.20, 41.55, 0.65, 42.65),
        "pbf_subs": ("aragon",),
        "mid_radius_km": {"large_airport": 9.0, "medium_airport": 7.5, "small_airport": 6.5, "airstrip": 5.5},
        "inset_radius_km": {"large_airport": 3.2, "medium_airport": 2.4, "small_airport": 2.0, "airstrip": 1.6},
        "landmark_insets": [
            ("ZARAGOZA_CITY", 41.656, -0.878, 9.0, 5.5),
            ("HUESCA_CITY",   42.136, -0.408, 6.0, 3.2),
            ("JACA_CITY",     42.570, -0.550, 5.5, 2.6),
            ("MEDIANO_PENA",  42.395,  0.198, 12.0, 7.5),
            ("BENASQUE_MTNS", 42.580,  0.510, 10.0, 6.0),
        ],
    }),
    "zaragoza-pyrenees": _preset({
        "region_id": "es_aragon_pyrenees",
        "name": "Aragon & Central Pyrenees",
        "bbox": (-1.30, 41.50, 0.75, 42.85),
        "pbf_subs": ("aragon",),
        "cruise_imagery_zooms": (10, 11, 12, 13),
        "mid_imagery_zooms": (14,),
        "mid_radius_km": {"large_airport": 12.0, "medium_airport": 10.0, "small_airport": 8.0, "airstrip": 7.0},
        "inset_radius_km": {"large_airport": 5.0, "medium_airport": 3.8, "small_airport": 2.8, "airstrip": 2.2},
        "close_radius_km": {"large_airport": 2.0, "medium_airport": 1.5, "small_airport": 1.2, "airstrip": 1.0},
    }),
    "aragon-south": _preset({
        "region_id": "es_aragon_south",
        "name": "Southern Aragon & Teruel (Iberian System)",
        "bbox": (-2.15, 39.85, 0.55, 41.65),
        "pbf_subs": ("aragon",),
        "landmark_insets": [("TERUEL", 40.345, -1.106, 6.5, 3.2)],
    }),
    "navarra-basque": _preset({
        "region_id": "es_navarra_euskadi",
        "name": "Navarra, Rioja & Basque Coast",
        "bbox": (-3.45, 42.15, -1.05, 43.48),
        "pbf_subs": ("pais-vasco", "navarra", "la-rioja"),
        "landmark_insets": [("BILBAO", 43.263, -2.935, 8.0, 4.0), ("PAMPLONA", 42.812, -1.646, 6.5, 3.2)],
    }),
    "catalonia-pyrenees": _preset({
        "region_id": "es_catalonia_pyrenees",
        "name": "Catalan Pyrenees, Lleida & Andorra",
        "bbox": (0.45, 41.55, 2.35, 42.75),
        "pbf_subs": ("cataluna",),
        "landmark_insets": [("LLEIDA", 41.617, 0.620, 6.5, 3.2), ("ANDORRA", 42.507, 1.522, 6.5, 3.5)],
    }),
    "catalonia-coast": _preset({
        "region_id": "es_catalonia_coast",
        "name": "Barcelona, Tarragona, Girona & Costa Brava",
        "bbox": (0.35, 40.50, 3.35, 42.45),
        "pbf_subs": ("cataluna",),
        "landmark_insets": [("BARCELONA", 41.387, 2.170, 10.0, 5.5), ("GIRONA", 41.979, 2.821, 6.0, 3.0)],
    }),
    "asturias-cantabria": _preset({
        "region_id": "es_picos_europa",
        "name": "Picos de Europa & Cantabrian Coast",
        "bbox": (-7.05, 42.78, -3.15, 43.68),
        "pbf_subs": ("asturias", "cantabria"),
        "landmark_insets": [("OVIEDO_GIJON", 43.460, -5.750, 8.0, 3.8), ("SANTANDER", 43.462, -3.810, 7.0, 3.5)],
    }),
    "galicia": _preset({
        "region_id": "es_galicia",
        "name": "Galicia & Atlantic Coast",
        "bbox": (-9.30, 41.80, -6.70, 43.80),
        "pbf_subs": ("galicia",),
        "mid_radius_km": {"large_airport": 10.0, "medium_airport": 8.0, "small_airport": 7.0, "airstrip": 6.0},
        "inset_radius_km": {"large_airport": 4.5, "medium_airport": 3.5, "small_airport": 2.6, "airstrip": 2.0},
        "close_radius_km": {"large_airport": 1.6, "medium_airport": 1.3, "small_airport": 1.0, "airstrip": 0.9},
        "landmark_insets": [("SANTIAGO", 42.878, -8.545, 7.0, 3.5), ("VIGO_CORUNA", 43.362, -8.411, 7.5, 3.5)],
    }),
    "castilla-y-leon": _preset({
        "region_id": "es_castilla_y_leon",
        "name": "Castilla y Leon (Meseta Norte & Duero)",
        "bbox": (-7.05, 40.30, -1.80, 42.90),
        "pbf_subs": ("castilla-y-leon",),
        "landmark_insets": [("VALLADOLID", 41.652, -4.724, 7.5, 3.8), ("SALAMANCA", 40.970, -5.663, 6.5, 3.2)],
    }),
    "madrid-central": _preset({
        "region_id": "es_madrid_central",
        "name": "Madrid & Sierra de Guadarrama / Gredos",
        "bbox": (-4.95, 39.85, -2.90, 41.35),
        "pbf_subs": ("madrid", "castilla-y-leon", "castilla-la-mancha"),
        "landmark_insets": [("MADRID_CITY", 40.417, -3.704, 11.0, 6.0), ("GUADARRAMA", 40.780, -3.990, 8.0, 4.0)],
    }),
    "extremadura": _preset({
        "region_id": "es_extremadura",
        "name": "Extremadura (Caceres, Badajoz & Tajo)",
        "bbox": (-7.55, 37.95, -4.65, 40.50),
        "pbf_subs": ("extremadura",),
        "landmark_insets": [("BADAJOZ", 38.879, -6.970, 6.5, 3.2), ("CACERES", 39.475, -6.372, 6.0, 3.0)],
    }),
    "castilla-la-mancha": _preset({
        "region_id": "es_castilla_la_mancha",
        "name": "Castilla-La Mancha (Toledo, Albacete & Cuenca)",
        "bbox": (-5.40, 38.00, -1.00, 40.40),
        "pbf_subs": ("castilla-la-mancha",),
        "landmark_insets": [("TOLEDO", 39.863, -4.027, 7.0, 3.5), ("ALBACETE", 38.994, -1.858, 6.5, 3.2)],
    }),
    "valencia-murcia": _preset({
        "region_id": "es_valencia_murcia",
        "name": "Valencia, Alicante, Castellon & Murcia",
        "bbox": (-2.40, 37.40, 0.75, 40.80),
        "pbf_subs": ("valencia", "murcia"),
        "landmark_insets": [("VALENCIA", 39.470, -0.376, 9.0, 4.8), ("ALICANTE_MURCIA", 38.345, -0.491, 8.0, 4.0)],
    }),
    "andalucia-west": _preset({
        "region_id": "es_andalucia_west",
        "name": "Western Andalusia (Seville, Malaga, Cadiz)",
        "bbox": (-7.55, 35.95, -4.10, 38.70),
        "pbf_subs": ("andalucia",),
        "landmark_insets": [("SEVILLA", 37.389, -5.984, 9.5, 5.0), ("MALAGA", 36.721, -4.421, 8.0, 4.2)],
    }),
    "andalucia-east": _preset({
        "region_id": "es_andalucia_east",
        "name": "Eastern Andalusia & Sierra Nevada (Granada, Almeria)",
        "bbox": (-4.40, 36.65, -1.55, 38.75),
        "pbf_subs": ("andalucia",),
        "landmark_insets": [("GRANADA", 37.177, -3.599, 8.0, 4.2), ("SIERRA_NEVADA", 37.055, -3.311, 10.0, 5.5)],
    }),
    "islas-baleares": _preset({
        "region_id": "es_baleares",
        "name": "Balearic Islands (Mallorca, Ibiza, Menorca)",
        "bbox": (1.15, 38.60, 4.35, 40.15),
        "pbf_subs": ("islas-baleares",),
        "landmark_insets": [("PALMA", 39.570, 2.650, 8.0, 4.0)],
    }),
    "canary-islands": _preset({
        "region_id": "es_canary_islands",
        "name": "Canary Islands (Tenerife, Gran Canaria, Lanzarote)",
        "bbox": (-18.20, 27.60, -13.35, 29.35),
        "pbf_subs": ("canary-islands",),
        "landmark_insets": [("TEIDE_TENERIFE", 28.272, -16.642, 9.0, 4.5), ("LAS_PALMAS", 28.124, -15.430, 7.5, 3.8)],
    }),
}

# General-purpose layout for user-drawn bounds. Spain-specific airport/landmark
# insets are deliberately omitted; data layers come from the chosen providers.
CUSTOM_WORLD_PRESET = _preset({
    "region_id": "world_custom",
    "name": "Custom world area",
    "bbox": (-1.0, 40.0, 0.0, 41.0),
    "pbf_subs": (),
    "landmark_insets": [],
    "mid_radius_km": {"large_airport": 9.0, "medium_airport": 7.5, "small_airport": 6.5, "airstrip": 5.5},
    "inset_radius_km": {"large_airport": 3.2, "medium_airport": 2.4, "small_airport": 2.0, "airstrip": 1.6},
    "close_radius_km": {"large_airport": 1.6, "medium_airport": 1.2, "small_airport": 1.0, "airstrip": 0.8},
})

ALL_SPAIN_PRESETS: tuple[str, ...] = (
    "galicia", "asturias-cantabria", "navarra-basque",
    "zaragoza-pyrenees-quick", "aragon-south",
    "catalonia-pyrenees", "catalonia-coast",
    "castilla-y-leon", "madrid-central", "extremadura",
    "castilla-la-mancha", "valencia-murcia",
    "andalucia-west", "andalucia-east", "islas-baleares",
)

# Quality levels scaling imagery/DEM resolution, inset radius & pre-baked shadow/night-light map resolution!
QUALITY_PROFILES: dict[str, dict[str, Any]] = {
    "fast": {
        "desc": "Fast plug-and-play pack (z10-12 cruise, z15 airports, z13+z15 LiDAR, 64px 10-TOD; pairs with online PNOA Z20-14)",
        "cruise_img": (10, 11, 12), "cruise_elev": (10, 11, 12),
        "mid_img": (13,),           "mid_elev": (13,),
        "inset_img": (15,),         "inset_elev": (14,),
        "close_img": (),            "rad_scale": 0.65, "sh_res": 64, "veg_zoom": 13,
    },
    "low": {
        "desc": "Compact iGPU/laptop pack (z10-15 ortho, z10-12 DEM, 64px 7.8m/px shadows)",
        "cruise_img": (10, 11), "cruise_elev": (10, 11),
        "mid_img": (12, 13),    "mid_elev": (12,),
        "inset_img": (14, 15),  "inset_elev": (13,),
        "close_img": (),        "rad_scale": 0.65, "sh_res": 64, "veg_zoom": 13,
    },
    "medium": {
        "desc": "Balanced VFR pack (z10-16 ortho, z10-13 DEM, 96px 5.2m/px 10-TOD shadows)",
        "cruise_img": (10, 11, 12), "cruise_elev": (10, 11, 12),
        "mid_img": (13,),           "mid_elev": (13,),
        "inset_img": (15, 16),      "inset_elev": (14,),
        "close_img": (),            "rad_scale": 0.85, "sh_res": 96, "veg_zoom": 14,
    },
    "high": {
        "desc": "Full HD VFR pack (z10-17 sub-meter runways, 5m MDT05, 128px 3.9m/px 10-TOD + RT Night)",
        "cruise_img": (10, 11, 12), "cruise_elev": (10, 11, 12),
        "mid_img": (13, 14),        "mid_elev": (13,),
        "inset_img": (15, 16),      "inset_elev": (14,),
        "close_img": (17,),         "rad_scale": 1.0, "sh_res": 128, "veg_zoom": 15,
    },
    "ultra": {
        "desc": "Enthusiast pack (z10-13 full cruise, z17 wide runways, 160px 3.1m/px 10-TOD + RT Night)",
        "cruise_img": (10, 11, 12, 13), "cruise_elev": (10, 11, 12),
        "mid_img": (14,),               "mid_elev": (13,),
        "inset_img": (15, 16),          "inset_elev": (14,),
        "close_img": (17,),             "rad_scale": 1.35, "sh_res": 160, "veg_zoom": 15,
    },
}

PACKS_DIR = ROOT / "packs"
REGISTRY_PATH = PACKS_DIR / "registry.json"

_log_lock = threading.Lock()
_thread_local = threading.local()


def log(msg: str) -> None:
    with _log_lock:
        print(msg, flush=True)


def _session() -> requests.Session:
    sess = getattr(_thread_local, "sess", None)
    if sess is None:
        sess = requests.Session()
        sess.headers["User-Agent"] = USER_AGENT
        _thread_local.sess = sess
    return sess


# ---------------------------------------------------------------------------
# Web Mercator XYZ tile math
# ---------------------------------------------------------------------------
def deg2tile(lat_deg: float, lon_deg: float, z: int) -> tuple[int, int]:
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** z
    x = int((lon_deg + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    lim = int(n) - 1
    return max(0, min(lim, x)), max(0, min(lim, y))


def tile_bounds(z: int, x: int, y: int) -> tuple[float, float, float, float]:
    """Return (lon_min, lat_min, lon_max, lat_max) of an XYZ tile."""
    n = 2.0 ** z
    lon_min = x / n * 360.0 - 180.0
    lon_max = (x + 1) / n * 360.0 - 180.0
    lat_max = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n))))
    lat_min = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y + 1) / n))))
    return lon_min, lat_min, lon_max, lat_max


def tile_grid_latlon(z: int, x: int, y: int, size: int = 257) -> tuple[np.ndarray, np.ndarray]:
    """Exact Mercator sample coordinates (lat[size], lon[size]) including right/bottom border seam."""
    n = 2.0 ** z
    u = np.linspace(0.0, 1.0, size, dtype=np.float64)
    lons = (x + u) / n * 360.0 - 180.0
    lats = np.degrees(np.arctan(np.sinh(math.pi * (1.0 - 2.0 * (y + u) / n))))
    return lats, lons


def tiles_in_bbox(bbox: tuple[float, float, float, float], z: int) -> list[tuple[int, int]]:
    x0, y1 = deg2tile(bbox[1], bbox[0], z)
    x1, y0 = deg2tile(bbox[3], bbox[2], z)
    return [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]


# ---------------------------------------------------------------------------
# Fetchers with local disk cache (resumable)
# ---------------------------------------------------------------------------
def fetch_pnoa_tile(z: int, x: int, y: int, cache_dir: Path, rate: float = 0.01) -> bytes | None:
    p = cache_dir / "pnoa" / str(z) / str(x) / f"{y}.jpg"
    if p.exists() and p.stat().st_size > 400:
        return p.read_bytes()
    url = PNOA_WMTS_URL.format(z=z, x=x, y=y)
    for attempt in range(3):
        try:
            if rate:
                time.sleep(rate)
            r = _session().get(url, timeout=20)
            if r.status_code == 200 and len(r.content) > 400 and r.content[:2] == b"\xff\xd8":
                p.parent.mkdir(parents=True, exist_ok=True)
                tmp = p.with_suffix(".jpg.part")
                tmp.write_bytes(r.content)
                tmp.replace(p)
                return r.content
        except Exception:
            pass
        time.sleep(0.3 * (attempt + 1))
    return None


def read_local_xyz_image(z: int, x: int, y: int, source_dir: Path) -> bytes | None:
    """Read one user-provided XYZ raster tile and normalize it to a 256px JPEG.

    Accepted layout is <source_dir>/<z>/<x>/<y>.(jpg|jpeg|png|webp). The user
    remains responsible for the data license; its attribution is recorded in
    the generated manifest.
    """
    for suffix in ("jpg", "jpeg", "png", "webp"):
        p = source_dir / str(z) / str(x) / f"{y}.{suffix}"
        if not p.is_file():
            continue
        try:
            with Image.open(p) as im:
                im = im.convert("RGB")
                if im.size != (256, 256):
                    im = im.resize((256, 256), Image.Resampling.LANCZOS)
                buf = io.BytesIO()
                im.save(buf, format="JPEG", quality=93, optimize=True)
                return buf.getvalue()
        except (OSError, ValueError) as exc:
            raise ValueError(f"Cannot decode local XYZ imagery tile {p}: {exc}") from exc
    return None


def read_local_xyz_terrarium(z: int, x: int, y: int, source_dir: Path) -> np.ndarray | None:
    """Decode a Terrarium RGB XYZ height tile to the builder's 257x257 meter grid."""
    p = source_dir / str(z) / str(x) / f"{y}.png"
    if not p.is_file():
        return None
    try:
        with Image.open(p) as im:
            rgb = np.asarray(im.convert("RGB"), dtype=np.float32)
        height = rgb[..., 0] * 256.0 + rgb[..., 1] + rgb[..., 2] / 256.0 - 32768.0
        return map_coordinates(
            height,
            np.meshgrid(np.linspace(0, height.shape[0] - 1, 257),
                        np.linspace(0, height.shape[1] - 1, 257), indexing="ij"),
            order=1,
            mode="nearest",
        ).astype(np.float32)
    except (OSError, ValueError) as exc:
        raise ValueError(f"Cannot decode local Terrarium elevation tile {p}: {exc}") from exc


def _parse_arcgrid(text: str) -> tuple[np.ndarray, float, float, float, float, float] | None:
    """Parse multipart or raw ESRI ArcGrid ASCII text -> (grid[nrows,ncols], xll, yll, dx, dy, nodata)."""
    lines = text.splitlines()
    start = -1
    for i, line in enumerate(lines[:30]):
        if line.strip().lower().startswith("ncols"):
            start = i
            break
    if start < 0:
        return None
    hdr: dict[str, float] = {}
    idx = start
    while idx < len(lines):
        parts = lines[idx].strip().split()
        if len(parts) == 2 and parts[0].lower() in (
            "ncols", "nrows", "xllcorner", "yllcorner", "xllcenter", "yllcenter",
            "cellsize", "dx", "dy", "nodata_value",
        ):
            hdr[parts[0].lower()] = float(parts[1])
            idx += 1
        else:
            break
    ncols = int(hdr.get("ncols", 0))
    nrows = int(hdr.get("nrows", 0))
    if ncols < 2 or nrows < 2:
        return None
    dx = hdr.get("dx", hdr.get("cellsize", 0.0))
    dy = hdr.get("dy", hdr.get("cellsize", 0.0))
    if dx <= 0.0 or dy <= 0.0:
        return None
    xll = hdr.get("xllcorner", hdr.get("xllcenter", 0.0) - dx * 0.5)
    yll = hdr.get("yllcorner", hdr.get("yllcenter", 0.0) - dy * 0.5)
    nodata = hdr.get("nodata_value", -9999.0)

    # Trim trailing MIME boundary if multipart
    body_lines = []
    for line in lines[idx:]:
        if line.startswith("--"):
            break
        body_lines.append(line)
    vals = np.fromstring(" ".join(body_lines), dtype=np.float32, sep=" ")
    if vals.size < ncols * nrows:
        return None
    grid = vals[: ncols * nrows].reshape((nrows, ncols))
    return grid, xll, yll, dx, dy, nodata


def _fetch_terrarium_grid(z: int, x: int, y: int, cache_dir: Path, size: int = 257) -> np.ndarray | None:
    p = cache_dir / "terrarium" / str(z) / str(x) / f"{y}.png"
    raw: bytes | None = None
    if p.exists() and p.stat().st_size > 200:
        raw = p.read_bytes()
    else:
        url = TERRARIUM_URL.format(z=z, x=x, y=y)
        for attempt in range(3):
            try:
                r = _session().get(url, timeout=20)
                if r.status_code == 200 and len(r.content) > 200:
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(r.content)
                    raw = r.content
                    break
            except Exception:
                pass
            time.sleep(0.3 * (attempt + 1))
    if raw is None:
        return None
    rgb = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"), dtype=np.float32)
    h256 = rgb[:, :, 0] * 256.0 + rgb[:, :, 1] + rgb[:, :, 2] / 256.0 - 32768.0
    coords_y = np.linspace(0.0, 255.0, size)
    coords_x = np.linspace(0.0, 255.0, size)
    yy, xx = np.meshgrid(coords_y, coords_x, indexing="ij")
    return map_coordinates(h256, [yy, xx], order=1, mode="nearest").astype(np.float32)


def fetch_elevation_tile(
    z: int, x: int, y: int, cache_dir: Path, use_mdt05: bool, rate: float = 0.015
) -> np.ndarray | None:
    """Fetch 257x257 float32 height grid in meters from IGN WCS (MDT05 or MDT25), with Terrarium border fallback."""
    cov = "Elevacion4258_5" if use_mdt05 else "Elevacion4258_25"
    p = cache_dir / f"ign_{cov}" / str(z) / str(x) / f"{y}.npy"
    if p.exists() and p.stat().st_size > 1000:
        return np.load(p)

    lon0, lat0, lon1, lat1 = tile_bounds(z, x, y)
    if not (
        PNOA_COVERAGE_BBOX[0] <= lon0 and lon1 <= PNOA_COVERAGE_BBOX[2]
        and PNOA_COVERAGE_BBOX[1] <= lat0 and lat1 <= PNOA_COVERAGE_BBOX[3]
    ):
        # Outside the IGN footprint, skip three futile WCS retries per tile.
        grid_out = _fetch_terrarium_grid(z, x, y, cache_dir, 257)
        if grid_out is not None:
            p.parent.mkdir(parents=True, exist_ok=True)
            tmp = p.with_suffix(".tmp.npy")
            np.save(tmp, grid_out)
            tmp.replace(p)
        return grid_out
    # Pad query slightly so edge samples interpolate cleanly without border clamp.
    pad_lon = (lon1 - lon0) * 0.03 + 0.0005
    pad_lat = (lat1 - lat0) * 0.03 + 0.0005

    grid_out: np.ndarray | None = None
    # IGN WCS caps single-request pixel dimensions; at z<=11 Elevacion4258_25 is ~1500x1100 px (well within limit),
    # at z>=13 Elevacion4258_5 is ~800x600 px (fast and sharp!).
    params = [
        ("SERVICE", "WCS"),
        ("VERSION", "2.0.1"),
        ("REQUEST", "GetCoverage"),
        ("COVERAGEID", cov),
        ("FORMAT", "ArcGrid"),
        ("SUBSET", f"Long({lon0 - pad_lon:.6f},{lon1 + pad_lon:.6f})"),
        ("SUBSET", f"Lat({lat0 - pad_lat:.6f},{lat1 + pad_lat:.6f})"),
        ("SCALESIZE", "Long(264),Lat(264)"),
    ]
    for attempt in range(3):
        try:
            if rate:
                time.sleep(rate)
            r = _session().get(IGN_WCS_URL, params=params, timeout=35)
            if r.status_code == 200 and b"ncols" in r.content[:600].lower():
                parsed = _parse_arcgrid(r.text)
                if parsed is not None:
                    src, xll, yll, dx, dy, nodata = parsed
                    nrows, ncols = src.shape
                    # ArcGrid row 0 is North (yll + nrows*dy), row nrows-1 is South (yll).
                    lats, lons = tile_grid_latlon(z, x, y, 257)
                    col_f = (lons - xll) / dx - 0.5
                    row_f = (yll + nrows * dy - lats) / dy - 0.5
                    rr, cc = np.meshgrid(row_f, col_f, indexing="ij")

                    invalid = (src <= nodata + 1.0) | (src < -500.0) | (src > 5000.0)
                    if np.all(invalid):
                        break  # Outside Spanish IGN coverage -> fall back to Terrarium
                    if np.any(invalid):
                        # Fill partial French border nodata with Terrarium before interpolating.
                        terr = _fetch_terrarium_grid(z, x, y, cache_dir, 257)
                        valid_src = np.where(invalid, np.nan, src)
                        # Nearest valid fallback if Terrarium unavailable
                        fill_val = float(np.nanmedian(valid_src))
                        clean_src = np.where(invalid, fill_val, src)
                        sampled = map_coordinates(clean_src, [rr, cc], order=1, mode="nearest").astype(np.float32)
                        mask_sampled = map_coordinates(invalid.astype(np.float32), [rr, cc], order=1, mode="nearest")
                        if terr is not None:
                            grid_out = np.where(mask_sampled > 0.25, terr, sampled).astype(np.float32)
                        else:
                            grid_out = sampled
                    else:
                        grid_out = map_coordinates(src, [rr, cc], order=1, mode="nearest").astype(np.float32)
                    break
        except Exception:
            pass
        time.sleep(0.4 * (attempt + 1))

    if grid_out is None:
        grid_out = _fetch_terrarium_grid(z, x, y, cache_dir, 257)

    if grid_out is not None:
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(".tmp.npy")
        np.save(tmp, grid_out)
        tmp.replace(p)
    return grid_out


# ---------------------------------------------------------------------------
# Slope-preserving runway corridor smoothing
# ---------------------------------------------------------------------------
def _sample_height_from_cache(
    lat: float, lon: float, elev_tiles: dict[tuple[int, int, int], np.ndarray], zooms_desc: list[int]
) -> float | None:
    for z in zooms_desc:
        n = 2.0 ** z
        xt = (lon + 180.0) / 360.0 * n
        yt = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
        tx, ty = int(math.floor(xt)), int(math.floor(yt))
        arr = elev_tiles.get((z, tx, ty))
        if arr is None:
            continue
        fx = max(0.0, min(1.0, xt - tx)) * (arr.shape[1] - 1)
        fy = max(0.0, min(1.0, yt - ty)) * (arr.shape[0] - 1)
        x0, y0 = int(math.floor(fx)), int(math.floor(fy))
        x1, y1 = min(arr.shape[1] - 1, x0 + 1), min(arr.shape[0] - 1, y0 + 1)
        dx, dy = fx - x0, fy - y0
        a = arr[y0, x0] * (1.0 - dx) + arr[y0, x1] * dx
        b = arr[y1, x0] * (1.0 - dx) + arr[y1, x1] * dx
        return float(a * (1.0 - dy) + b * dy)
    return None


def smooth_runways_in_dem(
    airports: list[dict[str, Any]],
    elev_tiles: dict[tuple[int, int, int], np.ndarray],
    zooms_desc: list[int],
) -> int:
    """Compute smooth slope-preserving runway profiles and bake them cleanly into DEM tiles."""
    smoothed_runways = 0
    for ap in airports:
        ap_elevs: list[float] = []
        for rw in ap["runways"]:
            lat_a, lon_a = rw["lat_a"], rw["lon_a"]
            lat_b, lon_b = rw["lat_b"], rw["lon_b"]
            length_m = max(100.0, haversine_m(lat_a, lon_a, lat_b, lon_b))
            width_m = float(rw.get("width_m", 20.0))

            # Sample 33 centerline stations from highest available DEM.
            n_st = 33
            raw_prof = np.zeros(n_st, dtype=np.float64)
            valid = 0
            for k in range(n_st):
                t = k / (n_st - 1)
                lat = lat_a + (lat_b - lat_a) * t
                lon = lon_a + (lon_b - lon_a) * t
                h = _sample_height_from_cache(lat, lon, elev_tiles, zooms_desc)
                if h is not None:
                    raw_prof[k] = h
                    valid += 1
                else:
                    raw_prof[k] = ap.get("elev_m", 0.0)
            if valid < 4:
                continue

            # Fit a smooth quadratic trend blended with Gaussian smoothing: preserves real mountain
            # runway grades/crests while suppressing sharp DEM ditches or culvert artifacts.
            t_grid = np.linspace(0.0, 1.0, n_st)
            poly_prof = np.polyval(np.polyfit(t_grid, raw_prof, 2), t_grid)
            gauss_prof = gaussian_filter1d(raw_prof, sigma=3.5, mode="nearest")
            blend = 0.80 if rw.get("surface") in ("asphalt", "concrete") else 0.70
            smooth_prof = poly_prof * blend + gauss_prof * (1.0 - blend)

            # Store 17 evenly spaced profile elevations for exact runway mesh placement.
            rw["profile"] = [round(float(smooth_prof[i * 2]), 2) for i in range(17)]
            rw["elev_a"] = rw["profile"][0]
            rw["elev_b"] = rw["profile"][-1]
            ap_elevs.append(float(np.mean(smooth_prof)))

            # Project DEM samples inside runway + feathered shoulder onto this profile.
            mid_lat = 0.5 * (lat_a + lat_b)
            m_lat = 111132.0
            m_lon = 111320.0 * math.cos(math.radians(mid_lat))
            ax, ay = lon_a * m_lon, lat_a * m_lat
            bx, by = lon_b * m_lon, lat_b * m_lat
            dx, dy = bx - ax, by - ay
            L = math.hypot(dx, dy)
            if L < 50.0:
                continue
            ux, uy = dx / L, dy / L
            nx, ny = -uy, ux

            half_w = width_m * 0.5 + 8.0
            shoulder = max(45.0, width_m * 1.6)
            end_pad = 65.0
            pad_deg_lat = (half_w + shoulder + end_pad) / m_lat
            pad_deg_lon = (half_w + shoulder + end_pad) / m_lon
            rw_bbox = (
                min(lon_a, lon_b) - pad_deg_lon,
                min(lat_a, lat_b) - pad_deg_lat,
                max(lon_a, lon_b) + pad_deg_lon,
                max(lat_a, lat_b) + pad_deg_lat,
            )

            for z in zooms_desc:
                for tx, ty in tiles_in_bbox(rw_bbox, z):
                    arr = elev_tiles.get((z, tx, ty))
                    if arr is None:
                        continue
                    lats, lons = tile_grid_latlon(z, tx, ty, arr.shape[0])
                    px = lons[None, :] * m_lon - ax
                    py = lats[:, None] * m_lat - ay
                    along = px * ux + py * uy
                    across = np.abs(px * nx + py * ny)

                    mask = (along >= -end_pad) & (along <= L + end_pad) & (across <= half_w + shoulder)
                    if not np.any(mask):
                        continue

                    t_clamped = np.clip(along[mask] / L, 0.0, 1.0)
                    target_h = np.interp(t_clamped, np.linspace(0.0, 1.0, n_st), smooth_prof)

                    # Weight = 1 inside runway width, smoothstep to 0 over shoulder & ends.
                    w_lat = np.clip(1.0 - (across[mask] - half_w) / shoulder, 0.0, 1.0)
                    w_lat = w_lat * w_lat * (3.0 - 2.0 * w_lat)
                    over_end = np.maximum(0.0, np.maximum(-along[mask], along[mask] - L))
                    w_end = np.clip(1.0 - over_end / end_pad, 0.0, 1.0)
                    w_end = w_end * w_end * (3.0 - 2.0 * w_end)
                    w = w_lat * w_end

                    arr[mask] = (arr[mask] * (1.0 - w) + target_h * w).astype(np.float32)

            smoothed_runways += 1

        if ap_elevs:
            ap["elev_m"] = round(float(np.mean(ap_elevs)), 2)
    return smoothed_runways


# ---------------------------------------------------------------------------
# Surface / Forest / Water heuristic mask (128x128 PNG)
# shortcut: estimates forest canopy from PNOA RGB + local canopy texture variance + DEM slope/elevation;
# upgrade to raw IGN LiDAR canopy height model + SIOSE polygons in Milestone 2.
# ---------------------------------------------------------------------------
def build_surface_mask(jpeg_bytes: bytes, elev_tile: np.ndarray | None) -> bytes:
    from scipy.ndimage import uniform_filter

    im = Image.open(io.BytesIO(jpeg_bytes)).convert("RGB").resize((128, 128), Image.BILINEAR)
    rgb = np.asarray(im, dtype=np.float32) / 255.0
    r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
    luma = 0.299 * r + 0.587 * g + 0.114 * b

    # Real Spanish pine/holm-oak forests are darker & greener than bare soil, AND have rougher
    # pixel-to-pixel crown texture variance than smooth irrigated barley/alfalfa crop fields.
    mean_l = uniform_filter(luma, size=5)
    local_std = np.sqrt(np.maximum(0.0, uniform_filter((luma - mean_l) ** 2, size=5)))

    # Greenness + darkness score tuned for Iberian conifers (dark green-brown) & valley broadleaf
    green_rel = (g - 0.5 * (r + b)) + 0.35 * (g - b)
    dark_canopy = np.clip((0.40 - luma) / 0.24, 0.0, 1.0)
    textured = np.clip((local_std - 0.015) / 0.065, 0.15, 1.0)
    # Forests: dark greenish/olive textured patches OR rich green textured hedgerows/groves
    forest = np.clip(
        (dark_canopy * 0.85 + np.clip(green_rel * 4.5, 0.0, 0.85)) * textured
        - np.clip((luma - 0.46) * 3.5, 0.0, 1.0),
        0.0,
        1.0,
    )
    # Exclude bright pale tan/sand/concrete surfaces strictly
    forest *= np.clip((0.52 - luma) / 0.14, 0.0, 1.0)
    # Dark blue/teal -> water proxy (lakes, reservoirs, rivers)
    water = np.clip(((b - r) * 4.0 + (0.28 - luma) * 2.0) * (b > r + 0.03), 0.0, 1.0)
    forest *= (1.0 - water)

    # Surface class channel G: 0=fields/grass, 85=forest/scrub, 170=rock/cliff, 255=snow
    sclass = np.zeros((128, 128), dtype=np.uint8)
    sclass[forest > 0.28] = 85
    if elev_tile is not None:
        h128 = elev_tile[::2, ::2][:128, :128]
        gy, gx = np.gradient(h128)
        slope = np.hypot(gx, gy)
        # No trees on steep rock faces or above alpine treeline (>2150m)
        forest *= np.clip((2200.0 - h128) / 250.0, 0.0, 1.0)
        forest *= np.clip((18.0 - slope) / 8.0, 0.0, 1.0)
        sclass[slope > 9.0] = 170
        sclass[(h128 > 2100.0) & (luma > 0.58)] = 255
    else:
        sclass[(luma > 0.72) & (np.abs(r - b) < 0.06)] = 255

    out = np.stack([
        np.clip(forest * 255.0, 0, 255).astype(np.uint8),
        sclass,
        np.clip(water * 255.0, 0, 255).astype(np.uint8),
    ], axis=-1)
    buf = io.BytesIO()
    Image.fromarray(out, mode="RGB").save(buf, format="PNG", optimize=True)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Main build pipeline
# ---------------------------------------------------------------------------
def resolve_build_plan(args: argparse.Namespace) -> dict[str, Any]:
    preset_key = getattr(args, "preset", None) or "zaragoza-pyrenees-quick"
    if preset_key == "custom-world":
        preset = CUSTOM_WORLD_PRESET
        if not getattr(args, "bbox", None):
            raise SystemExit("The custom-world preset requires --bbox.")
    else:
        preset = PRESETS.get(preset_key)
        if preset is None and preset_key != "all-spain":
            raise SystemExit(f"Unknown preset '{preset_key}'.")
        if preset_key == "all-spain":
            preset = PRESETS["zaragoza-pyrenees-quick"]
    quality = getattr(args, "quality", "high") or "high"
    qp = QUALITY_PROFILES[quality]

    bbox = tuple(float(v) for v in args.bbox) if getattr(args, "bbox", None) else tuple(preset["bbox"])
    if len(bbox) != 4 or bbox[0] >= bbox[2] or bbox[1] >= bbox[3]:
        raise SystemExit(f"Invalid bbox (expected lon_min lat_min lon_max lat_max with min < max): {bbox}")
    if bbox[0] < -180.0 or bbox[2] > 180.0 or bbox[1] < -85.0511 or bbox[3] > 85.0511:
        raise SystemExit("Bounds must fit the supported Web Mercator domain: longitude [-180, 180], latitude [-85.0511, 85.0511]. Split antimeridian-crossing areas into two builds.")
    custom_id = "world_" + hashlib.sha256(",".join(f"{v:.6f}" for v in bbox).encode("ascii")).hexdigest()[:12]
    region_id = getattr(args, "region_id", None) or (preset["region_id"] if not getattr(args, "bbox", None) else custom_id)
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", region_id):
        raise SystemExit("Region ID must be 1–64 letters, numbers, underscores or hyphens.")
    name = getattr(args, "name", None) or (preset["name"] if not getattr(args, "bbox", None) else f"Custom Region ({region_id})")
    out_path = Path(args.out).resolve() if getattr(args, "out", None) else (PACKS_DIR / f"{region_id}.nspack")

    cruise_img_z = tuple(int(v) for v in args.cruise_img_zooms.split(",")) if getattr(args, "cruise_img_zooms", None) else qp["cruise_img"]
    cruise_elev_z = tuple(int(v) for v in args.cruise_elev_zooms.split(",")) if getattr(args, "cruise_elev_zooms", None) else qp["cruise_elev"]
    mid_img_z = qp["mid_img"]
    mid_elev_z = qp["mid_elev"]
    inset_img_z = tuple(int(v) for v in args.inset_img_zooms.split(",")) if getattr(args, "inset_img_zooms", None) else qp["inset_img"]
    inset_elev_z = tuple(int(v) for v in args.inset_elev_zooms.split(",")) if getattr(args, "inset_elev_zooms", None) else qp["inset_elev"]
    close_img_z = qp["close_img"]
    rad_scale = float(qp["rad_scale"])
    imagery_source = getattr(args, "imagery_source", "pnoa")
    elevation_source = getattr(args, "elevation_source", "ign+terrarium")
    if imagery_source == "pnoa" and not (
        PNOA_COVERAGE_BBOX[0] <= bbox[0] and bbox[2] <= PNOA_COVERAGE_BBOX[2]
        and PNOA_COVERAGE_BBOX[1] <= bbox[1] and bbox[3] <= PNOA_COVERAGE_BBOX[3]
    ):
        raise SystemExit(
            "PNOA imagery is limited to Spain. For custom bounds choose --imagery-source local-xyz "
            "with licensed XYZ tiles, or --imagery-source none for elevation-only terrain."
        )
    if imagery_source == "local-xyz" and not getattr(args, "imagery_dir", None):
        raise SystemExit("--imagery-source local-xyz requires --imagery-dir.")
    if imagery_source == "local-xyz" and not getattr(args, "imagery_attribution", None):
        raise SystemExit("Local imagery requires --imagery-attribution so its origin and license are carried into the region pack.")
    if elevation_source == "local-terrarium" and not getattr(args, "elevation_dir", None):
        raise SystemExit("--elevation-source local-terrarium requires --elevation-dir.")
    if elevation_source == "local-terrarium" and not getattr(args, "elevation_attribution", None):
        raise SystemExit("Local elevation requires --elevation-attribution so its origin and license are carried into the region pack.")
    imagery_dir = getattr(args, "imagery_dir", None)
    if imagery_source == "local-xyz" and not Path(imagery_dir).is_dir():
        raise SystemExit(f"Local imagery directory does not exist: {imagery_dir}")
    elevation_dir = getattr(args, "elevation_dir", None)
    if elevation_source == "local-terrarium" and not Path(elevation_dir).is_dir():
        raise SystemExit(f"Local elevation directory does not exist: {elevation_dir}")
    pbf = getattr(args, "pbf", None)
    if pbf and not Path(pbf).is_file():
        raise SystemExit(f"OSM PBF file does not exist: {pbf}")
    return {
        "preset": preset, "preset_key": preset_key, "quality": quality, "qp": qp,
        "bbox": bbox, "region_id": region_id, "name": name, "out_path": out_path,
        "cruise_img_z": cruise_img_z, "cruise_elev_z": cruise_elev_z,
        "mid_img_z": mid_img_z, "mid_elev_z": mid_elev_z,
        "inset_img_z": inset_img_z, "inset_elev_z": inset_elev_z,
        "close_img_z": close_img_z, "rad_scale": rad_scale,
        "imagery_source": imagery_source,
        "elevation_source": elevation_source,
    }


def _resolve_pbfs(
    bbox: tuple[float, float, float, float], preset: dict[str, Any], pbf_arg: str | None, cache_dir: Path
) -> list[Path]:
    if pbf_arg:
        return [Path(pbf_arg).resolve()]
    osm_dir = cache_dir / "osm"
    osm_dir.mkdir(parents=True, exist_ok=True)
    spain_pbf = osm_dir / "spain-latest.osm.pbf"
    if spain_pbf.exists() and spain_pbf.stat().st_size > 100_000_000:
        return [spain_pbf]

    subs = list(preset.get("pbf_subs") or [])
    if not subs or bbox != tuple(preset.get("bbox", ())):
        lon0, lat0, lon1, lat1 = bbox
        subs = [
            s for s, (s0, a0, s1, a1) in SPAIN_OSM_BOXES.items()
            if s1 >= lon0 and s0 <= lon1 and a1 >= lat0 and a0 <= lat1
        ]
    if not subs:
        return []

    out: list[Path] = []
    for sub in subs:
        legacy = osm_dir / f"{sub}.osm.pbf"
        if legacy.exists() and legacy.stat().st_size > 1_000_000:
            out.append(legacy)
            continue
        dst = osm_dir / f"{sub}-latest.osm.pbf"
        if dst.exists() and dst.stat().st_size > 1_000_000:
            out.append(dst)
            continue
        url = GEOFABRIK_CANARY_URL if sub == "canary-islands" else GEOFABRIK_SPAIN_URL.format(sub=sub)
        log(f"[OSM] Downloading Geofabrik PBF for '{sub}' -> {dst.name}...")
        tmp = dst.with_suffix(".pbf.part")
        with _session().get(url, stream=True, timeout=120) as r:
            r.raise_for_status()
            with tmp.open("wb") as f:
                for chunk in r.iter_content(chunk_size=1024 * 1024):
                    if chunk:
                        f.write(chunk)
        tmp.replace(dst)
        out.append(dst)
    return out


def build_region(args: argparse.Namespace) -> int:
    if getattr(args, "preset", None) == "all-spain":
        installed = set() if getattr(args, "force", False) else set(_discover_installed_regions().keys())
        log(f"=== ALL SPAIN MODULAR BUILD ({len(ALL_SPAIN_PRESETS)} regions) ===")
        rc_all = 0
        for pk in ALL_SPAIN_PRESETS:
            rid = PRESETS[pk]["region_id"]
            if rid in installed and not getattr(args, "dry_run", False):
                log(f"[SKIP] {pk} ({rid}) already installed (pass --force to rebuild)")
                continue
            sub_args = argparse.Namespace(**vars(args))
            sub_args.preset = pk
            sub_args.bbox = None
            sub_args.region_id = None
            sub_args.name = None
            sub_args.out = None
            rc = build_region(sub_args)
            if rc != 0:
                rc_all = rc
        return rc_all

    plan = resolve_build_plan(args)
    preset = plan["preset"]
    bbox = plan["bbox"]
    region_id = plan["region_id"]
    name = plan["name"]
    quality = plan["quality"]
    cache_dir = Path(args.cache).resolve()
    out_path: Path = plan["out_path"]
    cruise_img_z, cruise_elev_z = plan["cruise_img_z"], plan["cruise_elev_z"]
    mid_img_z, mid_elev_z = plan["mid_img_z"], plan["mid_elev_z"]
    inset_img_z, inset_elev_z = plan["inset_img_z"], plan["inset_elev_z"]
    close_img_z, rad_scale = plan["close_img_z"], plan["rad_scale"]

    wants_osm_bakes = getattr(args, "bake_scenery", False) or getattr(args, "bake_cities", False)
    if wants_osm_bakes and not getattr(args, "pbf", None):
        lon0, lat0, lon1, lat1 = bbox
        within_spain_sources = (-18.2 <= lon0 and lon1 <= 4.35 and 27.6 <= lat0 and lat1 <= 43.8)
        if not within_spain_sources:
            raise SystemExit("OSM scenery/city baking outside the bundled Spain extracts requires --pbf with a local, licensed OSM extract.")

    log("=" * 72)
    log(f"  WORLD REGION PACKAGE MANAGER  --  {name} [{quality.upper()}]")
    log("=" * 72)
    log(f"  region_id    : {region_id}")
    log(f"  bbox         : {bbox}")
    log(f"  output       : {out_path}")
    log(f"  sources      : imagery={plan['imagery_source']} elevation={plan['elevation_source']}")
    log(f"  cruise zooms : img={cruise_img_z}  elev={cruise_elev_z}")
    log(f"  mid zooms    : img={mid_img_z}  elev={mid_elev_z}")
    log(f"  inset zooms  : img={inset_img_z}  elev={inset_elev_z}  close_img={close_img_z}  rad_x={rad_scale:.2f}")
    if getattr(args, "dry_run", False):
        est_cruise = sum(len(tiles_in_bbox(bbox, z)) for z in cruise_img_z)
        will_cities = getattr(args, "full", False) or getattr(args, "bake_cities", False)
        log(f"  [DRY-RUN] Cruise imagery tiles={est_cruise:,} | source={plan['imagery_source']} | shadow_res={plan['qp']['sh_res']}px | bake_cities={will_cities} | night_rt={not getattr(args, 'no_night_lights', False)}")
        return 0

    t0 = time.time()
    log("[1/5] Fetching airports & airstrips (OurAirports + OSM)...")
    airports = merge_airports(bbox, cache_dir, use_osm=not args.no_osm)
    kinds = {}
    for a in airports:
        kinds[a["kind"]] = kinds.get(a["kind"], 0) + 1
    log(f"  Found {len(airports)} airfields with runways: {kinds}")
    for a in airports[:12]:
        r0 = a["runways"][0]
        log(f"    - {a['id']:8s} [{a['kind']:14s}] {a['name'][:36]:36s} {r0['length_m']:.0f}m {r0['surface']}")

    # Collect required tile coordinates: full-bbox cruise tiles + high-res airport/airstrip insets.
    img_jobs: set[tuple[int, int, int]] = set()
    elev_jobs: set[tuple[int, int, int]] = set()
    inset_bboxes: list[dict[str, Any]] = []

    for z in cruise_img_z:
        for x, y in tiles_in_bbox(bbox, z):
            img_jobs.add((z, x, y))
    for z in cruise_elev_z:
        for x, y in tiles_in_bbox(bbox, z):
            elev_jobs.add((z, x, y))

    def _ap_bbox(ap: dict[str, Any], rad_km: float) -> tuple[float, float, float, float]:
        d_lat = rad_km / 111.132
        d_lon = rad_km / (111.320 * math.cos(math.radians(ap["lat"])))
        lons = [ap["lon"]] + [r["lon_a"] for r in ap["runways"]] + [r["lon_b"] for r in ap["runways"]]
        lats = [ap["lat"]] + [r["lat_a"] for r in ap["runways"]] + [r["lat_b"] for r in ap["runways"]]
        return (
            max(bbox[0], min(lons) - d_lon),
            max(bbox[1], min(lats) - d_lat),
            min(bbox[2], max(lons) + d_lon),
            min(bbox[3], max(lats) + d_lat),
        )

    for ap in airports:
        # Mid-range valley & mountain ring (z13-z14 PNOA + z13 5m MDT05)
        if mid_img_z or mid_elev_z:
            mb = _ap_bbox(ap, preset.get("mid_radius_km", {}).get(ap["kind"], 6.0) * rad_scale)
            for z in mid_img_z:
                for x, y in tiles_in_bbox(mb, z):
                    img_jobs.add((z, x, y))
            for z in mid_elev_z:
                for x, y in tiles_in_bbox(mb, z):
                    elev_jobs.add((z, x, y))

        # High-res approach inset (z15-z16 PNOA + z14 5m MDT05)
        ib = _ap_bbox(ap, preset["inset_radius_km"].get(ap["kind"], 2.0) * rad_scale)
        inset_bboxes.append({"id": ap["id"], "bbox": [round(v, 5) for v in ib]})
        for z in inset_img_z:
            for x, y in tiles_in_bbox(ib, z):
                img_jobs.add((z, x, y))
        for z in inset_elev_z:
            for x, y in tiles_in_bbox(ib, z):
                elev_jobs.add((z, x, y))

        # Sub-meter close runway ring (z17 ~0.88 m/px IGN PNOA)
        if close_img_z:
            cb = _ap_bbox(ap, preset.get("close_radius_km", {}).get(ap["kind"], 0.8) * rad_scale)
            for z in close_img_z:
                for x, y in tiles_in_bbox(cb, z):
                    img_jobs.add((z, x, y))

    # City & Pyrenees mountain massif insets (5m MDT05 z14 DEM + z14-15 PNOA imagery!)
    for lid, llat, llon, mrad, hrad in preset.get("landmark_insets", []):
        for rad_km, izs, ezs in ((mrad, mid_img_z, mid_elev_z), (hrad, (15,), inset_elev_z)):
            d_lat = rad_km / 111.132
            d_lon = rad_km / (111.320 * math.cos(math.radians(llat)))
            lb = (max(bbox[0], llon - d_lon), max(bbox[1], llat - d_lat), min(bbox[2], llon + d_lon), min(bbox[3], llat + d_lat))
            for z in izs:
                for x, y in tiles_in_bbox(lb, z):
                    img_jobs.add((z, x, y))
            for z in ezs:
                for x, y in tiles_in_bbox(lb, z):
                    elev_jobs.add((z, x, y))

    if plan["imagery_source"] == "none":
        img_jobs.clear()
    elev_label = {
        "ign+terrarium": "IGN MDT with Terrarium fallback",
        "terrarium": "Terrarium RGB",
        "local-terrarium": "imported Terrarium XYZ",
    }[plan["elevation_source"]]
    log(f"[2/5] Loading {elev_label} elevation ({len(elev_jobs)} tiles)...")
    elev_tiles: dict[tuple[int, int, int], np.ndarray] = {}
    elev_fail = 0
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        def elevation_job(z: int, x: int, y: int) -> np.ndarray | None:
            if plan["elevation_source"] == "local-terrarium":
                return read_local_xyz_terrarium(z, x, y, Path(args.elevation_dir))
            if plan["elevation_source"] == "terrarium":
                return _fetch_terrarium_grid(z, x, y, cache_dir, 257)
            if quality == "fast" and z <= 13:
                # Use already-cached IGN MDT if present, else fast CDN Terrarium for cruise/mid z10-13
                # while keeping 5 m IGN MDT05 for z14 airport & runway insets (~15x faster!).
                cov = "Elevacion4258_5" if z >= 13 else "Elevacion4258_25"
                cached = cache_dir / f"ign_{cov}" / str(z) / str(x) / f"{y}.npy"
                if cached.exists() and cached.stat().st_size > 1000:
                    return np.load(cached)
                return _fetch_terrarium_grid(z, x, y, cache_dir, 257)
            return fetch_elevation_tile(z, x, y, cache_dir, z >= 13, args.rate)

        futs = {pool.submit(elevation_job, z, x, y): (z, x, y) for (z, x, y) in sorted(elev_jobs)}
        done = 0
        for fut in as_completed(futs):
            key = futs[fut]
            arr = fut.result()
            if arr is not None:
                elev_tiles[key] = arr
            else:
                elev_fail += 1
            done += 1
            if done % 25 == 0 or done == len(futs):
                log(f"  elev {done}/{len(futs)} (ok={len(elev_tiles)} fail={elev_fail})")

    log("[3/5] Baking slope-preserving runway profiles into DEM...")
    all_elev_z = sorted({z for (z, _, _) in elev_tiles.keys()}, reverse=True)
    n_smooth = smooth_runways_in_dem(airports, elev_tiles, all_elev_z)
    log(f"  Smoothed {n_smooth} runways across {len(airports)} airfields (true mountain slopes preserved).")

    imagery_label = {"pnoa": "IGN PNOA orthophoto", "local-xyz": "imported XYZ orthophoto", "none": "terrain-only"}[plan["imagery_source"]]
    log(f"[4/5] Loading {imagery_label} ({len(img_jobs)} tiles)...")
    img_tiles: dict[tuple[int, int, int], bytes] = {}
    img_fail = 0
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        def imagery_job(z: int, x: int, y: int) -> bytes | None:
            if plan["imagery_source"] == "local-xyz":
                return read_local_xyz_image(z, x, y, Path(args.imagery_dir))
            return fetch_pnoa_tile(z, x, y, cache_dir, args.rate)

        futs = {pool.submit(imagery_job, z, x, y): (z, x, y) for (z, x, y) in sorted(img_jobs)}
        done = 0
        for fut in as_completed(futs):
            key = futs[fut]
            data = fut.result()
            if data is not None:
                img_tiles[key] = data
            else:
                img_fail += 1
            done += 1
            if done % 50 == 0 or done == len(futs):
                log(f"  pnoa {done}/{len(futs)} (ok={len(img_tiles)} fail={img_fail})")
    if img_jobs:
        coverage = len(img_tiles) / len(img_jobs)
        log(f"  imagery coverage: {coverage:.1%} ({len(img_tiles)}/{len(img_jobs)} requested tiles)")

    log("[5/5] Encoding tiles, generating surface/forest masks, and writing .nspack...")
    records: list[TileRecord] = []
    for (z, x, y), arr in elev_tiles.items():
        payload, enc = encode_height_tile(arr, compress=True)
        records.append(TileRecord(LAYER_ELEVATION, z, x, y, arr.shape[1], arr.shape[0], enc, payload))

    # Build surface/forest masks for cruise max zoom, mid valley zoom, and high-res approach zoom
    mask_zooms = ({max(cruise_img_z), *(mid_img_z[-1:] if mid_img_z else ()), min(inset_img_z)}
                  if img_tiles else set())
    for (z, x, y), jpg in img_tiles.items():
        records.append(TileRecord(LAYER_IMAGERY, z, x, y, 256, 256, ENC_JPEG, jpg))
        if z in mask_zooms:
            # Find matching or parent elevation tile for slope/snow estimate
            et = elev_tiles.get((z, x, y))
            if et is None:
                et = elev_tiles.get((z - 1, x >> 1, y >> 1))
            mask_png = build_surface_mask(jpg, et)
            records.append(TileRecord(LAYER_SURFACE, z, x, y, 128, 128, ENC_PNG, mask_png))

    if not elev_tiles:
        log("[ERROR] No elevation tiles were produced; refusing to install an empty region pack.")
        return 1
    if plan["imagery_source"] != "none" and not img_tiles:
        log("[ERROR] No imagery tiles matched this region. Choose terrain-only or check the licensed XYZ source and bounds.")
        return 1

    imagery_attribution = (
        getattr(args, "imagery_attribution", None)
        or ("PNOA cedido por © Instituto Geografico Nacional de Espana (CC-BY 4.0)" if plan["imagery_source"] == "pnoa"
            else "No imagery layer included" if plan["imagery_source"] == "none"
            else "User-imported XYZ imagery; source attribution must be reviewed before redistribution")
    )
    elevation_attribution = (
        getattr(args, "elevation_attribution", None)
        or ("MDT05/MDT25 © Instituto Geografico Nacional de Espana (CC-BY 4.0); border fallback AWS Terrarium"
            if plan["elevation_source"] == "ign+terrarium"
            else "AWS Terrarium RGB elevation"
            if plan["elevation_source"] == "terrarium"
            else "User-imported Terrarium XYZ elevation; source attribution must be reviewed before redistribution")
    )
    # Keep the OSM boundary rings used during ingestion out of the runtime manifest.
    manifest_airports = [
        {key: value for key, value in airport.items() if key != "boundary_polygons"}
        for airport in airports
    ]
    manifest = {
        "pack_version": 2,
        "region_id": region_id,
        "name": name,
        "bbox": list(bbox),
        "projection": "EPSG:3857 / Web Mercator XYZ",
        "sources": {"imagery": plan["imagery_source"], "elevation": plan["elevation_source"]},
        "elevation_zooms": sorted({z for (z, _, _) in elev_tiles.keys()}),
        "imagery_zooms": sorted({z for (z, _, _) in img_tiles.keys()}),
        "surface_zooms": sorted(mask_zooms),
        "high_res_insets": inset_bboxes,
        "airports": manifest_airports,
        "attribution": {
            "imagery": imagery_attribution,
            "elevation": elevation_attribution,
            "airports": "OurAirports (Public Domain), OpenStreetMap contributors (ODbL), ENAIRE AIP",
        },
        "counts": {
            "elevation_tiles": len(elev_tiles),
            "imagery_tiles": len(img_tiles),
            "imagery_job_tiles": len(img_jobs),
            "imagery_coverage": round(len(img_tiles) / len(img_jobs), 4) if img_jobs else 0.0,
            "airports": len(airports),
            "runways": sum(len(a["runways"]) for a in airports),
        },
    }

    write_pack(out_path, region_id, bbox, records, manifest)
    if not args.no_osm:
        try:
            from builder.water_mask import patch_pack_with_osm_water
            patch_pack_with_osm_water(out_path, cache_dir)
        except Exception as exc:
            log(f"  [warn] OSM water bake skipped: {exc}")
    size_mb = out_path.stat().st_size / (1024 * 1024)
    elapsed = time.time() - t0
    log("=" * 72)
    log(f"  BASE PACK DONE in {elapsed:.1f}s -> {out_path} ({size_mb:.2f} MB)")
    log(f"  tiles={len(records)}  airports={len(airports)}  runways={manifest['counts']['runways']}")

    # Optional pre-baking stages controlled by Package Manager options!
    stem_base = "es_aragon" if region_id == "es_aragon_pyrenees" else region_id
    baked_files: dict[str, str] = {"nspack": out_path.name}
    full_plug = bool(getattr(args, "full", False))
    want_scenery = full_plug or bool(getattr(args, "bake_scenery", False))
    want_cities = full_plug or bool(getattr(args, "bake_cities", False))
    want_weather = full_plug or bool(getattr(args, "bake_weather", False))
    if not getattr(args, "no_bake", False):
        if getattr(args, "bake_vegetation", True):
            in_spain = (-18.2 <= bbox[0] and bbox[2] <= 4.35 and 27.6 <= bbox[1] and bbox[3] <= 43.8)
            if in_spain:
                veg_zoom = int(plan["qp"].get("veg_zoom", 15))
                veg_cmd = [
                    sys.executable, str(ROOT / "builder" / "bake_vegetation.py"),
                    "--pack", str(out_path), "--out", str(out_path),
                    "--zoom", str(veg_zoom), "--workers", str(max(4, min(10, args.workers))),
                ]
                if veg_zoom < 15:
                    veg_cmd.append("--near-airports")
                log(f"[BAKE] Downloading IGN LiDAR vegetation heights (z{veg_zoom}{' + z15 airports' if veg_zoom < 15 else ''})...")
                subprocess.run(veg_cmd, check=True)
            else:
                log("[SKIP] IGN LiDAR vegetation bake is Spain-only; the region pack keeps imagery and terrain layers.")
        pbfs: list[Path] = []
        if want_scenery or want_cities:
            pbfs = _resolve_pbfs(bbox, preset, getattr(args, "pbf", None), cache_dir)
        if want_scenery:
            scn_out = PACKS_DIR / f"{stem_base}_scenery.nsscn"
            log(f"[BAKE] Building OSM buildings & roads scenery -> {scn_out.name}...")
            subprocess.run([
                sys.executable, str(ROOT / "builder" / "build_scenery.py"),
                "--pack", str(out_path), "--out", str(scn_out),
                "--pbf", *[str(p) for p in pbfs],
            ], check=True)
            if scn_out.exists():
                baked_files["nsscn"] = scn_out.name
        if want_cities:
            city_out = PACKS_DIR / f"{stem_base}_cities.nscity"
            sh_out = PACKS_DIR / f"{stem_base}_city_sh.nsshd"
            cmd = [
                sys.executable, str(ROOT / "builder" / "bake_static_cities.py"),
                "--pack", str(out_path),
                "--out-city", str(city_out),
                "--out-sh", str(sh_out),
                "--sh-res", str(plan["qp"]["sh_res"]),
                "--workers", str(args.workers),
                "--pbf", *[str(p) for p in pbfs],
            ]
            if getattr(args, "no_night_lights", False):
                cmd.append("--no-night-lights")
            log(f"[BAKE] Pre-baking 10-TOD exact-polygon settlements & shadows ({plan['qp']['sh_res']}px, night_rt={not getattr(args, 'no_night_lights', False)})...")
            subprocess.run(cmd, check=True)
            if city_out.exists():
                baked_files["nscity"] = city_out.name
            if sh_out.exists():
                baked_files["nsshd"] = sh_out.name
        if want_weather:
            wx_out = PACKS_DIR / f"{stem_base}_weather.nswx"
            log(f"[BAKE] Pre-baking ERA5 mesoscale weather states -> {wx_out.name}...")
            subprocess.run([
                sys.executable, str(ROOT / "builder" / "bake_weather.py"),
                "--pack", str(out_path), "--out", str(wx_out),
            ], check=True)
            if wx_out.exists():
                baked_files["nswx"] = wx_out.name

    from bake_map_relief import bake as bake_map_relief
    relief_path = bake_map_relief(out_path)
    baked_files["relief"] = relief_path.name

    _update_registry_entry(region_id, {
        "region_id": region_id,
        "name": name,
        "preset": plan["preset_key"],
        "quality": quality,
        "bbox": list(bbox),
        "files": baked_files,
        "tod_count": 10,
        "night_rt": not getattr(args, "no_night_lights", False),
        "updated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    })
    if full_plug:
        log(f"[VERIFY] Checking pack integrity -> {out_path.name}...")
        subprocess.run([sys.executable, str(ROOT / "builder" / "test_pack.py"), str(out_path)], check=True)
    log("=" * 72)
    return 0 if elev_tiles and (img_tiles or plan["imagery_source"] == "none") else 1


# ---------------------------------------------------------------------------
# World Region Package Manager (registry + list / presets / info / remove)
# ---------------------------------------------------------------------------
def _load_registry() -> dict[str, Any]:
    if REGISTRY_PATH.exists():
        try:
            return json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {"schema_version": 1, "regions": {}}


def _save_registry(reg: dict[str, Any]) -> None:
    PACKS_DIR.mkdir(parents=True, exist_ok=True)
    REGISTRY_PATH.write_text(json.dumps(reg, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _update_registry_entry(region_id: str, entry: dict[str, Any]) -> None:
    reg = _load_registry()
    reg.setdefault("regions", {})[region_id] = entry
    _save_registry(reg)


def _discover_installed_regions() -> dict[str, dict[str, Any]]:
    from protocol.pack_format import PackReader
    reg = _load_registry().get("regions", {})
    found: dict[str, dict[str, Any]] = {}
    if PACKS_DIR.exists():
        for p in sorted(PACKS_DIR.glob("*.nspack")):
            rid = p.stem
            info: dict[str, Any] = dict(reg.get(rid, {}))
            info.setdefault("region_id", rid)
            try:
                with PackReader(p) as r:
                    m = r.manifest
                    info["name"] = m.get("name", info.get("name", rid))
                    info["bbox"] = m.get("bbox", info.get("bbox", []))
                    info["airports"] = len(m.get("airports", []))
                    info["imagery_zooms"] = m.get("imagery_zooms", [])
                    info["elevation_zooms"] = m.get("elevation_zooms", [])
            except Exception:
                info.setdefault("name", rid)
            stem_base = "es_aragon" if rid == "es_aragon_pyrenees" else rid
            files = {"nspack": p.name}
            relief = p.with_suffix(".relief.png")
            if relief.is_file():
                files["relief"] = relief.name
            for ext, key in (
                ("_scenery.nsscn", "nsscn"), ("_cities.nscity", "nscity"),
                ("_city_sh.nsshd", "nsshd"), ("_weather.nswx", "nswx"),
            ):
                cand = PACKS_DIR / f"{stem_base}{ext}"
                if not cand.exists():
                    cand = PACKS_DIR / f"{rid}{ext}"
                if cand.exists():
                    files[key] = cand.name
            info["files"] = files
            info["total_mb"] = round(sum((PACKS_DIR / fn).stat().st_size for fn in files.values() if (PACKS_DIR / fn).exists()) / 1e6, 2)
            found[rid] = info
    return found


def cmd_list() -> int:
    inst = _discover_installed_regions()
    if not inst:
        print("No regions installed in packs/. Run: python3 builder/build_region.py presets")
        return 0
    print(f"{'REGION ID':24s} {'QUALITY':8s} {'SIZE':9s} {'AIRPORTS':8s} {'PRE-BAKED LAYERS':28s} NAME")
    print("-" * 108)
    for rid, info in inst.items():
        fl = info["files"]
        layers = []
        if "nsscn" in fl: layers.append("OSM-Roads")
        if "nscity" in fl: layers.append("Cities-10TOD")
        if "nsshd" in fl: layers.append("Shadows+NightRT")
        if "nswx" in fl: layers.append("Weather-ERA5")
        q = info.get("quality", "high")
        print(f"{rid:24s} {q:8s} {info['total_mb']:6.1f} MB {str(info.get('airports', '?')):8s} {','.join(layers) or 'terrain-only':28s} {info.get('name', '')}")
    return 0


def cmd_presets() -> int:
    print("Available Regional Presets (or supply custom --bbox LON0 LAT0 LON1 LAT1):")
    print("  - custom-world              -> requires --bbox; imports user-selected global data layers")
    for k, v in sorted(PRESETS.items()):
        print(f"  - {k:26s} -> {v['region_id']:22s} bbox={v['bbox']}  ({v['name']})")
    print("\nQuality Levels (--quality):")
    for qk, qv in QUALITY_PROFILES.items():
        print(f"  - {qk:8s} : {qv['desc']}")
    return 0


def cmd_info(region_id: str) -> int:
    inst = _discover_installed_regions()
    if region_id not in inst:
        print(f"Region '{region_id}' not found. Installed: {', '.join(inst.keys()) or 'none'}")
        return 1
    print(json.dumps(inst[region_id], indent=2))
    return 0


def cmd_remove(region_id: str, yes: bool) -> int:
    inst = _discover_installed_regions()
    if region_id not in inst:
        print(f"Region '{region_id}' is not installed.")
        return 1
    files = [PACKS_DIR / fn for fn in inst[region_id]["files"].values()]
    if not yes:
        print(f"Would remove {len(files)} file(s) for '{region_id}' ({inst[region_id]['total_mb']} MB). Re-run with --yes to confirm.")
        for fp in files:
            print(f"  - {fp}")
        return 0
    for fp in files:
        if fp.exists():
            fp.unlink()
            print(f"Removed {fp}")
    reg = _load_registry()
    reg.get("regions", {}).pop(region_id, None)
    _save_registry(reg)
    return 0


def _add_build_flags(p: argparse.ArgumentParser) -> None:
    p.add_argument("--preset", choices=sorted(PRESETS.keys()) + ["all-spain", "custom-world"], default="zaragoza-pyrenees-quick")
    p.add_argument("--quality", choices=list(QUALITY_PROFILES.keys()), default="high")
    p.add_argument("--bbox", nargs=4, type=float, metavar=("LON_MIN", "LAT_MIN", "LON_MAX", "LAT_MAX"))
    p.add_argument("--region-id", default=None)
    p.add_argument("--name", default=None)
    p.add_argument("--imagery-source", choices=("pnoa", "local-xyz", "none"), default="pnoa",
                   help="PNOA is Spain-only; local-xyz imports user-licensed <dir>/<z>/<x>/<y> tiles; none makes elevation-only terrain.")
    p.add_argument("--imagery-dir", default=None, help="XYZ imagery directory for --imagery-source local-xyz")
    p.add_argument("--imagery-attribution", default=None, help="source/license notice recorded for imported imagery")
    p.add_argument("--elevation-source", choices=("ign+terrarium", "terrarium", "local-terrarium"), default="ign+terrarium",
                   help="Use IGN Spain with Terrarium fallback, Terrarium, or imported Terrarium XYZ tiles.")
    p.add_argument("--elevation-dir", default=None, help="XYZ Terrarium PNG directory for --elevation-source local-terrarium")
    p.add_argument("--elevation-attribution", default=None, help="source/license notice recorded for imported elevation")
    p.add_argument("--cruise-img-zooms", default=None)
    p.add_argument("--cruise-elev-zooms", default=None)
    p.add_argument("--inset-img-zooms", default=None)
    p.add_argument("--inset-elev-zooms", default=None)
    p.add_argument("--bake-scenery", action="store_true", help="also build OSM roads & buildings .nsscn")
    p.add_argument("--bake-cities", action="store_true", help="pre-bake 10-TOD exact-polygon cities & shadows")
    p.add_argument("--bake-weather", action="store_true", help="bake regional 3D ERA5 weather .nswx")
    p.add_argument("--full", "--plug-and-play", dest="full", action="store_true",
                   help="plug-and-play install: pack + LiDAR + OSM roads/buildings + 10-TOD cities + ERA5 weather + verify")
    p.add_argument("--bake-vegetation", action=argparse.BooleanOptionalAction, default=True,
                   help="bake region-wide IGN LiDAR canopy heights for Spanish regions")
    p.add_argument("--no-night-lights", action="store_true", help="skip ray-traced night local lights bake")
    p.add_argument("--no-bake", action="store_true", help="skip all post-pack pre-baking steps")
    p.add_argument("--force", action="store_true", help="rebuild already-installed regions during all-spain build")
    p.add_argument("--pbf", default=None, help="path to OSM .pbf extract for pre-baking")
    p.add_argument("--workers", type=int, default=max(4, min(14, (os.cpu_count() or 4) - 2)))
    p.add_argument("--rate", type=float, default=0.01)
    p.add_argument("--no-osm", action="store_true", help="skip Overpass OSM query (use OurAirports + curated only)")
    p.add_argument("--cache", default=str(ROOT / "builder" / ".cache"))
    p.add_argument("--out", default=None)
    p.add_argument("--dry-run", action="store_true", help="print build & pre-bake plan without downloading")


def run_selfcheck() -> int:
    # Verify all quality tiers, presets, custom bbox resolution & installed region discovery cleanly
    for q in QUALITY_PROFILES:
        ns = argparse.Namespace(
            preset="navarra-basque", quality=q, bbox=None, region_id=None, name=None,
            cruise_img_zooms=None, cruise_elev_zooms=None, inset_img_zooms=None, inset_elev_zooms=None,
            out=None, dry_run=True, bake_cities=True, no_night_lights=False,
        )
        pl = resolve_build_plan(ns)
        assert pl["quality"] == q and pl["qp"]["sh_res"] in (64, 96, 128, 160)
    custom_ns = argparse.Namespace(
        preset="zaragoza-pyrenees-quick", quality="medium", bbox=[-0.5, 42.0, 0.2, 42.5], region_id="es_test_zone",
        name="Test Zone", cruise_img_zooms=None, cruise_elev_zooms=None, inset_img_zooms=None, inset_elev_zooms=None,
        out=None, dry_run=True, bake_cities=True, no_night_lights=False,
    )
    pl2 = resolve_build_plan(custom_ns)
    assert pl2["region_id"] == "es_test_zone" and pl2["bbox"] == (-0.5, 42.0, 0.2, 42.5)
    inst = _discover_installed_regions()
    assert "es_aragon_pyrenees" in inst and "nscity" in inst["es_aragon_pyrenees"]["files"]
    print(f"[PKG-MGR SELFTEST OK] {len(PRESETS)} presets, {len(QUALITY_PROFILES)} quality tiers, {len(inst)} installed region(s) verified.")
    return 0


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    # Preserve 100% backward compatibility with legacy flags (`build_region.py --preset ...`)
    subcmds = {"list", "presets", "info", "install", "build", "remove", "selftest"}
    if not raw or (raw[0].startswith("-") and raw[0] not in ("-h", "--help")):
        p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        _add_build_flags(p)
        return build_region(p.parse_args(raw))

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd")
    sp.add_parser("list", help="list installed regional packs and pre-baked layers")
    sp.add_parser("presets", help="list built-in Northern Spain regional presets & quality tiers")
    sp.add_parser("selftest", help="run package manager self-checks")
    p_info = sp.add_parser("info", help="show detailed JSON manifest for an installed region")
    p_info.add_argument("region_id")
    p_rem = sp.add_parser("remove", help="uninstall a region pack and its pre-baked files")
    p_rem.add_argument("region_id")
    p_rem.add_argument("--yes", action="store_true", help="confirm file deletion")
    for cname in ("install", "build"):
        p_inst = sp.add_parser(cname, help="download, build and pre-bake one or more regional world packages")
        p_inst.add_argument("target_presets", nargs="*", help="one or more preset names (or omit with --bbox)")
        _add_build_flags(p_inst)

    args = p.parse_args(raw)
    if args.cmd == "list": return cmd_list()
    if args.cmd == "presets": return cmd_presets()
    if args.cmd == "selftest": return run_selfcheck()
    if args.cmd == "info": return cmd_info(args.region_id)
    if args.cmd == "remove": return cmd_remove(args.region_id, args.yes)
    if args.cmd in ("install", "build"):
        targets = list(args.target_presets or [])
        if targets:
            rc = 0
            for tp in targets:
                if tp not in PRESETS and tp not in ("all-spain", "custom-world"):
                    raise SystemExit(f"Unknown preset '{tp}'. Valid: {', '.join(sorted(PRESETS))}, all-spain, custom-world")
                sub = argparse.Namespace(**vars(args))
                sub.preset = tp
                if build_region(sub) != 0:
                    rc = 1
            return rc
        return build_region(args)
    p.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
