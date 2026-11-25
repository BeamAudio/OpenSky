#!/usr/bin/env python3
"""Add IGN PNOA-LiDAR normalized vegetation heights to a scenery pack."""
import argparse
import io
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import numpy as np
import requests
from PIL import Image
from scipy.ndimage import gaussian_filter, map_coordinates
from builder.build_region import tile_bounds, tile_grid_latlon, tiles_in_bbox
from protocol.pack_format import ENC_PNG, LAYER_VEGETATION, PackReader, TileRecord, write_pack

BLOCK_TILES = 8


def _zero_tile_bytes() -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.zeros((256, 256, 3), dtype=np.uint8)).save(buf, format="PNG", compress_level=3)
    return buf.getvalue()


def fetch_block(z, tiles, cache, allow_border_gaps=True):
    """Fetch one GeoTIFF window and split it into the runtime's XYZ tiles."""
    missing = [(x, y) for x, y in tiles
               if not (cache / str(z) / str(x) / f"{y}.png").exists()]
    if not missing:
        return [(x, y, (cache / str(z) / str(x) / f"{y}.png").read_bytes()) for x, y in tiles]
    x0, x1 = min(x for x, _ in missing), max(x for x, _ in missing)
    y0, y1 = min(y for _, y in missing), max(y for _, y in missing)
    west, _, _, north = tile_bounds(z, x0, y0)
    _, south, east, _ = tile_bounds(z, x1, y1)
    params = [
        ("SERVICE", "WCS"), ("VERSION", "2.0.1"), ("REQUEST", "GetCoverage"),
        ("COVERAGEID", "mdsn_v025"), ("FORMAT", "image/tiff"),
        ("SUBSETTINGCRS", "http://www.opengis.net/def/crs/EPSG/0/4326"),
        ("OUTPUTCRS", "http://www.opengis.net/def/crs/EPSG/0/4326"),
        ("SUBSET", f"Long({west:.10f},{east:.10f})"),
        ("SUBSET", f"Lat({south:.10f},{north:.10f})"),
        ("SCALESIZE", f"Long({(x1-x0+1)*256}),Lat({(y1-y0+1)*256})"),
    ]
    for attempt in range(3):
        try:
            response = requests.get("https://wcs-mds.idee.es/mds", params=params, timeout=60)
            if response.status_code in (400, 404) and allow_border_gaps:
                # Offshore Mediterranean/Atlantic sea or cross-border Andorra/France block outside IGN coverage.
                zero = _zero_tile_bytes()
                for x, y in missing:
                    target = cache / str(z) / str(x) / f"{y}.png"
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(zero)
                return [(x, y, (cache / str(z) / str(x) / f"{y}.png").read_bytes()) for x, y in tiles]
            response.raise_for_status()
            with Image.open(io.BytesIO(response.content)) as image:
                if image.format != "TIFF" or len(image.getbands()) != 1:
                    raise ValueError("expected single-band GeoTIFF height data")
                dx, dy, _ = image.tag_v2[33550]
                px, py, _, tie_lon, tie_lat, _ = image.tag_v2[33922][:6]
                left, top = tie_lon - px * dx, tie_lat + py * dy
                grid = np.asarray(image, dtype=np.float32)
            valid = np.isfinite(grid) & (grid >= 0) & (grid < 100)
            # Filter across XYZ boundaries before packing, rather than
            # smoothing each tile independently.
            heights = gaussian_filter(np.where(valid, grid, 0), 0.65, mode="nearest")
            for x, y in missing:
                lats, lons = tile_grid_latlon(z, x, y, 256)
                rows, cols = np.meshgrid((top - lats) / dy - 0.5,
                                         (lons - left) / dx - 0.5, indexing="ij")
                sampled = map_coordinates(heights, [rows, cols], order=1, mode="nearest")
                rgb = np.zeros((256, 256, 3), dtype=np.uint8)
                rgb[..., 0] = np.rint(sampled * (255.0 / 25.0)).clip(0, 255).astype(np.uint8)
                rgb[..., 1] = map_coordinates(valid.astype(np.uint8), [rows, cols],
                                                order=0, mode="nearest") * 255
                target = cache / str(z) / str(x) / f"{y}.png"
                target.parent.mkdir(parents=True, exist_ok=True)
                temp = target.with_suffix(".tmp")
                Image.fromarray(rgb).save(temp, format="PNG", compress_level=3)
                temp.replace(target)
            return [(x, y, (cache / str(z) / str(x) / f"{y}.png").read_bytes()) for x, y in tiles]
        except (requests.RequestException, OSError, KeyError, ValueError) as error:
            if attempt == 2:
                if allow_border_gaps:
                    zero = _zero_tile_bytes()
                    for x, y in missing:
                        target = cache / str(z) / str(x) / f"{y}.png"
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(zero)
                    print(f"[VEGETATION] border/offshore gap {z}/{x0}/{y0} -> 0-height fallback", flush=True)
                    return [(x, y, (cache / str(z) / str(x) / f"{y}.png").read_bytes()) for x, y in tiles]
                raise RuntimeError(f"IGN vegetation block {z}/{x0}/{y0} failed") from error


def build_overviews(z, coords, cache):
    """Keep distant canopy coherent without reading dense source tiles in flight."""
    child_tiles = {(x, y) for _, x, y in coords}
    child_cache = cache
    overview_cache = cache / "overviews"
    for level in range(z - 1, min(z, 12) - 1, -1):
        parents = {(x // 2, y // 2) for x, y in child_tiles}
        for parent_x, parent_y in sorted(parents):
            mosaic = np.zeros((512, 512, 3), dtype=np.uint8)
            for j in range(2):
                for i in range(2):
                    x, y = parent_x * 2 + i, parent_y * 2 + j
                    if (x, y) not in child_tiles:
                        continue
                    with Image.open(child_cache / str(level + 1) / str(x) / f"{y}.png") as image:
                        mosaic[j * 256:(j + 1) * 256, i * 256:(i + 1) * 256] = image
            blocks = mosaic.reshape(256, 2, 256, 2, 3)
            average = blocks[:, :, :, :, 0].mean(axis=(1, 3))
            maximum = blocks[:, :, :, :, 0].max(axis=(1, 3))
            overview = np.zeros((256, 256, 3), dtype=np.uint8)
            overview[..., 0] = np.rint(average * .65 + maximum * .35).astype(np.uint8)
            overview[..., 1] = blocks[:, :, :, :, 1].max(axis=(1, 3))
            target = overview_cache / str(level) / str(parent_x) / f"{parent_y}.png"
            target.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(overview).save(target, format="PNG", compress_level=3)
            yield TileRecord(LAYER_VEGETATION, level, parent_x, parent_y, 256, 256, ENC_PNG,
                             target.read_bytes())
        child_tiles = parents
        child_cache = overview_cache


def bake(pack, output, cache, zoom, near_airports, airport_ids, workers=8, allow_border_gaps=True):
    full_region = not airport_ids
    with PackReader(pack) as reader:
        coords = {(zoom, x, y) for x, y in tiles_in_bbox(reader.bbox, zoom)} if full_region else set()
        if near_airports or airport_ids:
            for airport in reader.manifest.get('airports', []):
                if airport_ids and airport['id'] not in airport_ids:
                    continue
                lat, lon = airport['lat'], airport['lon']
                bbox_near = (lon - 0.025, lat - 0.02, lon + 0.025, lat + 0.02)
                coords.update((15, x, y) for x, y in tiles_in_bbox(bbox_near, 15))
        manifest, region, bbox = reader.manifest, reader.region_id, reader.bbox
        records = []
        for key, (offset, size, width, height, encoding, crc) in reader.index.items():
            layer, z, x, y = (key >> 56) & 255, (key >> 48) & 255, (key >> 24) & 0xffffff, key & 0xffffff
            if layer == LAYER_VEGETATION and (full_region or (z, x, y) in coords):
                continue
            reader._f.seek(offset)
            records.append(TileRecord(layer, z, x, y, width, height, encoding, reader._f.read(size)))
    # Use a separate cache from the older per-tile ArcGrid workflow.
    blocks = {}
    for z, x, y in coords:
        blocks.setdefault((z, x // BLOCK_TILES, y // BLOCK_TILES), []).append((x, y))
    failures = []
    count = 0
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        jobs = {pool.submit(fetch_block, z, tiles, cache / "geotiff", allow_border_gaps): (z, tiles)
                for (z, _, _), tiles in sorted(blocks.items())}
        for future in as_completed(jobs):
            z, tiles = jobs[future]
            try:
                results = future.result()
            except RuntimeError as error:
                print(f"[VEGETATION] {error}: {error.__cause__}", flush=True)
                failures.extend((z, x, y) for x, y in tiles)
                results = []
            for x, y, data in results:
                records.append(TileRecord(LAYER_VEGETATION, z, x, y, 256, 256, ENC_PNG, data))
            count += len(tiles)
            print(f'[VEGETATION] {count}/{len(coords)} tiles, {len(failures)} failed', flush=True)
    if failures:
        raise RuntimeError(f'{len(failures)} vegetation tiles failed; pack was not written')
    if full_region:
        regional_tiles = [(z, x, y) for z, x, y in coords if z == zoom]
        records.extend(build_overviews(zoom, regional_tiles, cache / "geotiff"))
    vegetation = manifest.setdefault('vegetation', {})
    vegetation.update(source='IGN PNOA-LiDAR MDSnV 2.5m',
                      attribution='© Instituto Geográfico Nacional de España (CC-BY 4.0)')
    if full_region:
        vegetation['zoom'] = zoom
        vegetation['coverage'] = 'full region' + (' + z15 airports' if near_airports else '')
        vegetation.pop('approaches', None)
        vegetation.pop('insets_z15', None)
    write_pack(output, region, bbox, records, manifest)
    print(f'[VEGETATION] wrote {output}', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cache', type=Path, default=ROOT / 'builder/.cache/vegetation')
    parser.add_argument('--zoom', type=int, default=15, choices=range(10, 17))
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--strict', action='store_true', help='fail on missing offshore/border tiles instead of zero-filling')
    parser.add_argument('--near-airports', action='store_true', help='include high-resolution airport approaches')
    parser.add_argument('--airport', action='append', default=[], help='bake only this airport approach, repeatable')
    args = parser.parse_args()
    bake(args.pack, args.out, args.cache, args.zoom, args.near_airports, set(args.airport),
         workers=args.workers, allow_border_gaps=not args.strict)
