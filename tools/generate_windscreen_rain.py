#!/usr/bin/env python3
"""Offline physical droplet simulation -> weather-layered C172 glass relief LUT.

The game performs no particle or fluid simulation. This CPU asset build simulates
rain deposition, contact-line pinning, viscous relaxation, gravity, airflow shear,
and drop coalescence for four rainfall regimes, then bakes normals/height/coverage
into two 3D-LUT volumes: front windshield and side/rear cockpit glazing.
"""
from pathlib import Path
import math

import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "game/assets/pbr"
SIZE = 512
RAIN_MM_H = (0.35, 2.5, 12.0, 45.0)
REFERENCE_AIRSPEED_MPS = 55.0
DT = 0.025
STEPS = 160
FRAME_STRIDE = 4


def spawn_drops(rng, level):
    counts = (420, 720, 1160, 1680)
    lo = (0.55, 0.65, 0.80, 1.0)[level]
    hi = (1.65, 2.15, 3.0, 4.3)[level]
    n = counts[level]
    # Radius is a projected bead radius in the relief atlas, sampled from a
    # positively skewed raindrop spectrum rather than a uniform billboard size.
    radius = np.clip(rng.lognormal(math.log((lo + hi) * 0.34), 0.46, n), lo, hi)
    return (rng.random((n, 2)).astype(np.float32), radius.astype(np.float32),
            np.zeros((n, 2), np.float32), np.ones(n, dtype=bool))


def coalesce(pos, radius, vel, live, rng, cell_limit=4.8):
    """Merge touching water beads, conserving projected water volume and momentum."""
    ids = np.flatnonzero(live)
    if len(ids) < 2:
        return
    px = pos[ids] * SIZE
    pairs = cKDTree(px).query_pairs(cell_limit, output_type="ndarray")
    if not len(pairs):
        return
    rng.shuffle(pairs)
    merged = set()
    for aa, bb in pairs:
        i, j = int(ids[aa]), int(ids[bb])
        if i in merged or j in merged or not live[i] or not live[j]:
            continue
        delta = (pos[i] - pos[j]) * SIZE
        if np.linalg.norm(delta) > radius[i] + radius[j] + 0.55:
            continue
        vi, vj = radius[i] ** 3, radius[j] ** 3
        total = vi + vj
        pos[i] = (pos[i] * vi + pos[j] * vj) / total
        vel[i] = (vel[i] * vi + vel[j] * vj) / total
        radius[i] = min(7.0, total ** (1.0 / 3.0))
        live[j] = False
        merged.add(i)
        merged.add(j)


def physical_trajectory(level, orientation, seed):
    rng = np.random.default_rng(seed)
    pos, radius, velocity, live = spawn_drops(rng, level)
    direction = np.array((0.0, 1.0), np.float32) if orientation == "front" else np.array((-1.0, 0.0), np.float32)
    pane_m = np.array((1.22, 0.72), np.float32) if orientation == "front" else np.array((0.48, 0.42), np.float32)
    radius_scale = np.maximum(radius / 1.25, 0.35) ** 0.42
    rain_gain = (0.78, 0.92, 1.08, 1.22)[level]
    shear_mps = 0.00105 * REFERENCE_AIRSPEED_MPS * rain_gain * radius_scale
    # Gravity is projected into the pane tangent plane; its low-speed motion is
    # resisted by contact-angle hysteresis and a viscous, size-dependent lag.
    gravity_mps = 0.0034 * radius_scale ** 1.5
    target = direction[None, :] * shear_mps[:, None]
    if orientation == "front":
        target[:, 1] -= gravity_mps
    else:
        target[:, 1] -= gravity_mps
    tau = np.clip(0.075 + radius * 0.028, 0.09, 0.24)
    paths = []
    for step in range(STEPS):
        # Contact-line pinning: weak shear leaves fine drops almost fixed, while
        # larger beads overcome the hysteresis threshold and begin to slide.
        threshold = 0.009 / np.maximum(radius, 0.5) ** 1.15
        target_speed = np.linalg.norm(target, axis=1)
        mobile = target_speed > threshold
        desired = np.where(mobile[:, None], target, 0.0)
        velocity += (desired - velocity) * np.minimum(1.0, DT / tau)[:, None]
        pos += velocity * DT / pane_m[None, :]
        pos %= 1.0
        if step % 8 == 7:
            coalesce(pos, radius, velocity, live, rng)
        if step % FRAME_STRIDE == 0:
            paths.append((pos.copy(), radius.copy(), live.copy()))
    return pos, radius, velocity, live, paths


def stamp_bead(height, coverage, x, y, radius, flow, orientation):
    # Wind elongates wet beads along the pane's actual airflow axis. The local
    # dome is a height field; its finite-difference normal creates parallax.
    stretch = 1.0 + min(1.8, float(np.linalg.norm(flow)) * 8.0)
    rx = max(0.65, float(radius) * (stretch if orientation == "side" else 1.0))
    ry = max(0.65, float(radius) * (stretch if orientation == "front" else 1.0))
    reach_x, reach_y = int(rx * 2.0 + 2), int(ry * 2.0 + 2)
    cx, cy = int(x * SIZE), int(y * SIZE)
    for oy in range(-reach_y, reach_y + 1):
        py = (cy + oy) % SIZE
        yy = oy / ry
        for ox in range(-reach_x, reach_x + 1):
            px = (cx + ox) % SIZE
            xx = ox / rx
            r2 = xx * xx + yy * yy + 0.025 * xx * yy
            if r2 >= 1.2:
                continue
            dome = max(0.0, 1.0 - r2)
            h = (0.22 + 0.78 * dome * dome) * (0.42 + min(0.58, radius * 0.12))
            height[py, px] = max(height[py, px], h)
            edge = np.clip((1.2 - r2) * 3.3, 0.0, 1.0)
            coverage[py, px] = max(coverage[py, px], edge * (0.55 + min(0.4, radius * 0.09)))


def render_lut_slice(level, orientation, seed):
    pos, radius, velocity, live, paths = physical_trajectory(level, orientation, seed)
    height = np.zeros((SIZE, SIZE), np.float32)
    coverage = np.zeros_like(height)
    trails = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(trails)
    live_ids = np.flatnonzero(live)
    # A reduced-fps path history deposits thin films behind sliding beads.
    for frame in range(1, len(paths)):
        p0, r0, a0 = paths[frame - 1]
        p1, _, a1 = paths[frame]
        valid = np.flatnonzero(a0 & a1)
        for i in valid:
            delta = (p1[i] - p0[i])
            if np.any(np.abs(delta) > 0.12):
                continue
            x0, y0 = p0[i] * SIZE
            x1, y1 = p1[i] * SIZE
            width = max(1, int(r0[i] * (0.42 + level * 0.10)))
            opacity = int(15 + level * 8)
            draw.line((x0, y0, x1, y1), fill=opacity, width=width)
    trail = np.asarray(trails, dtype=np.float32) / 255.0
    coverage = np.maximum(coverage, trail * (0.42 + level * 0.12))
    height = np.maximum(height, trail * 0.12)
    for i in live_ids:
        stamp_bead(height, coverage, pos[i, 0], pos[i, 1], radius[i], velocity[i], orientation)

    # Rain intensity also builds a thin, softly broken water film, strongest in
    # downpour cells and shallow enough not to turn the panes opaque.
    film = (0.035, 0.075, 0.13, 0.21)[level]
    noise = np.random.default_rng(seed + 902).random((SIZE, SIZE), dtype=np.float32)
    coverage = np.clip(coverage + np.maximum(0.0, noise - 0.84) * film, 0.0, 1.0)
    gy, gx = np.gradient(height)
    nx = np.clip(-gx * 9.0, -0.97, 0.97)
    ny = np.clip(-gy * 9.0, -0.97, 0.97)
    rgba = np.empty((SIZE, SIZE, 4), np.uint8)
    rgba[..., 0] = np.clip((0.5 + 0.5 * nx) * 255, 0, 255).astype(np.uint8)
    rgba[..., 1] = np.clip((0.5 + 0.5 * ny) * 255, 0, 255).astype(np.uint8)
    rgba[..., 2] = np.clip(height * 255, 0, 255).astype(np.uint8)
    rgba[..., 3] = np.clip(coverage * 255, 0, 255).astype(np.uint8)
    return rgba, int(len(live_ids))


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for orientation in ("front", "side"):
        slices, counts = [], []
        for level, rain in enumerate(RAIN_MM_H):
            atlas, count = render_lut_slice(level, orientation, seed=172 + level * 53 + (1000 if orientation == "side" else 0))
            slices.append(atlas)
            counts.append(count)
            print(f"{orientation}: {rain:g} mm/h simulated, {count} surviving drops")
        stacked = np.concatenate(slices, axis=0)
        out = OUT_DIR / f"c172_windscreen_rain_{orientation}.png"
        Image.fromarray(stacked, "RGBA").save(out, optimize=True)
        print(f"wrote {out} ({out.stat().st_size:,} bytes, {SIZE}x{SIZE}x4 RGBA weather LUT)")


if __name__ == "__main__":
    main()
