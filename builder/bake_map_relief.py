"""Bake a north-up shaded relief PNG beside an installed NSPK terrain pack."""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from protocol.pack_format import (  # noqa: E402
    ENTRY_FMT, ENTRY_SIZE, HEADER_FMT, HEADER_SIZE, LAYER_ELEVATION,
    PACK_MAGIC, PACK_VERSION, decode_height_tile,
)
import struct

WIDTH, HEIGHT = 1536, 1024


def bake(pack: Path) -> Path:
    with pack.open("rb") as handle:
        magic, version, _, _, west, south, east, north, index_offset, count, *_ = (
            struct.unpack(HEADER_FMT, handle.read(HEADER_SIZE))
        )
        if (magic, version) != (PACK_MAGIC, PACK_VERSION):
            raise ValueError(f"Unsupported terrain pack: {pack}")
        handle.seek(index_offset)
        records = list(struct.iter_unpack(ENTRY_FMT, handle.read(count * ENTRY_SIZE)))
        height = np.full((HEIGHT, WIDTH), np.nan, dtype=np.float32)
        lon = west + (np.arange(WIDTH) + 0.5) * (east - west) / WIDTH
        lat = north - (np.arange(HEIGHT) + 0.5) * (north - south) / HEIGHT
        merc_x = (lon + 180.0) / 360.0
        merc_y = (1.0 - np.arcsinh(np.tan(np.radians(lat))) / math.pi) * 0.5
        spacing_e = (east - west) * 111132 * math.cos(math.radians((north + south) / 2)) / WIDTH
        spacing_n = (north - south) * 111132 / HEIGHT
        zoom_limit = math.ceil(math.log2(156543 / max(1, min(spacing_e, spacing_n) * 0.5)))

        for key, offset, size, cols, rows, encoding, *_ in records:
            if key >> 56 != LAYER_ELEVATION:
                continue
            zoom = key >> 48 & 255
            x, y = key >> 24 & 0xFFFFFF, key & 0xFFFFFF
            tx, ty = merc_x * (1 << zoom) - x, merc_y * (1 << zoom) - y
            ix, iy = np.flatnonzero((tx >= 0) & (tx < 1)), np.flatnonzero((ty >= 0) & (ty < 1))
            if not ix.size or not iy.size:
                continue
            box = np.ix_(iy, ix)
            old = height[box]
            if zoom > zoom_limit and np.isfinite(old).all():
                continue
            handle.seek(offset)
            tile = decode_height_tile(handle.read(size), cols, rows, encoding)
            px, py = tx[ix] * (cols - 1), ty[iy] * (rows - 1)
            x0, y0 = px.astype(np.intp), py.astype(np.intp)
            x1, y1 = np.minimum(x0 + 1, cols - 1), np.minimum(y0 + 1, rows - 1)
            dx, dy = px - x0, (py - y0)[:, None]
            a = tile[np.ix_(y0, x0)] * (1 - dx) + tile[np.ix_(y0, x1)] * dx
            b = tile[np.ix_(y1, x0)] * (1 - dx) + tile[np.ix_(y1, x1)] * dx
            new = a * (1 - dy) + b * dy
            height[box] = np.where(np.isfinite(old), old, new) if zoom > zoom_limit else new

    if not np.isfinite(height).any():
        raise ValueError(f"No valid elevation tiles: {pack}")
    coverage = np.isfinite(height)
    height = np.maximum(np.nan_to_num(height), 0)
    rise_s, rise_e = np.gradient(height, max(1, spacing_n), max(1, spacing_e))
    shade = np.clip((rise_e + rise_s) * 0.50, -0.25, 0.26)[..., None]
    low, high, alpine = (np.array(rgb) for rgb in ((0.15, 0.41, 0.30), (0.47, 0.53, 0.41), (0.65, 0.72, 0.70)))

    def transition(bottom: float, top: float) -> np.ndarray:
        t = np.clip((height - bottom) / (top - bottom), 0, 1)[..., None]
        return t * t * (3 - 2 * t)

    rgb = low + (high - low) * transition(380, 2500)
    rgb += (alpine - rgb) * transition(2350, 3150)
    rgba = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
    rgba[..., :3] = np.round(np.clip(rgb + shade, 0, 1) * 255)
    rgba[..., 3] = np.where(coverage, 255, 0)
    output = pack.with_suffix(".relief.png")
    temporary = output.with_suffix(".png.tmp")
    Image.fromarray(rgba).save(temporary, format="PNG")
    temporary.replace(output)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packs", type=Path, nargs="+")
    for pack in parser.parse_args().packs:
        print(bake(pack))


if __name__ == "__main__":
    main()
