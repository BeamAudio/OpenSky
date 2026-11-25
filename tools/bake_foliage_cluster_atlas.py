#!/usr/bin/env python3
"""Offline baker for HQ seamless 2x2 botanical foliage cluster albedo+alpha & normal atlas.

Generates:
  - game/assets/pbr/foliage_cluster_albedo.png (1024x1024 RGBA, mip-safe dilated RGB)
  - game/assets/pbr/foliage_cluster_normal.png (1024x1024 RGB tangent-space normals)

Quadrants (each 512x512 seamlessly tileable):
  (0,0) Holm Oak (Encina)       - dense oval Mediterranean evergreen leaves
  (1,0) Scots Pine (Pino)       - radiating pine needle fascicles & brown twigs
  (0,1) Valley Poplar (Chopo)   - broad bright riverine leaves
  (1,1) Maquis Shrub (Matorral) - fine tight Mediterranean scrub foliage
"""

from __future__ import annotations

import math
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import binary_dilation, distance_transform_edt, gaussian_filter

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "game" / "assets" / "pbr"
TILE = 512


def _stamp_wrapped(buf_rgb: np.ndarray, buf_a: np.ndarray, buf_h: np.ndarray,
                   buf_nx: np.ndarray, buf_ny: np.ndarray,
                   cx: float, cy: float, angle: float,
                   rx: float, ry: float, col: np.ndarray,
                   depth: float, pointed: float = 0.35) -> None:
    """Stamp one shaded botanical leaf with center vein and height-ordered occlusion."""
    rad = int(math.ceil(max(rx, ry))) + 2
    ys = np.arange(-rad, rad + 1, dtype=np.float32)
    xs = np.arange(-rad, rad + 1, dtype=np.float32)
    gx, gy = np.meshgrid(xs, ys)
    ca, sa = math.cos(angle), math.sin(angle)
    lx = (gx * ca + gy * sa) / max(rx, 1.0)
    ly = (-gx * sa + gy * ca) / max(ry, 1.0)

    # Taper leaf width toward tip (+x) and stem (-x) for realistic botanical silhouette.
    taper = np.clip(1.0 - pointed * (lx * lx) - 0.18 * np.maximum(0.0, lx), 0.18, 1.15)
    r2 = lx * lx + (ly / taper) ** 2
    mask = r2 <= 1.0
    if not np.any(mask):
        return

    # Leaf height (domed across width with subtle midrib crease) + alpha feather on edge.
    edge_a = np.clip((1.0 - r2) * 4.5, 0.0, 1.0)
    vein = 0.08 * np.exp(-((ly * 5.5) ** 2))
    dome = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
    h_val = depth + 0.12 * dome + vein

    # Local leaf tilt normal rotated into tile space.
    lnx = -0.35 * lx + 0.12 * np.sign(lx) * (1.0 - dome)
    lny = -0.55 * ly / taper + (0.28 * np.sign(ly) * np.exp(-((ly * 6.0) ** 2)))
    tnx = lnx * ca - lny * sa
    tny = lnx * sa + lny * ca

    # Leaf surface shading variation: slightly brighter toward sunlit tip and midrib.
    shade = (0.84 + 0.24 * dome + 0.10 * lx + 0.08 * vein).astype(np.float32)

    ix = (int(round(cx)) + np.arange(-rad, rad + 1)) % TILE
    iy = (int(round(cy)) + np.arange(-rad, rad + 1)) % TILE
    sub_h = buf_h[np.ix_(iy, ix)]
    sub_a = buf_a[np.ix_(iy, ix)]

    # Depth-test against already stamped leaves; allow anti-aliased edge compositing.
    front = mask & (h_val >= sub_h - 0.025)
    if not np.any(front):
        return

    src_a = np.where(front, edge_a, 0.0)
    w = src_a / np.maximum(src_a + sub_a * (1.0 - src_a), 1e-4)
    out_a = np.clip(src_a + sub_a * (1.0 - src_a), 0.0, 1.0)

    sub_rgb = buf_rgb[np.ix_(iy, ix)]
    leaf_rgb = np.clip(col[None, None, :] * shade[:, :, None], 0.0, 1.0)
    sub_rgb = sub_rgb * (1.0 - w[:, :, None]) + leaf_rgb * w[:, :, None]

    sub_nx = buf_nx[np.ix_(iy, ix)]
    sub_ny = buf_ny[np.ix_(iy, ix)]
    sub_nx = sub_nx * (1.0 - w) + tnx * w
    sub_ny = sub_ny * (1.0 - w) + tny * w
    sub_h = np.where(front & (edge_a > 0.35), np.maximum(sub_h, h_val), sub_h)

    buf_rgb[np.ix_(iy, ix)] = sub_rgb
    buf_a[np.ix_(iy, ix)] = out_a
    buf_h[np.ix_(iy, ix)] = sub_h
    buf_nx[np.ix_(iy, ix)] = sub_nx
    buf_ny[np.ix_(iy, ix)] = sub_ny


def _draw_twig(buf_rgb: np.ndarray, buf_a: np.ndarray, buf_h: np.ndarray,
               buf_nx: np.ndarray, buf_ny: np.ndarray,
               x0: float, y0: float, angle: float, length: float, width: float,
               col: np.ndarray, depth: float) -> None:
    steps = max(8, int(length * 1.4))
    for s in range(steps):
        t = s / float(steps - 1)
        px = x0 + math.cos(angle) * length * t
        py = y0 + math.sin(angle) * length * t
        w_here = width * (1.0 - 0.55 * t)
        _stamp_wrapped(buf_rgb, buf_a, buf_h, buf_nx, buf_ny,
                       px, py, angle, max(2.2, w_here * 1.6), max(1.1, w_here),
                       col, depth, pointed=0.1)


def _dilate_rgb_into_transparent(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Fill transparent pixels with nearest opaque leaf color to prevent dark mip fringes."""
    solid = alpha > 0.20
    if np.all(solid) or not np.any(solid):
        return rgb
    _, idx = distance_transform_edt(~solid, return_indices=True)
    filled = rgb[idx[0], idx[1]]
    return np.where(solid[:, :, None], rgb, filled)


def _bake_species(kind: int, seed: int) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    rgb = np.zeros((TILE, TILE, 3), dtype=np.float32)
    alpha = np.zeros((TILE, TILE), dtype=np.float32)
    height = np.zeros((TILE, TILE), dtype=np.float32)
    nx = np.zeros((TILE, TILE), dtype=np.float32)
    ny = np.zeros((TILE, TILE), dtype=np.float32)

    # Authentic dark, low-saturation Pyrenean / Mediterranean botanical palettes
    # (prevents plastic neon-green look under bright sunlight; runtime blends PNOA + season).
    if kind == 0:  # Holm Oak (Encina): dense dark olive-grey Mediterranean leaves
        twig_col = np.array([0.10, 0.08, 0.06], dtype=np.float32)
        base_cols = [
            np.array([0.075, 0.115, 0.052], dtype=np.float32),
            np.array([0.098, 0.145, 0.064], dtype=np.float32),
            np.array([0.055, 0.088, 0.042], dtype=np.float32),
            np.array([0.125, 0.165, 0.078], dtype=np.float32),
        ]
        grid_n = 9
        leaves_per_cluster = 16
        rx_range = (9.5, 16.0)
        ry_range = (4.6, 7.8)
        spread = 32.0
        pointed = 0.30
    elif kind == 1:  # Scots Pine (Pino silvestre): dark blue-green needle tufts & reddish twigs
        twig_col = np.array([0.16, 0.09, 0.055], dtype=np.float32)
        base_cols = [
            np.array([0.052, 0.098, 0.056], dtype=np.float32),
            np.array([0.074, 0.130, 0.068], dtype=np.float32),
            np.array([0.040, 0.076, 0.046], dtype=np.float32),
            np.array([0.096, 0.148, 0.078], dtype=np.float32),
        ]
        grid_n = 9
        leaves_per_cluster = 18
        rx_range = (11.5, 19.5)
        ry_range = (1.5, 2.5)
        spread = 28.0
        pointed = 0.58
    elif kind == 2:  # Valley Poplar (Chopo): natural mid-olive riverine leaves
        twig_col = np.array([0.13, 0.11, 0.075], dtype=np.float32)
        base_cols = [
            np.array([0.110, 0.175, 0.060], dtype=np.float32),
            np.array([0.145, 0.215, 0.074], dtype=np.float32),
            np.array([0.085, 0.138, 0.048], dtype=np.float32),
            np.array([0.175, 0.238, 0.092], dtype=np.float32),
        ]
        grid_n = 8
        leaves_per_cluster = 15
        rx_range = (11.5, 19.0)
        ry_range = (7.2, 12.2)
        spread = 35.0
        pointed = 0.44
    else:  # Maquis Shrub (Matorral): dusty Mediterranean sage/olive scrub sprigs
        twig_col = np.array([0.12, 0.095, 0.068], dtype=np.float32)
        base_cols = [
            np.array([0.095, 0.128, 0.064], dtype=np.float32),
            np.array([0.125, 0.155, 0.078], dtype=np.float32),
            np.array([0.072, 0.098, 0.050], dtype=np.float32),
            np.array([0.148, 0.168, 0.088], dtype=np.float32),
        ]
        grid_n = 10
        leaves_per_cluster = 17
        rx_range = (6.8, 11.5)
        ry_range = (2.8, 4.8)
        spread = 26.0
        pointed = 0.36

    # Stratified jittered cluster centers prevent big bald holes while keeping organic ~25-32% sky gaps.
    centers = []
    cell = TILE / float(grid_n)
    for gy in range(grid_n):
        for gx in range(grid_n):
            centers.append((
                (gx + float(rng.uniform(0.12, 0.88))) * cell,
                (gy + float(rng.uniform(0.12, 0.88))) * cell,
            ))
    rng.shuffle(centers)
    n_clusters = len(centers)

    # Layer 1 (deeper interior): woody twigs & shaded inner foliage (~0.15..0.50 depth)
    # Layer 2 (mid/outer): sunlit outer leaf rosettes (~0.50..0.95 depth)
    for ci, (cx, cy) in enumerate(centers):
        layer_t = ci / float(max(1, n_clusters - 1))
        base_ang = float(rng.uniform(0.0, math.tau))
        depth_c = 0.18 + 0.72 * layer_t

        # Subtle woody branch stem inside cluster
        if rng.random() < 0.72:
            _draw_twig(rgb, alpha, height, nx, ny, cx, cy, base_ang,
                       float(rng.uniform(18.0, 42.0)), float(rng.uniform(1.8, 3.0)),
                       twig_col * (0.72 + 0.40 * layer_t), depth_c - 0.04)

        for li in range(leaves_per_cluster):
            ang = base_ang + (li / float(leaves_per_cluster)) * math.tau + float(rng.normal(0.0, 0.28))
            dist = float(rng.uniform(2.0, spread)) * (0.55 + 0.45 * rng.random())
            lx = cx + math.cos(ang) * dist
            ly = cy + math.sin(ang) * dist
            rx = float(rng.uniform(*rx_range))
            ry = float(rng.uniform(*ry_range))
            col = base_cols[int(rng.integers(0, len(base_cols)))].copy()
            # Natural depth AO: inner clusters darker, outer tips brighter & warmer
            col *= (0.64 + 0.48 * layer_t) * float(rng.uniform(0.88, 1.12))
            if kind == 1:
                # Pine fascicle: 3 needles fanning out from shoot point
                for fan in (-0.32, 0.0, 0.32):
                    _stamp_wrapped(rgb, alpha, height, nx, ny,
                                   lx + math.cos(ang + fan) * rx * 0.45,
                                   ly + math.sin(ang + fan) * rx * 0.45,
                                   ang + fan, rx, ry, col, depth_c + float(rng.uniform(-0.04, 0.05)), pointed)
            else:
                _stamp_wrapped(rgb, alpha, height, nx, ny,
                               lx, ly, ang, rx, ry, col, depth_c + float(rng.uniform(-0.04, 0.06)), pointed)

    # Slight macro height-gradient normal blend for coherent cluster shading
    h_smooth = gaussian_filter(height, sigma=2.2, mode="wrap")
    gx = (np.roll(h_smooth, -1, axis=1) - np.roll(h_smooth, 1, axis=1)) * 2.2
    gy = (np.roll(h_smooth, -1, axis=0) - np.roll(h_smooth, 1, axis=0)) * 2.2
    nx_tot = np.clip(nx * 0.72 - gx, -0.92, 0.92)
    ny_tot = np.clip(ny * 0.72 - gy, -0.92, 0.92)
    nz_tot = np.sqrt(np.clip(1.0 - nx_tot * nx_tot - ny_tot * ny_tot, 0.08, 1.0))

    rgb_dilated = _dilate_rgb_into_transparent(rgb, alpha)
    rgba_u8 = np.zeros((TILE, TILE, 4), dtype=np.uint8)
    rgba_u8[:, :, :3] = np.clip(np.power(np.clip(rgb_dilated, 0.0, 1.0), 1.0 / 2.2) * 255.0, 0, 255).astype(np.uint8)
    rgba_u8[:, :, 3] = np.clip(alpha * 255.0, 0, 255).astype(np.uint8)

    nrm_u8 = np.zeros((TILE, TILE, 3), dtype=np.uint8)
    nrm_u8[:, :, 0] = np.clip((nx_tot * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8)
    nrm_u8[:, :, 1] = np.clip((ny_tot * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8)
    nrm_u8[:, :, 2] = np.clip((nz_tot * 0.5 + 0.5) * 255.0, 0, 255).astype(np.uint8)

    cov = float(np.mean(alpha > 0.45)) * 100.0
    print(f"[FOLIAGE] Species {kind}: opaque coverage={cov:.1f}%")
    return rgba_u8, nrm_u8


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    atlas_alb = np.zeros((TILE * 2, TILE * 2, 4), dtype=np.uint8)
    atlas_nrm = np.zeros((TILE * 2, TILE * 2, 3), dtype=np.uint8)
    coords = [(0, 0), (1, 0), (0, 1), (1, 1)]
    for kind, (qx, qy) in enumerate(coords):
        alb, nrm = _bake_species(kind, seed=4201 + kind * 97)
        atlas_alb[qy * TILE:(qy + 1) * TILE, qx * TILE:(qx + 1) * TILE] = alb
        atlas_nrm[qy * TILE:(qy + 1) * TILE, qx * TILE:(qx + 1) * TILE] = nrm

    alb_path = OUT_DIR / "foliage_cluster_albedo.png"
    nrm_path = OUT_DIR / "foliage_cluster_normal.png"
    Image.fromarray(atlas_alb, mode="RGBA").save(alb_path, optimize=True)
    Image.fromarray(atlas_nrm, mode="RGB").save(nrm_path, optimize=True)
    print(f"[FOLIAGE] Wrote {alb_path} & {nrm_path}")


if __name__ == "__main__":
    main()
