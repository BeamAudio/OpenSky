#!/usr/bin/env python3
"""Fetch official Spanish Hydrography (IGN Spain INSPIRE WMS, CC-BY 4.0)
and bake crisp rivers, streams, lakes & reservoirs into LAYER_SURFACE channel B of .nspack files.
"""

from __future__ import annotations

import io
import math
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np
import requests
from PIL import Image
from scipy.ndimage import gaussian_filter

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from protocol.pack_format import (  # noqa: E402
    ENC_PNG,
    LAYER_IMAGERY,
    LAYER_SURFACE,
    PackReader,
    TileRecord,
    write_pack,
)

IGN_HYDRO_WMS = "https://servicios.idee.es/wms-inspire/hidrografia"
USER_AGENT = "NorthernSpainVFRSim/0.1 (IGN CC-BY 4.0 hydrography baker)"
MERC_HALF = 20037508.342789244


def _tile_merc_bbox(z: int, x: int, y: int) -> tuple[float, float, float, float]:
    n = 2.0 ** z
    step = (2.0 * MERC_HALF) / n
    mx0 = -MERC_HALF + x * step
    mx1 = mx0 + step
    my1 = MERC_HALF - y * step
    my0 = my1 - step
    return mx0, my0, mx1, my1


def _fetch_hydro_tile(z: int, x: int, y: int, px: int, cache_dir: Path) -> np.ndarray | None:
    """Fetch a single EPSG:3857 tile mask (float32 0..1) from IGN INSPIRE Hydrography WMS."""
    cp = cache_dir / "ign_hydro" / f"{z}_{x}_{y}_{px}.png"
    if cp.exists():
        arr = np.asarray(Image.open(cp).convert("L"), dtype=np.float32) / 255.0
        return arr if arr.max() > 0.02 else None

    mx0, my0, mx1, my1 = _tile_merc_bbox(z, x, y)
    params = {
        "SERVICE": "WMS",
        "VERSION": "1.3.0",
        "REQUEST": "GetMap",
        "LAYERS": "HY.PhysicalWaters.Waterbodies,HY.Network",
        "STYLES": "",
        "CRS": "EPSG:3857",
        "BBOX": f"{mx0:.2f},{my0:.2f},{mx1:.2f},{my1:.2f}",
        "WIDTH": str(px),
        "HEIGHT": str(px),
        "FORMAT": "image/png",
        "TRANSPARENT": "TRUE",
    }
    for attempt in range(3):
        try:
            r = requests.get(IGN_HYDRO_WMS, params=params, headers={"User-Agent": USER_AGENT}, timeout=25)
            if r.status_code == 200 and r.content[:4] == b"\x89PNG":
                rgba = np.asarray(Image.open(io.BytesIO(r.content)).convert("RGBA"))
                # IGN Hydrography renders all waterbodies & rivers in pure sky-blue (174, 209, 255);
                # filtering B > 205 & R > 105 & G > 145 strips dark navy text labels cleanly!
                is_water = (rgba[:, :, 3] > 50) & (rgba[:, :, 0] > 100) & (rgba[:, :, 1] > 140) & (rgba[:, :, 2] > 200)
                mask = is_water.astype(np.float32)
                if mask.max() > 0.0:
                    # Slight anti-aliased feather for natural smooth river/shoreline transitions
                    mask = np.clip(gaussian_filter(mask, sigma=0.75) * 1.25, 0.0, 1.0)
                cp.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray(np.clip(mask * 255.0, 0, 255).astype(np.uint8), mode="L").save(cp, optimize=True)
                return mask if mask.max() > 0.02 else None
        except Exception:
            pass
        time.sleep(0.35 * (attempt + 1))
    return None


def patch_pack_with_osm_water(pack_path: Path, cache_dir: Path) -> None:
    """Bake crisp official IGN Spain rivers, streams, lakes & reservoirs into all pack surface tiles."""
    t0 = time.time()
    reader = PackReader(pack_path)
    bbox = tuple(reader.bbox)
    region_id = reader.region_id
    existing_keys = set(reader.index.keys())

    # Collect unique z11 super-tiles covering all surface tiles (1024x1024 each -> ~14m/px across full pack!)
    # AND unique z13 super-tiles covering high-res z15/z16 airfield areas (1024x1024 each -> ~3.5m/px!)
    z11_needed: set[tuple[int, int]] = set()
    z13_needed: set[tuple[int, int]] = set()
    surf_coords: list[tuple[int, int, int]] = []
    img16_coords: list[tuple[int, int]] = []

    for key in existing_keys:
        layer = (key >> 56) & 0xFF
        z = (key >> 48) & 0xFF
        x = (key >> 24) & 0xFFFFFF
        y = key & 0xFFFFFF
        if layer == LAYER_SURFACE:
            surf_coords.append((z, x, y))
            if z >= 11:
                z11_needed.add((x >> (z - 11), y >> (z - 11)))
            if z >= 14:
                z13_needed.add((x >> (z - 13), y >> (z - 13)))
        elif layer == LAYER_IMAGERY and z == 16:
            img16_coords.append((x, y))
            z13_needed.add((x >> 3, y >> 3))

    print(f"[WATER] Fetching IGN Spain INSPIRE Hydrography ({len(z11_needed)} z11 + {len(z13_needed)} z13 HD tiles)...")
    z11_maps: dict[tuple[int, int], np.ndarray] = {}
    z13_maps: dict[tuple[int, int], np.ndarray] = {}

    jobs = [(11, x, y, 1024) for (x, y) in z11_needed] + [(13, x, y, 1024) for (x, y) in z13_needed]
    with ThreadPoolExecutor(max_workers=8) as pool:
        futs = {pool.submit(_fetch_hydro_tile, z, x, y, px, cache_dir): (z, x, y) for (z, x, y, px) in jobs}
        for fut in as_completed(futs):
            z, x, y = futs[fut]
            arr = fut.result()
            if arr is not None:
                if z == 11:
                    z11_maps[(x, y)] = arr
                else:
                    z13_maps[(x, y)] = arr

    print(f"[WATER] Hydrography ready in {time.time() - t0:.1f}s ({len(z11_maps)} z11 + {len(z13_maps)} z13 wet areas).")

    def slice_water(z: int, x: int, y: int) -> np.ndarray | None:
        # Prefer HD z13 map (3.5m/px!) when available; else z11 map (14m/px!)
        if z >= 13:
            sx, sy = x >> (z - 13), y >> (z - 13)
            src = z13_maps.get((sx, sy))
            if src is not None:
                shift = z - 13
                sub = 1024 >> shift
                ox = (x - (sx << shift)) * sub
                oy = (y - (sy << shift)) * sub
                crop = src[oy : oy + sub, ox : ox + sub]
                if crop.max() > 0.02:
                    return np.asarray(Image.fromarray((crop * 255).astype(np.uint8)).resize((128, 128), Image.BILINEAR), dtype=np.float32) / 255.0
                return None
        if z >= 11:
            sx, sy = x >> (z - 11), y >> (z - 11)
            src = z11_maps.get((sx, sy))
            if src is not None:
                shift = z - 11
                sub = 1024 >> shift
                ox = (x - (sx << shift)) * sub
                oy = (y - (sy << shift)) * sub
                crop = src[oy : oy + sub, ox : ox + sub]
                if crop.max() > 0.02:
                    return np.asarray(Image.fromarray((crop * 255).astype(np.uint8)).resize((128, 128), Image.BILINEAR), dtype=np.float32) / 255.0
        return None

    tiles: list[TileRecord] = []
    updated_surf = 0
    for key, (off, size, w, h, enc, _crc) in reader.index.items():
        layer = (key >> 56) & 0xFF
        z = (key >> 48) & 0xFF
        x = (key >> 24) & 0xFFFFFF
        y = key & 0xFFFFFF
        reader._f.seek(off)
        raw = reader._f.read(size)
        if layer == LAYER_SURFACE:
            wmask = slice_water(z, x, y)
            if wmask is not None:
                arr = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"), dtype=np.uint8).copy()
                w_u8 = np.clip(wmask * 255.0, 0, 255).astype(np.uint8)
                arr[:, :, 2] = np.maximum(arr[:, :, 2], w_u8)
                arr[:, :, 0] = np.where(w_u8 > 80, 0, arr[:, :, 0])
                buf = io.BytesIO()
                Image.fromarray(arr, mode="RGB").save(buf, format="PNG", optimize=True)
                raw = buf.getvalue()
                updated_surf += 1
        tiles.append(TileRecord(layer, z, x, y, w, h, enc, raw, 0))

    # Add crisp z16 water surface tiles wherever z16 airfield imagery crosses rivers/lakes!
    added_z16 = 0
    for x, y in img16_coords:
        skey = (LAYER_SURFACE << 56) | (16 << 48) | (x << 24) | y
        if skey in existing_keys:
            continue
        wmask = slice_water(16, x, y)
        if wmask is None:
            continue
        parent = reader.read_tile(LAYER_SURFACE, 15, x >> 1, y >> 1, verify_crc=False)
        if parent is not None:
            pim = Image.open(io.BytesIO(parent)).convert("RGB")
            qx, qy = (x & 1) * 64, (y & 1) * 64
            arr = np.asarray(pim.crop((qx, qy, qx + 64, qy + 64)).resize((128, 128), Image.BILINEAR), dtype=np.uint8).copy()
        else:
            arr = np.zeros((128, 128, 3), dtype=np.uint8)
        w_u8 = np.clip(wmask * 255.0, 0, 255).astype(np.uint8)
        arr[:, :, 2] = w_u8
        arr[:, :, 0] = np.where(w_u8 > 80, 0, arr[:, :, 0])
        buf = io.BytesIO()
        Image.fromarray(arr, mode="RGB").save(buf, format="PNG", optimize=True)
        tiles.append(TileRecord(LAYER_SURFACE, 16, x, y, 128, 128, ENC_PNG, buf.getvalue()))
        added_z16 += 1

    reader.close()
    print(f"[WATER] Patched {updated_surf} surface tiles + added {added_z16} HD z16 water tiles. Writing pack...")
    write_pack(pack_path, region_id, bbox, tiles, reader.manifest)
    print(f"[WATER] DONE in {time.time() - t0:.1f}s -> {pack_path} ({pack_path.stat().st_size / (1024*1024):.2f} MB)")


if __name__ == "__main__":
    p = Path(sys.argv[1]) if len(sys.argv) > 1 else (ROOT / "packs" / "es_aragon_pyrenees.nspack")
    patch_pack_with_osm_water(p, ROOT / "builder" / ".cache")
