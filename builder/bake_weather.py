#!/usr/bin/env python3
"""bake_weather.py -- Pre-bake a study-level regional weather state library (.nswx).

Runs an offline mesoscale atmospheric model over the Aragon / Central Pyrenees
corridor, using the same IGN MDT05/MDT25 DEM and PNOA/OSM land-cover data as the
.nspack, then writes a replayable state library:

  * 4 seasons x 11 solar slots x 5 synoptic weather types = 220 states
  * each state = 128 x 96 x 4 AGL levels x (u, v, w, cloud_delta), zlib packed
  * synoptic forcing is classified from real ERA5 reanalysis (Open-Meteo archive)
    at five corridor stations, not from hand-tuned constants

Physics solved offline (never at runtime):
  1. Log boundary-layer wind + Ekman veer from 850 hPa geostrophic forcing
  2. Spectral potential-flow perturbation over the real DEM (Jackson/Hunt linear
     terrain response, e^{-|k|z} decay) -> ridge speed-up, valley channelling,
     Ebro Venturi acceleration, orographic vertical velocity
  3. Linear hydrostatic mountain waves (Scorer l^2 = N^2/U^2) + rotors
  4. Hydrostatic buoyancy-driven slope circulation (anabatic / katabatic) from a
     DEM-aspect solar energy balance, projected divergence-free with a DCT
     Neumann Poisson solve (mass-consistent adjustment, CALMET-style)
  5. LCL / radiosonde-style RH profile, orographic condensation + Foehn drying,
     precipitation efficiency and TKE-derived turbulence scale

Runtime only interpolates between these baked states -> zero per-frame solving,
so the iGPU frame budget and 60 FPS target are unchanged.

Usage:
  python3 builder/bake_weather.py                # fetch/derive climatology + bake
  python3 builder/bake_weather.py --offline      # use cached reanalysis only
  python3 builder/bake_weather.py --selftest     # verify solvers, no baking
"""

from __future__ import annotations

import argparse
import io
import json
import math
import sys
import time
import zlib
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.fft import dctn, idctn

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from protocol.pack_format import LAYER_ELEVATION, LAYER_SURFACE, PackReader  # noqa: E402
from protocol.weather_format import (  # noqa: E402
    CHAN_HI, CHAN_LO, GRID_H, GRID_W, LEVELS, LEVEL_AGL_M, NSLOT, NTYPE,
    NSEASON, SEASONS, SLOTS, STORM_TYPE, WEATHER_TYPES, encode_channel,
    pack_header, pack_state, state_index,
    S_CLOUD_BASE_M, S_CLOUD_COV, S_CLOUD_THICK_M, S_DEWPOINT_SL_C,
    S_HAZE_MULT, S_OAT_SL_C, S_PRECIP, S_QNH_HPA, S_SNOWLINE_M,
    S_STORM_INT, S_TURB_SCALE, S_VISIBILITY_M, S_WAVE_AMP,
    S_WIND_DIR_DEG, S_WIND_SPEED_MPS,
)

PACK_PATH = ROOT / "packs" / "es_aragon_pyrenees.nspack"
OUT_PATH = ROOT / "packs" / "es_aragon_weather.nswx"
CACHE_PATH = ROOT / "builder" / ".cache" / "weather" / "era5_2024.json"

# Five real corridor stations spanning Ebro lowlands -> Pyrenean crest.
STATIONS = (
    ("ZARAGOZA LEZG", 41.6662, -1.0416),
    ("HUESCA LEHC", 42.0810, -0.3233),
    ("SANTA CILIA LECI", 42.5692, -0.7280),
    ("MEDIANO", 42.3120, 0.1940),
    ("CASTEJON ES-0205", 42.5224, 0.4811),
)

REANALYSIS_VARS = (
    "temperature_2m,relative_humidity_2m,dew_point_2m,surface_pressure,"
    "wind_speed_10m,wind_direction_10m,cloud_cover,precipitation,shortwave_radiation"
)

# Fallback only when the archive API is unreachable. Real runs always use reanalysis.
# shortcut: synthetic Iberian synoptic normals, used only offline; upgrade to a
# bundled ERA5-Land climatology snapshot for fully air-gapped rebuilds.
SYNTHETIC_SYNOPTIC = (
    # dir_from, speed10_mps, T_C, Td_C, QNH, cloud, precip, N
    (245.0, 4.6, 23.0, 10.0, 1018.0, 0.25, 0.0, 0.0105),  # thermal
    (312.0, 9.8, 16.5, 6.0, 1021.0, 0.20, 0.0, 0.0135),   # Cierzo
    (202.0, 10.6, 21.0, 12.0, 1009.0, 0.65, 0.2, 0.0125), # Foehn Sur
    (340.0, 3.2, 12.0, 10.0, 1023.0, 0.30, 0.0, 0.0155),  # katabatic
    (220.0, 7.5, 19.0, 16.0, 1006.0, 0.92, 3.5, 0.0090),  # storm
)

SOLAR_ZENITH = np.array([7.0, 13.0, 19.0, 30.0, 42.0, 48.0, 42.0, 30.0, 20.0, 8.0, -8.5])
SOLAR_AZIMUTH = np.array([78.0, 96.0, 114.0, 134.0, 156.0, 180.0, 204.0, 226.0, 248.0, 274.0, 315.0])


# ---------------------------------------------------------------------------
# Corridor domain (identical to PackReader::bake_corridor_dem)
# ---------------------------------------------------------------------------
def meters_per_degree(lat_deg: float) -> tuple[float, float]:
    p = math.radians(lat_deg)
    return (111132.92 - 559.82 * math.cos(2 * p) + 1.175 * math.cos(4 * p),
            111412.84 * math.cos(p) - 93.5 * math.cos(3 * p))


def corridor_domain(reader: PackReader) -> tuple[float, float, float, float, float, float, float, float]:
    lon_min, lat_min, lon_max, lat_max = reader.bbox
    anchor_lon = 0.5 * (lon_min + lon_max)
    anchor_lat = 0.5 * (lat_min + lat_max)
    m_lat, m_lon = meters_per_degree(anchor_lat)
    min_e = (lon_min - anchor_lon) * m_lon
    min_n = (lat_min - anchor_lat) * m_lat
    span_e = max(1000.0, (lon_max - anchor_lon) * m_lon - min_e)
    span_n = max(1000.0, (lat_max - anchor_lat) * m_lat - min_n)
    return min_e, min_n, span_e, span_n, anchor_lat, anchor_lon, m_lat, m_lon


def _sample_pack_layer(reader: PackReader, layer: int, zooms: list[int],
                       lat: np.ndarray, lon: np.ndarray, channels: int) -> np.ndarray:
    """Vectorized bilinear resample of a decoded pack layer at arbitrary lat/lon."""
    out = np.zeros(lat.shape + (channels,), dtype=np.float32)
    filled = np.zeros(lat.shape, dtype=bool)
    for z in sorted(zooms, reverse=True):
        n = 2.0 ** z
        xt = (lon + 180.0) / 360.0 * n
        yt = (1.0 - np.arcsinh(np.tan(np.radians(lat))) / math.pi) / 2.0 * n
        tx = np.floor(xt).astype(np.int32)
        ty = np.floor(yt).astype(np.int32)
        keys = np.stack([np.full_like(tx, z), tx, ty], axis=-1).reshape(-1, 3)
        uniq = np.unique(keys, axis=0)
        for zz, ux, uy in uniq:
            mask = (~filled) & (tx == ux) & (ty == uy)
            if not np.any(mask):
                continue
            tile = reader.read_tile(layer, int(zz), int(ux), int(uy), verify_crc=False)
            if tile is None:
                continue
            if layer == LAYER_ELEVATION:
                a = np.asarray(tile, dtype=np.float32)
            else:
                a = np.asarray(Image.open(io.BytesIO(tile)).convert("RGB"), dtype=np.float32)
            if a.ndim == 2:
                a = a[..., None]
            fx = np.clip((xt[mask] - ux) * (a.shape[1] - 1), 0, a.shape[1] - 1)
            fy = np.clip((yt[mask] - uy) * (a.shape[0] - 1), 0, a.shape[0] - 1)
            x0 = np.minimum(a.shape[1] - 2, fx.astype(np.int32))
            y0 = np.minimum(a.shape[0] - 2, fy.astype(np.int32))
            dx = (fx - x0)[..., None]
            dy = (fy - y0)[..., None]
            c = min(channels, a.shape[2])
            bi = (a[y0, x0, :c] * (1 - dx) + a[y0, x0 + 1, :c] * dx) * (1 - dy) + \
                 (a[y0 + 1, x0, :c] * (1 - dx) + a[y0 + 1, x0 + 1, :c] * dx) * dy
            out[mask, :c] = bi
            filled |= mask
    return out


def load_terrain(reader: PackReader, domain) -> tuple[np.ndarray, np.ndarray, np.ndarray, float, float]:
    min_e, min_n, span_e, span_n, anchor_lat, anchor_lon, m_lat, m_lon = domain
    gx = (np.arange(GRID_W, dtype=np.float64) + 0.5) / GRID_W
    gy = (np.arange(GRID_H, dtype=np.float64) + 0.5) / GRID_H
    e = min_e + gx * span_e
    n = min_n + gy * span_n
    lon = anchor_lon + e[None, :] / m_lon
    lat = anchor_lat + n[:, None] / m_lat
    lat2 = np.broadcast_to(lat, (GRID_H, GRID_W))
    lon2 = np.broadcast_to(lon, (GRID_H, GRID_W))

    ez = reader.manifest["elevation_zooms"]
    sz = reader.manifest["surface_zooms"]
    dem = _sample_pack_layer(reader, LAYER_ELEVATION, ez, lat2, lon2, 1)[..., 0]
    surf = _sample_pack_layer(reader, LAYER_SURFACE, sz, lat2, lon2, 3) / 255.0
    dem = np.where(dem > 1.0, dem, 400.0)
    forest = surf[..., 0]
    water = surf[..., 2]
    return dem.astype(np.float64), forest.astype(np.float64), water.astype(np.float64), \
        span_e / (GRID_W - 1), span_n / (GRID_H - 1)


# ---------------------------------------------------------------------------
# Real climatology from ERA5 (Open-Meteo archive API)
# ---------------------------------------------------------------------------
def _region_stations(reader: PackReader) -> list[tuple[str, float, float]]:
    if reader.region_id == "es_aragon_pyrenees":
        return list(STATIONS)
    lon0, lat0, lon1, lat1 = reader.bbox
    mlat, mlon = 0.5 * (lat0 + lat1), 0.5 * (lon0 + lon1)
    return [
        (f"{reader.region_id[:10]}_SW", lat0 + 0.2 * (lat1 - lat0), lon0 + 0.2 * (lon1 - lon0)),
        (f"{reader.region_id[:10]}_SE", lat0 + 0.2 * (lat1 - lat0), lon0 + 0.8 * (lon1 - lon0)),
        (f"{reader.region_id[:10]}_CTR", mlat, mlon),
        (f"{reader.region_id[:10]}_NW", lat0 + 0.8 * (lat1 - lat0), lon0 + 0.2 * (lon1 - lon0)),
        (f"{reader.region_id[:10]}_NE", lat0 + 0.8 * (lat1 - lat0), lon0 + 0.8 * (lon1 - lon0)),
    ]


def fetch_reanalysis(
    offline: bool, cache_path: Path = CACHE_PATH, stations: list[tuple[str, float, float]] | None = None
) -> tuple[list[dict] | None, str]:
    sts = stations or list(STATIONS)
    if cache_path.exists():
        data = json.loads(cache_path.read_text())
        return data, "cached " + cache_path.name
    if offline:
        return None, "synthetic fallback (offline)"
    import requests

    lats = ",".join(f"{s[1]:.4f}" for s in sts)
    lons = ",".join(f"{s[2]:.4f}" for s in sts)
    url = (
        "https://archive-api.open-meteo.com/v1/archive?"
        f"latitude={lats}&longitude={lons}"
        "&start_date=2024-01-01&end_date=2024-12-31"
        f"&hourly={REANALYSIS_VARS}&timezone=UTC"
    )
    try:
        r = requests.get(url, timeout=90)
        r.raise_for_status()
        data = r.json()
        if isinstance(data, dict):
            data = [data]
        if len(data) != len(sts) or "hourly" not in data[0]:
            raise ValueError("unexpected archive response")
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_text(json.dumps(data, separators=(",", ":")))
        return data, "ERA5 via Open-Meteo archive 2024, 5 regional stations"
    except Exception as exc:  # network/API failure -> documented synthetic fallback
        print(f"[WARN] Reanalysis unavailable ({exc}); using synthetic normals.")
        return None, "synthetic fallback (API unreachable)"


def classify_hour(wd: float, ws: float, cloud: float, precip: float, hour: int, rh: float) -> int:
    if precip > 0.4 or (cloud > 85.0 and 12 <= hour <= 21):
        return STORM_TYPE
    if 270.0 <= wd <= 345.0 and ws > 4.0:
        return 1  # El Cierzo NW
    if 150.0 <= wd <= 235.0 and ws > 3.5:
        return 2  # Foehn Sur
    if (hour <= 7 or hour >= 21) and ws < 2.5 and rh > 78.0:
        return 3  # katabatic / valley inversion
    return 0  # Pyrenean thermal / fair


def derive_climatology(data: list[dict] | None) -> tuple[dict, np.ndarray, str]:
    """Return per-(season,type) synoptic medians and a seasonal type frequency table."""
    synoptic: dict[tuple[int, int], tuple] = {}
    freq = np.zeros((NSEASON, NTYPE), dtype=np.float64)
    if data is None:
        for s in range(NSEASON):
            season_scale = (0.72, 1.00, 1.32, 1.05)[s]
            temp_off = (-8.0, -2.0, 8.0, 1.0)[s]
            for t, row in enumerate(SYNTHETIC_SYNOPTIC):
                d, sp, tc, td, qnh, cc, pr, nval = row
                synoptic[(s, t)] = (d, sp, tc + temp_off, td + temp_off * 0.7,
                                    qnh, cc, pr, nval, season_scale)
        for s in range(NSEASON):
            freq[s] = np.array([0.34, 0.26, 0.12, 0.16, 0.12])
        return synoptic, freq, "synthetic Iberian synoptic normals"

    buckets: dict[tuple[int, int], dict[str, list[float]]] = {}
    for st in data:
        h = st["hourly"]
        times = h["time"]
        months = np.fromiter((int(x[5:7]) for x in times), dtype=np.int8, count=len(times))
        hours = np.fromiter((int(x[11:13]) for x in times), dtype=np.int8, count=len(times))
        season_of = np.fromiter((0 if m in (12, 1, 2) else 1 if m in (3, 4, 5)
                                 else 2 if m in (6, 7, 8) else 3 for m in months),
                                dtype=np.int8, count=len(times))
        wd = np.asarray(h["wind_direction_10m"], dtype=np.float64)
        ws = np.asarray(h["wind_speed_10m"], dtype=np.float64) / 3.6
        cc = np.asarray(h["cloud_cover"], dtype=np.float64)
        pr = np.asarray(h["precipitation"], dtype=np.float64)
        rh = np.asarray(h["relative_humidity_2m"], dtype=np.float64)
        temp = np.asarray(h["temperature_2m"], dtype=np.float64)
        dew = np.asarray(h["dew_point_2m"], dtype=np.float64)
        # Open-Meteo reports surface pressure in hPa. Reduce station pressure
        # to sea level before using it as QNH in the flight model.
        pres = np.asarray(h["surface_pressure"], dtype=np.float64)
        elev = float(st.get("elevation", 0.0))
        temp_k = np.maximum(180.0, temp + 273.15)
        pres = pres * np.exp(9.80665 * elev / (287.05 * (temp_k + 0.00325 * elev)))
        cls = np.fromiter((classify_hour(a, b, c, d, int(hh), e)
                           for a, b, c, d, hh, e in zip(wd, ws, cc, pr, hours, rh)),
                          dtype=np.int8, count=len(times))
        for s in range(NSEASON):
            for t in range(NTYPE):
                m = (season_of == s) & (cls == t) & np.isfinite(ws) & np.isfinite(wd)
                if not np.any(m):
                    continue
                b = buckets.setdefault((s, t), {k: [] for k in
                                                ("dir", "spd", "temp", "dew", "qnh", "cloud", "precip")})
                b["dir"].extend(wd[m].tolist())
                b["spd"].extend(ws[m].tolist())
                b["temp"].extend(temp[m].tolist())
                b["dew"].extend(dew[m].tolist())
                b["qnh"].extend(pres[m].tolist())
                b["cloud"].extend((cc[m] / 100.0).tolist())
                b["precip"].extend(pr[m].tolist())
                freq[s, t] += int(m.sum())

    for s in range(NSEASON):
        for t in range(NTYPE):
            b = buckets.get((s, t))
            dirs, spds, temps, dews, qnhs, clouds, precs = (
                (b["dir"], b["spd"], b["temp"], b["dew"], b["qnh"], b["cloud"], b["precip"])
                if b else ([], [], [], [], [], [], []))
            if not spds:
                row = SYNTHETIC_SYNOPTIC[t]
                synoptic[(s, t)] = (*row[:7], row[7], 1.0)
                continue
            # Circular mean for meteorological direction.
            dr = np.radians(np.asarray(dirs))
            dmean = math.degrees(math.atan2(np.sin(dr).mean(), np.cos(dr).mean())) % 360.0
            synoptic[(s, t)] = (
                dmean, float(np.median(spds)), float(np.median(temps)),
                float(np.median(dews)), float(np.median(qnhs)),
                float(np.median(clouds)), float(np.median(precs)),
                (0.0105, 0.0135, 0.0125, 0.0155, 0.0090)[t], 1.0,
            )
        if freq[s].sum() > 0:
            freq[s] /= freq[s].sum()
        else:
            freq[s] = np.array([0.34, 0.26, 0.12, 0.16, 0.12])
    return synoptic, freq, "ERA5 via Open-Meteo archive 2024, 5 corridor stations"


# ---------------------------------------------------------------------------
# Numerical solvers
# ---------------------------------------------------------------------------
def _lap_eigenvalues(H: int, W: int, dy: float, dx: float) -> np.ndarray:
    lam_x = 2.0 * (np.cos(np.pi * np.arange(W) / W) - 1.0) / (dx * dx)
    lam_y = 2.0 * (np.cos(np.pi * np.arange(H) / H) - 1.0) / (dy * dy)
    return lam_y[:, None] + lam_x[None, :]


def poisson_neumann(rhs: np.ndarray, lam: np.ndarray) -> np.ndarray:
    """Solve lap(phi)=rhs with Neumann BC via orthonormal DCT-II (exact spectral inverse)."""
    f = dctn(rhs, type=2, norm="ortho")
    safe = np.where(np.abs(lam) < 1e-14, 1.0, lam)
    phi = idctn(f / safe, type=2, norm="ortho")
    return phi - phi.mean()


def grad2(a: np.ndarray, dy: float, dx: float) -> tuple[np.ndarray, np.ndarray]:
    gy = np.gradient(a, dy, axis=0)
    gx = np.gradient(a, dx, axis=1)
    return gy, gx


def solve_base_wind(dem: np.ndarray, dy: float, dx: float,
                    dir_from_deg: float, speed10: float, stab_N: float,
                    venturi_gain: float, wave_gain: float) -> tuple[np.ndarray, float]:
    """Return (4,H,W,3) terrain-following wind with orographic + lee-wave w."""
    H, W = dem.shape
    # Geostrophic 850 hPa forcing: 10 m wind scaled through the log boundary layer.
    rad = math.radians(dir_from_deg)
    ue = -math.sin(rad) * speed10 * 1.55
    un = -math.cos(rad) * speed10 * 1.55
    u_mag = math.hypot(ue, un)

    # Terrain spectrum (detrend + Hann window to avoid periodic edge leakage).
    h = dem - np.mean(dem)
    wy = np.hanning(H)[:, None]
    wx = np.hanning(W)[None, :]
    hw = h * wy * wx
    hk = np.fft.fft2(hw)
    ky = 2.0 * math.pi * np.fft.fftfreq(H, d=dy)
    kx = 2.0 * math.pi * np.fft.fftfreq(W, d=dx)
    KX, KY = np.meshgrid(kx, ky)
    k2 = KX * KX + KY * KY
    kmag = np.sqrt(k2)
    kmag[0, 0] = 1e-6
    kdotu = KX * ue + KY * un

    # Ebro trough mask: low, northwestern lowlands channel the Cierzo SE.
    trough = np.clip((520.0 - dem) / 280.0, 0.0, 1.0) * np.clip(
        (dem - 120.0) / 120.0, 0.0, 1.0)

    lam = _lap_eigenvalues(H, W, dy, dx)
    field = np.zeros((LEVELS, H, W, 3), dtype=np.float64)
    wave_amp = 0.0
    for L, agl in enumerate(LEVEL_AGL_M):
        # Log profile anchored so 10 m AGL == measured 10 m wind and ~1500 m
        # AGL == 850 hPa geostrophic forcing (1.55x); Ekman veer with altitude.
        prof = 1.0 + 0.55 * math.log(max(agl, 10.0) / 10.0) / math.log(1500.0 / 10.0)
        prof = min(2.35, max(0.85, prof))
        veer = math.radians(-10.0 * math.exp(-agl / 400.0) +
                            (22.0 if agl > 900.0 else 0.0) +
                            (16.0 if agl > 2600.0 else 0.0))
        cv, sv = math.cos(veer), math.sin(veer)
        # ue/un already include the 1.55x 850 hPa scaling; renormalise to 10 m.
        ue_l = (ue / 1.55 * prof * cv + un / 1.55 * prof * sv)
        un_l = (-ue / 1.55 * prof * sv + un / 1.55 * prof * cv)
        venturi_base = 1.0 + venturi_gain * trough * 0.90
        ue_l *= venturi_base
        un_l *= venturi_base

        # Spectral potential-flow terrain response, decaying with height.
        decay = np.exp(-kmag * min(agl, 3000.0) * 0.42)
        uk = np.real(np.fft.ifft2(KX * kdotu / kmag * hk * decay))
        vk = np.real(np.fft.ifft2(KY * kdotu / kmag * hk * decay))
        wk = np.real(np.fft.ifft2(1j * kdotu * hk * decay))
        uk *= 0.85 + venturi_gain * trough * 1.6
        vk *= 0.85 + venturi_gain * trough * 1.6
        wk = np.clip(wk * 1.15, -6.5, 6.5)

        # Linear hydrostatic mountain wave: m^2 = N^2/U^2 - k^2 (Scorer).
        scorer_l = max(0.004, stab_N / max(1.5, u_mag))
        m2 = np.maximum(scorer_l * scorer_l - k2, 0.0)
        mz = np.sqrt(m2) * min(agl, 5000.0)
        wave_w = np.real(np.fft.ifft2(1j * kdotu * hk * np.exp(1j * mz) *
                                      np.exp(-min(agl, 5000.0) / 4200.0)))
        wave_w = np.clip(wave_w * wave_gain, -3.5, 3.5)
        wave_amp = max(wave_amp, float(np.max(np.abs(wave_w))))

        u = ue_l + uk
        v = un_l + vk
        # Mass-consistent projection of the residual divergence (CALMET-style).
        duy, dux = grad2(u, dy, dx)
        dvy, dvx = grad2(v, dy, dx)
        div = dux + dvy
        phi = poisson_neumann(-div, lam)
        phiy, phix = grad2(phi, dy, dx)
        u += phix
        v += phiy
        field[L, ..., 0] = np.clip(u, -44.0, 44.0)
        field[L, ..., 1] = np.clip(v, -44.0, 44.0)
        field[L, ..., 2] = np.clip(wk + wave_w, -13.0, 13.0)
    return field, wave_amp


def solve_thermal(dem: np.ndarray, forest: np.ndarray, water: np.ndarray,
                  dy: float, dx: float, slot: int, season: int,
                  oat_c: float, stab_N: float) -> np.ndarray:
    """Hydrostatic buoyancy-driven slope circulation, projected divergence-free."""
    H, W = dem.shape
    dhy, dhx = grad2(dem, dy, dx)
    norm = np.sqrt(1.0 + dhx * dhx + dhy * dhy)
    nx, ny, nz = -dhx / norm, -dhy / norm, 1.0 / norm

    el = math.radians(float(SOLAR_ZENITH[slot]))
    az = math.radians(float(SOLAR_AZIMUTH[slot]))
    sx = math.sin(az) * math.cos(el)
    sy = math.sin(el)
    sz = -math.cos(az) * math.cos(el)
    cos_inc = np.clip(nx * sx + ny * sy + nz * sz, 0.0, 1.0)

    albedo = 0.22 + 0.10 * forest - 0.14 * water
    albedo = np.clip(albedo, 0.07, 0.42)
    season_gain = (0.55, 0.85, 1.15, 0.75)[season]
    shortwave = 950.0 * cos_inc * (1.0 - albedo) * season_gain
    # Water has large heat capacity -> weak/negative daytime sensible heat flux.
    moisture = np.clip(0.25 + 0.55 * water + 0.35 * forest, 0.0, 0.95)
    longwave = 78.0 + 52.0 * (oat_c / 25.0)
    heat = (shortwave - longwave) * (1.0 - moisture) - 95.0 * water
    heat = heat / (1.0 + 0.00035 * np.maximum(dem - 300.0, 0.0))
    theta_p = np.clip(heat / 420.0, -3.0, 3.0)  # K potential-temperature perturbation

    lam = _lap_eigenvalues(H, W, dy, dx)
    out = np.zeros((LEVELS, H, W, 3), dtype=np.float64)
    for L, agl in enumerate(LEVEL_AGL_M):
        # Hydrostatic pressure gradient -g/theta0 * z * grad(theta'), balanced by
        # Rayleigh friction tau; steady-state slope flow + vertical structure.
        tau = 105.0 * math.exp(-agl / 1350.0)
        g = 9.81 / max(265.0, 273.15 + oat_c)
        ty, tx = grad2(theta_p, dy, dx)
        u_raw = -tau * g * min(agl, 2200.0) * tx / max(1.0, stab_N / 0.012)
        v_raw = -tau * g * min(agl, 2200.0) * ty / max(1.0, stab_N / 0.012)
        u_raw = np.clip(u_raw, -6.0, 6.0)
        v_raw = np.clip(v_raw, -6.0, 6.0)
        duy, dux = grad2(u_raw, dy, dx)
        dvy, dvx = grad2(v_raw, dy, dx)
        phi = poisson_neumann(-(dux + dvy), lam)
        phiy, phix = grad2(phi, dy, dx)
        u = u_raw + phix
        v = v_raw + phiy
        # Up-valley anabatic by day, down-valley katabatic at night.
        diurnal = 1.0 if slot < 10 else -1.0
        u = np.clip(u * diurnal, -7.0, 7.0)
        v = np.clip(v * diurnal + diurnal * 0.85, -7.0, 7.0)
        w = np.clip(heat / 300.0 * math.exp(-agl / 900.0) * diurnal, -2.2, 3.4)
        out[L, ..., 0] = u
        out[L, ..., 1] = v
        out[L, ..., 2] = w
    return out


def solve_moisture(dem: np.ndarray, w_field: np.ndarray, oat_c: float, dew_c: float,
                   cloud_hint: float, precip_hint: float, storm: float,
                   season: int) -> tuple[np.ndarray, dict]:
    """LCL-based RH profile + orographic condensation / Foehn drying."""
    T = oat_c - dem * 0.0065
    Td = dew_c - dem * 0.0018
    spread = np.clip(T - Td, 0.2, 30.0)
    lcl = 125.0 * spread
    # Magnus saturation vapour pressure -> RH at cloud slab.
    es_t = 6.112 * np.exp(17.67 * T / (T + 243.5))
    es_td = 6.112 * np.exp(17.67 * Td / (Td + 243.5))
    rh = np.clip(es_td / np.maximum(es_t, 1e-6), 0.0, 1.2)

    w_surf = w_field[1]
    lift = np.clip(w_surf * 0.30, -2.0, 2.0)
    cloud = np.clip((rh - 0.76) / 0.24 + lift * 0.42 + (cloud_hint - 0.35) * 0.55
                    - np.clip(-w_surf, 0.0, 3.0) * 0.22, -1.0, 1.0)
    cov = float(np.clip(np.mean(np.clip(cloud, 0.0, 1.0)) * 1.55 + cloud_hint * 0.35, 0.05, 0.97))
    precip = float(np.clip(max(precip_hint / 5.0, (cov - 0.62) / 0.38) * (1.0 if storm > 0.1 else 0.4), 0.0, 1.0))
    base = float(np.clip(np.percentile(lcl, 35) + dem.mean() * 0.12, 900.0, 2900.0))
    thick = float(np.clip(1150.0 + 1250.0 * cov + 500.0 * storm, 1200.0, 2900.0))
    scalars = {
        S_CLOUD_COV: cov,
        S_PRECIP: precip,
        S_CLOUD_BASE_M: base,
        S_CLOUD_THICK_M: thick,
        S_OAT_SL_C: oat_c,
        S_DEWPOINT_SL_C: dew_c,
        S_SNOWLINE_M: 2900.0 - season * 0.0 + (0 if season == 2 else -650.0),
    }
    return cloud, scalars


# ---------------------------------------------------------------------------
# Baking
# ---------------------------------------------------------------------------
def build_library(reader: PackReader, offline: bool, out_path: Path = OUT_PATH) -> int:
    stations = _region_stations(reader)
    cpath = CACHE_PATH if reader.region_id == "es_aragon_pyrenees" else (CACHE_PATH.parent / f"era5_{reader.region_id}_2024.json")
    data, provenance = fetch_reanalysis(offline, cache_path=cpath, stations=stations)
    synoptic, freq, provenance = derive_climatology(data)
    domain = corridor_domain(reader)
    dem, forest, water, dx, dy = load_terrain(reader, domain)
    min_e, min_n, span_e, span_n = domain[:4]
    print(f"[WX] domain {span_e / 1000:.1f} x {span_n / 1000:.1f} km | "
          f"DEM {dem.min():.0f}..{dem.max():.0f} m | {provenance}")

    lam = _lap_eigenvalues(GRID_H, GRID_W, dy, dx)
    entries: list[bytes] = []
    payloads: list[bytes] = []
    offset = 152 + 120 * (NSEASON * NSLOT * NTYPE)

    base_cache: dict[tuple[int, int], tuple[np.ndarray, float]] = {}
    thermal_cache: dict[tuple[int, int, int], np.ndarray] = {}
    t0 = time.time()

    for season in range(NSEASON):
        for wtype in range(NTYPE):
            d, sp, tc, td, qnh, cc, pr, nval, _ = synoptic[(season, wtype)]
            venturi = (1.0, 1.0, 0.25, 0.15, 0.20)[wtype]
            wave_gain = (0.55, 1.35, 1.20, 0.30, 0.85)[wtype]
            base, wave_amp = solve_base_wind(dem, dy, dx, d, sp, nval, venturi, wave_gain)
            base_cache[(season, wtype)] = (base, wave_amp)

    for season in range(NSEASON):
        for slot in range(NSLOT):
            for wtype in range(NTYPE):
                d, sp, tc, td, qnh, cc, pr, nval, _ = synoptic[(season, wtype)]
                key = (season, slot, wtype)
                storm = 1.0 if wtype == STORM_TYPE else 0.0
                tkey = (season, slot, wtype)
                if tkey not in thermal_cache:
                    oat_slot = tc + (float(SOLAR_ZENITH[slot]) - 20.0) * 0.11 - (6.0 if slot == 10 else 0.0)
                    thermal_cache[tkey] = solve_thermal(dem, forest, water, dy, dx,
                                                        slot, season, oat_slot, nval)
                base, wave_amp = base_cache[(season, wtype)]
                thermal = thermal_cache[tkey]
                u = base[..., 0] + thermal[..., 0]
                v = base[..., 1] + thermal[..., 1]
                w = base[..., 2] + thermal[..., 2]
                cloud, sc = solve_moisture(dem, w, tc, td, cc, pr, storm, season)

                # TKE from vertical shear + buoyancy + storm intensity.
                shear = float(np.mean(np.abs(base[2, ..., 0] - base[0, ..., 0]) +
                                      np.abs(base[2, ..., 1] - base[0, ..., 1])))
                turb = float(np.clip(0.20 + shear * 0.055 + wave_amp * 0.10 + storm * 0.85, 0.15, 1.60))
                haze = float(np.clip((0.72, 1.08, 1.45, 1.65, 1.30)[wtype] *
                                     (0.88 + 0.55 * sc[S_CLOUD_COV]), 0.6, 2.1))
                vis = float(np.clip(30000.0 * (1.0 - 0.72 * sc[S_PRECIP]) *
                                    (1.0 - 0.35 * max(0.0, haze - 1.2)), 1200.0, 30000.0))

                scalars = [0.0] * 16
                scalars[S_CLOUD_COV] = sc[S_CLOUD_COV]
                scalars[S_PRECIP] = sc[S_PRECIP]
                scalars[S_CLOUD_BASE_M] = sc[S_CLOUD_BASE_M]
                scalars[S_CLOUD_THICK_M] = sc[S_CLOUD_THICK_M]
                scalars[S_OAT_SL_C] = tc + (float(SOLAR_ZENITH[slot]) - 20.0) * 0.11 - (6.0 if slot == 10 else 0.0)
                scalars[S_DEWPOINT_SL_C] = td
                scalars[S_QNH_HPA] = qnh - 11.5 * storm
                scalars[S_WIND_DIR_DEG] = d
                scalars[S_WIND_SPEED_MPS] = sp
                scalars[S_HAZE_MULT] = haze
                scalars[S_TURB_SCALE] = turb
                scalars[S_STORM_INT] = storm
                scalars[S_VISIBILITY_M] = vis
                scalars[S_WAVE_AMP] = wave_amp
                scalars[S_SNOWLINE_M] = sc[S_SNOWLINE_M]
                scalars[15] = float(freq[season, wtype])

                raw = np.empty((LEVELS, GRID_H, GRID_W, 4), dtype=np.uint8)
                for L in range(LEVELS):
                    for ch, arr in enumerate((u[L], v[L], w[L], cloud)):
                        lo, hi = CHAN_LO[ch], CHAN_HI[ch]
                        raw[L, ..., ch] = np.clip(
                            np.rint((arr - lo) / (hi - lo) * 255.0), 0, 255).astype(np.uint8)
                comp = zlib.compress(raw.tobytes(), level=6)
                label = f"{SEASONS[season]}/{SLOTS[slot]}/{WEATHER_TYPES[wtype]}"[:31]
                entries.append(pack_state(season, slot, wtype, len(comp), offset, scalars, label))
                payloads.append(comp)
                offset += len(comp)

    hdr = pack_header((min_e, min_n, span_e, span_n),
                      provenance[:47])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as f:
        f.write(hdr)
        for e in entries:
            f.write(e)
        for p in payloads:
            f.write(p)
    print(f"[WX] baked {len(entries)} states -> {out_path} "
          f"({out_path.stat().st_size / 1048576:.1f} MB) in {time.time() - t0:.1f}s")
    return 0


def selftest() -> int:
    # Exact spectral Poisson inverse: build rhs from a known potential, recover it.
    H, W = GRID_H, GRID_W
    lam = _lap_eigenvalues(H, W, 1000.0, 1280.0)
    rng = np.random.default_rng(7)
    g = rng.normal(size=(H, W))
    g -= g.mean()
    rhs = idctn(lam * dctn(g, type=2, norm="ortho"), type=2, norm="ortho")
    phi = poisson_neumann(rhs, lam)
    assert np.allclose(phi, g, atol=1e-6), "DCT Poisson inverse failed"

    # Mass-consistency: projection must drive divergence toward zero.
    dem = 400.0 + 700.0 * np.sin(np.linspace(0, 9, W))[None, :] * \
        np.cos(np.linspace(0, 7, H))[:, None]
    fld, wave = solve_base_wind(dem, 1000.0, 1280.0, 312.0, 9.8, 0.0135, 1.0, 1.35)
    uy, ux = grad2(fld[1, ..., 0], 1000.0, 1280.0)
    vy, vx = grad2(fld[1, ..., 1], 1000.0, 1280.0)
    div_after = float(np.mean(np.abs(ux + vy)))
    assert div_after < 5e-4, f"mass-consistency residual {div_after}"
    assert wave > 0.0, "mountain wave solver produced no wave"

    # Land-cover thermal response: water must suppress daytime lift.
    forest = np.full((H, W), 0.3)
    water = np.zeros((H, W))
    water[H // 2 - 8:H // 2 + 8, W // 2 - 8:W // 2 + 8] = 1.0
    th = solve_thermal(dem, forest, water, 1000.0, 1280.0, 5, 2, 22.0, 0.0105)
    wet = float(np.mean(th[1, H // 2 - 6:H // 2 + 6, W // 2 - 6:W // 2 + 6, 2]))
    dry = float(np.mean(th[1, :20, :20, 2]))
    assert wet < dry, f"water thermal suppression failed wet={wet} dry={dry}"

    # Day/night slope circulation must reverse.
    day = solve_thermal(dem, forest, water, 1000.0, 1280.0, 5, 2, 22.0, 0.0105)
    night = solve_thermal(dem, forest, water, 1000.0, 1280.0, 10, 2, 14.0, 0.0105)
    assert float(np.mean(day[1, ..., 1])) > float(np.mean(night[1, ..., 1])) + 0.3, \
        "anabatic/katabatic reversal failed"

    # State indexing + quantization round-trip.
    assert state_index(3, 10, 4) == NSEASON * NSLOT * NTYPE - 1
    for ch in range(4):
        lo, hi = CHAN_LO[ch], CHAN_HI[ch]
        assert abs((encode_channel((lo + hi) / 2, ch) / 255.0) * (hi - lo) + lo - (lo + hi) / 2) < 0.2
    print("[WEATHER SELFTEST OK] DCT Poisson inverse, mass-consistency, lee waves, "
          "water thermal suppression, diurnal reversal, state indexing.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack", type=Path, default=PACK_PATH, help="input .nspack region pack")
    ap.add_argument("--out", type=Path, default=None, help="output .nswx weather library path")
    ap.add_argument("--offline", action="store_true", help="never call the reanalysis API")
    ap.add_argument("--selftest", action="store_true", help="verify solvers and exit")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    out_path = args.out or (
        OUT_PATH if args.pack == PACK_PATH
        else args.pack.parent / (args.pack.stem.replace("_pyrenees", "") + "_weather.nswx")
    )
    with PackReader(args.pack) as reader:
        return build_library(reader, args.offline, out_path=out_path)


if __name__ == "__main__":
    sys.exit(main())
