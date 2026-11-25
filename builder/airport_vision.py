"""Image-derived airport building footprints for region packs.

This is a deterministic, CPU-side orthophoto recognition pass. It finds compact,
rectangular low-texture roof regions, georeferences their rotated footprints, and
uses the source pixels for roof colour. OSM/OurAirports remain the authority for
airport/runway identity; image detections only fill building gaps.

The detector deliberately avoids a learned model download so the world editor
works offline with cached, properly licensed XYZ imagery and has no GPU cost.
OpenCV accelerates morphology/components when installed; SciPy provides the same
path on lean installations.
"""

from __future__ import annotations

import math
import io
from typing import Any

import numpy as np
from PIL import Image
from scipy.ndimage import binary_closing, binary_opening, find_objects, label, uniform_filter

try:  # Optional acceleration; the detector has a SciPy fallback.
    import cv2  # type: ignore
except ImportError:  # pragma: no cover - depends on builder installation
    cv2 = None

EARTH_RADIUS_M = 6378137.0
TILE_PIXELS = 256


def _tile_pixel(lat: float, lon: float, zoom: int) -> tuple[float, float]:
    n = 2.0**zoom
    x = (lon + 180.0) / 360.0 * n * TILE_PIXELS
    lat = max(-85.05112878, min(85.05112878, lat))
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) * 0.5 * n * TILE_PIXELS
    return x, y


def _pixel_latlon(x: float, y: float, zoom: int) -> tuple[float, float]:
    n = 2.0**zoom
    lon = x / (TILE_PIXELS * n) * 360.0 - 180.0
    merc_y = math.pi * (1.0 - 2.0 * y / (TILE_PIXELS * n))
    lat = math.degrees(math.atan(math.sinh(merc_y)))
    return lat, lon


def _airport_roi(ap: dict[str, Any]) -> tuple[float, float, float, float]:
    runways = ap.get("runways", [])
    lats = [float(ap["lat"])]
    lons = [float(ap["lon"])]
    for rw in runways:
        for key in ("lat_a", "lat_b"):
            if key in rw:
                lats.append(float(rw[key]))
        for key in ("lon_a", "lon_b"):
            if key in rw:
                lons.append(float(rw[key]))

    # Airport types describe typical facility scale. Mapped aerodrome boundaries,
    # when available, replace this conservative runway-centred envelope below.
    radius_m = {
        "large_airport": 1900.0,
        "medium_airport": 1350.0,
        "small_airport": 850.0,
        "airstrip": 520.0,
    }.get(ap.get("kind", "airstrip"), 700.0)
    lat_mid = sum(lats) / len(lats)
    dlat = radius_m / 111132.0
    dlon = radius_m / max(1000.0, 111320.0 * math.cos(math.radians(lat_mid)))
    return min(lons) - dlon, min(lats) - dlat, max(lons) + dlon, max(lats) + dlat


def _latlon_in_polygon(lat: float, lon: float, pts: list[list[float]]) -> bool:
    if len(pts) < 3:
        return False
    inside = False
    x, y = lon, lat
    j = len(pts) - 1
    for i, (py, px) in enumerate(pts):
        yj, xj = pts[j]
        if ((py > y) != (yj > y)) and x < (xj - px) * (y - py) / ((yj - py) or 1e-15) + px:
            inside = not inside
        j = i
    return inside


def _point_segment_distance_m(lat: float, lon: float, a: tuple[float, float], b: tuple[float, float]) -> float:
    m_lon = 111320.0 * math.cos(math.radians(lat))
    dx, dy = (lon - a[1]) * m_lon, (lat - a[0]) * 111132.0
    vx, vy = (b[1] - a[1]) * m_lon, (b[0] - a[0]) * 111132.0
    den = vx * vx + vy * vy
    t = max(0.0, min(1.0, (dx * vx + dy * vy) / den)) if den > 1e-6 else 0.0
    return math.hypot(dx - t * vx, dy - t * vy)


def _near_runway(lat: float, lon: float, ap: dict[str, Any]) -> bool:
    limits = {"large_airport": 2200.0, "medium_airport": 1500.0,
              "small_airport": 1000.0, "airstrip": 620.0}
    for rw in ap.get("runways", []):
        a = (float(rw["lat_a"]), float(rw["lon_a"]))
        b = (float(rw["lat_b"]), float(rw["lon_b"]))
        if _point_segment_distance_m(lat, lon, a, b) <= limits.get(ap.get("kind"), 900.0):
            return True
    return False


def _known_building_near(lat: float, lon: float, width_m: float, depth_m: float,
                         buildings: list[dict[str, Any]]) -> bool:
    limit = max(5.0, min(width_m, depth_m) * 0.45)
    for building in buildings:
        pts = building.get("pts", [])
        if len(pts) < 3:
            continue
        if _latlon_in_polygon(lat, lon, pts):
            return True
        # OSM rings are usually short; a centre-radius check is enough to catch
        # a roof split into separate colour components without a GIS dependency.
        clat = sum(float(p[0]) for p in pts) / len(pts)
        clon = sum(float(p[1]) for p in pts) / len(pts)
        de = (lon - clon) * 111320.0 * math.cos(math.radians(lat))
        dn = (lat - clat) * 111132.0
        if de * de + dn * dn < limit * limit:
            return True
    return False


def _tile_indices(bounds: tuple[float, float, float, float], zoom: int) -> tuple[int, int, int, int]:
    lon0, lat0, lon1, lat1 = bounds
    x0, y0 = _tile_pixel(lat1, lon0, zoom)
    x1, y1 = _tile_pixel(lat0, lon1, zoom)
    return (int(x0 // TILE_PIXELS), int(y0 // TILE_PIXELS),
            int(x1 // TILE_PIXELS), int(y1 // TILE_PIXELS))


def _roof_mask(rgb: np.ndarray) -> np.ndarray:
    f = rgb.astype(np.float32) / 255.0
    hi, lo = f.max(axis=2), f.min(axis=2)
    sat = (hi - lo) / np.maximum(hi, 1e-3)
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    green = (g > r * 1.055) & (g > b * 0.96) & (g > 0.17)
    neutral_roof = (sat < 0.27) & (hi > 0.23) & (hi < 0.96)
    warm_roof = (r > g * 1.08) & (g > b * 1.04) & (r > 0.25) & (sat < 0.72)
    gray = (0.299 * r + 0.587 * g + 0.114 * b)
    if cv2 is not None:
        mean = cv2.blur(gray, (7, 7))
        mean2 = cv2.blur(gray * gray, (7, 7))
    else:
        mean = uniform_filter(gray, size=7, mode="nearest")
        mean2 = uniform_filter(gray * gray, size=7, mode="nearest")
    local_std = np.sqrt(np.maximum(0.0, mean2 - mean * mean))
    return ((neutral_roof | warm_roof) & ~green & (local_std < 0.105) & (gray > 0.18) & (gray < 0.96))


def _components(mask: np.ndarray) -> list[np.ndarray]:
    if cv2 is not None:
        kernel = np.ones((3, 3), dtype=np.uint8)
        closed = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_CLOSE, kernel)
        opened = cv2.morphologyEx(closed, cv2.MORPH_OPEN, np.ones((2, 2), dtype=np.uint8))
        count, labels, stats, _ = cv2.connectedComponentsWithStats(opened, 8)
        result = []
        for i in range(1, count):
            if stats[i, cv2.CC_STAT_AREA] < 24:
                continue
            x, y, w, h = (int(v) for v in stats[i, :4])
            yy, xx = np.where(labels[y:y + h, x:x + w] == i)
            result.append(np.column_stack((xx + x, yy + y)))
        return result
    clean = binary_opening(binary_closing(mask, structure=np.ones((3, 3))), structure=np.ones((2, 2)))
    labels, count = label(clean)
    sizes = np.bincount(labels.ravel(), minlength=count + 1)
    result = []
    for i, bounds in enumerate(find_objects(labels), start=1):
        if bounds is None or sizes[i] < 24:
            continue
        ys, xs = bounds
        yy, xx = np.where(labels[bounds] == i)
        result.append(np.column_stack((xx + xs.start, yy + ys.start)))
    return result


def _oriented_box(points_xy: np.ndarray) -> tuple[np.ndarray, float, float, float] | None:
    if len(points_xy) < 4:
        return None
    pts = points_xy.astype(np.float64)
    center = pts.mean(axis=0)
    centered = pts - center
    cov = centered.T @ centered / max(1, len(pts) - 1)
    values, vectors = np.linalg.eigh(cov)
    axis = vectors[:, int(np.argmax(values))]
    side = np.array([-axis[1], axis[0]])
    u, v = centered @ axis, centered @ side
    u0, u1 = float(u.min()), float(u.max())
    v0, v1 = float(v.min()), float(v.max())
    w, d = u1 - u0 + 1.0, v1 - v0 + 1.0
    if w < d:
        axis, side, w, d = side, -axis, d, w
        u, v = centered @ axis, centered @ side
        u0, u1 = float(u.min()), float(u.max())
        v0, v1 = float(v.min()), float(v.max())
    box = np.array([
        center + axis * u0 + side * v0,
        center + axis * u1 + side * v0,
        center + axis * u1 + side * v1,
        center + axis * u0 + side * v1,
    ])
    yaw = math.atan2(float(axis[1]), float(axis[0]))
    return box, w, d, yaw


def _roof_kind(area_m2: float, width_m: float, depth_m: float) -> tuple[str, float]:
    if area_m2 >= 1800.0 and width_m >= 45.0:
        return "terminal", 11.0
    if area_m2 >= 140.0 and width_m >= 18.0 and width_m / max(depth_m, 1.0) >= 1.45:
        return "hangar", 7.5
    return "airport_aux", 5.0


def recognize_airport_buildings(
    airports: list[dict[str, Any]],
    imagery: dict[tuple[int, int, int], bytes],
) -> dict[str, int]:
    """Append high-confidence orthophoto roof footprints missing from OSM.

    ``imagery`` uses XYZ keys ``(zoom, x, y)`` and RGB/JPEG bytes. Existing OSM
    structures are retained, while visual duplicates are suppressed.
    """
    totals = {"airports_with_imagery": 0, "detected": 0, "skipped_low_confidence": 0}
    decoded: dict[tuple[int, int, int], np.ndarray] = {}
    keys_by_zoom: dict[int, list[tuple[int, int, int]]] = {}
    for key in imagery:
        keys_by_zoom.setdefault(key[0], []).append(key)
    for ap in airports:
        roi = _airport_roi(ap)
        features = ap.setdefault("buildings", [])
        max_new = 180 if ap.get("kind") in ("large_airport", "medium_airport") else 80
        added = 0
        airport_has_images = False
        for zoom in sorted((z for z in keys_by_zoom if z >= 14), reverse=True):
            x0, y0, x1, y1 = _tile_indices(roi, zoom)
            keys = [key for key in keys_by_zoom[zoom]
                    if x0 <= key[1] <= x1 and y0 <= key[2] <= y1]
            if not keys:
                continue
            airport_has_images = True

            # Keep builder work bounded at very large hubs. Tiles nearest mapped
            # runway geometry are examined first; the OSM boundary check below
            # excludes adjacent industrial blocks when a boundary is available.
            def tile_priority(key: tuple[int, int, int]) -> float:
                _, tx, ty = key
                tlat, tlon = _pixel_latlon((tx + 0.5) * TILE_PIXELS, (ty + 0.5) * TILE_PIXELS, zoom)
                runways = ap.get("runways", [])
                return min((_point_segment_distance_m(tlat, tlon,
                            (float(rw["lat_a"]), float(rw["lon_a"])),
                            (float(rw["lat_b"]), float(rw["lon_b"]))) for rw in runways), default=1e9)
            keys.sort(key=tile_priority)
            tile_cap = 220 if ap.get("kind") in ("large_airport", "medium_airport") else 90
            for key in keys[:tile_cap]:
                _, tile_x, tile_y = key
                rgb = decoded.get(key)
                if rgb is None:
                    try:
                        with Image.open(io.BytesIO(imagery[key])) as im:
                            rgb = np.asarray(im.convert("RGB"), dtype=np.uint8)
                    except (OSError, ValueError):
                        continue
                    if rgb.shape[:2] != (TILE_PIXELS, TILE_PIXELS):
                        rgb = np.asarray(Image.fromarray(rgb).resize(
                            (TILE_PIXELS, TILE_PIXELS), Image.Resampling.BILINEAR))
                    if len(decoded) >= 32:
                        decoded.pop(next(iter(decoded)))
                    decoded[key] = rgb

                scale = (2.0 * math.pi * EARTH_RADIUS_M * math.cos(math.radians(float(ap["lat"]))) /
                         (TILE_PIXELS * (2.0**zoom)))
                for comp in _components(_roof_mask(rgb)):
                    obb = _oriented_box(comp)
                    if obb is None:
                        continue
                    box, w_px, d_px, _ = obb
                    if (comp[:, 0].min() < 2 or comp[:, 0].max() > 253 or
                            comp[:, 1].min() < 2 or comp[:, 1].max() > 253):
                        continue  # avoid registering clipped roofs at XYZ tile seams
                    fill = len(comp) / max(1.0, w_px * d_px)
                    if fill < 0.30:
                        continue
                    cx_px, cy_px = float(box[:, 0].mean()), float(box[:, 1].mean())
                    lat, lon = _pixel_latlon((tile_x + cx_px / TILE_PIXELS) * TILE_PIXELS,
                                             (tile_y + cy_px / TILE_PIXELS) * TILE_PIXELS, zoom)
                    boundary_polys = ap.get("boundary_polygons", [])
                    if boundary_polys:
                        if not any(_latlon_in_polygon(lat, lon, poly) for poly in boundary_polys):
                            continue
                    elif not _near_runway(lat, lon, ap):
                        continue

                    width_m, depth_m = w_px * scale, d_px * scale
                    area_m2 = width_m * depth_m
                    if (width_m < 5.0 or depth_m < 4.0 or area_m2 < 42.0 or area_m2 > 18000.0 or
                            max(width_m, depth_m) > 180.0 or width_m / max(depth_m, 0.1) > 8.5):
                        continue

                    ring: list[list[float]] = []
                    for bx, by in box:
                        blat, blon = _pixel_latlon((tile_x + float(bx) / TILE_PIXELS) * TILE_PIXELS,
                                                   (tile_y + float(by) / TILE_PIXELS) * TILE_PIXELS, zoom)
                        ring.append([round(blat, 7), round(blon, 7)])
                    ring.append(ring[0])

                    if _known_building_near(lat, lon, width_m, depth_m, features):
                        continue
                    if any(
                        math.hypot((float(f.get("lon", -999.0)) - lon) * 111320.0 * math.cos(math.radians(lat)),
                                   (float(f.get("lat", -999.0)) - lat) * 111132.0) < max(5.0, min(width_m, depth_m) * 0.38)
                        for f in features if "lat" in f and "lon" in f
                    ):
                        continue

                    resolution_score = min(1.0, min(w_px, d_px) / 8.0)
                    confidence = min(0.98, 0.54 + 0.28 * min(1.0, fill) + 0.18 * resolution_score)
                    if confidence < (0.76 if zoom <= 15 else 0.70):
                        totals["skipped_low_confidence"] += 1
                        continue
                    roof_rgb = np.median(rgb[comp[:, 1], comp[:, 0]], axis=0).astype(int).tolist()
                    kind, height = _roof_kind(area_m2, width_m, depth_m)
                    features.append({
                        "kind": kind,
                        "height_m": height,
                        "pts": ring,
                        "source": "orthophoto_vision",
                        "confidence": round(confidence, 3),
                        "roof_r": int(roof_rgb[0]),
                        "roof_g": int(roof_rgb[1]),
                        "roof_b": int(roof_rgb[2]),
                        "footprint_area_m2": round(area_m2, 1),
                        "imagery_zoom": zoom,
                    })
                    totals["detected"] += 1
                    added += 1
                    if added >= max_new:
                        break
                if added >= max_new:
                    break
            if added >= max_new:
                break
        if airport_has_images:
            totals["airports_with_imagery"] += 1
    return totals
