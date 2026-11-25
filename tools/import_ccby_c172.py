#!/usr/bin/env python3
"""Import & rig CC BY 4.0 artist-sculpted Cessna 172 ('e737' via Sketchfab / veyron007/AUREL).

Converts EXT_texture_webp + KHR_materials_specular into standard glTF 2.0 Metallic-Roughness PBR
with crisp 1024x1024 PNG/JPEG textures, re-orients to engine frame (-Z forward, +X right, +Y up),
splits animated PropHub + PropBlades, and adds subtle PropBlurDisc + animated aviation lights.
"""

from __future__ import annotations

import io
import json
import math
import struct
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

from export_glb_models import GLBBuilder, GeoPart, verify_glb, _build_prop_blur_tex

ROOT = Path(__file__).resolve().parent.parent
SRC_CACHE = ROOT / "tools" / "cache" / "aurel_c172_ccby.glb"
SRC_URL = "https://raw.githubusercontent.com/veyron007/AUREL/main/public/models/c172.glb"
OUT_GLB = ROOT / "game" / "assets" / "models" / "aircraft" / "c172s_exterior.glb"


def _read_acc(data: bytes, g: dict, bin_off: int, acc_i: int) -> np.ndarray:
    acc = g["accessors"][acc_i]
    bv = g["bufferViews"][acc["bufferView"]]
    off = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    dt = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}[acc["componentType"]]
    esize = np.dtype(dt).itemsize * ncomp
    if stride == 0 or stride == esize:
        return np.frombuffer(data, dtype=dt, count=acc["count"] * ncomp, offset=off).reshape(acc["count"], ncomp).copy()
    out = np.empty((acc["count"], ncomp), dtype=dt)
    for k in range(acc["count"]):
        out[k] = np.frombuffer(data, dtype=dt, count=ncomp, offset=off + k * stride)
    return out


def _extract_image(data: bytes, g: dict, bin_off: int, img_i: int, size: int = 1024) -> Image.Image:
    bv = g["bufferViews"][g["images"][img_i]["bufferView"]]
    raw = data[bin_off + bv["byteOffset"] : bin_off + bv["byteOffset"] + bv["byteLength"]]
    im = Image.open(io.BytesIO(raw)).convert("RGB")
    if im.size[0] < size:
        im = im.resize((size, size), Image.Resampling.LANCZOS).filter(
            ImageFilter.UnsharpMask(radius=1.4, percent=135, threshold=2)
        )
    return im


def _convert_specgloss_to_pbr(base_im: Image.Image, mr_im: Image.Image, spec_im: Image.Image, is_exterior: bool) -> tuple[Image.Image, Image.Image]:
    """Recover standard Metallic-Roughness Albedo + ORM from Sketchfab KHR_materials_specular export."""
    base = np.asarray(base_im, dtype=np.float32) / 255.0
    mr = np.asarray(mr_im, dtype=np.float32) / 255.0
    spec = np.asarray(spec_im, dtype=np.float32) / 255.0

    # Spec-gloss stores gloss/diffuse brightness inside specularColor on painted surfaces
    spec_luma = 0.2126 * spec[..., 0] + 0.7152 * spec[..., 1] + 0.0722 * spec[..., 2]
    base_luma = 0.2126 * base[..., 0] + 0.7152 * base[..., 1] + 0.0722 * base[..., 2]

    if is_exterior:
        # Recover crisp pearl-white & metallic-slate Skyhawk exterior + artist baked AO (zero blotchy UV color hacks!)
        ao = np.clip(0.58 + 0.42 * np.maximum(spec_luma, base_luma), 0.36, 1.0)
        alb = np.clip(spec * 0.88 + base * 0.22, 0.05, 0.96)
        # Subtle warm ivory-white aviation gloss polish & clean slate accents
        alb[..., 0] = np.clip(alb[..., 0] * 1.01, 0.0, 0.97)
        alb[..., 1] = np.clip(alb[..., 1] * 1.00, 0.0, 0.96)
        alb[..., 2] = np.clip(alb[..., 2] * 0.98, 0.0, 0.95)
        rough = np.clip(0.16 + mr[..., 1] * 0.40, 0.14, 0.65)
        metal = np.where((spec_luma < 0.35) & (base_luma < 0.25), 0.40, 0.14).astype(np.float32)
    else:
        # Interior cabin, seats, avionics, propeller, landing gear tires & struts
        ao = np.clip(0.52 + 0.48 * np.maximum(spec_luma, base_luma), 0.32, 1.0)
        # Preserve genuine colored instruments/screens in base while lifting metallic/painted surfaces from spec
        chroma = np.max(base, axis=-1) - np.min(base, axis=-1)
        alb = np.where(chroma[..., None] > 0.12, np.clip(base * 1.15, 0.0, 1.0), np.clip(base * 0.50 + spec * 0.56, 0.03, 0.94))
        rough = np.clip(0.24 + mr[..., 1] * 0.58, 0.18, 0.86)
        metal = np.where((spec_luma > 0.72) & (base_luma < 0.48), 0.62, 0.14).astype(np.float32)

    alb_img = Image.fromarray((np.clip(alb, 0.0, 1.0) * 255.0).astype(np.uint8), "RGB")
    orm_arr = np.stack([ao, rough, metal], axis=-1)
    orm_img = Image.fromarray((np.clip(orm_arr, 0.0, 1.0) * 255.0).astype(np.uint8), "RGB")
    return alb_img, orm_img


def _make_rivet_normal(size: int = 1024) -> Image.Image:
    """Subtle aluminum airframe micro-grain + panel sheen normal map for crisp aviation sunlight glint."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    h = 0.015 * np.sin(xx * 0.12) * np.cos(yy * 0.12) + 0.008 * np.sin(xx * 0.45 + yy * 0.31)
    gx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 6.0
    gy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 6.0
    nz = np.ones_like(h)
    inv = 1.0 / np.sqrt(gx * gx + gy * gy + nz * nz)
    n = np.stack([(-gx * inv) * 0.5 + 0.5, (-gy * inv) * 0.5 + 0.5, (nz * inv) * 0.5 + 0.5], axis=-1)
    return Image.fromarray((np.clip(n, 0.0, 1.0) * 255.0).astype(np.uint8), "RGB")


def _extract_submesh(pos: np.ndarray, nrm: np.ndarray, uv: np.ndarray, tris: np.ndarray, pivot: tuple[float, float, float] = (0.0, 0.0, 0.0)) -> dict:
    used = np.unique(tris.reshape(-1))
    remap = np.full(len(pos), -1, dtype=np.int32)
    remap[used] = np.arange(len(used), dtype=np.int32)
    p = pos[used] - np.array(pivot, dtype=np.float32)
    n = nrm[used]
    t = uv[used]
    idx = remap[tris.reshape(-1)].astype(np.uint32)
    return {"pos": p, "nrm": n, "uv": t, "idx": idx}


def _rot_aurel_to_engine(v: np.ndarray) -> np.ndarray:
    """Proper +1-det rotation: nose -X -> -Z, right -Z -> +X, up +Y -> +Y."""
    out = np.empty_like(v)
    out[:, 0] = -v[:, 2]
    out[:, 1] = v[:, 1]
    out[:, 2] = v[:, 0]
    return out


def main() -> None:
    SRC_CACHE.parent.mkdir(parents=True, exist_ok=True)
    if not SRC_CACHE.exists():
        print(f"Downloading CC BY 4.0 Cessna 172 from {SRC_URL} ...")
        urllib.request.urlretrieve(SRC_URL, SRC_CACHE)

    data = SRC_CACHE.read_bytes()
    magic, ver, _ = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and ver == 2, "Invalid source GLB"
    jlen, _ = struct.unpack_from("<II", data, 12)
    g = json.loads(data[20 : 20 + jlen].decode("utf-8"))
    bin_off = 20 + jlen + 8

    # Extract & convert artist textures (WebP -> crisp 1024x1024 PBR PNG/JPEG)
    # Material 0 (interior/prop/gear): base=img0, mr=img3, spec=img2
    # Material 1 (exterior airframe):  base=img1, mr=img5, spec=img4
    im0 = _extract_image(data, g, bin_off, 0, 1024)
    im1 = _extract_image(data, g, bin_off, 1, 1024)
    im2 = _extract_image(data, g, bin_off, 2, 1024)
    im3 = _extract_image(data, g, bin_off, 3, 1024)
    im4 = _extract_image(data, g, bin_off, 4, 1024)
    im5 = _extract_image(data, g, bin_off, 5, 1024)

    int_alb, int_orm = _convert_specgloss_to_pbr(im0, im3, im2, is_exterior=False)
    ext_alb, ext_orm = _convert_specgloss_to_pbr(im1, im5, im4, is_exterior=True)
    nrm_air = _make_rivet_normal(512)

    gb = GLBBuilder()
    t_int_alb = gb.add_texture_pil(int_alb, fmt="JPEG", quality=92, clamp=True)
    t_int_orm = gb.add_texture_pil(int_orm, fmt="PNG", clamp=True)
    t_ext_alb = gb.add_texture_pil(ext_alb, fmt="JPEG", quality=93, clamp=True)
    t_ext_orm = gb.add_texture_pil(ext_orm, fmt="PNG", clamp=True)
    t_nrm = gb.add_texture_pil(nrm_air, fmt="JPEG", quality=86)
    t_blur = gb.add_texture_pil(_build_prop_blur_tex(), fmt="PNG", clamp=True)

    m_int = gb.add_material("C172_Details_CCBY", (1, 1, 1, 1), metallic=0.25, roughness=0.72, base_tex=t_int_alb, mr_tex=t_int_orm, double_sided=True)
    m_ext = gb.add_material("C172_Airframe_CCBY", (1, 1, 1, 1), metallic=0.18, roughness=0.34, base_tex=t_ext_alb, normal_tex=t_nrm, normal_scale=0.35, mr_tex=t_ext_orm, double_sided=True)
    m_blur = gb.add_material("PropBlur", (1, 1, 1, 0.90), metallic=0.0, roughness=0.45, base_tex=t_blur, alpha_mode="BLEND", double_sided=True)
    m_nav_r = gb.add_material("NavRed", (1.0, 0.14, 0.08, 1.0), metallic=0.0, roughness=0.15, emissive=(2.4, 0.25, 0.12))
    m_nav_g = gb.add_material("NavGreen", (0.10, 1.0, 0.28, 1.0), metallic=0.0, roughness=0.15, emissive=(0.18, 2.4, 0.45))
    m_strobe = gb.add_material("StrobeWhite", (1.0, 1.0, 1.0, 1.0), metallic=0.0, roughness=0.12, emissive=(3.2, 3.3, 3.6))
    m_beacon = gb.add_material("BeaconRed", (1.0, 0.12, 0.06, 1.0), metallic=0.0, roughness=0.14, emissive=(2.8, 0.20, 0.10))
    m_land = gb.add_material("LandingLight", (1.0, 0.96, 0.85, 1.0), metallic=0.0, roughness=0.10, emissive=(2.6, 2.4, 1.9))

    # Load both artist meshes & rotate into engine frame (-Z nose, +X right, +Y up)
    p0_prim = g["meshes"][0]["primitives"][0]
    pos0 = _rot_aurel_to_engine(_read_acc(data, g, bin_off, p0_prim["attributes"]["POSITION"]))
    nrm0 = _rot_aurel_to_engine(_read_acc(data, g, bin_off, p0_prim["attributes"]["NORMAL"]))
    uv0 = _read_acc(data, g, bin_off, p0_prim["attributes"]["TEXCOORD_0"])
    idx0 = _read_acc(data, g, bin_off, p0_prim["indices"]).reshape(-1, 3).astype(np.int32)

    p1_prim = g["meshes"][1]["primitives"][0]
    pos1 = _rot_aurel_to_engine(_read_acc(data, g, bin_off, p1_prim["attributes"]["POSITION"]))
    nrm1 = _rot_aurel_to_engine(_read_acc(data, g, bin_off, p1_prim["attributes"]["NORMAL"]))
    uv1 = _read_acc(data, g, bin_off, p1_prim["attributes"]["TEXCOORD_0"])
    idx1 = _read_acc(data, g, bin_off, p1_prim["indices"]).reshape(-1, 3).astype(np.int32)

    # Split Mesh 0 into static cabin/gear/details vs animated PropHub (spinner) + PropBlades!
    tri_c0 = pos0[idx0].mean(axis=1)
    is_prop = tri_c0[:, 2] < -3.70
    prop_tris = idx0[is_prop]
    static0_tris = idx0[~is_prop]

    # Exact propeller rotation axis center from spinner cone
    prop_verts = pos0[prop_tris].reshape(-1, 3)
    hub_cx = float(np.median(prop_verts[:, 0]))
    hub_cy = float(0.5 * (prop_verts[:, 1].min() + prop_verts[:, 1].max()))
    hub_cz = float(np.percentile(prop_verts[:, 2], 45))
    hub_pivot = (hub_cx, hub_cy, hub_cz)

    prop_rc = np.hypot(tri_c0[is_prop, 0] - hub_cx, tri_c0[is_prop, 1] - hub_cy)
    spin_tris = prop_tris[prop_rc < 0.165]
    blade_tris = prop_tris[prop_rc >= 0.165]
    assert len(spin_tris) > 10 and len(blade_tris) > 10, f"Unexpected prop split: spin={len(spin_tris)} blades={len(blade_tris)}"

    # Main Fuselage + Airframe node (both artist meshes cleanly merged with their PBR materials!)
    sub_static0 = _extract_submesh(pos0, nrm0, uv0, static0_tris)
    sub_static0["mat"] = m_int
    sub_ext1 = _extract_submesh(pos1, nrm1, uv1, idx1)
    sub_ext1["mat"] = m_ext
    gb.add_node("Fuselage", gb.add_mesh("Fuselage", [sub_ext1, sub_static0]))

    # Animated PropHub (spinner) + child PropBlades + soft spinning PropBlurDisc!
    sub_spin = _extract_submesh(pos0, nrm0, uv0, spin_tris, pivot=hub_pivot)
    sub_spin["mat"] = m_int
    hub_node = gb.add_node("PropHub", gb.add_mesh("PropHub", [sub_spin]), translation=hub_pivot)

    sub_blades = _extract_submesh(pos0, nrm0, uv0, blade_tris, pivot=hub_pivot)
    sub_blades["mat"] = m_int
    gb.add_node("PropBlades", gb.add_mesh("PropBlades", [sub_blades]), parent=hub_node)

    blur_geo = GeoPart()
    r_blur = 0.90
    blur_geo.add_quad((-r_blur, -r_blur, 0.0), (r_blur, -r_blur, 0.0), (r_blur, r_blur, 0.0), (-r_blur, r_blur, 0.0), (0, 1), (1, 1), (1, 0), (0, 0), (0, 0, -1))
    gb.add_node("PropBlurDisc", gb.add_mesh("PropBlurDisc", [blur_geo.to_prim(m_blur)]), translation=(hub_cx, hub_cy, hub_cz - 0.03))

    # Exact wingtip, tail-top & leading-edge light positions derived straight from artist mesh!
    wt_r = pos1[pos1[:, 0] > 5.35].mean(axis=0)
    wt_l = pos1[pos1[:, 0] < -5.35].mean(axis=0)
    tail_top = pos1[np.argmax(pos1[:, 1])]

    def add_light_node(name: str, mat: int, centers: list[tuple[float, float, float]], r: float) -> None:
        g_l = GeoPart()
        for c in centers:
            g_l.add_box(c, (r, r, r))
        gb.add_node(name, gb.add_mesh(name, [g_l.to_prim(mat)]))

    add_light_node("NavLightRed",   m_nav_r,  [(float(wt_l[0]) - 0.02, float(wt_l[1]), float(wt_l[2]) - 0.25)], 0.058)
    add_light_node("NavLightGreen", m_nav_g,  [(float(wt_r[0]) + 0.02, float(wt_r[1]), float(wt_r[2]) - 0.25)], 0.058)
    add_light_node("StrobeWhite",   m_strobe, [(float(wt_l[0]) - 0.03, float(wt_l[1]), float(wt_l[2]) - 0.08),
                                               (float(wt_r[0]) + 0.03, float(wt_r[1]), float(wt_r[2]) - 0.08)], 0.064)
    add_light_node("BeaconRed",     m_beacon, [(float(tail_top[0]), float(tail_top[1]) + 0.025, float(tail_top[2]) + 0.06)], 0.052)
    add_light_node("LandingLight",  m_land,   [(-2.25, float(wt_l[1]) - 0.04, float(wt_l[2]) - 0.58)], 0.085)

    gb.save(OUT_GLB)
    nn, nm, npr, nt = verify_glb(OUT_GLB, ["Fuselage", "PropHub", "PropBlades", "PropBlurDisc", "NavLightRed", "NavLightGreen", "StrobeWhite", "BeaconRed", "LandingLight"])
    print(f"[OK] Rigged CC BY 4.0 Cessna 172 -> {OUT_GLB.relative_to(ROOT)} | {OUT_GLB.stat().st_size // 1024} KB | nodes={nn} surfs={npr} tris={nt}")
    print(f"     PropHub pivot={tuple(round(x,3) for x in hub_pivot)} | spin_tris={len(spin_tris)} blade_tris={len(blade_tris)}")
    print(f"     Wingtips L={tuple(round(float(x),2) for x in wt_l)} R={tuple(round(float(x),2) for x in wt_r)} TailTop={tuple(round(float(x),2) for x in tail_top)}")


if __name__ == "__main__":
    main()
