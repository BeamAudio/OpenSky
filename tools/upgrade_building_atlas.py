#!/usr/bin/env python3
"""upgrade_building_atlas.py -- Bake 16-Tile (4x4) 2048x2048 Spanish Architectural PBR Atlas
AND 6 Truly Distinct 3D Spanish Building .glb Archetypes (zero generic gable clones!).

Atlas Layout (4 columns x 4 rows, 512x512 per tile -> 2048x2048 total):
  Row 0 (Roofs - seamlessly tileable in X & Y):
    (0,0) Warm variegated Spanish Teja Arabe clay barrel tiles
    (1,0) Sun-bleached weathered historic clay tiles (ochre/sienna/dusty grey mosaic)
    (2,0) Spanish flat apartment terrace roof (terracotta pavers, gravel, skylights, vents, drains)
    (3,0) Galvanized standing-seam industrial steel & Pyrenean slate roof
  Row 1 (Ground-Floor Commercial Shopfronts & Plinths - seamlessly tileable in X):
    (0,1) Zaragoza sandstone/brick retail arcade & varied subtle cafe awnings
    (1,1) Historic Aragonese rusticated stone archway & wooden portals
    (2,1) Modern Spanish granite/concrete commercial lobby & glass storefronts
    (3,1) Industrial poured-concrete dock & corrugated roll-up shutter doors
  Row 2 (Upper-Floor Facades Set A - 2 stories x 4 bays, seamlessly tileable in X & Y):
    (0,2) Zaragoza red Ladrillo Caravista exposed brick + white balcony slabs + persianas
    (1,2) Aragonese warm ochre stucco + wrought-iron Juliet balconies + wooden shutters
    (2,2) Modern Spanish limestone/precast concrete apartments + wide glass loggias
    (3,2) Pyrenean dressed golden-grey ashlar stone masonry + timber lintels
  Row 3 (Upper-Floor Facades Set B - 2 stories x 4 bays, seamlessly tileable in X & Y):
    (0,3) Ensanche terracotta-salmon/cream dual-tone stucco + enclosed white miradores
    (1,3) Historic golden sandstone civic block + tall molded French windows
    (2,3) Contemporary dark-bronze glass & warm brick mixed residential/office
    (3,3) Corrugated galvanized industrial cladding + precast ribs + clerestory strips
"""

from __future__ import annotations

import math
from pathlib import Path
import numpy as np
from PIL import Image

from export_glb_models import GLBBuilder, GeoPart, verify_glb

ROOT = Path(__file__).resolve().parent.parent
PBR = ROOT / "game" / "assets" / "pbr"
ARCH_DIR = ROOT / "game" / "assets" / "models" / "architecture"

T = 512  # 512x512 per tile -> 4x4 = 2048x2048


def fbm2(n: int, base_p: int, octaves: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    acc = np.zeros((n, n), dtype=np.float32)
    amp, norm, p = 0.5, 0.0, base_p
    for _ in range(octaves):
        g = rng.random((p, p), dtype=np.float32)
        c = np.linspace(0.0, p, n, endpoint=False, dtype=np.float32)
        i0 = np.floor(c).astype(np.int32) % p
        i1 = (i0 + 1) % p
        f = c - np.floor(c)
        u = f * f * (3.0 - 2.0 * f)
        a = g[i0[:, None], i0[None, :]]
        b = g[i0[:, None], i1[None, :]]
        cc = g[i1[:, None], i0[None, :]]
        d = g[i1[:, None], i1[None, :]]
        v = (a * (1.0 - u[None, :]) + b * u[None, :]) * (1.0 - u[:, None]) + (
            cc * (1.0 - u[None, :]) + d * u[None, :]
        ) * u[:, None]
        acc += amp * v
        norm += amp
        amp *= 0.5
        p = min(n, p * 2)
    return acc / norm


def height_to_normal(h: np.ndarray, strength: float) -> np.ndarray:
    gx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * (0.5 * strength)
    gy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * (0.5 * strength)
    nz = np.ones_like(h, dtype=np.float32)
    inv = 1.0 / np.sqrt(gx * gx + gy * gy + nz * nz)
    nrm = np.empty((h.shape[0], h.shape[1], 3), dtype=np.float32)
    nrm[..., 0] = (-gx * inv) * 0.5 + 0.5
    nrm[..., 1] = (-gy * inv) * 0.5 + 0.5
    nrm[..., 2] = (nz * inv) * 0.5 + 0.5
    return np.clip(nrm, 0.0, 1.0)


def _tile_clay_roof(seed: int, warm_shift: tuple[float, float, float], bleach: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    cols, rows = 24, 16
    ci = np.floor(xx * cols).astype(np.int32)
    ri = np.floor(yy * rows).astype(np.int32)
    fx = (xx * cols) - ci
    fy = (yy * rows) - ri

    rng = np.random.default_rng(seed)
    tv = rng.uniform(-0.16, 0.16, size=(rows, cols)).astype(np.float32)[ri, ci]
    th = rng.uniform(-0.10, 0.10, size=(rows, cols)).astype(np.float32)[ri, ci]
    aged = (rng.random((rows, cols)) < 0.22).astype(np.float32)[ri, ci]

    barrel = np.sin(fx * math.pi) ** 0.72
    overlap = np.clip((fy - 0.85) / 0.15, 0.0, 1.0)
    grout_x = np.clip(1.0 - np.abs(fx - 0.5) * 2.0, 0.0, 1.0)
    trough_ao = 0.54 + 0.46 * (grout_x ** 0.45)
    lip_sh = 1.0 - 0.34 * (1.0 - np.clip(fy / 0.14, 0.0, 1.0))

    weather = fbm2(T, 8, 4, seed + 1)
    grit = fbm2(T, 32, 3, seed + 2)
    h = barrel * 0.72 + (1.0 - overlap) * 0.22 + grit * 0.06

    r = np.clip(warm_shift[0] + tv * 0.72 + th * 0.48 + (weather - 0.5) * 0.18 - aged * 0.06, 0.24, 0.88)
    g = np.clip(warm_shift[1] + tv * 0.55 + th * 0.12 + (weather - 0.5) * 0.15 + aged * 0.04, 0.16, 0.72)
    b = np.clip(warm_shift[2] + tv * 0.38 - th * 0.10 + (weather - 0.5) * 0.12 + aged * 0.07, 0.11, 0.58)
    luma = 0.299 * r + 0.587 * g + 0.114 * b
    r = r * (1.0 - bleach) + luma * bleach
    g = g * (1.0 - bleach) + luma * bleach
    b = b * (1.0 - bleach) + luma * bleach

    ao = np.clip(trough_ao * lip_sh * (0.86 + 0.20 * weather), 0.38, 1.0)
    alb = np.stack([r, g, b], axis=-1) * (0.82 + 0.24 * grit[..., None]) * ao[..., None]
    rough = np.clip(0.76 - 0.10 * barrel + 0.12 * (1.0 - weather), 0.54, 0.94)
    orm = np.stack([ao, rough, np.zeros((T, T), np.float32)], axis=-1)
    return np.clip(alb, 0, 1), height_to_normal(h, 11.0), orm


def _tile_flat_terrace_roof() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Spanish flat apartment roof: warm gravel & terracotta pavers, skylights, rooftop vents & AC pads."""
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    grav = fbm2(T, 24, 4, 150)
    stain = fbm2(T, 6, 4, 151)

    # Terracotta / warm sandstone rooftop paver grid (8x8)
    gx = (xx * 8.0) % 1.0
    gy = (yy * 8.0) % 1.0
    seam = ((gx < 0.03) | (gy < 0.03)).astype(np.float32)
    alb = np.empty((T, T, 3), dtype=np.float32)
    alb[..., 0] = 0.56 + (grav - 0.5) * 0.14 + (stain - 0.5) * 0.12 - seam * 0.14
    alb[..., 1] = 0.49 + (grav - 0.5) * 0.12 + (stain - 0.5) * 0.10 - seam * 0.12
    alb[..., 2] = 0.43 + (grav - 0.5) * 0.10 + (stain - 0.5) * 0.09 - seam * 0.10
    h = 0.35 + grav * 0.14 - seam * 0.18
    ao = np.clip(0.92 - seam * 0.25 - (1.0 - stain) * 0.16, 0.45, 1.0)
    rough = np.clip(0.84 + (grav - 0.5) * 0.12, 0.65, 0.96)
    metal = np.zeros((T, T), dtype=np.float32)

    # Skylight / courtyard lightwell frame + louvered HVAC units + solar panel array (looks awesome from air!)
    def add_rect(x0, y0, x1, y1, col, h_val, r_val, m_val=0.0, ao_val=0.92):
        mask = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1)
        sh = (xx >= x0 + 0.015) & (xx <= x1 + 0.025) & (yy >= y0 + 0.015) & (yy <= y1 + 0.025) & (~mask)
        ao[sh] *= 0.55
        alb[mask] = col
        h[mask] = h_val
        rough[mask] = r_val
        metal[mask] = m_val
        ao[mask] = ao_val

    # Dark recessed lightwell / skylight frame
    add_rect(0.10, 0.12, 0.36, 0.34, np.array([0.48, 0.46, 0.44]), 0.65, 0.72, 0.15)
    add_rect(0.13, 0.15, 0.33, 0.31, np.array([0.11, 0.15, 0.19]), 0.22, 0.16, 0.25, 0.65)
    # Pair of HVAC / chiller housings with louver grilles
    add_rect(0.58, 0.16, 0.76, 0.30, np.array([0.62, 0.64, 0.66]), 0.78, 0.42, 0.48)
    add_rect(0.62, 0.19, 0.72, 0.27, np.array([0.22, 0.24, 0.26]), 0.62, 0.65, 0.30)
    add_rect(0.79, 0.16, 0.90, 0.28, np.array([0.58, 0.60, 0.62]), 0.75, 0.45, 0.45)
    # Contemporary Spanish rooftop photovoltaic panels (subtle dark blue-slate angled array)
    for px in (0.14, 0.30, 0.46):
        add_rect(px, 0.58, px + 0.13, 0.78, np.array([0.14, 0.18, 0.26]), 0.68, 0.18, 0.35)
    # Roof access hatch / concrete vent curb
    add_rect(0.68, 0.58, 0.88, 0.82, np.array([0.52, 0.48, 0.44]), 0.80, 0.78, 0.05)
    add_rect(0.71, 0.61, 0.85, 0.79, np.array([0.36, 0.34, 0.32]), 0.72, 0.82, 0.05)

    alb *= ao[..., None]
    orm = np.stack([ao, rough, metal], axis=-1)
    return np.clip(alb, 0, 1), height_to_normal(h, 9.0), orm


def _tile_metal_slate_roof() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    ribs = 32
    fx = (xx * ribs) % 1.0
    seam = np.exp(-((fx - 0.12) / 0.075) ** 2) + 0.25 * np.sin(fx * 2.0 * math.pi)
    pr = np.floor(yy * 4.0).astype(np.int32)
    pfy = (yy * 4.0) - pr
    pseam = np.clip(1.0 - np.abs(pfy - 0.02) / 0.02, 0.0, 1.0)
    w = fbm2(T, 8, 4, 201)
    s = fbm2(T, 24, 3, 202)
    rng = np.random.default_rng(203)
    pv = rng.uniform(-0.05, 0.05, size=(4, ribs)).astype(np.float32)[pr, np.floor(xx * ribs).astype(np.int32) % ribs]
    tone = np.clip(0.50 + pv + (w - 0.5) * 0.14 + (s - 0.5) * 0.06, 0.26, 0.72)
    ao = np.clip(0.84 + 0.16 * (1.0 - pseam * 0.6) - 0.15 * np.exp(-((fx - 0.22) / 0.08) ** 2), 0.45, 1.0)
    alb = np.stack([tone * 0.96, tone * 0.99, tone * 1.04], axis=-1) * ao[..., None]
    h = np.clip(seam * 0.65 - pseam * 0.28 + s * 0.08, 0, 1)
    rough = np.clip(0.32 + (1.0 - w) * 0.26 + pseam * 0.18, 0.20, 0.70)
    metal = np.clip(0.36 + 0.28 * (1.0 - pseam), 0.12, 0.65)
    return np.clip(alb, 0, 1), height_to_normal(h, 9.0), np.stack([ao, rough, metal], axis=-1)


def _tile_ground_shopfront(style: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Row 1: 4 distinct Spanish ground-floor plinths/shopfronts (0=Zaragoza arcade, 1=Historic stone portal, 2=Modern granite/glass, 3=Industrial dock)."""
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    noise = fbm2(T, 16, 4, 400 + style * 7)
    fine = fbm2(T, 48, 3, 401 + style * 7)

    # Base plinth masonry / concrete / granite
    pal = [
        (0.64, 0.58, 0.49),  # 0: warm sandstone arcade
        (0.54, 0.49, 0.42),  # 1: historic dressed stone
        (0.38, 0.39, 0.41),  # 2: polished urban granite/concrete
        (0.52, 0.53, 0.54),  # 3: industrial precast concrete
    ][style]
    alb = np.empty((T, T, 3), dtype=np.float32)
    for c in range(3):
        alb[..., c] = pal[c] + (noise - 0.5) * 0.09 + (fine - 0.5) * 0.05
    h = 0.40 + noise * 0.12 + fine * 0.06
    ao = np.clip(0.74 + 0.26 * np.clip(1.0 - (yy - 0.72) / 0.28, 0.0, 1.0), 0.52, 1.0)
    rough = np.full((T, T), 0.78 if style != 2 else 0.48, dtype=np.float32)
    glass = np.zeros((T, T), dtype=np.float32)

    # Ashlar stone block courses on styles 0 & 1
    if style in (0, 1):
        bh = (yy * 10.0) % 1.0
        row_i = np.floor(yy * 10.0)
        bw = ((xx * 6.0) + (row_i % 2) * 0.5) % 1.0
        mortar = (bh < 0.06) | (bw < 0.03)
        alb[mortar] *= 0.72
        h[mortar] -= 0.14
        ao[mortar] *= 0.80

    # Top stringcourse molding separating ground floor from upper floors
    cornice = yy < 0.08
    alb[cornice] *= 1.08
    h[cornice] += 0.22
    under_c = (yy >= 0.08) & (yy < 0.13)
    ao[under_c] *= 0.68

    # Ground sidewalk splash/grime strip (bottom 8%)
    kick = yy > 0.92
    alb[kick] *= 0.64
    ao[kick] *= 0.70

    cols = 4 if style != 3 else 3
    awning_cols = [(0.18, 0.24, 0.20), (0.36, 0.20, 0.18), (0.20, 0.24, 0.34), (0.44, 0.36, 0.24)]
    for c in range(cols):
        cx = (c + 0.5) / cols
        w_half = (0.38 if style == 2 else (0.34 if style == 0 else 0.28)) / cols
        x0, x1 = cx - w_half, cx + w_half
        y0, y1 = (0.18 if style != 1 else 0.24), (0.92 if (c == 1 or style == 3) else 0.84)

        frame = (xx >= x0 - 0.015) & (xx <= x1 + 0.015) & (yy >= y0 - 0.02) & (yy <= y1)
        hole = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1)
        alb[frame] *= 0.72
        h[frame] += 0.08

        if style == 3:
            # Corrugated steel industrial roll-up loading shutter!
            slat = 0.82 + 0.18 * np.sin(yy[hole] * 140.0)
            alb[hole] = np.stack([0.36 * slat, 0.38 * slat, 0.40 * slat], axis=-1)
            h[hole] = 0.20 + 0.08 * slat
            rough[hole] = 0.42
            ao[hole] *= 0.82
        elif style == 1 and c == 1:
            # Heavy historic timber carriage portal
            alb[hole] = np.array([0.22, 0.15, 0.10])
            h[hole] = 0.18
            rough[hole] = 0.84
            ao[hole] *= 0.74
        else:
            # Commercial glass shopfront / boutique window (triggers 3D interior parallax!)
            alb[hole] = np.array([0.09, 0.12, 0.15])
            h[hole] = 0.12
            rough[hole] = 0.08
            glass[hole] = 1.0
            # Vertical mullion bar
            mul = hole & (np.abs(xx - cx) < 0.006)
            alb[mul] = np.array([0.24, 0.24, 0.26])
            glass[mul] = 0.0
            rough[mul] = 0.45
            if style == 0 and c in (0, 2):
                # Subtle muted European shop awning (NOT bright red stripes!)
                awn = (xx >= x0 - 0.01) & (xx <= x1 + 0.01) & (yy >= y0 - 0.04) & (yy <= y0 + 0.08)
                st = 0.90 + 0.10 * np.sin(xx[awn] * 120.0)
                ac = awning_cols[c % len(awning_cols)]
                alb[awn] = np.stack([ac[0] * st, ac[1] * st, ac[2] * st], axis=-1)
                glass[awn] = 0.0
                rough[awn] = 0.86
                h[awn] = 0.62

    alb *= ao[..., None]
    orm = np.stack([ao, rough, glass], axis=-1)
    return np.clip(alb, 0, 1), height_to_normal(h, 8.0), orm


def _tile_upper_facade(style: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Rows 2 & 3: 8 completely distinct seamlessly-tileable Spanish upper-floor facades (2 stories x 4 bays)!
    style:
      0 = Zaragoza red Ladrillo Caravista exposed brick + white balcony slabs + persianas
      1 = Aragonese warm ochre plaster + wrought-iron Juliet balconies + varied wooden shutters
      2 = Modern Spanish limestone/precast concrete + wide glass loggias & spandrel bands
      3 = Pyrenean dressed golden-grey ashlar stone masonry + timber lintels
      4 = Ensanche dual-tone terracotta-salmon/cream stucco + white enclosed miradores
      5 = Historic golden sandstone civic block + tall narrow molded French windows
      6 = Contemporary dark-bronze glass & warm brick residential/office
      7 = Corrugated galvanized industrial metal cladding + precast ribs + clerestory strips
    """
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) / T
    rng = np.random.default_rng(600 + style * 31)
    n1 = fbm2(T, 12, 4, 610 + style * 7)
    n2 = fbm2(T, 40, 3, 611 + style * 7)

    base_pals = [
        (0.58, 0.30, 0.21),  # 0: Aragon Ladrillo Caravista brick!
        (0.78, 0.71, 0.59),  # 1: Warm Aragonese ochre/cream stucco
        (0.75, 0.74, 0.71),  # 2: Modern warm concrete/limestone
        (0.58, 0.54, 0.46),  # 3: Pyrenean ashlar stone
        (0.76, 0.54, 0.43),  # 4: Ensanche terracotta-salmon plaster
        (0.74, 0.66, 0.52),  # 5: Historic golden sandstone
        (0.52, 0.36, 0.28),  # 6: Contemporary brick/bronze
        (0.62, 0.64, 0.66),  # 7: Industrial corrugated steel/precast
    ]
    pr, pg, pb = base_pals[style]
    alb = np.empty((T, T, 3), dtype=np.float32)
    alb[..., 0] = pr + (n1 - 0.5) * 0.09 + (n2 - 0.5) * 0.05
    alb[..., 1] = pg + (n1 - 0.5) * 0.08 + (n2 - 0.5) * 0.04
    alb[..., 2] = pb + (n1 - 0.5) * 0.07 + (n2 - 0.5) * 0.04

    h = 0.42 + n1 * 0.10 + n2 * 0.06
    ao = np.ones((T, T), dtype=np.float32)
    rough = np.full((T, T), 0.84, dtype=np.float32)
    glass = np.zeros((T, T), dtype=np.float32)

    # Surface masonry relief specific to each architectural typology!
    if style in (0, 6):
        # Authentic Spanish exposed brick courses (Ladrillo Caravista!)
        row_f = yy * 44.0
        ri = np.floor(row_f).astype(np.int32)
        col_f = xx * 18.0 + (ri % 2) * 0.5
        ci = np.floor(col_f).astype(np.int32) % 18
        bvar = rng.uniform(-0.07, 0.07, size=(44, 18)).astype(np.float32)[ri % 44, ci]
        mortar = ((row_f % 1.0) < 0.14) | ((col_f % 1.0) < 0.06)
        alb[..., 0] += bvar
        alb[..., 1] += bvar * 0.55
        alb[..., 2] += bvar * 0.35
        alb[mortar] = np.array([0.52, 0.49, 0.45])
        h[mortar] -= 0.12
        ao[mortar] *= 0.84
    elif style == 3:
        # Pyrenean ashlar dressed stone blocks
        row_f = yy * 16.0
        ri = np.floor(row_f).astype(np.int32)
        col_f = xx * 8.0 + (ri % 2) * 0.5
        ci = np.floor(col_f).astype(np.int32) % 8
        svar = rng.uniform(-0.10, 0.10, size=(16, 8)).astype(np.float32)[ri % 16, ci]
        mortar = ((row_f % 1.0) < 0.08) | ((col_f % 1.0) < 0.04)
        alb += svar[..., None]
        alb[mortar] *= 0.64
        h[mortar] -= 0.20
        ao[mortar] *= 0.76
    elif style == 7:
        # Industrial corrugated vertical metal ribs + horizontal precast seams
        rib = 0.5 + 0.5 * np.sin(xx * 2.0 * math.pi * 40.0)
        alb *= (0.91 + 0.14 * rib[..., None])
        h += 0.16 * rib
        rough[:] = 0.48
        # Vertical steel structural pilasters
        pil = ((xx * 4.0) % 1.0) < 0.06
        alb[pil] = np.array([0.46, 0.48, 0.50])
        h[pil] += 0.12
        # High clerestory translucent industrial window band (2 rows)
        for fl in range(2):
            fy0 = fl * 0.5
            strip = (yy >= fy0 + 0.08) & (yy <= fy0 + 0.20) & (~pil)
            alb[strip] = np.array([0.16, 0.22, 0.27])
            glass[strip] = 0.85
            rough[strip] = 0.14
            h[strip] = 0.22
        alb *= ao[..., None]
        return np.clip(alb, 0, 1), height_to_normal(h, 8.5), np.stack([ao, rough, glass], axis=-1)

    # 2 stories per tile (fl=0 top story, fl=1 bottom story) so vertical tiling = exact 3.1m/story!
    cols = 4 if style != 3 else 3
    for fl in range(2):
        fy0 = fl * 0.50
        # Horizontal floor slab / Spanish concrete cornice / stringcourse!
        if style in (0, 2):
            # Projecting cream/white concrete balcony floor slab across building!
            slab = (yy >= fy0 + 0.41) & (yy <= fy0 + 0.48)
            alb[slab] = np.array([0.82, 0.80, 0.76]) if style == 0 else np.array([0.78, 0.77, 0.75])
            h[slab] = 0.82
            ao[(yy > fy0 + 0.48) & (yy <= fy0 + 0.50)] *= 0.62
        elif style in (1, 4, 5):
            # Molded plaster/stone belt course
            belt = (yy >= fy0 + 0.46) & (yy <= fy0 + 0.50)
            alb[belt] *= 1.06
            h[belt] += 0.14
            ao[(yy >= fy0 + 0.0) & (yy < fy0 + 0.03)] *= 0.78

        for c in range(cols):
            cx = (c + 0.5) / cols
            if style == 2:
                # Wide modern Spanish recessed balcony/loggia bay!
                w_half = 0.40 / cols
                y0, y1 = fy0 + 0.08, fy0 + 0.41
            elif style == 6:
                # Contemporary wide dark-bronze ribbon windows
                w_half = 0.42 / cols
                y0, y1 = fy0 + 0.09, fy0 + 0.37
            elif style == 4 and c in (1, 2):
                # Classic Zaragoza Ensanche enclosed white bay window (mirador)!
                w_half = 0.42 / cols
                y0, y1 = fy0 + 0.06, fy0 + 0.43
                mir = (xx >= cx - w_half - 0.015) & (xx <= cx + w_half + 0.015) & (yy >= y0 - 0.02) & (yy <= y1 + 0.02)
                alb[mir] = np.array([0.86, 0.84, 0.80])
                h[mir] = 0.78
            else:
                w_half = (0.28 if style == 5 else 0.31) / cols
                y0, y1 = fy0 + 0.09, fy0 + (0.40 if style in (0, 1) else 0.36)

            x0, x1 = cx - w_half, cx + w_half
            # Recessed reveal + shadow lintel
            rev = (xx >= x0 - 0.012) & (xx <= x1 + 0.012) & (yy >= y0 - 0.016) & (yy <= y1 + 0.014)
            ao[rev] *= 0.72
            if style in (1, 5):
                # Cream/stone window surround & sill
                sur = rev & ~((xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1))
                alb[sur] = np.array([0.82, 0.78, 0.71])
                h[sur] = 0.62
            elif style == 3:
                # Timber lintel beam above Pyrenean stone window
                lint = (xx >= x0 - 0.018) & (xx <= x1 + 0.018) & (yy >= y0 - 0.028) & (yy <= y0)
                alb[lint] = np.array([0.28, 0.21, 0.15])
                h[lint] = 0.66

            # Window opening + glass pane (for 3D Interior Parallax!)
            win = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1)
            alb[win] = np.array([0.09, 0.12, 0.16])
            h[win] = 0.14
            rough[win] = 0.08
            glass[win] = 1.0

            # Rolled Spanish exterior persiana (blind) at varied realistic drop heights per window!
            drop_frac = float(rng.choice([0.08, 0.14, 0.24, 0.42, 0.75]))
            pers = win & (yy <= y0 + (y1 - y0) * drop_frac)
            slat = 0.88 + 0.12 * np.sin(yy[pers] * 260.0)
            p_col = (0.72, 0.69, 0.64) if style in (0, 2, 4) else (0.38, 0.34, 0.29)
            alb[pers] = np.stack([p_col[0] * slat, p_col[1] * slat, p_col[2] * slat], axis=-1)
            glass[pers] = 0.0
            rough[pers] = 0.72
            h[pers] = 0.34

            # Window mullions
            mul = win & ((np.abs(xx - cx) < 0.004) | (np.abs(yy - (y0 + (y1 - y0) * 0.46)) < 0.004))
            mcol = np.array([0.84, 0.83, 0.80]) if style in (0, 4) else np.array([0.22, 0.22, 0.24])
            alb[mul] = mcol
            glass[mul] = 0.0
            rough[mul] = 0.48
            h[mul] = 0.38

            # Style-specific shutters or balcony railings!
            if style == 1 and (c + fl) % 2 == 0:
                # Subtle varied warm-brown/grey-sage shutters (only on some bays, NOT stamped on every building!)
                sw = 0.022
                sh_l = (xx >= x0 - sw - 0.004) & (xx <= x0 - 0.004) & (yy >= y0) & (yy <= y1)
                sh_r = (xx >= x1 + 0.004) & (xx <= x1 + sw + 0.004) & (yy >= y0) & (yy <= y1)
                sc = np.array([0.26, 0.24, 0.20]) if fl == 0 else np.array([0.24, 0.28, 0.24])
                alb[sh_l | sh_r] = sc
                h[sh_l | sh_r] = 0.58
            if style in (0, 1, 2, 5):
                # Balcony railing at lower third of window
                by0, by1 = y1 - 0.11, y1
                rail_band = (xx >= x0 - 0.01) & (xx <= x1 + 0.01) & (yy >= by0) & (yy <= by1)
                if style == 2:
                    # Modern frosted glass balustrade
                    alb[rail_band] = alb[rail_band] * 0.45 + np.array([0.34, 0.38, 0.40])
                    glass[rail_band] = 0.0
                    rough[rail_band] = 0.22
                else:
                    # Fine wrought-iron vertical bars + top handrail
                    bars = rail_band & ((np.abs(yy - by0) < 0.006) | (((xx * 96.0) % 1.0) < 0.22))
                    alb[bars] = np.array([0.14, 0.15, 0.16])
                    glass[bars] = 0.0
                    rough[bars] = 0.42
                    h[bars] = 0.68

    alb *= ao[..., None]
    orm = np.stack([ao, rough, glass], axis=-1)
    return np.clip(alb, 0, 1), height_to_normal(h, 8.5), orm


def bake_16tile_atlas() -> tuple[Image.Image, Image.Image]:
    """Bake 4x4 (16-tile) 2048x2048 Spanish Architectural PBR Atlas."""
    S = T * 4
    alb = np.zeros((S, S, 3), dtype=np.float32)
    nrm = np.zeros((S, S, 3), dtype=np.float32)
    orm = np.zeros((S, S, 3), dtype=np.float32)

    def put(col: int, row: int, res: tuple[np.ndarray, np.ndarray, np.ndarray]) -> None:
        a, n, o = res
        y0, x0 = row * T, col * T
        alb[y0 : y0 + T, x0 : x0 + T] = a
        nrm[y0 : y0 + T, x0 : x0 + T] = n
        orm[y0 : y0 + T, x0 : x0 + T] = o

    # Row 0: 4 Roof typologies
    put(0, 0, _tile_clay_roof(101, (0.66, 0.36, 0.23), bleach=0.14))  # Warm sun-baked Teja Arabe
    put(1, 0, _tile_clay_roof(121, (0.64, 0.46, 0.34), bleach=0.34))  # Weathered historic pale ochre clay
    put(2, 0, _tile_flat_terrace_roof())                              # Spanish flat apartment terrace roof!
    put(3, 0, _tile_metal_slate_roof())                               # Industrial ribbed steel / Pyrenean slate

    # Row 1: 4 Ground-Floor Plinths / Commercial Shopfronts
    for c in range(4):
        put(c, 1, _tile_ground_shopfront(c))

    # Rows 2 & 3: 8 Completely Distinct Upper-Floor Spanish Architectural Styles!
    for c in range(4):
        put(c, 2, _tile_upper_facade(c))
        put(c, 3, _tile_upper_facade(c + 4))

    alb_img = Image.fromarray((np.clip(alb, 0, 1) * 255).astype(np.uint8), "RGB")
    nrm_img = Image.fromarray((np.clip(nrm, 0, 1) * 255).astype(np.uint8), "RGB")
    orm_img = Image.fromarray((np.clip(orm, 0, 1) * 255).astype(np.uint8), "RGB")

    alb_img.save(PBR / "building_atlas_albedo.jpg", quality=93, subsampling=0)
    nrm_img.save(PBR / "building_atlas_normal.jpg", quality=92, subsampling=0)
    orm_img.save(PBR / "building_atlas_orm.png", optimize=True)
    print(f"[OK] Baked 16-Tile (4x4) 2048x2048 Spanish Architectural PBR Atlas -> {PBR}/building_atlas_*")
    return alb_img, nrm_img


# =============================================================================
# 6 TRULY DISTINCT 3D SPANISH ARCHITECTURAL .GLB ARCHETYPES
# UV Convention for building.frag:
#   - v < 0.50 => ROOF surface (u < 0.50: pitched/hipped clay/slate, u >= 0.50: flat terrace / metal)
#   - v >= 0.50 => WALL surface (building.frag uses world-meter 1:1 metric floor & bay tiling!)
# =============================================================================
WALL_UV = (0.05, 0.55, 0.45, 0.95)
ROOF_PITCHED_UV = (0.05, 0.05, 0.45, 0.45)
ROOF_FLAT_UV = (0.55, 0.05, 0.95, 0.45)


def _add_walls_box(geo: GeoPart, cx: float, cz: float, sx: float, sz: float, y0: float, y1: float, uv=WALL_UV) -> None:
    hx, hz = sx * 0.5, sz * 0.5
    u0, v0, u1, v1 = uv
    geo.add_quad((cx - hx, y0, cz + hz), (cx + hx, y0, cz + hz), (cx + hx, y1, cz + hz), (cx - hx, y1, cz + hz), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (0, 0, 1))
    geo.add_quad((cx + hx, y0, cz - hz), (cx - hx, y0, cz - hz), (cx - hx, y1, cz - hz), (cx + hx, y1, cz - hz), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (0, 0, -1))
    geo.add_quad((cx + hx, y0, cz + hz), (cx + hx, y0, cz - hz), (cx + hx, y1, cz - hz), (cx + hx, y1, cz + hz), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (1, 0, 0))
    geo.add_quad((cx - hx, y0, cz - hz), (cx - hx, y0, cz + hz), (cx - hx, y1, cz + hz), (cx - hx, y1, cz - hz), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (-1, 0, 0))


def _add_hipped_roof(geo: GeoPart, cx: float, cz: float, sx: float, sz: float, eave_y: float, ridge_y: float, ov: float = 0.04, ridge_inset: float = 0.24, uv=ROOF_PITCHED_UV) -> None:
    """Authentic Spanish 4-slope hipped roof (cubierta a cuatro aguas) — NOT a steep triangular gable barn!"""
    hx, hz = sx * 0.5 + ov, sz * 0.5 + ov
    rx = max(0.04, sx * 0.5 - sx * ridge_inset)
    u0, v0, u1, v1 = uv
    um = 0.5 * (u0 + u1)
    # Front (+Z) & Back (-Z) trapezoid slopes
    geo.add_quad((cx - hx, eave_y, cz + hz), (cx + hx, eave_y, cz + hz), (cx + rx, ridge_y, cz), (cx - rx, ridge_y, cz), (u0, v1), (u1, v1), (u1 - 0.06, v0), (u0 + 0.06, v0), (0, 0.85, 0.52))
    geo.add_quad((cx + hx, eave_y, cz - hz), (cx - hx, eave_y, cz - hz), (cx - rx, ridge_y, cz), (cx + rx, ridge_y, cz), (u0, v1), (u1, v1), (u1 - 0.06, v0), (u0 + 0.06, v0), (0, 0.85, -0.52))
    # Right (+X) & Left (-X) triangular hip slopes
    geo.add_tri((cx + hx, eave_y, cz + hz), (cx + hx, eave_y, cz - hz), (cx + rx, ridge_y, cz), (u0, v1), (u1, v1), (um, v0), (0.58, 0.81, 0))
    geo.add_tri((cx - hx, eave_y, cz - hz), (cx - hx, eave_y, cz + hz), (cx - rx, ridge_y, cz), (u0, v1), (u1, v1), (um, v0), (-0.58, 0.81, 0))


def _save_arch_glb(out_path: Path, geo: GeoPart) -> None:
    gb = GLBBuilder()
    # Lightweight placeholder material (C++ WorldObjects binds our shared 2K PBR atlas directly!)
    m_all = gb.add_material("SpanishBuildingAtlas", (1, 1, 1, 1), metallic=0.0, roughness=0.85)
    m_idx = gb.add_mesh(out_path.stem, [geo.to_prim(m_all)])
    gb.add_node(out_path.stem, m_idx)
    gb.save(out_path)
    _, _, _, nt = verify_glb(out_path)
    print(f"  [ARCH GLB] {out_path.name:28s} | tris={nt:3d}")


def build_all_6_archetypes() -> None:
    ARCH_DIR.mkdir(parents=True, exist_ok=True)
    skirt = -0.35

    # 0. aragon_house_stone.glb -- Pyrenean/Aragonese 4-slope hipped stone townhouse + chimney!
    g0 = GeoPart()
    _add_walls_box(g0, 0.0, 0.0, 1.0, 1.0, skirt, 0.78)
    _add_hipped_roof(g0, 0.0, 0.0, 1.0, 1.0, 0.77, 1.00, ov=0.055, ridge_inset=0.22)
    g0.add_box_uv_rect((0.22, 0.96, 0.16), (0.11, 0.14, 0.11), WALL_UV, include_bottom=False)
    _save_arch_glb(ARCH_DIR / "aragon_house_stone.glb", g0)

    # 1. aragon_house_terra.glb -- Historic Aragonese low-pitch 4-slope hipped town block + cornice + skylight + chimney!
    g1 = GeoPart()
    _add_walls_box(g1, 0.0, 0.0, 1.0, 1.0, skirt, 0.84)
    # Molded projecting cornice collar
    g1.add_box_uv_rect((0.0, 0.835, 0.0), (1.035, 0.03, 1.035), WALL_UV, include_bottom=False, include_top=False)
    _add_hipped_roof(g1, 0.0, 0.0, 1.0, 1.0, 0.84, 1.00, ov=0.045, ridge_inset=0.26)
    g1.add_box_uv_rect((-0.20, 0.96, -0.14), (0.10, 0.12, 0.10), WALL_UV, include_bottom=False)
    g1.add_box_uv_rect((0.18, 0.94, 0.16), (0.16, 0.06, 0.14), ROOF_FLAT_UV, include_bottom=False)
    _save_arch_glb(ARCH_DIR / "aragon_house_terra.glb", g1)

    # 2. aragon_apartment.glb -- Authentic Spanish Mid-Rise Flat-Roof Apartment Block (Parapet + Elevator Penthouse + HVAC!)
    g2 = GeoPart()
    _add_walls_box(g2, 0.0, 0.0, 1.0, 1.0, skirt, 0.94)
    # Projecting upper cornice trim
    g2.add_box_uv_rect((0.0, 0.93, 0.0), (1.028, 0.025, 1.028), WALL_UV, include_bottom=False, include_top=False)
    # Recessed flat terrace rooftop deck (sits below parapet wall!)
    u0, v0, u1, v1 = ROOF_FLAT_UV
    g2.add_quad((-0.46, 0.905, 0.46), (0.46, 0.905, 0.46), (0.46, 0.905, -0.46), (-0.46, 0.905, -0.46), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (0, 1, 0))
    # 4-sided 3D perimeter parapet coping walls around roof terrace!
    pw = 0.055
    g2.add_box_uv_rect((0.0, 0.935,  0.5 - pw * 0.5), (1.0, 0.065, pw), WALL_UV, include_bottom=False)
    g2.add_box_uv_rect((0.0, 0.935, -0.5 + pw * 0.5), (1.0, 0.065, pw), WALL_UV, include_bottom=False)
    g2.add_box_uv_rect(( 0.5 - pw * 0.5, 0.935, 0.0), (pw, 0.065, 1.0 - 2 * pw), WALL_UV, include_bottom=False)
    g2.add_box_uv_rect((-0.5 + pw * 0.5, 0.935, 0.0), (pw, 0.065, 1.0 - 2 * pw), WALL_UV, include_bottom=False)
    # Asymmetric rooftop elevator/stairwell penthouse (caseton) + flat roof cap!
    g2.add_box_uv_rect((0.16, 0.965, -0.10), (0.28, 0.12, 0.26), WALL_UV, include_bottom=False, include_top=False)
    g2.add_quad((0.02, 1.026, 0.03), (0.30, 1.026, 0.03), (0.30, 1.026, -0.23), (0.02, 1.026, -0.23), (u0, v1), (u1, v1), (u1, v0), (u0, v0), (0, 1, 0))
    # Rooftop HVAC / mechanical unit & ventilation stack
    g2.add_box_uv_rect((-0.20, 0.935, 0.15), (0.16, 0.06, 0.14), ROOF_FLAT_UV, include_bottom=False)
    g2.add_box_uv_rect((-0.24, 0.955, -0.20), (0.08, 0.10, 0.08), WALL_UV, include_bottom=False)
    _save_arch_glb(ARCH_DIR / "aragon_apartment.glb", g2)

    # 3. aragon_church_tower.glb -- Aragonese Mudejar/Romanesque Church (Hipped Nave + Stepped Bell Tower + Spire!)
    g3 = GeoPart()
    _add_walls_box(g3, -0.10, 0.0, 0.78, 0.58, skirt, 0.54)
    _add_hipped_roof(g3, -0.10, 0.0, 0.78, 0.58, 0.53, 0.67, ov=0.035, ridge_inset=0.18)
    _add_walls_box(g3, 0.30, 0.0, 0.30, 0.30, skirt, 0.88)
    g3.add_box_uv_rect((0.30, 0.88, 0.0), (0.33, 0.03, 0.33), WALL_UV, include_bottom=False)
    # Octagonal/pyramidal bell tower spire
    tx, ty0, ty1, th = 0.30, 0.895, 1.06, 0.165
    ru0, rv0, ru1, rv1 = ROOF_PITCHED_UV
    rm = 0.5 * (ru0 + ru1)
    apex = (tx, ty1, 0.0)
    g3.add_tri((tx - th, ty0, th), (tx + th, ty0, th), apex, (ru0, rv1), (ru1, rv1), (rm, rv0), (0, 0.55, 1))
    g3.add_tri((tx + th, ty0, th), (tx + th, ty0, -th), apex, (ru0, rv1), (ru1, rv1), (rm, rv0), (1, 0.55, 0))
    g3.add_tri((tx + th, ty0, -th), (tx - th, ty0, -th), apex, (ru0, rv1), (ru1, rv1), (rm, rv0), (0, 0.55, -1))
    g3.add_tri((tx - th, ty0, -th), (tx - th, ty0, th), apex, (ru0, rv1), (ru1, rv1), (rm, rv0), (-1, 0.55, 0))
    _save_arch_glb(ARCH_DIR / "aragon_church_tower.glb", g3)

    # 4. industrial_hangar.glb -- Spanish Poligono Warehouse (Low-pitch ribbed steel + Raised Ridge Skylight Monitor + Loading Canopy!)
    g4 = GeoPart()
    _add_walls_box(g4, 0.0, 0.0, 1.0, 1.0, skirt, 0.88)
    # Low-pitch industrial metal roof
    ru0, rv0, ru1, rv1 = ROOF_FLAT_UV
    g4.add_quad((-0.52, 0.88, 0.52), (0.52, 0.88, 0.52), (0.52, 0.96, 0.0), (-0.52, 0.96, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 0.98, 0.16))
    g4.add_quad((0.52, 0.88, -0.52), (-0.52, 0.88, -0.52), (-0.52, 0.96, 0.0), (0.52, 0.96, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 0.98, -0.16))
    # End gable triangles
    w0, wv0, w1, wv1 = WALL_UV
    g4.add_tri((0.5, 0.88, 0.5), (0.5, 0.88, -0.5), (0.5, 0.96, 0.0), (w0, wv0 + 0.05), (w1, wv0 + 0.05), (0.5 * (w0 + w1), wv0), (1, 0, 0))
    g4.add_tri((-0.5, 0.88, -0.5), (-0.5, 0.88, 0.5), (-0.5, 0.96, 0.0), (w0, wv0 + 0.05), (w1, wv0 + 0.05), (0.5 * (w0 + w1), wv0), (-1, 0, 0))
    # 3D raised ridge skylight monitor (lucernario) + front loading dock awning canopy!
    g4.add_box_uv_rect((0.0, 0.985, 0.0), (0.72, 0.06, 0.16), ROOF_FLAT_UV, include_bottom=False)
    g4.add_box_uv_rect((0.0, 0.42, 0.54), (0.68, 0.04, 0.09), ROOF_FLAT_UV, include_bottom=False)
    _save_arch_glb(ARCH_DIR / "industrial_hangar.glb", g4)

    # 5. rural_barn.glb -- Rural Aragonese Masia / Borda Farmhouse (Main gable block + side lean-to wing + chimney!)
    g5 = GeoPart()
    _add_walls_box(g5, -0.08, 0.0, 0.80, 0.96, skirt, 0.72)
    ru0, rv0, ru1, rv1 = ROOF_PITCHED_UV
    g5.add_quad((-0.52, 0.71, 0.52), (0.36, 0.71, 0.52), (0.36, 1.00, 0.0), (-0.52, 1.00, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 0.85, 0.52))
    g5.add_quad((0.36, 0.71, -0.52), (-0.52, 0.71, -0.52), (-0.52, 1.00, 0.0), (0.36, 1.00, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 0.85, -0.52))
    g5.add_tri((0.32, 0.72, 0.48), (0.32, 0.72, -0.48), (0.32, 1.00, 0.0), (w0, wv0 + 0.1), (w1, wv0 + 0.1), (0.5 * (w0 + w1), wv0), (1, 0, 0))
    g5.add_tri((-0.48, 0.72, -0.48), (-0.48, 0.72, 0.48), (-0.48, 1.00, 0.0), (w0, wv0 + 0.1), (w1, wv0 + 0.1), (0.5 * (w0 + w1), wv0), (-1, 0, 0))
    # Attached lower stone shed wing (+X side) + lean-to roof
    _add_walls_box(g5, 0.40, 0.0, 0.22, 0.76, skirt, 0.48)
    g5.add_quad((0.31, 0.58, 0.40), (0.53, 0.46, 0.40), (0.53, 0.46, -0.40), (0.31, 0.58, -0.40), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0.45, 0.89, 0))
    g5.add_box_uv_rect((-0.18, 0.94, 0.14), (0.11, 0.14, 0.11), WALL_UV, include_bottom=False)
    _save_arch_glb(ARCH_DIR / "rural_barn.glb", g5)


def main() -> None:
    bake_16tile_atlas()
    build_all_6_archetypes()


if __name__ == "__main__":
    main()
