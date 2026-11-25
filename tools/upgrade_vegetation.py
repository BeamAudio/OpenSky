#!/usr/bin/env python3
"""upgrade_vegetation.py -- Rebuild crisp 2048x512 5-slot CC0 Poly Haven Botanical Tree Atlas
(Scots Pine, Riparian Poplar, Holm Oak, Maquis Shrub + Opaque Bark Strip) with RGB alpha dilation
(zero mipmap halos!) AND matching 3D volumetric multi-tier .glb tree models with exact atlas UVs.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402
from PIL import Image, ImageFilter  # noqa: E402
from scipy.ndimage import distance_transform_edt  # noqa: E402
from export_glb_models import GLBBuilder, GeoPart  # noqa: E402

PBR_DIR = ROOT / "game" / "assets" / "pbr"
MODELS_DIR = ROOT / "game" / "assets" / "models" / "vegetation"


def _tile_src(img: Image.Image, w: int, h: int) -> np.ndarray:
    arr = np.asarray(img, dtype=np.float32) / 255.0
    sh, sw = arr.shape[:2]
    yy = np.arange(h) % sh
    xx = np.arange(w) % sw
    return arr[np.ix_(yy, xx)]


def _fbm2(xx: np.ndarray, yy: np.ndarray, seed: int) -> np.ndarray:
    out = np.zeros_like(xx, dtype=np.float32)
    amp = 0.55
    freq = 1.0
    for o in range(4):
        ph1 = seed * 1.37 + o * 3.1
        ph2 = seed * 2.19 + o * 4.7
        v = np.sin(xx * freq * 6.28318 + ph1 + np.cos(yy * freq * 5.1 + ph2) * 1.4) * \
            np.cos(yy * freq * 6.28318 + ph2 + np.sin(xx * freq * 5.7 + ph1) * 1.4)
        out += amp * (v * 0.5 + 0.5)
        freq *= 2.05
        amp *= 0.5
    return out


def _paint_foliage_slot(
    species: str, size: int,
    leaf_alb: np.ndarray, leaf_nrm: np.ndarray, bark_alb: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Paint one crisp 512x512 botanical species card with real clustered leaves, airy sky gaps, bark trunk & 3D shading."""
    H = W = size
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32) / float(size)  # y=0 top, y=1 bottom
    rgb = np.zeros((H, W, 3), dtype=np.float32)
    nrm = np.dstack([np.full((H, W), 0.5), np.full((H, W), 0.5), np.ones((H, W))]).astype(np.float32)
    alpha = np.zeros((H, W), dtype=np.float32)

    # High-res real CC0 Poly Haven leaf micro-texture (4x tiled for crisp leaf clusters at 15m!)
    la = _tile_src(Image.fromarray((leaf_alb * 255).astype(np.uint8)).resize((256, 256), Image.LANCZOS), W, H)
    ln = _tile_src(Image.fromarray((leaf_nrm * 255).astype(np.uint8)).resize((256, 256), Image.LANCZOS), W, H) * 2.0 - 1.0
    ba = _tile_src(Image.fromarray((bark_alb * 255).astype(np.uint8)).resize((128, 256), Image.LANCZOS), W, H)

    # 1. Paint visible tapered bark trunk & main woody limbs first (behind foliage clusters!)
    rng = np.random.default_rng({"pine": 11, "poplar": 23, "oak": 37, "shrub": 53}[species])
    if species != "shrub":
        trunk_top = {"pine": 0.24, "poplar": 0.22, "oak": 0.44}[species]
        w_bot = {"pine": 0.044, "poplar": 0.036, "oak": 0.056}[species]
        w_top = {"pine": 0.008, "poplar": 0.007, "oak": 0.018}[species]
        t_prog = np.clip((yy - trunk_top) / max(0.05, 1.0 - trunk_top), 0.0, 1.0)
        sway_x = 0.50 + np.sin(yy * 7.0 + (1.0 if species == "oak" else 0.0)) * (0.022 if species == "oak" else 0.007)
        half_w = w_top + (w_bot - w_top) * t_prog
        t_mask = (yy >= trunk_top) & (np.abs(xx - sway_x) <= half_w)
        # Cylindrical bark shading
        cyl_nx = np.clip((xx - sway_x) / np.maximum(0.004, half_w), -1.0, 1.0)
        cyl_shade = 0.55 + 0.45 * np.sqrt(np.maximum(0.0, 1.0 - cyl_nx * cyl_nx))
        bark_tint = np.array([0.85, 0.72, 0.58], dtype=np.float32) if species == "pine" else np.array([0.74, 0.68, 0.60], dtype=np.float32)
        rgb[t_mask] = np.clip(ba[t_mask] * bark_tint * cyl_shade[t_mask, None], 0.0, 1.0)
        nrm[t_mask, 0] = cyl_nx[t_mask] * 0.5 + 0.5
        nrm[t_mask, 1] = 0.5
        nrm[t_mask, 2] = np.sqrt(np.maximum(0.1, 1.0 - cyl_nx[t_mask] ** 2)) * 0.5 + 0.5
        alpha[t_mask] = 1.0

        # Add visible angled main branches on Oak & Pine so airy sky gaps reveal real wood!
        if species in ("oak", "pine"):
            n_br = 6 if species == "oak" else 8
            for bi in range(n_br):
                by0 = rng.uniform(0.32, 0.68) if species == "pine" else rng.uniform(0.42, 0.60)
                side = -1.0 if (bi % 2 == 0) else 1.0
                bx0 = 0.50
                bx1 = 0.50 + side * rng.uniform(0.16, 0.34)
                by1 = by0 - rng.uniform(0.08, 0.18) if species == "oak" else by0 + rng.uniform(0.01, 0.06)
                dx, dy = bx1 - bx0, by1 - by0
                l2 = dx * dx + dy * dy + 1e-6
                t = np.clip(((xx - bx0) * dx + (yy - by0) * dy) / l2, 0.0, 1.0)
                dist = np.hypot(xx - (bx0 + t * dx), yy - (by0 + t * dy))
                bw = (0.016 if species == "oak" else 0.011) * (1.0 - 0.65 * t)
                bm = dist < bw
                rgb[bm] = np.clip(ba[bm] * 0.68, 0.0, 1.0)
                alpha[bm] = 1.0

    # 2. Build organic clusters of individual crisp foliage sub-boughs (with real sky gaps & 3D volume shading!)
    leaf_noise = _fbm2(xx * 3.8, yy * 3.8, seed={"pine": 101, "poplar": 202, "oak": 303, "shrub": 404}[species])
    leaf_fine  = _fbm2(xx * 11.5, yy * 11.5, seed={"pine": 505, "poplar": 606, "oak": 707, "shrub": 808}[species])
    # Leaf silhouette edge texture from Poly Haven leaf luminance
    la_luma = la[..., 0] * 0.299 + la[..., 1] * 0.587 + la[..., 2] * 0.114

    clusters: list[tuple[float, float, float, float, float]] = []  # (cx, cy, rx, ry, depth_01)
    if species == "pine":
        # Tiered Pyrenean Scots Pine boughs: feathered horizontal needle branches fanning out from the trunk!
        tiers = [
            (0.08, 0.05, 0.036, 7),
            (0.16, 0.12, 0.040, 10),
            (0.26, 0.19, 0.044, 14),
            (0.37, 0.25, 0.048, 18),
            (0.49, 0.31, 0.052, 22),
            (0.62, 0.35, 0.054, 24),
            (0.75, 0.36, 0.052, 24),
        ]
        for ty, span, ry_b, cnt in tiers:
            for k in range(cnt):
                frac = (k / max(1, cnt - 1)) * 2.0 - 1.0
                cx = 0.50 + frac * span * rng.uniform(0.86, 1.04)
                cy = ty + abs(frac) * 0.055 + rng.uniform(-0.022, 0.022)
                rx = rng.uniform(0.050, 0.092) * (0.65 + 0.45 * (1.0 - abs(frac)))
                ry = ry_b * rng.uniform(0.55, 0.85)  # Wide flat conifer needle sprays, never round balls!
                clusters.append((cx, cy, rx, ry, rng.uniform(0.0, 1.0)))
    elif species == "poplar":
        # Tall feathery Spanish riverside poplar with tightly nested multi-scale leaf sprays (zero floating puffs!)
        for _ in range(135):
            cy = rng.uniform(0.06, 0.80)
            width_env = 0.08 + 0.19 * math.sin(max(0.0, min(1.0, (cy - 0.04) / 0.78)) * math.pi)
            cx = 0.50 + rng.uniform(-width_env, width_env) * rng.uniform(0.45, 1.0)
            rx = rng.uniform(0.038, 0.072)
            ry = rng.uniform(0.036, 0.068)
            clusters.append((cx, cy, rx, ry, rng.uniform(0.0, 1.0)))
    elif species == "oak":
        # Broad gnarled Spanish Holm Oak (Encina) umbrella crown with tightly nested leafy sub-sprays!
        lobes = [(0.50, 0.24, 0.23, 0.15), (0.32, 0.35, 0.18, 0.13), (0.68, 0.34, 0.18, 0.13),
                 (0.42, 0.44, 0.19, 0.12), (0.58, 0.44, 0.18, 0.12)]
        for lcx, lcy, lrx, lry in lobes:
            for _ in range(30):
                ang = rng.uniform(0.0, math.tau)
                rr = math.sqrt(rng.uniform(0.0, 1.0)) * 0.92
                cx = lcx + math.cos(ang) * lrx * rr
                cy = lcy + math.sin(ang) * lry * rr
                rx = rng.uniform(0.040, 0.072)
                ry = rng.uniform(0.032, 0.056)
                clusters.append((cx, cy, rx, ry, rng.uniform(0.0, 1.0)))
    else:
        # Dense grounded Mediterranean maquis bush mound (fills bottom cleanly, no floating base!)
        for _ in range(125):
            ang = rng.uniform(math.pi, math.tau)
            rr = math.sqrt(rng.uniform(0.0, 1.0)) * 0.94
            cx = 0.50 + math.cos(ang) * 0.40 * rr
            cy = 0.96 + math.sin(ang) * 0.54 * rr
            rx = rng.uniform(0.042, 0.075)
            ry = rng.uniform(0.035, 0.062)
            clusters.append((cx, cy, rx, ry, rng.uniform(0.0, 1.0)))

    # Painter's order: paint deeper/darker clusters first, brighter outer clusters on top!
    clusters.sort(key=lambda c: c[4])
    base_col = {
        "pine":   np.array([0.46, 0.62, 0.40], dtype=np.float32),
        "poplar": np.array([0.66, 0.80, 0.46], dtype=np.float32),
        "oak":    np.array([0.52, 0.64, 0.40], dtype=np.float32),
        "shrub":  np.array([0.58, 0.68, 0.40], dtype=np.float32),
    }[species]

    for (cx, cy, rx, ry, depth) in clusters:
        x0 = max(0, int((cx - rx * 1.35) * W))
        x1 = min(W, int((cx + rx * 1.35) * W) + 1)
        y0 = max(0, int((cy - ry * 1.35) * H))
        y1 = min(H, int((cy + ry * 1.35) * H) + 1)
        if x1 <= x0 or y1 <= y0:
            continue
        sx = (xx[y0:y1, x0:x1] - cx) / max(0.01, rx)
        sy = (yy[y0:y1, x0:x1] - cy) / max(0.01, ry)
        r2 = sx * sx + sy * sy
        # Crisp ragged individual-leaf silhouette mask (organic scalloped leaf tufts, NOT blurry airbrush!)
        jagged = (leaf_noise[y0:y1, x0:x1] - 0.5) * 0.85 + (leaf_fine[y0:y1, x0:x1] - 0.5) * 0.65 + (la_luma[y0:y1, x0:x1] - 0.35) * 0.50
        mask = (r2 + jagged) < 0.90
        if not np.any(mask):
            continue

        # 3D cluster dome lighting + tree-level upper-sun / lower-AO gradient!
        c_shade = np.clip(0.58 + 0.44 * depth - 0.24 * sy - 0.14 * r2, 0.34, 1.24)
        h_grad  = np.clip(1.12 - 0.36 * yy[y0:y1, x0:x1], 0.68, 1.15)
        leaf_c = np.clip(la[y0:y1, x0:x1] * base_col * (1.28 * c_shade * h_grad)[..., None], 0.02, 0.96)
        # Subtle warm sunlit highlight on upper-right leaf tips
        tip_w = np.clip((-sy * 0.6 + sx * 0.4 + 0.35) * depth, 0.0, 1.0)
        leaf_c = np.clip(leaf_c * (1.0 + tip_w[..., None] * np.array([0.22, 0.20, 0.06], dtype=np.float32)), 0.02, 0.98)

        # Blend real leaf normal + cluster spherical dome normal
        dnx = np.clip(sx * 0.68, -0.85, 0.85)
        dny = np.clip(-sy * 0.68 + 0.22, -0.75, 0.90)
        dnz = np.sqrt(np.maximum(0.08, 1.0 - dnx * dnx - dny * dny))
        cn = ln[y0:y1, x0:x1]
        nx = cn[..., 0] * 0.48 + dnx * 0.52
        ny = cn[..., 1] * 0.48 + dny * 0.52
        nz = cn[..., 2] * 0.48 + dnz * 0.52
        nl = np.sqrt(nx * nx + ny * ny + nz * nz + 1e-6)

        sub_rgb = rgb[y0:y1, x0:x1]
        sub_nrm = nrm[y0:y1, x0:x1]
        sub_a = alpha[y0:y1, x0:x1]
        sub_rgb[mask] = leaf_c[mask]
        sub_nrm[mask, 0] = (nx[mask] / nl[mask]) * 0.5 + 0.5
        sub_nrm[mask, 1] = (ny[mask] / nl[mask]) * 0.5 + 0.5
        sub_nrm[mask, 2] = (nz[mask] / nl[mask]) * 0.5 + 0.5
        sub_a[mask] = 1.0

    # Dilate nearest opaque foliage RGB into transparent pixels -> ZERO grey/white mipmap halos!
    solid = alpha > 0.5
    if np.any(solid) and not np.all(solid):
        _, nearest_idx = distance_transform_edt(~solid, return_indices=True)
        rgb[~solid] = rgb[nearest_idx[0][~solid], nearest_idx[1][~solid]]
        nrm[~solid] = nrm[nearest_idx[0][~solid], nearest_idx[1][~solid]]

    rgba_u8 = np.dstack([
        np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8),
        np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8),
    ])
    nrm_u8 = np.clip(nrm * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return rgba_u8, nrm_u8


def build_5slot_tree_atlas() -> None:
    """Build 2048x512 5-slot atlas: 0=pine, 1=poplar, 2=oak, 3=shrub, 4=opaque bark strip!"""
    leaf_alb = np.asarray(Image.open(PBR_DIR / "leaf_src_albedo.jpg").convert("RGB"), dtype=np.float32) / 255.0
    leaf_nrm = np.asarray(Image.open(PBR_DIR / "leaf_src_normal.jpg").convert("RGB"), dtype=np.float32) / 255.0
    bark_alb = np.asarray(Image.open(PBR_DIR / "bark_src_albedo.jpg").convert("RGB"), dtype=np.float32) / 255.0

    # 5 equal slots of 400px -> 2000px? Use exact power-of-2: 2048x512 where each slot is 409.6px -> better:
    # 5 slots packed cleanly at u = [k*0.20 .. (k+1)*0.20] on a 2560x512 or 2048x512 image (exact 512px per slot = 2560x512?
    # Keep power-of-two friendly 2048x512: 4 foliage slots x 448px + 1 bark slot x 256px!
    # Wait: even cleaner & 100% power-of-two exact! 4 columns x 512px = 2048x512, where each species has its OWN
    # crisp bark trunk painted inside its slot + 3D trunk cylinder samples the bottom-center bark strip of slot 0..2,
    # OR 2048x512 with 4 slots [0..0.25, 0.25..0.50, 0.50..0.75, 0.75..1.00] AND 3D trunk samples bark at u=[0.004..0.04]
    # No: 5 slots at exact u=[0.0..0.2, 0.2..0.4, 0.4..0.6, 0.6..0.8, 0.8..1.0] on 2048x512 works cleanly with mipmaps
    # if we pad 4px bleed inside each slot! Let's use 2048x512 -> 4x 448px + 1x 256px OR exact 2560x512?
    # OpenGL 4.6 handles NPOT mipmaps cleanly, AND 2048x512 with 5 exact slots (410, 410, 410, 410, 408) is power-of-2!)
    slot_w = 408
    W, H = 2048, 512
    atlas_rgba = np.zeros((H, W, 4), dtype=np.uint8)
    atlas_nrm  = np.zeros((H, W, 3), dtype=np.uint8)

    species_list = ["pine", "poplar", "oak", "shrub"]
    for idx, sp in enumerate(species_list):
        rgba_512, nrm_512 = _paint_foliage_slot(sp, 512, leaf_alb, leaf_nrm, bark_alb)
        im_r = Image.fromarray(rgba_512, "RGBA").resize((slot_w, H), Image.LANCZOS)
        im_n = Image.fromarray(nrm_512, "RGB").resize((slot_w, H), Image.LANCZOS)
        x0 = idx * 410
        atlas_rgba[:, x0 : x0 + slot_w] = np.asarray(im_r, dtype=np.uint8)
        atlas_nrm[:,  x0 : x0 + slot_w] = np.asarray(im_n, dtype=np.uint8)
        # 2px clamp bleed between slots
        atlas_rgba[:, x0 + slot_w : x0 + 410] = atlas_rgba[:, x0 + slot_w - 1 : x0 + slot_w]
        atlas_nrm[:,  x0 + slot_w : x0 + 410] = atlas_nrm[:,  x0 + slot_w - 1 : x0 + slot_w]

    # Slot 4 (x = 1640 .. 2048): Opaque Poly Haven Bark Strip (alpha = 255!)
    bw = W - 1640
    b_im = Image.open(PBR_DIR / "bark_src_albedo.jpg").convert("RGB").resize((bw, H), Image.LANCZOS)
    b_arr = np.asarray(b_im, dtype=np.uint8)
    atlas_rgba[:, 1640:W, :3] = b_arr
    atlas_rgba[:, 1640:W, 3] = 255
    atlas_nrm[:, 1640:W] = np.array([128, 128, 255], dtype=np.uint8)

    Image.fromarray(atlas_rgba, "RGBA").save(PBR_DIR / "trees_atlas_albedo.png", optimize=True)
    Image.fromarray(atlas_nrm, "RGB").save(PBR_DIR / "trees_atlas_normal.png", optimize=True)
    print("Wrote 2048x512 crisp 5-slot trees_atlas_albedo.png & trees_atlas_normal.png")


def build_tree_glb(species: str, slot_idx: int, out_path: Path) -> None:
    """Build volumetric 3D multi-tier botanical tree .glb whose UVs map EXACTLY into our 5-slot trees_atlas!"""
    gb = GLBBuilder()
    mat = gb.add_material("TreeAtlasMat", (1, 1, 1, 1), metallic=0.0, roughness=0.92, alpha_mode="MASK", alpha_cutoff=0.40, double_sided=True)
    geo = GeoPart()

    # Exact UV bounds inside 2048x512 5-slot atlas (with safe 3px margin!)
    u0 = (slot_idx * 410 + 3.0) / 2048.0
    u1 = (slot_idx * 410 + 405.0) / 2048.0
    um = 0.5 * (u0 + u1)
    bu0, bu1 = (1648.0 / 2048.0), (2040.0 / 2048.0)

    if species == "pine":
        # Real 3D tapered trunk + 3 crossed vertical branch fans + 3 tiered conical bough skirts (volumetric from above!)
        geo.add_cyl_between((0.0, -0.14, 0.0), (0.0, 0.72, 0.0), 0.055, 0.012, segs=5, u_range=(bu0, bu1), v_range=(0.04, 0.96))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.48, math.sin(ang) * 0.48
            geo.add_quad((-cx, 0.02, -cz), (cx, 0.02, cz), (cx, 1.0, cz), (-cx, 1.0, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))
        for yb, yt, rad_t, rot, v_apex, v_rim in [
            (0.22, 0.50, 0.45, 0.00, 0.22, 0.62),
            (0.45, 0.74, 0.34, 0.52, 0.16, 0.50),
            (0.68, 0.96, 0.22, 1.05, 0.08, 0.38),
        ]:
            apex = (0.0, yt, 0.0)
            for s in range(5):
                a0, a1 = rot + s * (math.tau / 5), rot + (s + 1) * (math.tau / 5)
                p0 = (math.cos(a0) * rad_t, yb, math.sin(a0) * rad_t)
                p1 = (math.cos(a1) * rad_t, yb, math.sin(a1) * rad_t)
                n = (math.cos((a0 + a1) * 0.5) * 0.75, 0.66, math.sin((a0 + a1) * 0.5) * 0.75)
                geo.add_tri(apex, p1, p0, (um, v_apex), (u1 - 0.03, v_rim), (u0 + 0.03, v_rim), n)
    elif species == "poplar":
        geo.add_cyl_between((0.0, -0.14, 0.0), (0.0, 0.78, 0.0), 0.042, 0.010, segs=5, u_range=(bu0, bu1), v_range=(0.04, 0.96))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.30, math.sin(ang) * 0.30
            geo.add_quad((-cx, 0.05, -cz), (cx, 0.05, cz), (cx, 1.0, cz), (-cx, 1.0, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))
        # Upper crown dome so top-down views look leafy instead of flat star-crosses
        apex = (0.0, 0.96, 0.0)
        for s in range(5):
            a0, a1 = s * (math.tau / 5), (s + 1) * (math.tau / 5)
            p0 = (math.cos(a0) * 0.24, 0.58, math.sin(a0) * 0.24)
            p1 = (math.cos(a1) * 0.24, 0.58, math.sin(a1) * 0.24)
            n = (math.cos((a0 + a1) * 0.5) * 0.65, 0.76, math.sin((a0 + a1) * 0.5) * 0.65)
            geo.add_tri(apex, p1, p0, (um, 0.14), (u1 - 0.04, 0.52), (u0 + 0.04, 0.52), n)
    elif species == "oak":
        geo.add_cyl_between((0.0, -0.14, 0.0), (0.0, 0.48, 0.0), 0.072, 0.036, segs=5, u_range=(bu0, bu1), v_range=(0.04, 0.96))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.54, math.sin(ang) * 0.54
            geo.add_quad((-cx, 0.04, -cz), (cx, 0.04, cz), (cx, 0.98, cz), (-cx, 0.98, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))
        # Broad double-tier Mediterranean umbrella canopy dome!
        for yb, yt, rad_t, rot in [(0.42, 0.88, 0.50, 0.0), (0.60, 0.98, 0.36, 0.62)]:
            top = (0.0, yt, 0.0)
            for s in range(6):
                a0, a1 = rot + s * (math.tau / 6), rot + (s + 1) * (math.tau / 6)
                p0 = (math.cos(a0) * rad_t, yb, math.sin(a0) * rad_t)
                p1 = (math.cos(a1) * rad_t, yb, math.sin(a1) * rad_t)
                n = (math.cos((a0 + a1) * 0.5) * 0.58, 0.81, math.sin((a0 + a1) * 0.5) * 0.58)
                geo.add_tri(top, p1, p0, (um, 0.16), (u1 - 0.03, 0.48), (u0 + 0.03, 0.48), n)
    else:
        # Rounded volumetric maquis bush (3 crossed cards + soft crown cap -> lush undergrowth!)
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.58, math.sin(ang) * 0.58
            geo.add_quad((-cx, -0.05, -cz), (cx, -0.05, cz), (cx, 0.92, cz), (-cx, 0.92, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.34), (u0, 0.34))
        top = (0.0, 0.90, 0.0)
        for s in range(5):
            a0, a1 = s * (math.tau / 5), (s + 1) * (math.tau / 5)
            p0 = (math.cos(a0) * 0.46, 0.42, math.sin(a0) * 0.46)
            p1 = (math.cos(a1) * 0.46, 0.42, math.sin(a1) * 0.46)
            geo.add_tri(top, p1, p0, (um, 0.48), (u1 - 0.04, 0.82), (u0 + 0.04, 0.82), (0.0, 1.0, 0.0))

    m_idx = gb.add_mesh(out_path.stem, [geo.to_prim(mat)])
    gb.add_node(out_path.stem, m_idx)
    gb.save(out_path)


def main() -> None:
    build_5slot_tree_atlas()
    for idx, (sp, fname) in enumerate([
        ("pine",   "tree_scots_pine.glb"),
        ("poplar", "tree_poplar.glb"),
        ("oak",    "tree_holm_oak.glb"),
        ("shrub",  "shrub_maquis.glb"),
    ]):
        build_tree_glb(sp, idx, MODELS_DIR / fname)
    # Self-check: verify atlas dimensions & opaque bark alpha
    chk = np.asarray(Image.open(PBR_DIR / "trees_atlas_albedo.png").convert("RGBA"))
    assert chk.shape == (512, 2048, 4), f"Unexpected atlas shape {chk.shape}"
    assert chk[:, 1650:2040, 3].min() == 255, "Bark strip must be 100% opaque!"
    print("[OK] Upgraded 4 volumetric vegetation .glb models + 5-slot botanical atlas!")


if __name__ == "__main__":
    main()
