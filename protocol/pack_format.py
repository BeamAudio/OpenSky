"""Shared binary specification for .nspack regional terrain packs (matches protocol/pack_format.h)."""

from __future__ import annotations

import json
import struct
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

import numpy as np

PACK_MAGIC = b"NSPK"
PACK_VERSION = 1

LAYER_ELEVATION = 1  # 257x257 quantized u16 heightmap
LAYER_IMAGERY = 2    # 256x256 IGN PNOA ortho
LAYER_SURFACE = 3    # 128x128 RGB mask: R=forest, G=surface_class, B=water
LAYER_VEGETATION = 4  # RGB PNG: R=vegetation height / 25m, G=valid

ENC_RAW_U16_HEIGHT = 1
ENC_ZLIB_U16_HEIGHT = 2
ENC_JPEG = 3
ENC_PNG = 4
ENC_WEBP = 5

HEADER_FMT = "<4sIII4dQIIQQQ32s8s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 128

ENTRY_FMT = "<QQIHHBBHI"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
assert ENTRY_SIZE == 32

HEIGHT_HDR_FMT = "<ff"
HEIGHT_HDR_SIZE = struct.calcsize(HEIGHT_HDR_FMT)
assert HEIGHT_HDR_SIZE == 8


def make_tile_key(layer: int, z: int, x: int, y: int) -> int:
    return ((layer & 0xFF) << 56) | ((z & 0xFF) << 48) | ((x & 0xFFFFFF) << 24) | (y & 0xFFFFFF)


def encode_height_tile(heights_f32: np.ndarray, compress: bool = True) -> tuple[bytes, int]:
    """Quantize a 2D float32 height grid into u16 (sub-5cm precision across 3km relief)."""
    arr = np.asarray(heights_f32, dtype=np.float32)
    h_min = float(np.nanmin(arr))
    h_max = float(np.nanmax(arr))
    span = max(h_max - h_min, 1.0)
    step = span / 65535.0
    q = np.clip(np.round((np.nan_to_num(arr, nan=h_min) - h_min) / step), 0, 65535).astype("<u2")
    raw = struct.pack(HEIGHT_HDR_FMT, h_min, step) + q.tobytes()
    if compress:
        return zlib.compress(raw, level=6), ENC_ZLIB_U16_HEIGHT
    return raw, ENC_RAW_U16_HEIGHT


def decode_height_tile(payload: bytes, width: int, height: int, encoding: int) -> np.ndarray:
    raw = zlib.decompress(payload) if encoding == ENC_ZLIB_U16_HEIGHT else payload
    base_m, step_m = struct.unpack_from(HEIGHT_HDR_FMT, raw, 0)
    q = np.frombuffer(raw, dtype="<u2", offset=HEIGHT_HDR_SIZE).reshape((height, width))
    return base_m + q.astype(np.float32) * step_m


@dataclass
class TileRecord:
    layer: int
    z: int
    x: int
    y: int
    width: int
    height: int
    encoding: int
    payload: bytes
    flags: int = 0


def write_pack(
    out_path: Path,
    region_id: str,
    bbox: tuple[float, float, float, float],
    tiles: list[TileRecord],
    manifest: dict,
) -> None:
    """Write a sorted, CRC-verified .nspack file atomically."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + ".part")

    # Sort tiles by packed key so C++ can binary-search directly on the index.
    ordered = sorted(tiles, key=lambda t: make_tile_key(t.layer, t.z, t.x, t.y))
    # Deduplicate keeping last write if any duplicate key exists.
    dedup: dict[int, TileRecord] = {}
    for t in ordered:
        dedup[make_tile_key(t.layer, t.z, t.x, t.y)] = t
    ordered_items = sorted(dedup.items(), key=lambda kv: kv[0])

    manifest_bytes = json.dumps(manifest, separators=(",", ":"), ensure_ascii=False).encode("utf-8")

    with tmp.open("wb") as f:
        f.write(b"\x00" * HEADER_SIZE)
        entries: list[bytes] = []
        for key, t in ordered_items:
            # 8-byte align every payload
            pad = (-f.tell()) & 7
            if pad:
                f.write(b"\x00" * pad)
            off = f.tell()
            f.write(t.payload)
            crc = zlib.crc32(t.payload) & 0xFFFFFFFF
            entries.append(
                struct.pack(
                    ENTRY_FMT,
                    key,
                    off,
                    len(t.payload),
                    t.width,
                    t.height,
                    t.encoding,
                    t.flags,
                    0,
                    crc,
                )
            )

        pad = (-f.tell()) & 7
        if pad:
            f.write(b"\x00" * pad)
        index_offset = f.tell()
        for e in entries:
            f.write(e)

        pad = (-f.tell()) & 7
        if pad:
            f.write(b"\x00" * pad)
        manifest_offset = f.tell()
        f.write(manifest_bytes)

        rid = region_id.encode("ascii", errors="ignore")[:31]
        rid_padded = rid + b"\x00" * (32 - len(rid))
        hdr = struct.pack(
            HEADER_FMT,
            PACK_MAGIC,
            PACK_VERSION,
            0,
            0,
            bbox[0],
            bbox[1],
            bbox[2],
            bbox[3],
            index_offset,
            len(entries),
            0,
            manifest_offset,
            len(manifest_bytes),
            int(time.time()),
            rid_padded,
            b"\x00" * 8,
        )
        f.seek(0)
        f.write(hdr)

    tmp.replace(out_path)


class PackReader:
    """Lightweight Python reader for verifying and inspecting .nspack files."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self._f: BinaryIO = self.path.open("rb")
        hdr_bytes = self._f.read(HEADER_SIZE)
        vals = struct.unpack(HEADER_FMT, hdr_bytes)
        if vals[0] != PACK_MAGIC or vals[1] != PACK_VERSION:
            raise ValueError(f"Invalid .nspack header: magic={vals[0]!r}, ver={vals[1]}")
        self.bbox = (vals[4], vals[5], vals[6], vals[7])
        self.index_offset = vals[8]
        self.index_count = vals[9]
        self.manifest_offset = vals[11]
        self.manifest_size = vals[12]
        self.created_unix = vals[13]
        self.region_id = vals[14].split(b"\x00", 1)[0].decode("ascii")

        self._f.seek(self.manifest_offset)
        self.manifest = json.loads(self._f.read(self.manifest_size).decode("utf-8"))

        self._f.seek(self.index_offset)
        idx_raw = self._f.read(self.index_count * ENTRY_SIZE)
        self.index: dict[int, tuple[int, int, int, int, int, int]] = {}
        for i in range(self.index_count):
            key, off, size, w, h, enc, flags, _, crc = struct.unpack_from(ENTRY_FMT, idx_raw, i * ENTRY_SIZE)
            self.index[key] = (off, size, w, h, enc, crc)

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> PackReader:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def read_tile(self, layer: int, z: int, x: int, y: int, verify_crc: bool = True):
        key = make_tile_key(layer, z, x, y)
        meta = self.index.get(key)
        if meta is None:
            return None
        off, size, w, h, enc, crc = meta
        self._f.seek(off)
        payload = self._f.read(size)
        if verify_crc and (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
            raise ValueError(f"CRC mismatch for tile ({layer},{z},{x},{y})")
        if layer == LAYER_ELEVATION:
            return decode_height_tile(payload, w, h, enc)
        return payload
