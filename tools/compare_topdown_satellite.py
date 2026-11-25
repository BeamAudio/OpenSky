#!/usr/bin/env python3
"""
Comprehensive Top-Down Satellite vs. Game Comparison Tool (Offline QA Verification).

Compares calibrated nadir screenshots from `./run_custom_sim.sh --topdown <dir>`
against openly licensed IGN Spain PNOA (0.25 m/px CC-BY 4.0) or local reference
captures (--ref-dir).

Outputs per site:
  - 4-panel diagnostic board: Satellite | Game | 50/50 Diagonal Split-Wipe | Diff Heatmap
  - Quantitative metrics: FFT phase-correlation geo-shift (dE_m, dN_m), luminance SSIM,
    edge sharpness ratio, CIE76 Lab Delta-E, RGB/luma bias, saturation ratio
  - Master multi-site comparison sheet + report.json + report.md

NOTE: Offline QA tool only — never packs reference imagery into shipped game assets.
"""

from __future__ import annotations

import argparse
import io
import json
import math
import sys
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

USER_AGENT = "NorthernSpainVFR-TopDownQA/1.0"


# ---------------------------------------------------------------------------
# Web Mercator & Quadkey Geometry
# ---------------------------------------------------------------------------
def latlon_to_merc_px(lat: float, lon: float, zoom: int) -> tuple[float, float]:
    n = 256.0 * (1 << zoom)
    x = (lon + 180.0) / 360.0 * n
    sin_lat = math.sin(math.radians(max(-85.05112878, min(85.05112878, lat))))
    y = (0.5 - math.log((1.0 + sin_lat) / (1.0 - sin_lat)) / (4.0 * math.pi)) * n
    return x, y


def _http_get(url: str, timeout: float = 20.0) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


# ---------------------------------------------------------------------------
# Satellite Imagery Providers (Exact Geographic Registration to meta.json)
# ---------------------------------------------------------------------------
def fetch_pnoa_wms(site: dict) -> Image.Image:
    """Fetch 0.25m/px IGN Spain PNOA Orthoimage via open CC-BY 4.0 WMS in exact EPSG:4326."""
    w, h = int(site["width"]), int(site["height"])
    bbox = f"{site['min_lon']:.7f},{site['min_lat']:.7f},{site['max_lon']:.7f},{site['max_lat']:.7f}"
    params = urllib.parse.urlencode({
        "SERVICE": "WMS",
        "VERSION": "1.1.1",
        "REQUEST": "GetMap",
        "LAYERS": "OI.OrthoimageCoverage",
        "STYLES": "",
        "SRS": "EPSG:4326",
        "BBOX": bbox,
        "WIDTH": str(w),
        "HEIGHT": str(h),
        "FORMAT": "image/jpeg",
    })
    url = f"https://www.ign.es/wms-inspire/pnoa-ma?{params}"
    return Image.open(io.BytesIO(_http_get(url, timeout=30.0))).convert("RGB")


def load_or_fetch_reference(site: dict, cache_dir: Path, ref_dir: Path | None) -> tuple[Image.Image, str]:
    w, h = int(site["width"]), int(site["height"])
    name = site["name"]
    if ref_dir is not None:
        for ext in (".png", ".jpg", ".jpeg", ".webp"):
            p = ref_dir / f"{name}{ext}"
            if p.exists():
                return Image.open(p).convert("RGB").resize((w, h), Image.Resampling.BICUBIC), f"Local Ref ({p.name})"
        raise FileNotFoundError(f"Missing local reference image for '{name}' in {ref_dir}")

    cache_dir.mkdir(parents=True, exist_ok=True)
    cpath = cache_dir / f"pnoa_{name}.jpg"
    if cpath.exists():
        return Image.open(cpath).convert("RGB").resize((w, h), Image.Resampling.BICUBIC), "IGN PNOA 0.25m"
    img = fetch_pnoa_wms(site)
    img.save(cpath, quality=95)
    return img, "IGN PNOA 0.25m"


# ---------------------------------------------------------------------------
# Quantitative Image Quality & Geo-Alignment Metrics (Pure NumPy)
# ---------------------------------------------------------------------------
def _to_luma(rgb01: np.ndarray) -> np.ndarray:
    return 0.2126 * rgb01[..., 0] + 0.7152 * rgb01[..., 1] + 0.0722 * rgb01[..., 2]


def _sobel_mag(gray: np.ndarray) -> np.ndarray:
    p = np.pad(gray, 1, mode="edge")
    gx = (p[:-2, 2:] + 2.0 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2.0 * p[1:-1, :-2] + p[2:, :-2])
    gy = (p[2:, :-2] + 2.0 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2.0 * p[:-2, 1:-1] + p[:-2, 2:])
    return np.hypot(gx, gy)


def _box_mean(img: np.ndarray, k: int = 11) -> np.ndarray:
    r = k // 2
    p = np.pad(img, ((r, r), (r, r)), mode="edge")
    cs = np.pad(p, ((1, 0), (1, 0)), mode="constant").cumsum(axis=0).cumsum(axis=1)
    return (cs[k:, k:] - cs[:-k, k:] - cs[k:, :-k] + cs[:-k, :-k]) / float(k * k)


def compute_ssim(gray_a: np.ndarray, gray_b: np.ndarray) -> float:
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    mu_a, mu_b = _box_mean(gray_a, 11), _box_mean(gray_b, 11)
    sa2 = np.maximum(0.0, _box_mean(gray_a * gray_a, 11) - mu_a * mu_a)
    sb2 = np.maximum(0.0, _box_mean(gray_b * gray_b, 11) - mu_b * mu_b)
    sab = _box_mean(gray_a * gray_b, 11) - mu_a * mu_b
    num = (2.0 * mu_a * mu_b + c1) * (2.0 * sab + c2)
    den = (mu_a * mu_a + mu_b * mu_b + c1) * (sa2 + sb2 + c2)
    return float(np.mean(num / np.maximum(den, 1e-8)))


def phase_correlation_shift_px(ref_gray: np.ndarray, game_gray: np.ndarray) -> tuple[float, float, float]:
    """
    FFT normalized phase-correlation on Sobel edge maps to detect geo-alignment shift
    (dx_px, dy_px) needed to shift game -> ref (positive dx = game shifted West of ref).
    """
    h, w = ref_gray.shape
    win = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    e_ref = _sobel_mag(ref_gray) * win
    e_gam = _sobel_mag(game_gray) * win
    f_ref = np.fft.rfft2(e_ref)
    f_gam = np.fft.rfft2(e_gam)
    cross = f_ref * np.conj(f_gam)
    cross /= np.maximum(np.abs(cross), 1e-8)
    corr = np.fft.irfft2(cross, s=(h, w))
    # Search within +-15% window so noise spikes at far corners can't hijack the peak
    max_s = max(8, int(min(h, w) * 0.15))
    corr_s = np.fft.fftshift(corr)
    cy, cx = h // 2, w // 2
    sub = corr_s[cy - max_s : cy + max_s + 1, cx - max_s : cx + max_s + 1]
    iy, ix = np.unravel_index(np.argmax(sub), sub.shape)
    peak = float(sub[iy, ix])
    dx = float(ix - max_s)
    dy = float(iy - max_s)
    # 3-point parabolic sub-pixel refinement
    if 0 < ix < sub.shape[1] - 1:
        den = sub[iy, ix - 1] - 2.0 * sub[iy, ix] + sub[iy, ix + 1]
        if abs(den) > 1e-9:
            dx += 0.5 * (sub[iy, ix - 1] - sub[iy, ix + 1]) / den
    if 0 < iy < sub.shape[0] - 1:
        den = sub[iy - 1, ix] - 2.0 * sub[iy, ix] + sub[iy + 1, ix]
        if abs(den) > 1e-9:
            dy += 0.5 * (sub[iy - 1, ix] - sub[iy + 1, ix]) / den
    return dx, dy, peak


def srgb_to_lab(rgb01: np.ndarray) -> np.ndarray:
    lin = np.where(rgb01 <= 0.04045, rgb01 / 12.92, ((rgb01 + 0.055) / 1.055) ** 2.4)
    x = (lin[..., 0] * 0.4124564 + lin[..., 1] * 0.3575761 + lin[..., 2] * 0.1804375) / 0.95047
    y = (lin[..., 0] * 0.2126729 + lin[..., 1] * 0.7151522 + lin[..., 2] * 0.0721750) / 1.00000
    z = (lin[..., 0] * 0.0193339 + lin[..., 1] * 0.1191920 + lin[..., 2] * 0.9503041) / 1.08883
    eps, kappa = 216.0 / 24389.0, 24389.0 / 27.0
    f = lambda t: np.where(t > eps, np.cbrt(np.maximum(t, 0.0)), (kappa * t + 16.0) / 116.0)
    fx, fy, fz = f(x), f(y), f(z)
    return np.stack([116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)], axis=-1)


def evaluate_pair(ref_img: Image.Image, game_img: Image.Image, span_e_m: float, span_n_m: float) -> tuple[dict, np.ndarray]:
    ref_np = np.asarray(ref_img, dtype=np.float32) / 255.0
    gam_np = np.asarray(game_img, dtype=np.float32) / 255.0
    h, w, _ = ref_np.shape

    g_ref, g_gam = _to_luma(ref_np), _to_luma(gam_np)
    dx_px, dy_px, corr_peak = phase_correlation_shift_px(g_ref, g_gam)
    mpp_e = span_e_m / max(1, w)
    mpp_n = span_n_m / max(1, h)
    de_m = dx_px * mpp_e
    dn_m = -dy_px * mpp_n
    shift_m = math.hypot(de_m, dn_m)

    ssim_raw = compute_ssim(g_ref, g_gam)
    # Also compute edge sharpness ratio
    edge_ref = _sobel_mag(g_ref)
    edge_gam = _sobel_mag(g_gam)
    edge_ratio = float(np.mean(edge_gam) / max(1e-5, float(np.mean(edge_ref))))

    # CIE76 Lab Delta-E (subsampled 2x for speed) & RGB/saturation stats
    lab_ref = srgb_to_lab(ref_np[::2, ::2])
    lab_gam = srgb_to_lab(gam_np[::2, ::2])
    delta_e_map = np.linalg.norm(lab_gam - lab_ref, axis=-1)
    mean_de = float(np.mean(delta_e_map))

    rgb_bias = (np.mean(gam_np, axis=(0, 1)) - np.mean(ref_np, axis=(0, 1))).tolist()
    luma_bias = float(np.mean(g_gam) - np.mean(g_ref))
    sat_ref = float(np.mean(np.max(ref_np, axis=-1) - np.min(ref_np, axis=-1)))
    sat_gam = float(np.mean(np.max(gam_np, axis=-1) - np.min(gam_np, axis=-1)))
    sat_ratio = sat_gam / max(1e-4, sat_ref)

    # Full-res normalized visual diff map (weighted luma + color diff) for diagnostic heatmap
    diff_map = np.clip(0.55 * np.abs(g_gam - g_ref) + 0.45 * np.mean(np.abs(gam_np - ref_np), axis=-1), 0.0, 1.0)

    # Verdict heuristic
    if shift_m <= 15.0 and mean_de <= 22.0 and 0.55 <= edge_ratio <= 2.2:
        verdict = "PASS"
    elif shift_m <= 35.0 and mean_de <= 32.0:
        verdict = "WARN"
    else:
        verdict = "FAIL"

    metrics = {
        "dx_px": round(dx_px, 2),
        "dy_px": round(dy_px, 2),
        "shift_east_m": round(de_m, 2),
        "shift_north_m": round(dn_m, 2),
        "shift_total_m": round(shift_m, 2),
        "phase_corr_peak": round(corr_peak, 4),
        "ssim": round(ssim_raw, 4),
        "edge_ratio": round(edge_ratio, 3),
        "mean_delta_e": round(mean_de, 2),
        "luma_bias": round(float(luma_bias), 3),
        "rgb_bias": [round(float(v), 3) for v in rgb_bias],
        "sat_ratio": round(float(sat_ratio), 3),
        "verdict": verdict,
    }
    return metrics, diff_map


# ---------------------------------------------------------------------------
# Diagnostic Visual Boards
# ---------------------------------------------------------------------------
def _inferno_colormap(val01: np.ndarray) -> Image.Image:
    v = np.clip(val01 * 2.4, 0.0, 1.0)  # Amplify 0..0.42 error to full vivid scale
    r = np.clip(1.6 * v - 0.15, 0.0, 1.0)
    g = np.clip(1.8 * np.maximum(0.0, v - 0.35), 0.0, 1.0)
    b = np.clip(0.85 * np.sin(v * math.pi) * (1.0 - 0.55 * v) + 0.08, 0.0, 1.0)
    rgb = np.stack([r, g, b], axis=-1)
    return Image.fromarray((rgb * 255.0).astype(np.uint8))


def _make_split_wipe(ref_img: Image.Image, game_img: Image.Image) -> Image.Image:
    r = np.asarray(ref_img)
    g = np.asarray(game_img)
    h, w, _ = r.shape
    yy, xx = np.mgrid[0:h, 0:w]
    # Diagonal split: top-left = Satellite Reference, bottom-right = Game Top-Down
    mask = (xx / float(w) + yy / float(h)) > 1.0
    out = np.where(mask[..., None], g, r)
    # Crisp cyan diagonal divider + subtle 4x4 alignment grid
    diag_dist = np.abs(xx / float(w) + yy / float(h) - 1.0)
    line = diag_dist < (2.5 / max(w, h))
    out[line] = np.array([40, 235, 255], dtype=np.uint8)
    for frac in (0.25, 0.5, 0.75):
        gx, gy = int(w * frac), int(h * frac)
        out[gy, ::2] = (out[gy, ::2].astype(np.int32) * 2 + np.array([255, 255, 255])) // 3
        out[::2, gx] = (out[::2, gx].astype(np.int32) * 2 + np.array([255, 255, 255])) // 3
    return Image.fromarray(out)


def _draw_scale_and_north(img: Image.Image, span_e_m: float, title: str) -> Image.Image:
    out = img.copy()
    d = ImageDraw.Draw(out, "RGBA")
    w, h = out.size
    d.rectangle([0, 0, w, 28], fill=(10, 14, 22, 195))
    d.text((10, 8), title, fill=(240, 246, 255, 255))
    # North arrow top-right
    d.rectangle([w - 36, 34, w - 8, 66], fill=(10, 14, 22, 180))
    d.text((w - 26, 36), "N", fill=(110, 235, 255, 255))
    d.line([(w - 22, 62), (w - 22, 48)], fill=(110, 235, 255, 255), width=2)
    # 100m or 200m scale bar bottom-left
    bar_m = 200.0 if span_e_m > 750.0 else 100.0
    bar_px = max(20, int(w * (bar_m / max(1.0, span_e_m))))
    d.rectangle([10, h - 32, 22 + bar_px, h - 8], fill=(10, 14, 22, 185))
    d.line([(16, h - 15), (16 + bar_px, h - 15)], fill=(255, 255, 255, 255), width=3)
    d.text((16, h - 30), f"{int(bar_m)} m", fill=(235, 240, 250, 255))
    return out


def render_4panel_board(
    site: dict, ref_img: Image.Image, game_img: Image.Image, diff_map: np.ndarray, metrics: dict, ref_label: str
) -> Image.Image:
    pw = 512
    ref_s = _draw_scale_and_north(ref_img.resize((pw, pw), Image.Resampling.LANCZOS), site["span_e_m"], f"REAL: {ref_label}")
    gam_s = _draw_scale_and_north(game_img.resize((pw, pw), Image.Resampling.LANCZOS), site["span_e_m"], "GAME: Northern Spain VFR Top-Down")
    spl_s = _draw_scale_and_north(_make_split_wipe(ref_img, game_img).resize((pw, pw), Image.Resampling.LANCZOS), site["span_e_m"], "SPLIT: Top-Left=Real | Bot-Right=Game")
    het_s = _draw_scale_and_north(_inferno_colormap(diff_map).resize((pw, pw), Image.Resampling.LANCZOS), site["span_e_m"], "PER-PIXEL DIFF HEATMAP (Dark=Match, Bright=Diff)")

    pad, hdr, ftr = 10, 44, 52
    W = pw * 2 + pad * 3
    H = pw * 2 + pad * 3 + hdr + ftr
    board = Image.new("RGB", (W, H), (14, 18, 26))
    board.paste(ref_s, (pad, hdr + pad))
    board.paste(gam_s, (pw + pad * 2, hdr + pad))
    board.paste(spl_s, (pad, hdr + pw + pad * 2))
    board.paste(het_s, (pw + pad * 2, hdr + pw + pad * 2))

    d = ImageDraw.Draw(board)
    vcol = {"PASS": (80, 230, 125), "WARN": (245, 200, 75), "FAIL": (245, 85, 85)}[metrics["verdict"]]
    d.text((pad, 14), f"{site['label']}  ({site['lat']:.5f}, {site['lon']:.5f})  |  AGL {site['agl_m']:.0f}m  |  Footprint {site['span_e_m']:.0f}x{site['span_n_m']:.0f}m", fill=(240, 246, 255))
    d.text((W - 120, 14), f"[{metrics['verdict']}]", fill=vcol)

    fy = H - ftr + 10
    d.text((pad, fy),
           f"Geo-Align Shift: dE={metrics['shift_east_m']:+.1f}m, dN={metrics['shift_north_m']:+.1f}m (total={metrics['shift_total_m']:.1f}m)  |  "
           f"SSIM={metrics['ssim']:.3f}  |  Lab Delta-E={metrics['mean_delta_e']:.1f}",
           fill=(220, 232, 245))
    d.text((pad, fy + 20),
           f"Sharpness Ratio={metrics['edge_ratio']:.2f}x  |  Luma Bias={metrics['luma_bias']:+.3f}  |  "
           f"RGB Bias=({metrics['rgb_bias'][0]:+.2f}, {metrics['rgb_bias'][1]:+.2f}, {metrics['rgb_bias'][2]:+.2f})  |  "
           f"Saturation Ratio={metrics['sat_ratio']:.2f}x",
           fill=(175, 195, 215))
    return board


def render_master_sheet(rows: list[tuple[dict, Image.Image, Image.Image, dict, str]]) -> Image.Image:
    tw = 320
    pad, hdr = 8, 42
    row_h = tw + 26
    W = tw * 3 + pad * 4 + 230
    H = hdr + len(rows) * (row_h + pad) + pad
    sheet = Image.new("RGB", (W, H), (12, 16, 23))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 13), "NORTHERN SPAIN VFR - TOP-DOWN SATELLITE FIDELITY AUDIT (Real Satellite | Game Top-Down | 50/50 Diagonal Split | Metrics)", fill=(242, 247, 255))

    for i, (site, ref_img, game_img, m, ref_label) in enumerate(rows):
        y = hdr + pad + i * (row_h + pad)
        r_s = ref_img.resize((tw, tw), Image.Resampling.LANCZOS)
        g_s = game_img.resize((tw, tw), Image.Resampling.LANCZOS)
        s_s = _make_split_wipe(ref_img, game_img).resize((tw, tw), Image.Resampling.LANCZOS)
        sheet.paste(r_s, (pad, y + 20))
        sheet.paste(g_s, (tw + pad * 2, y + 20))
        sheet.paste(s_s, (tw * 2 + pad * 3, y + 20))

        d.text((pad, y + 3), f"{site['label']}  [{ref_label}]", fill=(220, 234, 250))
        d.text((tw + pad * 2, y + 3), f"Game Top-Down ({site['span_e_m']:.0f}m span)", fill=(220, 234, 250))
        d.text((tw * 2 + pad * 3, y + 3), "50/50 Split-Wipe Alignment", fill=(115, 232, 255))

        mx = tw * 3 + pad * 4 + 6
        vcol = {"PASS": (80, 230, 125), "WARN": (245, 200, 75), "FAIL": (245, 85, 85)}[m["verdict"]]
        d.text((mx, y + 26), f"VERDICT: {m['verdict']}", fill=vcol)
        lines = [
            f"Geo Shift: {m['shift_total_m']:.1f} m",
            f"  dE: {m['shift_east_m']:+.1f} m",
            f"  dN: {m['shift_north_m']:+.1f} m",
            f"SSIM:      {m['ssim']:.3f}",
            f"Lab dE:    {m['mean_delta_e']:.1f}",
            f"Edge:      {m['edge_ratio']:.2f}x",
            f"Luma Bias: {m['luma_bias']:+.3f}",
            f"Sat Ratio: {m['sat_ratio']:.2f}x",
            f"RGB Bias:",
            f"  R {m['rgb_bias'][0]:+.2f} G {m['rgb_bias'][1]:+.2f} B {m['rgb_bias'][2]:+.2f}",
        ]
        for li, line in enumerate(lines):
            d.text((mx, y + 54 + li * 22), line, fill=(205, 218, 232))
    return sheet


# ---------------------------------------------------------------------------
# Built-In Self-Check
# ---------------------------------------------------------------------------
def run_selfcheck() -> int:
    # 1. Web Mercator reference vector: (lat=0, lon=0, z=0) == (128, 128)
    mx, my = latlon_to_merc_px(0.0, 0.0, 0)
    assert abs(mx - 128.0) < 1e-6 and abs(my - 128.0) < 1e-6, (mx, my)

    # 2. Synthetic texture shift detection & SSIM/Delta-E exactness
    rng = np.random.default_rng(42)
    base = np.zeros((256, 256, 3), dtype=np.float32)
    for k in range(18):
        x0, y0 = int(rng.integers(20, 200)), int(rng.integers(20, 200))
        bw, bh = int(rng.integers(12, 42)), int(rng.integers(12, 42))
        base[y0 : y0 + bh, x0 : x0 + bw] = rng.uniform(0.2, 0.9, size=(3,))
    base_img = Image.fromarray((base * 255).astype(np.uint8))

    m_same, _ = evaluate_pair(base_img, base_img, 512.0, 512.0)
    assert m_same["ssim"] > 0.999 and m_same["mean_delta_e"] < 0.05 and m_same["shift_total_m"] < 0.5, m_same

    # Shift game image by (-14 px in X, +9 px in Y) => phase corr should report (+14 px, -9 px)
    shifted = np.roll(np.roll(base, -14, axis=1), 9, axis=0)
    shifted_img = Image.fromarray((shifted * 255).astype(np.uint8))
    m_sh, _ = evaluate_pair(base_img, shifted_img, 512.0, 512.0)
    assert abs(m_sh["dx_px"] - 14.0) < 1.0 and abs(m_sh["dy_px"] - (-9.0)) < 1.0, m_sh
    print(f"[SELFCHECK OK] Mercator(0,0,z0)=({mx:.1f},{my:.1f}) | Identical SSIM={m_same['ssim']:.4f} | Shift detected=({m_sh['dx_px']:+.2f}, {m_sh['dy_px']:+.2f})px")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Top-Down Satellite vs Game Comparison Tool (IGN PNOA / local refs)")
    ap.add_argument("--topdown-dir", type=Path, help="Directory generated by `./run_custom_sim.sh --topdown <dir>`")
    ap.add_argument("--out-dir", type=Path, help="Output directory for comparison boards & report.json")
    ap.add_argument("--ref-dir", type=Path, default=None, help="Optional folder of local top-down PNG/JPG captures")
    ap.add_argument("--selfcheck", action="store_true", help="Run built-in unit self-checks and exit")
    args = ap.parse_args()

    if args.selfcheck:
        return run_selfcheck()
    if not args.topdown_dir or not args.out_dir:
        ap.error("--topdown-dir and --out-dir are required (or pass --selfcheck)")

    meta_path = args.topdown_dir / "meta.json"
    if not meta_path.exists():
        print(f"[ERROR] Missing {meta_path}. Run `./run_custom_sim.sh --topdown {args.topdown_dir}` first.", file=sys.stderr)
        return 1

    meta = json.loads(meta_path.read_text())
    args.out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = args.out_dir / "ref_cache"

    results = []
    master_rows = []
    print(f"\n{'Site':<20} {'Provider':<18} {'Shift(m)':>9} {'SSIM':>7} {'Lab dE':>7} {'Edge':>6} {'Luma':>7} {'Verdict':>8}")
    print("-" * 88)

    for site in meta["sites"]:
        game_path = args.topdown_dir / site["image"]
        game_img = Image.open(game_path).convert("RGB")
        ref_img, ref_label = load_or_fetch_reference(site, cache_dir, args.ref_dir)
        metrics, diff_map = evaluate_pair(ref_img, game_img, float(site["span_e_m"]), float(site["span_n_m"]))
        board = render_4panel_board(site, ref_img, game_img, diff_map, metrics, ref_label)
        board_path = args.out_dir / f"compare_{site['name']}.png"
        board.save(board_path)

        results.append({"site": site, "provider": ref_label, "metrics": metrics, "board": str(board_path)})
        master_rows.append((site, ref_img, game_img, metrics, ref_label))
        print(
            f"{site['name']:<20} {ref_label:<18} {metrics['shift_total_m']:>8.1f}m "
            f"{metrics['ssim']:>7.3f} {metrics['mean_delta_e']:>7.1f} {metrics['edge_ratio']:>5.2f}x "
            f"{metrics['luma_bias']:>+7.3f} {metrics['verdict']:>8}"
        )

    master_path = args.out_dir / "topdown_master_comparison.png"
    render_master_sheet(master_rows).save(master_path)

    report_json = args.out_dir / "report.json"
    report_json.write_text(json.dumps(
        {"provider": "IGN PNOA 0.25m (CC-BY 4.0)", "results": results},
        indent=2,
        default=lambda o: float(o) if isinstance(o, np.floating) else int(o) if isinstance(o, np.integer) else str(o),
    ))

    md_lines = [
        "# Top-Down Satellite Comparison Report (IGN PNOA CC-BY 4.0)",
        "",
        "| Site | Provider | Geo Shift (m) | dE / dN (m) | SSIM | Lab Delta-E | Edge Ratio | Luma Bias | Sat Ratio | Verdict |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for r in results:
        m = r["metrics"]
        md_lines.append(
            f"| {r['site']['label']} | {r['provider']} | {m['shift_total_m']:.1f} | "
            f"{m['shift_east_m']:+.1f} / {m['shift_north_m']:+.1f} | {m['ssim']:.3f} | "
            f"{m['mean_delta_e']:.1f} | {m['edge_ratio']:.2f}x | {m['luma_bias']:+.3f} | {m['sat_ratio']:.2f}x | **{m['verdict']}** |"
        )
    (args.out_dir / "report.md").write_text("\n".join(md_lines) + "\n")
    print(f"\n[DONE] Saved {len(results)} 4-panel boards + master sheet -> {master_path}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
