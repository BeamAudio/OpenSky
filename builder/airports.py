"""Fetch, merge, and normalize regional airports & airstrips (OurAirports + OSM).

Source licenses and attribution:
  - OurAirports: Public Domain
  - OpenStreetMap: ODbL (attribution required; rendered game assets are Produced Works)
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import time
from pathlib import Path
from typing import Any

import requests

OURAIRPORTS_BASE = "https://davidmegginson.github.io/ourairports-data"
OVERPASS_URLS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
]
USER_AGENT = "OpenSky/0.1 (regional scenery builder; open data)"

def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2.0 * r * math.asin(min(1.0, math.sqrt(a)))


def normalize_surface(raw: str | None) -> str:
    s = (raw or "").strip().upper()
    if any(k in s for k in ("CON", "PCC", "HORM")):
        return "concrete"
    if any(k in s for k in ("ASP", "ASF", "BIT", "PEM", "TAR", "PAV", "HARD", "MAC")):
        return "asphalt"
    if any(k in s for k in ("GRS", "GRASS", "TURF", "SOD", "HIERBA", "CESPED")):
        return "grass"
    if any(k in s for k in ("GVL", "GRAV", "COMPACT", "CRUSH", "ZAHORRA", "FINE_GRAVEL")):
        return "gravel"
    if any(k in s for k in ("DIRT", "EARTH", "CLAY", "SOIL", "SAND", "UNPAV", "TIERRA", "ARENA")):
        return "dirt"
    # An absent or unrecognised tag is not evidence of a paved or dirt strip.
    return "unknown"


def heading_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    dl = math.radians(lon2 - lon1)
    p1, p2 = math.radians(lat1), math.radians(lat2)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def ident_from_heading(hdg: float) -> str:
    n = int(round(hdg / 10.0)) % 36
    return f"{36 if n == 0 else n:02d}"


def same_runway(a: dict[str, Any], b: dict[str, Any]) -> bool:
    """Match overlapping, nearly collinear runway records, including different lengths."""
    lat = (a["lat_a"] + a["lat_b"] + b["lat_a"] + b["lat_b"]) / 4.0
    mlon = 111320.0 * math.cos(math.radians(lat))
    ax, ay = (a["lon_b"] - a["lon_a"]) * mlon, (a["lat_b"] - a["lat_a"]) * 111132.0
    bx, by = (b["lon_b"] - b["lon_a"]) * mlon, (b["lat_b"] - b["lat_a"]) * 111132.0
    al, bl = math.hypot(ax, ay), math.hypot(bx, by)
    if min(al, bl) < 100.0 or abs(ax * bx + ay * by) / (al * bl) < 0.985:
        return False
    dx = (b["lon_a"] + b["lon_b"] - a["lon_a"] - a["lon_b"]) * 0.5 * mlon
    dy = (b["lat_a"] + b["lat_b"] - a["lat_a"] - a["lat_b"]) * 0.5 * 111132.0
    return abs(dx * ay - dy * ax) / al < 35.0 and abs(dx * ax + dy * ay) / al < (al + bl) * 0.35


def _cached_download(url: str, cache_path: Path, max_age_s: int = 86400 * 7) -> bytes:
    if cache_path.exists() and (time.time() - cache_path.stat().st_mtime) < max_age_s:
        return cache_path.read_bytes()
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    r = requests.get(url, headers={"User-Agent": USER_AGENT}, timeout=30)
    r.raise_for_status()
    tmp = cache_path.with_suffix(cache_path.suffix + ".part")
    tmp.write_bytes(r.content)
    tmp.replace(cache_path)
    return r.content


def _in_bbox(lat: float, lon: float, bbox: tuple[float, float, float, float]) -> bool:
    return bbox[0] <= lon <= bbox[2] and bbox[1] <= lat <= bbox[3]


def fetch_ourairports(bbox: tuple[float, float, float, float], cache_dir: Path) -> list[dict[str, Any]]:
    oa_dir = cache_dir / "ourairports"
    ap_csv = _cached_download(f"{OURAIRPORTS_BASE}/airports.csv", oa_dir / "airports.csv").decode("utf-8", errors="replace")
    rw_csv = _cached_download(f"{OURAIRPORTS_BASE}/runways.csv", oa_dir / "runways.csv").decode("utf-8", errors="replace")
    fr_csv = _cached_download(f"{OURAIRPORTS_BASE}/airport-frequencies.csv", oa_dir / "airport-frequencies.csv").decode("utf-8", errors="replace")

    airports_by_id: dict[str, dict[str, Any]] = {}
    allowed = {"large_airport", "medium_airport", "small_airport"}
    for row in csv.DictReader(io.StringIO(ap_csv)):
        if row.get("type") not in allowed:
            continue
        try:
            lat = float(row["latitude_deg"])
            lon = float(row["longitude_deg"])
        except (ValueError, KeyError):
            continue
        if not _in_bbox(lat, lon, bbox):
            continue
        elev_ft = float(row["elevation_ft"]) if row.get("elevation_ft") else 0.0
        ident = (row.get("icao_code") or row.get("gps_code") or row.get("ident") or "").strip()
        airports_by_id[row["id"]] = {
            "id": ident or f"OA-{row['id']}",
            "name": (row.get("name") or ident).strip(),
            "kind": row["type"],
            "lat": lat,
            "lon": lon,
            "elev_m": round(elev_ft * 0.3048, 2),
            "frequencies": [],
            "runways": [],
            "aprons": [],
            "taxiways": [],
            "buildings": [],
            "fences": [],
            "access_roads": [],
            "windsocks": [],
        }

    for row in csv.DictReader(io.StringIO(fr_csv)):
        ap = airports_by_id.get(row.get("airport_ref", ""))
        if not ap:
            continue
        try:
            mhz = float(row["frequency_mhz"])
        except (ValueError, KeyError):
            continue
        ap["frequencies"].append({
            "type": (row.get("type") or "INFO").strip(),
            "mhz": round(mhz, 3),
            "desc": (row.get("description") or "").strip(),
        })

    for row in csv.DictReader(io.StringIO(rw_csv)):
        ap = airports_by_id.get(row.get("airport_ref", ""))
        if not ap or row.get("closed") == "1":
            continue
        try:
            length_m = float(row["length_ft"]) * 0.3048 if row.get("length_ft") else 600.0
            width_m = float(row["width_ft"]) * 0.3048 if row.get("width_ft") else 20.0
        except ValueError:
            length_m, width_m = 600.0, 20.0
        if length_m < 180.0:
            continue
        surface = normalize_surface(row.get("surface"))
        lit = row.get("lighted") == "1"
        ident_a = (row.get("le_ident") or "").strip()
        ident_b = (row.get("he_ident") or "").strip()

        lat_a = float(row["le_latitude_deg"]) if row.get("le_latitude_deg") else None
        lon_a = float(row["le_longitude_deg"]) if row.get("le_longitude_deg") else None
        lat_b = float(row["he_latitude_deg"]) if row.get("he_latitude_deg") else None
        lon_b = float(row["he_longitude_deg"]) if row.get("he_longitude_deg") else None

        if None in (lat_a, lon_a, lat_b, lon_b):
            # Reconstruct endpoints from heading or ident number when OA omits endpoint coords.
            hdg = None
            if row.get("le_heading_degT"):
                try:
                    hdg = float(row["le_heading_degT"])
                except ValueError:
                    hdg = None
            if hdg is None and ident_a[:2].isdigit():
                hdg = int(ident_a[:2]) * 10.0
            if hdg is None:
                hdg = 90.0
            rad = math.radians(hdg)
            half = length_m * 0.5
            m_lat = 111132.0
            m_lon = 111320.0 * math.cos(math.radians(ap["lat"]))
            d_n = math.cos(rad) * half / m_lat
            d_e = math.sin(rad) * half / m_lon
            lat_a, lon_a = ap["lat"] - d_n, ap["lon"] - d_e
            lat_b, lon_b = ap["lat"] + d_n, ap["lon"] + d_e

        actual_len = haversine_m(lat_a, lon_a, lat_b, lon_b)
        hdg_ab = heading_deg(lat_a, lon_a, lat_b, lon_b)
        if not ident_a:
            ident_a = ident_from_heading(hdg_ab)
        if not ident_b:
            ident_b = ident_from_heading((hdg_ab + 180.0) % 360.0)

        ap["runways"].append({
            "ident_a": ident_a,
            "ident_b": ident_b,
            "lat_a": round(lat_a, 7),
            "lon_a": round(lon_a, 7),
            "lat_b": round(lat_b, 7),
            "lon_b": round(lon_b, 7),
            "length_m": round(actual_len, 1),
            "width_m": round(max(12.0, min(width_m, 75.0)), 1),
            "surface": surface,
            "lit": lit,
        })

    # Ensure every airport has at least one frequency (Spanish VFR CTAF 123.500 default).
    for ap in airports_by_id.values():
        if not ap["frequencies"]:
            ap["frequencies"].append({"type": "CTAF", "mhz": 123.500, "desc": f"{ap['name']} Traffic"})

    return list(airports_by_id.values())


def fetch_osm_aeroways(bbox: tuple[float, float, float, float], cache_dir: Path) -> dict[str, Any]:
    """Query Overpass for airfields, airport boundaries, surfaces, and buildings."""
    lon_min, lat_min, lon_max, lat_max = bbox
    key = hashlib.sha1(f"airport-boundaries-v2:{lon_min:.4f},{lat_min:.4f},{lon_max:.4f},{lat_max:.4f}".encode()).hexdigest()[:12]
    cache_path = cache_dir / "osm" / f"aeroways_{key}.json"
    if cache_path.exists():
        return json.loads(cache_path.read_text(encoding="utf-8"))

    # Keep the region-wide Overpass request limited to aviation features.
    query = (
        f"[out:json][timeout:35];("
        f'way["aeroway"~"^(runway|airstrip|taxiway|apron|terminal|hangar|control_tower|aerodrome)$"]({lat_min},{lon_min},{lat_max},{lon_max});'
        f'node["aeroway"~"^(aerodrome|windsock)$"]({lat_min},{lon_min},{lat_max},{lon_max});'
        f");out body;>;out skel qt;"
    )
    for url in OVERPASS_URLS:
        try:
            r = requests.post(url, data={"data": query}, headers={"User-Agent": USER_AGENT}, timeout=40)
            if r.ok and r.headers.get("content-type", "").startswith("application/json"):
                data = r.json()
                cache_path.parent.mkdir(parents=True, exist_ok=True)
                cache_path.write_text(json.dumps(data), encoding="utf-8")
                return data
        except Exception:
            continue
    return {"elements": []}


def _parse_osm_width(val: str | None, default_m: float) -> float:
    if not val:
        return default_m
    tok = val.strip().lower().replace("m", "").split()[0]
    try:
        return max(10.0, min(float(tok), 75.0))
    except ValueError:
        return default_m


def _point_in_polygon(lat: float, lon: float, pts: list[list[float]]) -> bool:
    if len(pts) < 3:
        return False
    inside = False
    j = len(pts) - 1
    for i, (py, px) in enumerate(pts):
        yj, xj = pts[j]
        if ((py > lat) != (yj > lat)) and lon < (xj - px) * (lat - py) / ((yj - py) or 1e-15) + px:
            inside = not inside
        j = i
    return inside


def _dist_point_to_runways_m(lat: float, lon: float, ap: dict[str, Any]) -> float:
    mlon = 111320.0 * math.cos(math.radians(lat))
    best = haversine_m(lat, lon, ap["lat"], ap["lon"])
    for rw in ap.get("runways", []):
        ax, ay = (rw["lon_a"] - lon) * mlon, (rw["lat_a"] - lat) * 111132.0
        bx, by = (rw["lon_b"] - lon) * mlon, (rw["lat_b"] - lat) * 111132.0
        dx, dy = bx - ax, by - ay
        l2 = dx * dx + dy * dy
        t = max(0.0, min(1.0, (-ax * dx - ay * dy) / l2)) if l2 > 1e-4 else 0.0
        d = math.hypot(ax + t * dx, ay + t * dy)
        if d < best:
            best = d
    return best


def _airport_contains_or_near(lat: float, lon: float, ap: dict[str, Any], margin_m: float) -> bool:
    for poly in ap.get("boundary_polygons", []):
        if _point_in_polygon(lat, lon, poly):
            return True
    return _dist_point_to_runways_m(lat, lon, ap) <= margin_m


def _surface_priority(surface: str) -> int:
    return {"asphalt": 5, "concrete": 5, "grass": 4, "gravel": 3, "dirt": 2, "unknown": 0}.get(surface, 1)


def _fetch_osm_airport_ground_overpass(airports: list[dict[str, Any]], cache_dir: Path) -> dict[str, Any]:
    """Fetch fences, access roads, and airport buildings around each airport via Overpass."""
    if not airports:
        return {"elements": []}
    sig = "|".join(f"{a['id']}:{a['lat']:.4f},{a['lon']:.4f}" for a in airports)
    key = hashlib.sha1(f"airport-ground-v1:{sig}".encode()).hexdigest()[:12]
    cache_path = cache_dir / "osm" / f"airport_ground_{key}.json"
    if cache_path.exists():
        return json.loads(cache_path.read_text(encoding="utf-8"))

    clauses: list[str] = []
    for ap in airports:
        rad = 1100 if ap["kind"] == "large_airport" else (750 if ap["kind"] == "medium_airport" else 340)
        lat, lon = ap["lat"], ap["lon"]
        clauses.append(f'way["barrier"~"^(fence|wall|hedge)$"](around:{rad},{lat:.5f},{lon:.5f});')
        clauses.append(f'way["highway"~"^(service|unclassified|track|residential)$"](around:{rad},{lat:.5f},{lon:.5f});')
        clauses.append(f'way["building"](around:{min(rad, 500)},{lat:.5f},{lon:.5f});')
    query = "[out:json][timeout:45];(" + "".join(clauses) + ");out body;>;out skel qt;"
    for url in OVERPASS_URLS:
        try:
            r = requests.post(url, data={"data": query}, headers={"User-Agent": USER_AGENT}, timeout=50)
            if r.ok and r.headers.get("content-type", "").startswith("application/json"):
                data = r.json()
                cache_path.parent.mkdir(parents=True, exist_ok=True)
                cache_path.write_text(json.dumps(data), encoding="utf-8")
                return data
        except Exception:
            continue
    return {"elements": []}


def _enrich_airports_from_pbf(
    airports: list[dict[str, Any]], pbf_paths: list[Path], bbox: tuple[float, float, float, float]
) -> dict[str, Any] | None:
    """Extract aeroways, fences, access roads, buildings, and windsocks from local OSM PBFs."""
    try:
        import sys
        pydeps = Path(__file__).resolve().parent.parent / ".tools" / "pydeps"
        if pydeps.exists() and str(pydeps) not in sys.path:
            sys.path.insert(0, str(pydeps))
        import osmium  # type: ignore
    except ImportError:
        return None

    existing = [p for p in pbf_paths if p.exists()]
    if not existing:
        return None

    # Pre-compute fast bounding boxes per airport (generous padding so large hubs are covered).
    ap_boxes: list[tuple[dict[str, Any], float, float, float, float]] = []
    for ap in airports:
        rad_m = 2600.0 if ap["kind"] == "large_airport" else (1600.0 if ap["kind"] == "medium_airport" else 750.0)
        dlat = rad_m / 111132.0
        dlon = rad_m / max(1000.0, 111320.0 * math.cos(math.radians(ap["lat"])))
        lons = [ap["lon"]] + [r["lon_a"] for r in ap["runways"]] + [r["lon_b"] for r in ap["runways"]]
        lats = [ap["lat"]] + [r["lat_a"] for r in ap["runways"]] + [r["lat_b"] for r in ap["runways"]]
        ap_boxes.append((ap, min(lons) - dlon, min(lats) - dlat, max(lons) + dlon, max(lats) + dlat))

    class _Handler(osmium.SimpleHandler):
        def __init__(self) -> None:
            super().__init__()
            self.aeroways: list[dict[str, Any]] = []
            self.ground: list[dict[str, Any]] = []
            self.windsocks: list[tuple[float, float]] = []

        def _near_any_ap(self, lat: float, lon: float) -> bool:
            for _, x0, y0, x1, y1 in ap_boxes:
                if x0 <= lon <= x1 and y0 <= lat <= y1:
                    return True
            return False

        def node(self, n: Any) -> None:
            if n.tags.get("aeroway") == "windsock" and n.location.valid():
                lat, lon = n.location.lat, n.location.lon
                if _in_bbox(lat, lon, bbox):
                    self.windsocks.append((lat, lon))

        def way(self, w: Any) -> None:
            tags = w.tags
            aw = tags.get("aeroway")
            bar = tags.get("barrier")
            bld = tags.get("building")
            hw = tags.get("highway")
            want_aw = aw in ("runway", "airstrip", "taxiway", "apron", "terminal", "hangar", "control_tower", "aerodrome")
            want_gr = (bar in ("fence", "wall", "hedge")) or (bld and bld != "no") or (hw in ("service", "unclassified", "track", "residential"))
            if not (want_aw or want_gr):
                return
            pts = [(nd.location.lat, nd.location.lon) for nd in w.nodes if nd.location.valid()]
            if len(pts) < 2:
                return
            clat = sum(p[0] for p in pts) / len(pts)
            clon = sum(p[1] for p in pts) / len(pts)
            if not _in_bbox(clat, clon, bbox):
                return
            tag_dict = {k: tags[k] for k in ("aeroway", "ref", "name", "width", "surface", "lit", "height", "building", "building:levels", "barrier", "highway", "service") if k in tags}
            if want_aw:
                self.aeroways.append({"tags": tag_dict, "pts": pts})
            elif want_gr and self._near_any_ap(clat, clon):
                self.ground.append({"tags": tag_dict, "pts": pts})

    handler = _Handler()
    for pbf in existing:
        handler.apply_file(str(pbf), locations=True)
    return {"aeroways": handler.aeroways, "ground": handler.ground, "windsocks": handler.windsocks}


def merge_airports(
    bbox: tuple[float, float, float, float],
    cache_dir: Path,
    use_osm: bool = True,
    pbf_paths: list[Path] | None = None,
) -> list[dict[str, Any]]:
    airports = fetch_ourairports(bbox, cache_dir)

    # OurAirports occasionally contains two records for the same field (e.g.,
    # Castejon de Sos ES-0205 grass + ES-0347 untagged). Merge coincident
    # records, keeping the explicit surface and primary ID.
    airports.sort(key=lambda a: (
        not bool(a["runways"]),
        -max((_surface_priority(r["surface"]) for r in a["runways"]), default=0),
        a["id"],
    ))
    merged: list[dict[str, Any]] = []
    for airport in airports:
        previous = next((a for a in merged if haversine_m(
            a["lat"], a["lon"], airport["lat"], airport["lon"]) < 80.0), None)
        if previous is None:
            merged.append(airport)
        else:
            previous.setdefault("aliases", []).append(airport["id"])
            for rw in airport["runways"]:
                match = next((r for r in previous["runways"] if same_runway(r, rw)), None)
                if match is None:
                    previous["runways"].append(rw)
                else:
                    if _surface_priority(rw["surface"]) > _surface_priority(match["surface"]):
                        match["surface"] = rw["surface"]
                    if rw["length_m"] > match["length_m"]:
                        match.update(lat_a=rw["lat_a"], lon_a=rw["lon_a"], lat_b=rw["lat_b"], lon_b=rw["lon_b"],
                                     length_m=rw["length_m"], ident_a=rw["ident_a"], ident_b=rw["ident_b"])
    airports = merged

    # Enrich & discover additional obscure strips + ground-level airport features from OSM.
    if use_osm:
        resolved_pbfs = list(pbf_paths) if pbf_paths else []
        if not resolved_pbfs:
            osm_dir = cache_dir / "osm"
            if osm_dir.is_dir():
                resolved_pbfs = sorted(osm_dir.glob("*.osm.pbf"))

        pbf_data = _enrich_airports_from_pbf(airports, resolved_pbfs, bbox) if resolved_pbfs else None
        aeroway_ways: list[dict[str, Any]] = []
        ground_ways: list[dict[str, Any]] = []
        windsock_pts: list[tuple[float, float]] = []

        if pbf_data is not None:
            aeroway_ways = pbf_data["aeroways"]
            ground_ways = pbf_data["ground"]
            windsock_pts = pbf_data["windsocks"]
        else:
            osm = fetch_osm_aeroways(bbox, cache_dir)
            nodes: dict[int, tuple[float, float]] = {}
            for el in osm.get("elements", []):
                if el["type"] == "node":
                    nodes[el["id"]] = (el["lat"], el["lon"])
                    if el.get("tags", {}).get("aeroway") == "windsock":
                        windsock_pts.append((el["lat"], el["lon"]))
            for el in osm.get("elements", []):
                if el["type"] == "way":
                    pts = [nodes[nid] for nid in el.get("nodes", []) if nid in nodes]
                    if len(pts) >= 2:
                        aeroway_ways.append({"tags": el.get("tags", {}), "pts": pts})

        def nearest_airport(lat: float, lon: float, max_dist_m: float) -> dict[str, Any] | None:
            best, best_d = None, max_dist_m
            for ap in airports:
                d = haversine_m(lat, lon, ap["lat"], ap["lon"])
                if d < best_d:
                    best, best_d = ap, d
            return best

        # Pass 1: discover obscure strips & upgrade matched OurAirports runways to surveyed OSM geometry/surface
        osm_idx = 1
        for w in aeroway_ways:
            tags = w["tags"]
            aw = tags.get("aeroway")
            if aw not in ("runway", "airstrip"):
                continue
            pts = w["pts"]
            if len(pts) < 2:
                continue
            if pts[0] == pts[-1] and len(pts) >= 4:
                best_pair = (pts[0], pts[1])
                best_d = 0.0
                for i in range(len(pts)):
                    for j in range(i + 1, len(pts)):
                        d = haversine_m(pts[i][0], pts[i][1], pts[j][0], pts[j][1])
                        if d > best_d:
                            best_d, best_pair = d, (pts[i], pts[j])
                (lat_a, lon_a), (lat_b, lon_b) = best_pair
            else:
                lat_a, lon_a = pts[0]
                lat_b, lon_b = pts[-1]
            length_m = haversine_m(lat_a, lon_a, lat_b, lon_b)
            if length_m < 220.0:
                continue
            clat, clon = (lat_a + lat_b) * 0.5, (lon_a + lon_b) * 0.5
            ap = None
            best_d = 1e9
            for cand in airports:
                lim = 4500.0 if cand["kind"] == "large_airport" else (2800.0 if cand["kind"] == "medium_airport" else 1800.0)
                d = haversine_m(clat, clon, cand["lat"], cand["lon"])
                if d < lim and d < best_d:
                    best_d, ap = d, cand
            hdg_ab = heading_deg(lat_a, lon_a, lat_b, lon_b)
            ref = tags.get("ref", "")
            parts = [p.strip() for p in ref.split("/") if p.strip()]
            ident_a = parts[0] if len(parts) >= 1 else ident_from_heading(hdg_ab)
            ident_b = parts[1] if len(parts) >= 2 else ident_from_heading((hdg_ab + 180.0) % 360.0)
            osm_surf = normalize_surface(tags.get("surface"))
            rw_obj = {
                "ident_a": ident_a,
                "ident_b": ident_b,
                "lat_a": round(lat_a, 7),
                "lon_a": round(lon_a, 7),
                "lat_b": round(lat_b, 7),
                "lon_b": round(lon_b, 7),
                "length_m": round(length_m, 1),
                "width_m": _parse_osm_width(tags.get("width"), 18.0 if aw == "airstrip" else 28.0),
                "surface": osm_surf,
                "lit": tags.get("lit") == "yes",
            }
            if ap is None:
                name = tags.get("name") or f"Pista VFR #{osm_idx}"
                airports.append({
                    "id": f"OSM-{osm_idx:03d}",
                    "name": name,
                    "kind": "airstrip",
                    "lat": round(clat, 6),
                    "lon": round(clon, 6),
                    "elev_m": 0.0,
                    "frequencies": [{"type": "CTAF", "mhz": 123.500, "desc": f"{name} CTAF"}],
                    "runways": [rw_obj],
                    "aprons": [],
                    "taxiways": [],
                    "buildings": [],
                    "fences": [],
                    "access_roads": [],
                    "windsocks": [],
                })
                osm_idx += 1
            else:
                match = next((r for r in ap["runways"] if same_runway(r, rw_obj)), None)
                if match is None:
                    ap["runways"].append(rw_obj)
                else:
                    # Upgrade to surveyed OSM centerline & explicit OSM surface (keeping OA surface if OSM omits it).
                    keep_surf = osm_surf if osm_surf != "unknown" else match["surface"]
                    keep_width = rw_obj["width_m"] if tags.get("width") else match["width_m"]
                    match.update(
                        lat_a=rw_obj["lat_a"], lon_a=rw_obj["lon_a"],
                        lat_b=rw_obj["lat_b"], lon_b=rw_obj["lon_b"],
                        length_m=rw_obj["length_m"], width_m=keep_width,
                        surface=keep_surf, lit=match["lit"] or rw_obj["lit"],
                    )
                    if parts:
                        match["ident_a"], match["ident_b"] = ident_a, ident_b

        # Pass 2: attach boundaries, taxiways, aprons, and aeroway buildings
        for w in aeroway_ways:
            tags = w["tags"]
            aw = tags.get("aeroway")
            if aw not in ("taxiway", "apron", "terminal", "hangar", "control_tower", "aerodrome"):
                continue
            pts = w["pts"]
            if len(pts) < 2:
                continue
            clat = sum(p[0] for p in pts) / len(pts)
            clon = sum(p[1] for p in pts) / len(pts)
            ap = nearest_airport(clat, clon, 3500.0)
            if ap is None:
                continue
            coords = [[round(p[0], 6), round(p[1], 6)] for p in pts]
            if aw == "aerodrome" and len(pts) >= 4 and pts[0] == pts[-1] and len(ap.setdefault("boundary_polygons", [])) < 8:
                ap["boundary_polygons"].append(coords)
            elif aw == "taxiway" and len(ap["taxiways"]) < 120:
                ap["taxiways"].append({
                    "width_m": _parse_osm_width(tags.get("width"), 15.0),
                    "surface": normalize_surface(tags.get("surface")),
                    "pts": coords,
                })
            elif aw == "apron" and len(pts) >= 4 and len(ap["aprons"]) < 32:
                ap["aprons"].append({"surface": normalize_surface(tags.get("surface")), "pts": coords})
            elif aw in ("terminal", "hangar", "control_tower") and len(pts) >= 4 and len(ap["buildings"]) < 240:
                default_h = {"terminal": 14.0, "hangar": 8.5, "control_tower": 28.0}[aw]
                try:
                    h = max(3.0, min(120.0, float((tags.get("height") or "").lower().replace("m", "").strip())))
                except ValueError:
                    h = default_h
                ap["buildings"].append({
                    "kind": aw,
                    "height_m": h,
                    "pts": coords,
                    "source": "openstreetmap",
                })

        # Pass 3: attach windsocks + ground-level fences, access roads, and airport buildings
        for wlat, wlon in windsock_pts:
            ap = nearest_airport(wlat, wlon, 2200.0)
            if ap is not None and len(ap.setdefault("windsocks", [])) < 8:
                ap["windsocks"].append([round(wlat, 6), round(wlon, 6)])

        if pbf_data is None and airports:
            ov = _fetch_osm_airport_ground_overpass(airports, cache_dir)
            gnodes: dict[int, tuple[float, float]] = {}
            for el in ov.get("elements", []):
                if el["type"] == "node":
                    gnodes[el["id"]] = (el["lat"], el["lon"])
            for el in ov.get("elements", []):
                if el["type"] == "way":
                    pts = [gnodes[nid] for nid in el.get("nodes", []) if nid in gnodes]
                    if len(pts) >= 2:
                        ground_ways.append({"tags": el.get("tags", {}), "pts": pts})

        for w in ground_ways:
            tags = w["tags"]
            pts = w["pts"]
            clat = sum(p[0] for p in pts) / len(pts)
            clon = sum(p[1] for p in pts) / len(pts)
            ap = nearest_airport(clat, clon, 3200.0)
            if ap is None:
                continue
            margin = 750.0 if ap["kind"] == "large_airport" else (480.0 if ap["kind"] == "medium_airport" else 260.0)
            if not _airport_contains_or_near(clat, clon, ap, margin):
                continue
            coords = [[round(p[0], 6), round(p[1], 6)] for p in pts]
            bar = tags.get("barrier")
            hw = tags.get("highway")
            bld = tags.get("building")
            if bar in ("fence", "wall", "hedge") and len(ap.setdefault("fences", [])) < 96:
                ap["fences"].append({"kind": bar, "pts": coords})
            elif hw in ("service", "unclassified", "track", "residential") and len(ap.setdefault("access_roads", [])) < 96:
                surf = normalize_surface(tags.get("surface"))
                if surf == "unknown":
                    surf = "gravel" if hw == "track" else "asphalt"
                width = 4.0 if hw == "track" else (5.0 if hw == "service" else 6.0)
                ap["access_roads"].append({"surface": surf, "width_m": width, "pts": coords})
            elif bld and bld != "no" and len(pts) >= 4 and len(ap["buildings"]) < 240:
                if not _airport_contains_or_near(clat, clon, ap, min(margin, 210.0)):
                    continue
                b_low = bld.lower()
                kind = "hangar" if b_low in ("hangar", "industrial", "warehouse") else ("terminal" if b_low in ("terminal", "commercial", "public") else "airport_aux")
                default_h = 8.0 if kind == "hangar" else (10.0 if kind == "terminal" else 5.5)
                try:
                    h = max(3.0, min(60.0, float((tags.get("height") or "").lower().replace("m", "").strip())))
                except ValueError:
                    try:
                        h = max(3.5, min(40.0, float(tags.get("building:levels") or "") * 3.2))
                    except ValueError:
                        h = default_h
                # Skip duplicate building if already recorded by aeroway pass.
                dup_b = any(
                    haversine_m(clat, clon, sum(p[0] for p in b["pts"]) / len(b["pts"]), sum(p[1] for p in b["pts"]) / len(b["pts"])) < 6.0
                    for b in ap["buildings"] if b.get("pts")
                )
                if not dup_b:
                    ap["buildings"].append({"kind": kind, "height_m": h, "pts": coords, "source": "openstreetmap"})

    # Keep only airfields with at least one valid runway, sorted largest-first then by ID.
    rank = {"large_airport": 0, "medium_airport": 1, "small_airport": 2, "airstrip": 3}
    airports = [a for a in airports if a.get("runways")]
    airports.sort(key=lambda a: (rank.get(a["kind"], 9), a["id"]))
    return airports
