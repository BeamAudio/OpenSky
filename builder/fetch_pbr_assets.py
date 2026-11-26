#!/usr/bin/env python3
"""Download 100% CC0 (Public Domain) 1K photogrammetry PBR textures from Poly Haven
into game/assets/pbr/ and composite a photographic Northern Spain botanical tree atlas.

All assets from Poly Haven are CC0 1.0 Public Domain. See the asset source notes.
"""

from __future__ import annotations

import io
import math
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np
import requests
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "game" / "assets" / "pbr"
USER_AGENT = "NorthernSpainVFRSim/0.1 (CC0 asset fetcher)"

# Curated 1K photogrammetry CC0 sets tuned for Northern Spain (Aragón & Pyrenees)
ASSETS: dict[str, tuple[str, list[str]]] = {
    "grass":    ("leafy_grass",        ["Diffuse", "nor_gl", "arm"]),
    "farm":     ("withered_grass",     ["Diffuse", "nor_gl", "arm"]),
    "forest":   ("forrest_ground_01",  ["Diffuse", "nor_gl", "arm"]),
    "rock":     ("rock_face_03",       ["Diffuse", "nor_gl", "arm"]),
    "asphalt":  ("aerial_asphalt_01",  ["Diffuse", "nor_gl"]),
    "gravel":   ("gravel",             ["Diffuse", "nor_gl"]),
    "hangar":   ("corrugated_iron_02", ["Diffuse", "nor_gl"]),
    "terminal": ("grey_plaster",       ["Diffuse", "nor_gl"]),
    "leaf_src": ("leafy_grass",        ["Diffuse", "nor_gl"]),
    "bark_src": ("pine_bark",          ["Diffuse"]),
}

MAP_SUFFIX = {"Diffuse": "albedo", "nor_gl": "normal", "arm": "arm"}


def fetch_one(local_name: str, ph_id: str, map_key: str, force: bool = False) -> str:
    suffix = MAP_SUFFIX[map_key]
    out_path = OUT_DIR / f"{local_name}_{suffix}.jpg"
    if not force and out_path.exists() and out_path.stat().st_size > 5000:
        return f"skip {out_path.name}"

    meta = requests.get(
        f"https://api.polyhaven.com/files/{ph_id}",
        headers={"User-Agent": USER_AGENT},
        timeout=20,
    ).json()
    url = meta[map_key]["1k"]["jpg"]["url"]
    r = requests.get(url, headers={"User-Agent": USER_AGENT}, timeout=30)
    r.raise_for_status()

    # Keep full 1024x1024 1K photogrammetry for razor-sharp close-up detail with mipmaps + anisotropy.
    im = Image.open(io.BytesIO(r.content)).convert("RGB")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(".jpg.part")
    im.save(tmp, format="JPEG", quality=86, optimize=True)
    tmp.replace(out_path)
    return f"ok   {out_path.name} ({out_path.stat().st_size // 1024} KB)"


def build_photographic_tree_atlas() -> None:
    """Composite real CC0 photographed bark & leafy foliage into a 1024x256 4-slot botanical atlas
    plus matching tangent-space normal map:
      Slot 0 (0..256):    Pyrenean Scots Pine / Silver Fir
      Slot 1 (256..512):  Sunlit Pyrenean Beech / Valley Broadleaf
      Slot 2 (512..768):  Mediterranean Holm Oak (Encina)
      Slot 3 (768..1024): Close-up 3D Meadow Grass Tuft & Wild Scrub Bush
    """
    albedo_path = OUT_DIR / "trees_atlas_albedo.png"
    normal_path = OUT_DIR / "trees_atlas_normal.png"

    leaf_img = np.asarray(
        Image.open(OUT_DIR / "leaf_src_albedo.jpg").convert("RGB").resize((256, 256), Image.LANCZOS),
        dtype=np.float32,
    ) / 255.0
    leaf_nrm = np.asarray(
        Image.open(OUT_DIR / "leaf_src_normal.jpg").convert("RGB").resize((256, 256), Image.LANCZOS),
        dtype=np.float32,
    ) / 255.0
    bark_img = np.asarray(
        Image.open(OUT_DIR / "bark_src_albedo.jpg").convert("RGB").resize((256, 256), Image.LANCZOS),
        dtype=np.float32,
    ) / 255.0

    leaf_luma = np.clip(0.299 * leaf_img[:, :, 0] + 0.587 * leaf_img[:, :, 1] + 0.114 * leaf_img[:, :, 2], 0.05, 1.0)
    leaf_detail = np.clip(leaf_luma / (np.mean(leaf_luma) + 1e-5), 0.35, 1.65)
    leaf_hi = np.clip((leaf_luma - np.roll(leaf_luma, 3, axis=0) * 0.3 - np.roll(leaf_luma, 3, axis=1) * 0.3 + 0.4), 0.0, 1.0)

    W, H = 1024, 256
    rgba = np.zeros((H, W, 4), dtype=np.float32)
    nrm_out = np.full((H, W, 3), [0.5, 0.5, 1.0], dtype=np.float32)

    yy, xx = np.mgrid[0:H, 0:256]
    t = 1.0 - (yy + 0.5) / H       # 0=ground, 1=top
    nx = (xx + 0.5) / 128.0 - 1.0  # [-1..+1]
    ax = np.abs(nx)

    from scipy.ndimage import gaussian_filter
    n_smooth = gaussian_filter(leaf_luma, sigma=4.5)
    n_smooth = (n_smooth - n_smooth.min()) / (n_smooth.max() - n_smooth.min() + 1e-5)
    n_med = gaussian_filter(leaf_luma, sigma=1.6)
    n_med = (n_med - n_med.min()) / (n_med.max() - n_med.min() + 1e-5)

    # --- SLOT 0 (0..256): PYRENEAN SCOTS PINE / SILVER FIR ---
    crown_t = np.clip((t - 0.10) / 0.88, 0.0, 1.0)
    in_crown_y = (t >= 0.10) & (t <= 0.98)
    env = np.power(np.maximum(0.0, 1.0 - crown_t), 0.78) * 0.88 * (0.58 + 0.42 * np.sin(crown_t * math.pi))
    tier_wave = 0.5 + 0.5 * np.sin(crown_t * 2.0 * math.pi * 8.5 - ax * 4.8)
    max_r = np.maximum(0.02, env * (0.56 + 0.48 * tier_wave) * (0.68 + 0.58 * n_smooth))
    dist = ax / max_r
    edge_tex = n_med * 0.52 + leaf_hi * 0.48
    cov = (1.05 - dist) * 2.1 + (edge_tex - 0.48) * 1.45
    alpha_p = np.where(in_crown_y & (cov > 0.46), np.clip((cov - 0.46) * 4.2, 0.0, 1.0), 0.0)
    shade_p = np.clip(0.22 + crown_t * 0.46 + (1.0 - tier_wave) * 0.22, 0.12, 1.0)
    # Deep cool Pyrenean pine green with sunlit warm needle tips
    pine_base = np.stack([
        (0.09 + shade_p * 0.15) * leaf_detail,
        (0.16 + shade_p * 0.22) * leaf_detail,
        (0.09 + shade_p * 0.11) * leaf_detail,
    ], axis=-1)
    trunk_w = 0.072 * (1.0 - t * 0.65)
    in_trunk_p = (t < 0.30) & (ax < trunk_w) & (alpha_p < 0.25)
    bark_col = bark_img * (0.55 + 0.45 * (1.0 - np.clip(ax / np.maximum(0.01, trunk_w), 0, 1)))[:, :, None]
    rgba[:, 0:256, :3] = np.where(in_trunk_p[:, :, None], bark_col, pine_base)
    rgba[:, 0:256, 3] = np.where(in_trunk_p, 1.0, alpha_p)
    nrm_out[:, 0:256, :] = leaf_nrm

    # --- SLOT 1 (256..512): SUNLIT PYRENEAN BEECH / VALLEY DECIDUOUS ---
    # Matches lush bright-to-deep green valley broadleaf seen along Rio Esera & Benasque!
    lobes_b = [
        (0.00, 0.64, 0.48, 0.30),
        (-0.38, 0.52, 0.36, 0.22),
        (0.38, 0.54, 0.36, 0.22),
        (-0.24, 0.78, 0.32, 0.18),
        (0.24, 0.80, 0.32, 0.18),
        (0.00, 0.89, 0.26, 0.12),
        (-0.14, 0.42, 0.34, 0.14),
        (0.18, 0.44, 0.34, 0.14),
    ]
    min_d = np.full((H, 256), 9.0, dtype=np.float32)
    lobe_top = np.full((H, 256), 0.5, dtype=np.float32)
    for cx, cy, rx, ry in lobes_b:
        d = np.sqrt(((nx - cx) / rx) ** 2 + ((t - cy) / ry) ** 2)
        closer = d < min_d
        min_d = np.where(closer, d, min_d)
        lobe_top = np.where(closer, np.clip(0.5 + (t - cy) / (ry * 1.2), 0.0, 1.0), lobe_top)
    n_med_1 = np.roll(n_med, 53, axis=1)
    leaf_hi_1 = np.roll(leaf_hi, 79, axis=0)
    leaf_det_1 = np.roll(leaf_detail, 37, axis=1)
    edge_1 = min_d - (n_smooth - 0.5) * 0.46 - (n_med_1 - 0.5) * 0.38
    holes_1 = np.where(min_d > 0.34, n_med_1 * 0.50 + leaf_hi_1 * 0.50, 0.85)
    alpha_1 = np.where((edge_1 < 0.94) & (holes_1 > 0.40), np.clip((0.94 - edge_1) * 5.8, 0.0, 1.0), 0.0)
    height_ao1 = np.clip((t - 0.22) / 0.70, 0.0, 1.0)
    shade_1 = np.clip(0.16 + height_ao1 * 0.46 + lobe_top * 0.26, 0.08, 1.0)
    # Authentic natural Pyrenean summer beech/poplar green (earthy & sunlit, NEVER neon lime!)
    beech_base = np.stack([
        (0.08 + shade_1 * 0.15) * leaf_det_1,
        (0.13 + shade_1 * 0.20) * leaf_det_1,
        (0.06 + shade_1 * 0.08) * leaf_det_1,
    ], axis=-1)
    trunk_w1 = 0.068 * (1.0 - t * 0.55)
    in_trunk_1 = (t < 0.44) & (np.abs(nx - np.sin(t * 6.0) * 0.025) < trunk_w1) & (alpha_1 < 0.25)
    rgba[:, 256:512, :3] = np.where(in_trunk_1[:, :, None], bark_img * 0.88, beech_base)
    rgba[:, 256:512, 3] = np.where(in_trunk_1, 1.0, alpha_1)
    nrm_out[:, 256:512, :] = np.roll(leaf_nrm, 37, axis=1)

    # --- SLOT 2 (512..768): MEDITERRANEAN HOLM OAK (WIDE SPREADING CROWN) ---
    lobes_o = [
        (0.00, 0.62, 0.52, 0.24),
        (-0.46, 0.54, 0.38, 0.19),
        (0.46, 0.52, 0.38, 0.19),
        (-0.28, 0.75, 0.32, 0.16),
        (0.28, 0.74, 0.32, 0.16),
        (0.00, 0.82, 0.28, 0.13),
    ]
    min_d = np.full((H, 256), 9.0, dtype=np.float32)
    lobe_top = np.full((H, 256), 0.5, dtype=np.float32)
    for cx, cy, rx, ry in lobes_o:
        d = np.sqrt(((nx - cx) / rx) ** 2 + ((t - cy) / ry) ** 2)
        closer = d < min_d
        min_d = np.where(closer, d, min_d)
        lobe_top = np.where(closer, np.clip(0.5 + (t - cy) / (ry * 1.25), 0.0, 1.0), lobe_top)
    n_med_2 = np.roll(n_med, 127, axis=1)
    leaf_hi_2 = np.roll(leaf_hi, 141, axis=0)
    leaf_det_2 = np.roll(leaf_detail, 113, axis=1)
    edge_2 = min_d - (n_smooth - 0.5) * 0.48 - (n_med_2 - 0.5) * 0.38
    holes_2 = np.where(min_d > 0.34, n_med_2 * 0.50 + leaf_hi_2 * 0.50, 0.84)
    alpha_2 = np.where((edge_2 < 0.93) & (holes_2 > 0.41), np.clip((0.93 - edge_2) * 5.8, 0.0, 1.0), 0.0)
    shade_2 = np.clip(0.18 + np.clip((t - 0.26) / 0.64, 0.0, 1.0) * 0.48 + lobe_top * 0.25, 0.10, 1.0)
    oak_base = np.stack([
        (0.12 + shade_2 * 0.16) * leaf_det_2,
        (0.15 + shade_2 * 0.19) * leaf_det_2,
        (0.09 + shade_2 * 0.11) * leaf_det_2,
    ], axis=-1)
    trunk_w2 = 0.082 * (1.0 - t * 0.55)
    in_trunk_2 = (t < 0.48) & (np.abs(nx - np.sin(t * 7.5) * 0.03) < trunk_w2) & (alpha_2 < 0.25)
    rgba[:, 512:768, :3] = np.where(in_trunk_2[:, :, None], bark_img * 0.80, oak_base)
    rgba[:, 512:768, 3] = np.where(in_trunk_2, 1.0, alpha_2)
    nrm_out[:, 512:768, :] = np.roll(leaf_nrm, 113, axis=1)

    # --- SLOT 3 (768..1024): ORGANIC SPANISH SCRUB BUSH & MEADOW GRASS CLUMP ---
    # Natural earthy Iberian wild scrub + tufted grass blades (blends seamlessly into meadows & riverbanks!)
    alpha_g = np.zeros((H, 256), dtype=np.float32)
    shade_g = np.zeros((H, 256), dtype=np.float32)
    rng = np.random.default_rng(42)
    # Soft organic rounded shrub/tuft base
    for bx_c, by_c, rx_c, ry_c in [(0.0, 0.28, 0.68, 0.32), (-0.42, 0.22, 0.44, 0.24), (0.42, 0.24, 0.44, 0.24), (0.0, 0.46, 0.48, 0.26)]:
        bd = np.sqrt(((nx - bx_c) / rx_c) ** 2 + ((t - by_c) / ry_c) ** 2)
        n_med_b = np.roll(n_med, 173, axis=0)
        core_b = (t > 0.02) & ((bd - (n_smooth - 0.5) * 0.36 - (n_med_b - 0.5) * 0.32) < 0.86) & (n_med_b > 0.32)
        alpha_g = np.where(core_b, 1.0, alpha_g)
        shade_g = np.where(core_b, np.maximum(shade_g, np.clip(0.22 + t * 0.78, 0.15, 1.0)), shade_g)
    # Tapered organic wild grass & twig sprigs emerging around bush perimeter
    for _ in range(28):
        bx = float(rng.uniform(-0.74, 0.74))
        bh = float(rng.uniform(0.32, 0.84))
        bw = float(rng.uniform(0.024, 0.055))
        lean = float(rng.uniform(-0.32, 0.32))
        bt = np.clip(t / max(0.05, bh), 0.0, 1.2)
        cx_b = bx + lean * (bt ** 1.6)
        half_bw = bw * np.maximum(0.0, 1.0 - bt * 0.94)
        inside = (t <= bh) & (np.abs(nx - cx_b) <= half_bw)
        b_shade = np.clip(0.26 + 0.66 * bt, 0.20, 1.0) * float(rng.uniform(0.84, 1.16))
        alpha_g = np.where(inside, 1.0, alpha_g)
        shade_g = np.where(inside, np.maximum(shade_g, b_shade), shade_g)
    grass_base = np.stack([
        np.clip((0.18 + shade_g * 0.20) * leaf_detail, 0.0, 1.0),
        np.clip((0.22 + shade_g * 0.24) * leaf_detail, 0.0, 1.0),
        np.clip((0.12 + shade_g * 0.12) * leaf_detail, 0.0, 1.0),
    ], axis=-1)
    rgba[:, 768:1024, :3] = grass_base
    rgba[:, 768:1024, 3] = alpha_g
    nrm_out[:, 768:1024, :] = np.roll(leaf_nrm, 173, axis=0)

    Image.fromarray(np.clip(rgba * 255.0, 0, 255).astype(np.uint8), mode="RGBA").save(albedo_path, optimize=True)
    Image.fromarray(np.clip(nrm_out * 255.0, 0, 255).astype(np.uint8), mode="RGB").save(normal_path, optimize=True)
    print(f"ok   {albedo_path.name} ({albedo_path.stat().st_size // 1024} KB) + {normal_path.name}")


def main() -> int:
    force = "--force" in sys.argv
    jobs = [(ln, ph, mk) for ln, (ph, maps) in ASSETS.items() for mk in maps]
    with ThreadPoolExecutor(max_workers=8) as pool:
        futs = [pool.submit(fetch_one, ln, ph, mk, force) for ln, ph, mk in jobs]
        for fut in as_completed(futs):
            print(" ", fut.result())
    build_photographic_tree_atlas()
    return 0


if __name__ == "__main__":
    sys.exit(main())
