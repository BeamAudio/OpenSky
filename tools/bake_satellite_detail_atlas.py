#!/usr/bin/env python3
"""bake_satellite_detail_atlas.py -- Offline Regional Pattern & 3D Cloud Volume Baker.

1. Bakes a 2048x2048 4x4 (16-Pattern) Real Northern Spain / Aragon Satellite Super-Resolution Atlas
   (game/assets/pbr/sat_detail_atlas.jpg, 512x512 seamless periodic per pattern, high-pass centered at 0.50):
     Row 0 (Lowland & Somontano Agriculture):
       0 (0,0): Monegros / Ebro Dryland Cereal Steppes (Secano: harvester stripes, narrow parcel tracks)
       1 (1,0): Plowed Red-Clay / Terracotta Fallow Fields (Barbecho: deep plowed tractor furrows)
       2 (2,0): Irrigated Ebro / Cinca Alfalfa & Fruit Plots (Regadio: lush green parcels & channels)
       3 (3,0): Somontano Olive Groves, Almond Orchards & Vineyards (regular 2-3m dotted tree rows + shadows)
     Row 1 (Escarpments, Mediterranean Foothills & Valley Pastures):
       4 (0,1): Ebro / Monegros Gypsum Badlands (Yesos: dendritic pale erosion rills & stratified gullies)
       5 (1,1): Mediterranean Garrigue / Rosemary Scrub (Matorral: 1-2m clumpy scrub dots + sharp black shadows)
       6 (2,1): Holm Oak & Gall Oak Woodland (Carrascal: mottled rounded 3-6m Mediterranean oak crowns)
       7 (3,1): Pyrenean Valley Hay Meadows & Stone-Wall Pastures (Prados: mowed plots & hedgerow lines)
     Row 2 (Rivers, Subalpine Forests, Cliffs & Alpine Plateaus):
       8 (0,2): Riparian Poplar Gallery & Braided River Shingle (Gravera: cobble bars & dense river trees)
       9 (1,2): Subalpine Pyrenean Scots Pine & Silver Fir Forest (Pinar: dense spiky conifer crowns & gaps)
      10 (2,2): Sedimentary Limestone Cliffs & Conglomerate (Mallos / Pena Montanesa: horizontal strata & cracks)
      11 (3,2): High-Alpine Pasture & Limestone Pavement (Pasto Alpino: grass tussocks, flat rock & trails)
     Row 3 (High Pyrenees & Urban Paving):
      12 (0,3): Pale Limestone Talus & Scree Fans (Pedrera: angular rubble gravel & gravity chutes)
      13 (1,3): High-Pyrenees Granite & Slate Peak Crags (Alta Montana: dark fractured ridges & arêtes)
      14 (2,3): High-Alpine Wind-Scoured Snowfields & Cirques (Neveros: wind-carved sastrugi & snow grain)
      15 (3,3): Spanish Urban / Village Paving & Courtyards (Casco Urbano: stone plazas, roofs & gravel lots)

2. Bakes a 512x512 4x4 Packed 3D Volumetric Cloud Noise Atlas (game/assets/pbr/cloud_vol_3d.png):
   16 seamless periodic 128x128 Z-slices (R=Perlin-Worley billow, G=mid Worley cauliflower erosion, B=fine curl wisps)
   for true 3D volumetric raymarched clouds with ZERO 2D sprites!
"""

from __future__ import annotations

import io
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from protocol.pack_format import LAYER_IMAGERY, PackReader  # noqa: E402

PACK_PATH = ROOT / "packs" / "es_aragon_pyrenees.nspack"
SAT_OUT_PATH = ROOT / "game" / "assets" / "pbr" / "sat_detail_atlas.jpg"
CLOUD_VOL_PATH = ROOT / "game" / "assets" / "pbr" / "cloud_vol_3d.png"


# ---------------------------------------------------------------------------
# Fast Periodic 2D / 3D Noise Helpers (NumPy vectorized -> < 1s total!)
# ---------------------------------------------------------------------------
def _periodic_value_noise_2d(N: int, period: int, rng: np.random.Generator) -> np.ndarray:
    grid = rng.random((period, period), dtype=np.float32)
    coords = np.linspace(0.0, period, N, endpoint=False, dtype=np.float32)
    i0 = np.floor(coords).astype(np.int32) % period
    i1 = (i0 + 1) % period
    f = coords - np.floor(coords)
    u = f * f * (3.0 - 2.0 * f)
    a = grid[i0[:, None], i0[None, :]]
    b = grid[i0[:, None], i1[None, :]]
    c = grid[i1[:, None], i0[None, :]]
    d = grid[i1[:, None], i1[None, :]]
    return (a * (1.0 - u[None, :]) + b * u[None, :]) * (1.0 - u[:, None]) + (c * (1.0 - u[None, :]) + d * u[None, :]) * u[:, None]


def _fbm_2d(N: int, base_p: int, octaves: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    acc = np.zeros((N, N), dtype=np.float32)
    amp, norm, p = 0.5, 0.0, base_p
    for _ in range(octaves):
        acc += amp * _periodic_value_noise_2d(N, p, rng)
        norm += amp
        amp *= 0.5
        p = min(N, p * 2)
    return acc / norm


def _worley_2d(N: int, cells: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pts_y = rng.random((cells, cells), dtype=np.float32)
    pts_x = rng.random((cells, cells), dtype=np.float32)
    ys = np.linspace(0.0, cells, N, endpoint=False, dtype=np.float32)
    xs = np.linspace(0.0, cells, N, endpoint=False, dtype=np.float32)
    iy = np.floor(ys).astype(np.int32)
    ix = np.floor(xs).astype(np.int32)
    fy = ys - iy
    fx = xs - ix
    min_d2 = np.full((N, N), 9.0, dtype=np.float32)
    for dy in (-1, 0, 1):
        cy = (iy + dy) % cells
        for dx in (-1, 0, 1):
            cx = (ix + dx) % cells
            py = pts_y[cy[:, None], cx[None, :]] + dy - fy[:, None]
            px = pts_x[cy[:, None], cx[None, :]] + dx - fx[None, :]
            min_d2 = np.minimum(min_d2, px * px + py * py)
    return 1.0 - np.clip(np.sqrt(min_d2), 0.0, 1.0)


def _normalize_hp(arr: np.ndarray, sigma: float = 14.0, contrast: float = 1.6) -> np.ndarray:
    """High-pass normalize an HxWx3 float32 image around 0.50 using luminance-locked local mean
    (prevents RGB channel divide-by-small-value magenta/cyan fringes on deep green crops!)."""
    if arr.ndim == 2:
        arr = np.repeat(arr[..., None], 3, axis=-1)
    luma = 0.2126 * arr[..., 0] + 0.7152 * arr[..., 1] + 0.0722 * arr[..., 2]
    low_luma = np.maximum(0.08, gaussian_filter(luma, sigma=sigma, mode="wrap"))
    hp_luma = (luma / low_luma) - 1.0
    # Preserve subtle natural chrominance detail (35%) without blowing up small channels
    low_rgb = np.stack([gaussian_filter(arr[..., c], sigma=sigma, mode="wrap") for c in range(3)], axis=-1)
    hp_chrom = (arr - low_rgb) / low_luma[..., None]
    hp = hp_luma[..., None] * 0.70 + hp_chrom * 0.30
    std = max(0.025, float(np.std(hp)))
    scaled = hp * (0.175 * contrast / std)
    out = 0.5 + 0.46 * np.tanh(scaled)
    return np.clip(out * 255.0, 0.0, 255.0).astype(np.uint8)


# ---------------------------------------------------------------------------
# Extract Real z=17 / z=16 IGN PNOA Natural Satellite Patches from .nspack
# ---------------------------------------------------------------------------
def collect_pnoa_bank(reader: PackReader) -> dict[str, list[np.ndarray]]:
    buckets: dict[str, list[tuple[float, np.ndarray]]] = {
        "cereal": [], "plowed": [], "irrigated": [], "olive": [],
        "badland": [], "scrub": [], "oak": [], "meadow": [],
        "riparian": [], "pine": [], "cliff": [], "urban": [],
    }
    keys = []
    for k in reader.index.keys():
        layer = (k >> 56) & 0xFF
        z = (k >> 48) & 0xFF
        if layer == LAYER_IMAGERY and z in (17, 16, 15, 14):
            keys.append((z, (k >> 24) & 0xFFFFFF, k & 0xFFFFFF))

    step = max(1, len(keys) // 520)
    for z, tx, ty in keys[::step]:
        raw = reader.read_tile(LAYER_IMAGERY, z, tx, ty, verify_crc=False)
        if raw is None:
            continue
        im = np.asarray(Image.open(io.BytesIO(raw)).convert("RGB"), dtype=np.float32) / 255.0
        if im.shape[0] < 256 or im.shape[1] < 256:
            continue
        for y0 in (0, 128):
            for x0 in (0, 128):
                p = im[y0:y0 + 128, x0:x0 + 128]
                r, g, b = p[..., 0], p[..., 1], p[..., 2]
                luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
                p99, p01 = float(np.percentile(luma, 99)), float(np.percentile(luma, 1))
                std_l = float(np.std(luma))
                if p01 < 0.02 or std_l < 0.022:
                    continue
                mr, mg, mb = float(np.mean(r)), float(np.mean(g)), float(np.mean(b))
                ml = float(np.mean(luma))
                g_ex = mg - 0.5 * (mr + mb)
                warm = mr - mb
                sat = abs(mr - mg) + abs(mg - mb)
                dark_f = float(np.mean(luma < ml * 0.72))

                # Urban paving / roofs (high contrast, moderate sat, sharp edges)
                if std_l > 0.11 and p99 > 0.65 and 0.24 < ml < 0.60:
                    buckets["urban"].append((std_l * 5.0, p))
                if p99 > 0.76 or (p99 - p01) > 0.64:
                    continue

                # Structure-tensor straight-line/stripe detector: rejects runways, highways & parallel orchard stripes from wild biomes!
                gx = np.diff(luma, axis=1)[:-1, :]
                gy = np.diff(luma, axis=0)[:, :-1]
                jxx, jyy, jxy = float(np.mean(gx * gx)), float(np.mean(gy * gy)), float(np.mean(gx * gy))
                tr = jxx + jyy + 1e-8
                coherence = math.hypot(jxx - jyy, 2.0 * jxy) / tr
                is_organic = coherence < 0.36 and (p99 - p01) < 0.56

                if warm > 0.08 and g_ex < 0.015 and 0.30 < ml < 0.58 and std_l < 0.095 and coherence < 0.42:
                    buckets["cereal"].append((warm * 3.0 + std_l * 2.5 - coherence, p))
                if (mr - mg) > 0.045 and warm > 0.09 and 0.20 < ml < 0.46 and coherence < 0.45:
                    buckets["plowed"].append(((mr - mg) * 5.0 + std_l * 3.0, p))
                if g_ex > 0.035 and mg > mr * 1.04 and 0.22 < ml < 0.48 and dark_f < 0.14 and coherence < 0.36:
                    buckets["irrigated"].append((g_ex * 5.0 + (0.20 - dark_f) * 2.0 - coherence, p))
                if warm > 0.05 and 0.08 < dark_f < 0.26 and 0.28 < ml < 0.54 and std_l > 0.05 and coherence < 0.38:
                    buckets["olive"].append((std_l * 5.0 + warm * 2.0, p))
                if is_organic and sat < 0.075 and ml > 0.38 and std_l > 0.045 and float(np.max(b - r)) < 0.04:
                    buckets["badland"].append((ml * 2.0 + std_l * 5.0, p))
                if is_organic and g_ex > 0.012 and 0.10 < dark_f < 0.32 and 0.22 < ml < 0.46 and std_l > 0.048:
                    buckets["scrub"].append((std_l * 5.0 + g_ex * 2.5, p))
                if is_organic and g_ex > 0.020 and 0.15 < ml < 0.35 and dark_f > 0.12 and warm > 0.025:
                    buckets["oak"].append((g_ex * 4.0 + std_l * 4.5, p))
                if is_organic and g_ex > 0.022 and 0.24 < ml < 0.48 and std_l > 0.038:
                    buckets["meadow"].append((g_ex * 4.5 + std_l * 3.5, p))
                if is_organic and sat < 0.08 and 0.22 < ml < 0.52 and std_l > 0.065 and float(np.max(b - r)) < 0.06:
                    buckets["riparian"].append((std_l * 5.0, p))
                if is_organic and g_ex > 0.022 and ml < 0.28 and dark_f > 0.14:
                    buckets["pine"].append(((0.35 - ml) * 4.0 + std_l * 6.0, p))
                if is_organic and sat < 0.07 and 0.20 < ml < 0.52 and std_l > 0.05 and float(np.max(b - r)) < 0.035:
                    buckets["cliff"].append(((0.09 - sat) * 5.0 + std_l * 5.5, p))

    out: dict[str, list[np.ndarray]] = {}
    for name, lst in buckets.items():
        lst.sort(key=lambda x: x[0], reverse=True)
        out[name] = [p for _, p in lst[:16]]
    return out


def quilt_512(patches: list[np.ndarray], seed: int, sigma: float = 14.0) -> np.ndarray:
    """Quilt 128x128 real PNOA satellite patches into a 512x512 seamless periodic float32 high-pass base."""
    N, P, STEP = 512, 128, 64
    rng = np.random.default_rng(seed)
    if not patches:
        return np.full((N, N, 3), 0.5, dtype=np.float32)

    hp_list = []
    for p in patches:
        luma = 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]
        low_l = np.maximum(0.08, gaussian_filter(luma, sigma=sigma))
        low_rgb = np.stack([gaussian_filter(p[..., c], sigma=sigma) for c in range(3)], axis=-1)
        hp = ((luma / low_l) - 1.0)[..., None] * 0.72 + ((p - low_rgb) / low_l[..., None]) * 0.28
        hp_list.append(np.clip(hp, -0.45, 0.45))

    u = np.arange(P, dtype=np.float32)
    w1d = 0.5 - 0.5 * np.cos(2.0 * np.pi * (u + 0.5) / P)
    win2d = (w1d[:, None] * w1d[None, :])[..., None]
    acc = np.zeros((N, N, 3), dtype=np.float32)
    wsum = np.zeros((N, N, 1), dtype=np.float32)
    for gy in range(N // STEP):
        for gx in range(N // STEP):
            pt = hp_list[int(rng.integers(0, min(len(hp_list), 10)))]
            pt = np.rot90(pt, int(rng.integers(0, 4)))
            if rng.random() < 0.5:
                pt = np.fliplr(pt)
            ys = np.arange(gy * STEP, gy * STEP + P) % N
            xs = np.arange(gx * STEP, gx * STEP + P) % N
            acc[np.ix_(ys, xs)] += pt * win2d
            wsum[np.ix_(ys, xs)] += win2d
    fused = acc / np.maximum(1e-5, wsum)
    return 0.5 + fused


def bake_16_regional_patterns(bank: dict[str, list[np.ndarray]]) -> None:
    S = 512
    OUT_S = 512  # Full 2048x2048 atlas (16 x 512x512 native GL_TEXTURE_2D_ARRAY layers -> 4x higher satellite detail!)
    atlas = Image.new("RGB", (OUT_S * 4, OUT_S * 4))
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) / S

    def put_slot(col: int, row: int, tile_u8: np.ndarray) -> None:
        im = Image.fromarray(tile_u8, "RGB").resize((OUT_S, OUT_S), Image.Resampling.LANCZOS)
        atlas.paste(im, (col * OUT_S, row * OUT_S))

    # --- ROW 0: LOWLAND & SOMONTANO AGRICULTURE ---
    # 0 (0,0): Monegros / Ebro Dryland Cereal Steppes (Secano: harvester stripes + narrow parcel borders)
    q0 = quilt_512(bank["cereal"] or bank["meadow"], seed=10)
    warp0 = (_fbm_2d(S, 4, 3, 101) - 0.5) * 0.04
    stripe0 = 0.07 * np.sin((xx * 38.0 + warp0 * 4.0) * 2.0 * math.pi)
    parcel0 = np.where((np.abs(np.sin((xx + warp0) * math.pi * 4.0)) < 0.03) | (np.abs(np.sin((yy - warp0) * math.pi * 3.0)) < 0.03), -0.14, 0.0)
    put_slot(0, 0, _normalize_hp(np.clip(q0 + stripe0[..., None] + parcel0[..., None], 0.05, 1.5), sigma=14.0, contrast=1.55))

    # 1 (1,0): Plowed Red-Clay Fallow Fields (Barbecho: deep plowed tractor furrows & headlands)
    q1 = quilt_512(bank["plowed"] or bank["cereal"], seed=11)
    fur1 = 0.11 * np.sin((yy * 46.0 + (_fbm_2d(S, 6, 3, 111) - 0.5) * 0.35) * 2.0 * math.pi)
    clod1 = (_worley_2d(S, 42, 112) - 0.5) * 0.10
    put_slot(1, 0, _normalize_hp(np.clip(q1 + fur1[..., None] + clod1[..., None], 0.05, 1.5), sigma=12.0, contrast=1.65))

    # 2 (2,0): Irrigated Ebro / Cinca Alfalfa & Fruit Plots (Regadio: fine irrigation rows & crisp channels)
    q2 = quilt_512(bank["irrigated"] or bank["meadow"], seed=12)
    row2 = 0.065 * np.sin((xx * 54.0) * 2.0 * math.pi) * (0.5 + 0.5 * np.sin(yy * 4.0 * math.pi))
    put_slot(2, 0, _normalize_hp(np.clip(q2 + row2[..., None], 0.05, 1.5), sigma=14.0, contrast=1.50))

    # 3 (3,0): Somontano Olive Groves, Almond Orchards & Vineyards (regular 2-3m dotted tree grid + dark sun shadows!)
    q3 = quilt_512(bank["olive"] or bank["cereal"], seed=13)
    # Regular agricultural tree grid (~7m spacing on 512px = 36 rows) with directional drop shadow!
    gx3 = np.cos((xx * 26.0 + 0.15 * np.sin(yy * 4.0 * math.pi)) * 2.0 * math.pi)
    gy3 = np.cos((yy * 26.0) * 2.0 * math.pi)
    tree_dot = np.clip((gx3 * gy3 - 0.42) * 2.8, 0.0, 1.0)
    sh_dot = np.roll(np.roll(tree_dot, 3, axis=0), -3, axis=1)
    mask_patch = np.clip((_fbm_2d(S, 4, 2, 131) - 0.25) * 2.4, 0.25, 1.0)
    olive_mod = np.zeros((S, S, 3), dtype=np.float32)
    olive_mod[..., 0] = (-0.16 * tree_dot - 0.22 * sh_dot * (1.0 - tree_dot)) * mask_patch
    olive_mod[..., 1] = (-0.09 * tree_dot - 0.22 * sh_dot * (1.0 - tree_dot)) * mask_patch
    olive_mod[..., 2] = (-0.17 * tree_dot - 0.20 * sh_dot * (1.0 - tree_dot)) * mask_patch
    put_slot(3, 0, _normalize_hp(np.clip(q3 + olive_mod, 0.05, 1.5), sigma=12.0, contrast=1.70))

    # --- ROW 1: ESCARPMENTS, MEDITERRANEAN FOOTHILLS & VALLEY PASTURES ---
    # 4 (0,1): Ebro / Monegros Gypsum Badlands (Yesos: dendritic pale erosion rills & stratified gullies)
    q4 = quilt_512(bank["badland"] or bank["cliff"], seed=14)
    fb4 = _fbm_2d(S, 8, 4, 141)
    rill4 = 1.0 - np.abs(_fbm_2d(S, 14, 4, 142) - 0.5) * 2.0
    strata4 = 0.09 * np.sin((yy * 18.0 + fb4 * 2.8) * 2.0 * math.pi)
    gully4 = -0.16 * (rill4 ** 3.2)
    put_slot(0, 1, _normalize_hp(np.clip(q4 + strata4[..., None] + gully4[..., None], 0.05, 1.5), sigma=14.0, contrast=1.75))

    # 5 (1,1): Mediterranean Garrigue / Rosemary Scrub (Matorral: 1-2m clumpy scrub dots + crisp sun shadows!)
    q5 = quilt_512(bank["scrub"] or bank["meadow"], seed=15)
    bush5 = np.clip((_worley_2d(S, 38, 151) * (0.65 + 0.55 * _fbm_2d(S, 8, 3, 152)) - 0.58) * 3.8, 0.0, 1.0)
    bsh5 = np.roll(np.roll(bush5, 2, axis=0), -2, axis=1)
    scrub_mod = np.zeros((S, S, 3), dtype=np.float32)
    scrub_mod[..., 0] = -0.18 * bush5 - 0.20 * bsh5 * (1.0 - bush5)
    scrub_mod[..., 1] = -0.11 * bush5 - 0.20 * bsh5 * (1.0 - bush5)
    scrub_mod[..., 2] = -0.18 * bush5 - 0.18 * bsh5 * (1.0 - bush5)
    put_slot(1, 1, _normalize_hp(np.clip(q5 + scrub_mod, 0.05, 1.5), sigma=12.0, contrast=1.75))

    # 6 (2,1): Holm Oak & Gall Oak Woodland (Carrascal: mottled rounded 3-6m Mediterranean oak crowns)
    q6 = quilt_512(bank["oak"] or bank["pine"], seed=16)
    oak6 = (_worley_2d(S, 28, 161) * 0.65 + _worley_2d(S, 54, 162) * 0.35 - 0.52) * 0.26
    put_slot(2, 1, _normalize_hp(np.clip(q6 + oak6[..., None], 0.05, 1.5), sigma=11.0, contrast=1.80))

    # 7 (3,1): Pyrenean Valley Hay Meadows & Stone-Wall Pastures (Prados: mowed pasture lines & hedgerows)
    q7 = quilt_512(bank["meadow"] or bank["irrigated"], seed=17)
    mow7 = 0.05 * np.sin((xx * 32.0 + (_fbm_2d(S, 5, 2, 171) - 0.5) * 0.2) * 2.0 * math.pi)
    put_slot(3, 1, _normalize_hp(np.clip(q7 + mow7[..., None], 0.05, 1.5), sigma=14.0, contrast=1.55))

    # --- ROW 2: RIVERS, SUBALPINE FORESTS, CLIFFS & ALPINE PLATEAUS ---
    # 8 (0,2): Riparian Poplar Gallery & Braided River Shingle (Gravera: cobble bars & dense river foliage)
    q8 = quilt_512(bank["riparian"] or bank["badland"], seed=18)
    cob8 = (_worley_2d(S, 64, 181) - 0.5) * 0.16 + (_fbm_2d(S, 12, 3, 182) - 0.5) * 0.12
    put_slot(0, 2, _normalize_hp(np.clip(q8 + cob8[..., None], 0.05, 1.5), sigma=12.0, contrast=1.65))

    # 9 (1,2): Subalpine Pyrenean Scots Pine & Silver Fir Forest (Pinar: spiky dark conifer crowns & black shadow gaps!)
    q9 = quilt_512(bank["pine"] or bank["oak"], seed=19)
    cr9 = _worley_2d(S, 44, 191) * 0.58 + _worley_2d(S, 86, 192) * 0.42
    cr_sh9 = np.roll(np.roll(cr9, 2, axis=0), -2, axis=1)
    pine_mod = (cr9 - cr_sh9 * 0.55 - 0.24) * 0.42
    put_slot(1, 2, _normalize_hp(np.clip(q9 + pine_mod[..., None], 0.05, 1.5), sigma=10.0, contrast=1.90))

    # 10 (2,2): Sedimentary Limestone Cliffs & Conglomerate (Mallos / Pena Montanesa: horizontal strata + vertical cracks!)
    q10 = quilt_512(bank["cliff"] or bank["badland"], seed=20)
    fw10 = _fbm_2d(S, 6, 4, 201)
    bed10 = 0.14 * np.sin((yy * 24.0 + fw10 * 2.2) * 2.0 * math.pi)
    crack10 = -0.18 * (np.clip(1.0 - np.abs(_fbm_2d(S, 16, 4, 202) - 0.5) * 4.2, 0.0, 1.0) ** 2.0)
    put_slot(2, 2, _normalize_hp(np.clip(q10 + bed10[..., None] + crack10[..., None], 0.05, 1.5), sigma=14.0, contrast=1.85))

    # 11 (3,2): High-Alpine Pasture & Limestone Pavement (Pasto Alpino: grass tussocks, flat rock & trails)
    q11 = quilt_512(bank["meadow"] or bank["scrub"], seed=21)
    rock_slab = np.clip((_fbm_2d(S, 10, 4, 211) - 0.58) * 3.5, 0.0, 1.0) * 0.14
    tuss11 = (_worley_2d(S, 48, 212) - 0.5) * 0.14
    put_slot(3, 2, _normalize_hp(np.clip(q11 + rock_slab[..., None] + tuss11[..., None], 0.05, 1.5), sigma=13.0, contrast=1.65))

    # --- ROW 3: HIGH PYRENEES & URBAN PAVING ---
    # 12 (0,3): Pale Limestone Talus & Scree Fans (Pedrera: angular limestone rubble gravel & gravity chutes!)
    q12 = quilt_512(bank["cliff"] or bank["badland"], seed=22)
    rubble = (_worley_2d(S, 72, 221) * 0.55 + _worley_2d(S, 128, 222) * 0.45 - 0.50) * 0.24
    chute = 0.09 * np.sin((xx * 14.0 + _fbm_2d(S, 8, 3, 223) * 1.6) * 2.0 * math.pi)
    put_slot(0, 3, _normalize_hp(np.clip(q12 + rubble[..., None] + chute[..., None], 0.05, 1.5), sigma=12.0, contrast=1.75))

    # 13 (1,3): High-Pyrenees Granite & Slate Peak Crags (Alta Montana: jagged arêtes & dark fractures!)
    q13 = quilt_512(bank["cliff"] or bank["pine"], seed=23)
    ridge13 = 1.0 - np.abs(_fbm_2d(S, 12, 4, 231) - 0.5) * 2.0
    shatter13 = (_worley_2d(S, 32, 232) - 0.5) * 0.22
    put_slot(1, 3, _normalize_hp(np.clip(q13 + (ridge13 - 0.5)[..., None] * 0.26 + shatter13[..., None], 0.05, 1.5), sigma=13.0, contrast=1.90))

    # 14 (2,3): High-Alpine Wind-Scoured Snowfields & Cirques (Neveros: crisp directional wind-carved sastrugi, zero dots!)
    w_s = (_fbm_2d(S, 5, 3, 241) - 0.5) * 0.35
    sast_ridge = np.abs(np.sin((yy * 14.0 + xx * 3.0 + w_s * 6.5) * math.pi)) * 0.14
    sast_fine = (_fbm_2d(S, 18, 4, 242) - 0.5) * 0.16 + (_fbm_2d(S, 48, 2, 243) - 0.5) * 0.08
    put_slot(2, 3, _normalize_hp(np.clip(0.5 + sast_ridge + sast_fine, 0.05, 1.5), sigma=18.0, contrast=1.12))

    # 15 (3,3): Spanish Urban / Village Paving & Courtyards (Casco Urbano: plazas, stone gravel & rooftops)
    q15 = quilt_512(bank["urban"] or bank["cereal"], seed=25, sigma=18.0)
    put_slot(3, 3, _normalize_hp(np.clip(q15, 0.05, 1.5), sigma=16.0, contrast=1.55))

    SAT_OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(SAT_OUT_PATH, quality=91, optimize=True)
    print(f"Saved 16-pattern Northern Spain satellite atlas: {SAT_OUT_PATH} ({SAT_OUT_PATH.stat().st_size / 1024:.1f} KB)")


# ---------------------------------------------------------------------------
# Bake 512x512 (4x4 = 16 Z-Slices x 128x128) True 3D Volumetric Cloud Noise Atlas
# ---------------------------------------------------------------------------
def _worley_3d_periodic(N: int, D: int, cells_xy: int, cells_z: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pts_z = rng.random((cells_z, cells_xy, cells_xy), dtype=np.float32)
    pts_y = rng.random((cells_z, cells_xy, cells_xy), dtype=np.float32)
    pts_x = rng.random((cells_z, cells_xy, cells_xy), dtype=np.float32)

    zs = np.linspace(0.0, cells_z, D, endpoint=False, dtype=np.float32)
    ys = np.linspace(0.0, cells_xy, N, endpoint=False, dtype=np.float32)
    xs = np.linspace(0.0, cells_xy, N, endpoint=False, dtype=np.float32)
    iz, iy, ix = np.floor(zs).astype(np.int32), np.floor(ys).astype(np.int32), np.floor(xs).astype(np.int32)
    fz, fy, fx = zs - iz, ys - iy, xs - ix

    min_d2 = np.full((D, N, N), 9.0, dtype=np.float32)
    for dz in (-1, 0, 1):
        cz = (iz + dz) % cells_z
        for dy in (-1, 0, 1):
            cy = (iy + dy) % cells_xy
            for dx in (-1, 0, 1):
                cx = (ix + dx) % cells_xy
                pz = pts_z[cz[:, None, None], cy[None, :, None], cx[None, None, :]] + dz - fz[:, None, None]
                py = pts_y[cz[:, None, None], cy[None, :, None], cx[None, None, :]] + dy - fy[None, :, None]
                px = pts_x[cz[:, None, None], cy[None, :, None], cx[None, None, :]] + dx - fx[None, None, :]
                min_d2 = np.minimum(min_d2, px * px + py * py + pz * pz)
    return 1.0 - np.clip(np.sqrt(min_d2), 0.0, 1.0)


def _perlin_3d_periodic(N: int, D: int, cells_xy: int, cells_z: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    grid = rng.random((cells_z, cells_xy, cells_xy), dtype=np.float32)
    zs = np.linspace(0.0, cells_z, D, endpoint=False, dtype=np.float32)
    ys = np.linspace(0.0, cells_xy, N, endpoint=False, dtype=np.float32)
    xs = np.linspace(0.0, cells_xy, N, endpoint=False, dtype=np.float32)
    z0, y0, x0 = np.floor(zs).astype(np.int32) % cells_z, np.floor(ys).astype(np.int32) % cells_xy, np.floor(xs).astype(np.int32) % cells_xy
    z1, y1, x1 = (z0 + 1) % cells_z, (y0 + 1) % cells_xy, (x0 + 1) % cells_xy
    fz, fy, fx = zs - np.floor(zs), ys - np.floor(ys), xs - np.floor(xs)
    uz, uy, ux = fz * fz * (3 - 2 * fz), fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)

    def interp_z(zi):
        a = grid[zi[:, None, None], y0[None, :, None], x0[None, None, :]]
        b = grid[zi[:, None, None], y0[None, :, None], x1[None, None, :]]
        c = grid[zi[:, None, None], y1[None, :, None], x0[None, None, :]]
        d = grid[zi[:, None, None], y1[None, :, None], x1[None, None, :]]
        top = a * (1.0 - ux[None, None, :]) + b * ux[None, None, :]
        bot = c * (1.0 - ux[None, None, :]) + d * ux[None, None, :]
        return top * (1.0 - uy[None, :, None]) + bot * uy[None, :, None]

    return interp_z(z0) * (1.0 - uz[:, None, None]) + interp_z(z1) * uz[:, None, None]


def bake_cloud_volume_3d() -> None:
    """Bake 16 seamless 128x128 3D volume slices packed into a 512x512 RGB8 atlas (unpacked to native GL_TEXTURE_3D!)."""
    N, D = 128, 16
    p1 = _perlin_3d_periodic(N, D, 4, 4, 8101)
    p2 = _perlin_3d_periodic(N, D, 8, 8, 8102)
    w1 = _worley_3d_periodic(N, D, 4, 4, 8201)
    w2 = _worley_3d_periodic(N, D, 8, 4, 8202)
    w3 = _worley_3d_periodic(N, D, 16, 8, 8203)

    # R: Perlin-Worley billowy cumulus base shape (Schneider / Horizon Zero Dawn style!)
    perlin = p1 * 0.65 + p2 * 0.35
    pw_base = np.clip(perlin * 0.52 + w1 * 0.36 + w2 * 0.12, 0.0, 1.0)
    # G: Mid-frequency cauliflower Worley erosion lobes
    ero_mid = np.clip(w2 * 0.62 + w3 * 0.38, 0.0, 1.0)
    # B: High-frequency wispy turbulence detail
    wisp_hi = np.clip(w3 * 0.65 + p2 * 0.35, 0.0, 1.0)

    atlas = np.zeros((N * 4, N * 4, 3), dtype=np.uint8)
    for z in range(D):
        row, col = z // 4, z % 4
        atlas[row * N:(row + 1) * N, col * N:(col + 1) * N, 0] = np.clip(pw_base[z] * 255.0, 0, 255).astype(np.uint8)
        atlas[row * N:(row + 1) * N, col * N:(col + 1) * N, 1] = np.clip(ero_mid[z] * 255.0, 0, 255).astype(np.uint8)
        atlas[row * N:(row + 1) * N, col * N:(col + 1) * N, 2] = np.clip(wisp_hi[z] * 255.0, 0, 255).astype(np.uint8)

    Image.fromarray(atlas, "RGB").save(CLOUD_VOL_PATH, optimize=True)
    print(f"Saved 3D volumetric cloud noise atlas: {CLOUD_VOL_PATH} ({CLOUD_VOL_PATH.stat().st_size / 1024:.1f} KB)")


def verify_baked_atlases() -> None:
    """Self-check both baked atlases: dimensions, neutral high-pass calibration, and non-flat variance on every slot/slice."""
    sat = np.asarray(Image.open(SAT_OUT_PATH).convert("RGB"), dtype=np.float32)
    assert sat.shape == (2048, 2048, 3), f"Unexpected sat atlas shape: {sat.shape}"
    for r in range(4):
        for c in range(4):
            tile = sat[r * 512:(r + 1) * 512, c * 512:(c + 1) * 512]
            mean_v = float(np.mean(tile))
            std_v = float(np.std(tile))
            assert 115.0 <= mean_v <= 140.0, f"Slot ({c},{r}) biased mean={mean_v:.1f}"
            assert std_v >= 12.0, f"Slot ({c},{r}) too flat std={std_v:.1f}"

    vol = np.asarray(Image.open(CLOUD_VOL_PATH).convert("RGB"), dtype=np.float32)
    assert vol.shape == (512, 512, 3), f"Unexpected cloud volume shape: {vol.shape}"
    for z in range(16):
        r, c = z // 4, z % 4
        sl = vol[r * 128:(r + 1) * 128, c * 128:(c + 1) * 128]
        assert float(np.std(sl[..., 0])) >= 14.0 and float(np.std(sl[..., 1])) >= 18.0, f"Cloud slice {z} too flat"
    print("[SELF-CHECK OK] 16-pattern satellite atlas (2048x2048) + 16-slice 3D cloud volume (512x512) verified.")


def main() -> None:
    assert PACK_PATH.exists(), f"Missing pack: {PACK_PATH}"
    with PackReader(PACK_PATH) as r:
        bank = collect_pnoa_bank(r)
        print("Collected real PNOA regional patches:", ", ".join(f"{k}={len(v)}" for k, v in bank.items()))
    bake_16_regional_patterns(bank)
    bake_cloud_volume_3d()
    verify_baked_atlases()


if __name__ == "__main__":
    main()
