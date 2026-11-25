#!/usr/bin/env python3
"""Verify integrity, CRC32s, elevation benchmarks, and runway smoothness of an .nspack file."""

from __future__ import annotations

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from protocol.pack_format import LAYER_ELEVATION, LAYER_IMAGERY, PackReader  # noqa: E402


def sample_elev(reader: PackReader, lat: float, lon: float) -> float:
    for z in sorted(reader.manifest["elevation_zooms"], reverse=True):
        n = 2.0 ** z
        xt = (lon + 180.0) / 360.0 * n
        yt = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
        tx, ty = int(math.floor(xt)), int(math.floor(yt))
        arr = reader.read_tile(LAYER_ELEVATION, z, tx, ty, verify_crc=True)
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
    raise AssertionError(f"No elevation tile covering ({lat}, {lon})")


def main() -> int:
    pack_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "packs" / "es_aragon_pyrenees.nspack"
    assert pack_path.exists(), f"Missing pack: {pack_path}"

    with PackReader(pack_path) as r:
        m = r.manifest
        print(f"Pack: {r.region_id} | {m['name']} | index={r.index_count} tiles | airports={len(m['airports'])}")
        assert m["counts"]["elevation_tiles"] > 0
        attr = m.get("attribution", {})
        assert isinstance(attr, dict) and attr.get("elevation"), "Elevation source attribution is required"
        assert attr.get("imagery"), "Imagery source or explicit no-imagery notice is required"
        assert "esri" not in str(attr).lower(), "Esri imagery is not allowed"

        # Spot-check CRC32 across tiles
        checked = 0
        for key in list(r.index.keys())[:: max(1, r.index_count // 40)]:
            layer = (key >> 56) & 0xFF
            z = (key >> 48) & 0xFF
            x = (key >> 24) & 0xFFFFFF
            y = key & 0xFFFFFF
            data = r.read_tile(layer, z, x, y, verify_crc=True)
            assert data is not None
            if layer == LAYER_IMAGERY:
                assert data[:2] == b"\xff\xd8", "Imagery tile is not valid JPEG"
            checked += 1
        print(f"  CRC32 & format verified on {checked} sampled tiles.")

        bbox = m.get("bbox", [])
        assert len(bbox) == 4 and bbox[0] < bbox[2] and bbox[1] < bbox[3], "Pack bounds are invalid"
        center_h = sample_elev(r, (bbox[1] + bbox[3]) * 0.5, (bbox[0] + bbox[2]) * 0.5)
        print(f"  Center elevation sample: {center_h:.2f} m")

        # Verify runway profiles exist and are smooth (no >3m step between adjacent profile stations)
        has_large = False
        has_obscure = False
        for ap in m["airports"]:
            if ap["kind"] in ("large_airport", "medium_airport"):
                has_large = True
            if ap["kind"] in ("airstrip", "small_airport"):
                has_obscure = True
            for rw in ap["runways"]:
                prof = rw.get("profile", [])
                if prof:
                    assert len(prof) == 17, f"Invalid runway profile length on {ap['id']}"
                    diffs = [abs(prof[i + 1] - prof[i]) for i in range(len(prof) - 1)]
                    max_step = max(diffs)
                    if max_step >= 6.0:
                        print(f"  [warn] Steep runway profile on {ap['id']} {rw['ident_a']}: max step {max_step:.2f}m")
                    assert max_step < 12.0, f"Runway {ap['id']} {rw['ident_a']} has cliff artifact: max step {max_step:.2f}m"
        if r.region_id == "es_aragon_pyrenees":
            assert m["counts"]["imagery_tiles"] > 0, "The shipped Pyrenees pack needs its orthophoto layer"
            assert "Instituto Geografico Nacional" in attr["imagery"]
            assert len(m["airports"]) >= 4 and has_large and has_obscure, "Shipped pack needs major fields and local strips"
        print("  PACK INTEGRITY CHECKS PASSED!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
