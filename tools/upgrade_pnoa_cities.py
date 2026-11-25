#!/usr/bin/env python3
"""Fetch high-resolution CC-BY 4.0 IGN Spain PNOA Z16/Z17 satellite tiles for Zaragoza, Huesca & Mediano
and merge them cleanly into packs/es_aragon_pyrenees.nspack (atomic write + CRC verification).
"""

from __future__ import annotations

import math
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from builder.build_region import fetch_pnoa_tile  # noqa: E402
from protocol.pack_format import (  # noqa: E402
    ENC_JPEG,
    LAYER_IMAGERY,
    PackReader,
    TileRecord,
    make_tile_key,
    write_pack,
)

PACK_PATH = ROOT / "packs" / "es_aragon_pyrenees.nspack"
CACHE_DIR = ROOT / "builder" / ".cache"

# High-res IGN PNOA zones: (name, zoom, min_lon, min_lat, max_lon, max_lat)
ZONES = [
    ("Zaragoza Z16", 16, -0.935, 41.615, -0.835, 41.685),
    ("Zaragoza Z17", 17, -0.900, 41.642, -0.862, 41.668),
    ("Huesca Z16",   16, -0.440, 42.115, -0.382, 42.160),
    ("Huesca Z17",   17, -0.422, 42.130, -0.396, 42.150),
    ("Mediano Z15",  15,  0.155, 42.285,  0.245, 42.355),
    ("Mediano Z16",  16,  0.175, 42.300,  0.228, 42.340),
]


def latlon_to_tile(lat: float, lon: float, z: int) -> tuple[int, int]:
    n = 1 << z
    tx = int((lon + 180.0) / 360.0 * n)
    s = math.sin(math.radians(lat))
    ty = int((0.5 - math.log((1.0 + s) / (1.0 - s)) / (4.0 * math.pi)) * n)
    return max(0, min(n - 1, tx)), max(0, min(n - 1, ty))


def main() -> None:
    t0 = time.time()
    wanted: set[tuple[int, int, int]] = set()
    for name, z, lo0, la0, lo1, la1 in ZONES:
        x0, y1 = latlon_to_tile(la0, lo0, z)
        x1, y0 = latlon_to_tile(la1, lo1, z)
        cnt = 0
        for y in range(min(y0, y1), max(y0, y1) + 1):
            for x in range(min(x0, x1), max(x0, x1) + 1):
                wanted.add((z, x, y))
                cnt += 1
        print(f"  Zone {name:14s}: {cnt:4d} tiles")

    # Read all existing tiles from .nspack so we only fetch missing tiles!
    existing_keys: set[int] = set()
    existing_tiles: list[TileRecord] = []
    with PackReader(PACK_PATH) as r:
        manifest = dict(r.manifest)
        bbox = r.bbox
        region_id = r.region_id
        for key, (off, size, w, h, enc, _crc) in r.index.items():
            existing_keys.add(key)
            r._f.seek(off)
            payload = r._f.read(size)
            layer = (key >> 56) & 0xFF
            z = (key >> 48) & 0xFF
            x = (key >> 24) & 0xFFFFFF
            y = key & 0xFFFFFF
            existing_tiles.append(TileRecord(layer, z, x, y, w, h, enc, payload, 0))

    missing = [(z, x, y) for (z, x, y) in sorted(wanted) if make_tile_key(LAYER_IMAGERY, z, x, y) not in existing_keys]
    print(f"Existing tiles: {len(existing_tiles)} | Missing high-res IGN PNOA tiles to fetch: {len(missing)}")

    new_tiles: list[TileRecord] = []
    if missing:
        with ThreadPoolExecutor(max_workers=12) as pool:
            futs = {pool.submit(fetch_pnoa_tile, z, x, y, CACHE_DIR, 0.005): (z, x, y) for (z, x, y) in missing}
            done = 0
            for fut in as_completed(futs):
                z, x, y = futs[fut]
                raw = fut.result()
                done += 1
                if raw:
                    new_tiles.append(TileRecord(LAYER_IMAGERY, z, x, y, 256, 256, ENC_JPEG, raw, 0))
                if done % 150 == 0 or done == len(missing):
                    print(f"  Fetched {done}/{len(missing)} ({len(new_tiles)} ok) in {time.time() - t0:.1f}s")

    if new_tiles:
        all_tiles = existing_tiles + new_tiles
        iz = sorted({t.z for t in all_tiles if t.layer == LAYER_IMAGERY})
        manifest["imagery_zooms"] = iz
        manifest["tile_count"] = len(all_tiles)
        write_pack(PACK_PATH, region_id, bbox, all_tiles, manifest)
        print(f"[OK] Updated {PACK_PATH.name}: +{len(new_tiles)} IGN PNOA tiles -> total={len(all_tiles)} tiles ({PACK_PATH.stat().st_size // (1024*1024)} MB) in {time.time() - t0:.1f}s")
    else:
        print("[OK] All target high-res PNOA tiles already in pack.")


if __name__ == "__main__":
    main()
