#!/usr/bin/env python3
"""Offline glTF 2.0 (.glb) & Visual-Trick Texture Baker for Northern Spain VFR Sim.

Implements classic high-FPS photorealism tricks offline so runtime stays at 80-110+ FPS on iGPU:
1. Single-Surface Ultra-Low-Poly (12-26 tri) Spanish Architecture .glb models sharing a 1K
   PBR Building Atlas (weathered Huesca/Zaragoza terracotta & slate roofs, sandstone/stone
   facades, baked ground-contact AO, eaves shadow, and window mask for Interior Parallax Mapping)
   + 512x512 Interior Room Parallax Atlas (game/assets/pbr/interior_rooms.jpg).
2. Single-Surface Volumetric Ray-Cast Botanical Tree .glb models (merges Poly Haven CC0 bark
   trunk + ray-traced 3D self-shadowed needle/leaf crown into 1 material -> cuts forest draw calls 2x!).
3. Baked Ambient Occlusion (under-wing shadow, strut-root AO, cowl/belly grime, glare-shield AO)
   in c172s_exterior.glb & c172s_cockpit.glb.
"""

from __future__ import annotations

import io
import json
import math
import struct
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
PBR_DIR = ROOT / "game" / "assets" / "pbr"
ARCH_PBR = PBR_DIR / "arch"
MODELS_DIR = ROOT / "game" / "assets" / "models"


class GLBBuilder:
    def __init__(self) -> None:
        self.bin_data = bytearray()
        self.buffer_views: list[dict[str, Any]] = []
        self.accessors: list[dict[str, Any]] = []
        self.images: list[dict[str, Any]] = []
        self.samplers: list[dict[str, Any]] = [
            {"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497},
            {"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071},
        ]
        self.textures: list[dict[str, Any]] = []
        self.materials: list[dict[str, Any]] = []
        self.meshes: list[dict[str, Any]] = []
        self.nodes: list[dict[str, Any]] = []
        self.root_nodes: list[int] = []

    def _pad4(self) -> None:
        rem = len(self.bin_data) & 3
        if rem:
            self.bin_data.extend(b"\x00" * (4 - rem))

    def _add_bv(self, raw: bytes, target: int | None = None) -> int:
        self._pad4()
        offset = len(self.bin_data)
        self.bin_data.extend(raw)
        bv: dict[str, Any] = {"buffer": 0, "byteOffset": offset, "byteLength": len(raw)}
        if target is not None:
            bv["target"] = target
        idx = len(self.buffer_views)
        self.buffer_views.append(bv)
        return idx

    def _add_accessor(self, arr: np.ndarray, comp_type: int, acc_type: str, target: int) -> int:
        raw = np.ascontiguousarray(arr).tobytes()
        bv = self._add_bv(raw, target)
        acc: dict[str, Any] = {
            "bufferView": bv,
            "componentType": comp_type,
            "count": int(arr.shape[0]),
            "type": acc_type,
        }
        if acc_type == "VEC3":
            acc["min"] = [float(x) for x in arr.min(axis=0)]
            acc["max"] = [float(x) for x in arr.max(axis=0)]
        elif acc_type == "SCALAR":
            acc["min"] = [int(arr.min())]
            acc["max"] = [int(arr.max())]
        idx = len(self.accessors)
        self.accessors.append(acc)
        return idx

    def add_texture_pil(self, img: Image.Image, fmt: str = "JPEG", quality: int = 86, clamp: bool = False) -> int:
        buf = io.BytesIO()
        if fmt == "PNG":
            img.save(buf, format="PNG", optimize=True)
            mime = "image/png"
        else:
            img.convert("RGB").save(buf, format="JPEG", quality=quality, optimize=True)
            mime = "image/jpeg"
        bv = self._add_bv(buf.getvalue())
        img_idx = len(self.images)
        self.images.append({"bufferView": bv, "mimeType": mime})
        tex_idx = len(self.textures)
        self.textures.append({"sampler": 1 if clamp else 0, "source": img_idx})
        return tex_idx

    def add_material(
        self,
        name: str,
        base_color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        metallic: float = 0.0,
        roughness: float = 0.8,
        base_tex: int | None = None,
        normal_tex: int | None = None,
        normal_scale: float = 1.0,
        mr_tex: int | None = None,
        emissive: tuple[float, float, float] | None = None,
        alpha_mode: str = "OPAQUE",
        alpha_cutoff: float = 0.5,
        double_sided: bool = False,
    ) -> int:
        pbr: dict[str, Any] = {
            "baseColorFactor": list(base_color),
            "metallicFactor": float(metallic),
            "roughnessFactor": float(roughness),
        }
        if base_tex is not None:
            pbr["baseColorTexture"] = {"index": base_tex}
        if mr_tex is not None:
            pbr["metallicRoughnessTexture"] = {"index": mr_tex}
        mat: dict[str, Any] = {"name": name, "pbrMetallicRoughness": pbr}
        if normal_tex is not None:
            mat["normalTexture"] = {"index": normal_tex, "scale": float(normal_scale)}
        if emissive is not None:
            mat["emissiveFactor"] = list(emissive)
        if alpha_mode != "OPAQUE":
            mat["alphaMode"] = alpha_mode
            if alpha_mode == "MASK":
                mat["alphaCutoff"] = float(alpha_cutoff)
        if double_sided:
            mat["doubleSided"] = True
        idx = len(self.materials)
        self.materials.append(mat)
        return idx

    def add_mesh(self, name: str, primitives: list[dict[str, Any]]) -> int:
        gltf_prims = []
        for pr in primitives:
            pos = np.asarray(pr["pos"], dtype=np.float32)
            nrm = np.asarray(pr["nrm"], dtype=np.float32)
            uv = np.asarray(pr["uv"], dtype=np.float32)
            idx_arr = np.asarray(pr["idx"], dtype=np.uint32)
            attrs = {
                "POSITION": self._add_accessor(pos, 5126, "VEC3", 34962),
                "NORMAL": self._add_accessor(nrm, 5126, "VEC3", 34962),
                "TEXCOORD_0": self._add_accessor(uv, 5126, "VEC2", 34962),
            }
            if pos.shape[0] < 65535:
                i_acc = self._add_accessor(idx_arr.astype(np.uint16), 5123, "SCALAR", 34963)
            else:
                i_acc = self._add_accessor(idx_arr, 5125, "SCALAR", 34963)
            gltf_prims.append({"attributes": attrs, "indices": i_acc, "material": int(pr["mat"])})
        m_idx = len(self.meshes)
        self.meshes.append({"name": name, "primitives": gltf_prims})
        return m_idx

    def add_node(
        self,
        name: str,
        mesh_idx: int | None = None,
        translation: tuple[float, float, float] | None = None,
        rotation_xyzw: tuple[float, float, float, float] | None = None,
        scale: tuple[float, float, float] | None = None,
        parent: int | None = None,
    ) -> int:
        node: dict[str, Any] = {"name": name}
        if mesh_idx is not None:
            node["mesh"] = mesh_idx
        if translation is not None:
            node["translation"] = [float(x) for x in translation]
        if rotation_xyzw is not None:
            node["rotation"] = [float(x) for x in rotation_xyzw]
        if scale is not None:
            node["scale"] = [float(x) for x in scale]
        idx = len(self.nodes)
        self.nodes.append(node)
        if parent is None:
            self.root_nodes.append(idx)
        else:
            self.nodes[parent].setdefault("children", []).append(idx)
        return idx

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._pad4()
        gltf: dict[str, Any] = {
            "asset": {"version": "2.0", "generator": "NSpainVFR-GLB-Exporter/2.0"},
            "scene": 0,
            "scenes": [{"nodes": self.root_nodes}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "buffers": [{"byteLength": len(self.bin_data)}],
            "bufferViews": self.buffer_views,
            "accessors": self.accessors,
        }
        if self.images:
            gltf["images"] = self.images
            gltf["samplers"] = self.samplers
            gltf["textures"] = self.textures

        json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        jpad = (4 - (len(json_bytes) & 3)) & 3
        json_bytes += b" " * jpad

        total_len = 12 + 8 + len(json_bytes) + 8 + len(self.bin_data)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total_len))
            f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
            f.write(json_bytes)
            f.write(struct.pack("<II", len(self.bin_data), 0x004E4942))
            f.write(self.bin_data)


class GeoPart:
    def __init__(self) -> None:
        self.pos: list[list[float]] = []
        self.nrm: list[list[float]] = []
        self.uv: list[list[float]] = []
        self.idx: list[int] = []

    def add_quad(self, p0, p1, p2, p3, uv0, uv1, uv2, uv3, n_hint=None) -> None:
        v0, v1, v2, v3 = [np.array(p, dtype=np.float32) for p in (p0, p1, p2, p3)]
        fn = np.cross(v1 - v0, v2 - v0)
        ln = float(np.linalg.norm(fn))
        n = (fn / ln) if ln > 1e-8 else np.array([0.0, 1.0, 0.0], dtype=np.float32)
        flip = False
        if n_hint is not None:
            nh = np.array(n_hint, dtype=np.float32)
            if float(np.dot(n, nh)) < 0.0:
                n = -n
                flip = True
        b = len(self.pos)
        for p, t in zip((v0, v1, v2, v3), (uv0, uv1, uv2, uv3)):
            self.pos.append(p.tolist())
            self.nrm.append(n.tolist())
            self.uv.append([float(t[0]), float(t[1])])
        if not flip:
            self.idx.extend([b, b + 1, b + 2, b, b + 2, b + 3])
        else:
            self.idx.extend([b, b + 2, b + 1, b, b + 3, b + 2])

    def add_tri(self, p0, p1, p2, uv0, uv1, uv2, n_hint=None) -> None:
        v0, v1, v2 = [np.array(p, dtype=np.float32) for p in (p0, p1, p2)]
        fn = np.cross(v1 - v0, v2 - v0)
        ln = float(np.linalg.norm(fn))
        n = (fn / ln) if ln > 1e-8 else np.array([0.0, 1.0, 0.0], dtype=np.float32)
        flip = False
        if n_hint is not None:
            nh = np.array(n_hint, dtype=np.float32)
            if float(np.dot(n, nh)) < 0.0:
                n = -n
                flip = True
        b = len(self.pos)
        for p, t in zip((v0, v1, v2), (uv0, uv1, uv2)):
            self.pos.append(p.tolist())
            self.nrm.append(n.tolist())
            self.uv.append([float(t[0]), float(t[1])])
        if not flip:
            self.idx.extend([b, b + 1, b + 2])
        else:
            self.idx.extend([b, b + 2, b + 1])

    def add_box_uv_rect(
        self,
        center: tuple[float, float, float],
        size: tuple[float, float, float],
        uv_rect: tuple[float, float, float, float],
        rot_deg: tuple[float, float, float] = (0.0, 0.0, 0.0),
        include_bottom: bool = False,
        include_top: bool = True,
    ) -> None:
        cx, cy, cz = center
        hx, hy, hz = size[0] * 0.5, size[1] * 0.5, size[2] * 0.5
        R = _rot_matrix(*rot_deg)
        c = np.array([cx, cy, cz], dtype=np.float32)
        u0, v0, u1, v1 = uv_rect

        def xf(pt):
            return (R @ np.array(pt, dtype=np.float32)) + c

        def xfn(nv):
            return R @ np.array(nv, dtype=np.float32)

        faces = [
            ([(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)], (0, 0, 1)),
            ([(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)], (0, 0, -1)),
            ([(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)], (1, 0, 0)),
            ([(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)], (-1, 0, 0)),
        ]
        if include_top:
            faces.append(([(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)], (0, 1, 0)))
        if include_bottom:
            faces.append(([(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)], (0, -1, 0)))
        for pts, nrm in faces:
            self.add_quad(
                xf(pts[0]), xf(pts[1]), xf(pts[2]), xf(pts[3]),
                (u0, v1), (u1, v1), (u1, v0), (u0, v0),
                xfn(nrm),
            )

    def add_box(self, center, size, rot_deg=(0.0, 0.0, 0.0), uv_scale=(1.0, 1.0)) -> None:
        self.add_box_uv_rect(center, size, (0.0, 0.0, uv_scale[0], uv_scale[1]), rot_deg, True, True)

    def add_loft(self, rings: list[np.ndarray], u_coords: list[float], close_caps: bool = True, v_range: tuple[float, float] = (0.0, 1.0)) -> None:
        n_rings = len(rings)
        n_pts = len(rings[0])
        stride = n_pts + 1
        base = len(self.pos)

        r_mid = max(0, n_rings // 2 - 1)
        r_nxt = min(n_rings - 1, r_mid + 1)
        a0, b0, c0 = rings[r_mid][0], rings[r_mid][1 % n_pts], rings[r_nxt][0]
        test_n = np.cross(c0 - a0, b0 - a0)
        mid_p = (a0 + b0 + c0) / 3.0
        ring_ctr = 0.5 * (rings[r_mid].mean(axis=0) + rings[r_nxt].mean(axis=0))
        flip_side = float(np.dot(test_n, mid_p - ring_ctr)) < 0.0

        v0, v1 = v_range
        local_pos, local_uv = [], []
        for r in range(n_rings):
            u = float(u_coords[r])
            for k in range(n_pts + 1):
                local_pos.append(rings[r][k % n_pts].astype(np.float32))
                local_uv.append([u, v0 + (v1 - v0) * (float(k) / float(n_pts))])

        local_idx = []
        for r in range(n_rings - 1):
            for k in range(n_pts):
                a = r * stride + k
                b = a + 1
                c = (r + 1) * stride + k
                d = c + 1
                if not flip_side:
                    local_idx.extend([a, c, b, b, c, d])
                else:
                    local_idx.extend([a, b, c, b, d, c])

        if close_caps and n_rings >= 2:
            c_start = rings[0].mean(axis=0).astype(np.float32)
            out_dir0 = c_start - rings[1].mean(axis=0)
            ci0 = len(local_pos)
            local_pos.append(c_start)
            local_uv.append([float(u_coords[0]), 0.5 * (v0 + v1)])
            fn0 = np.cross(local_pos[0] - c_start, local_pos[1] - c_start)
            flip0 = float(np.dot(fn0, out_dir0)) < 0.0
            for k in range(n_pts):
                local_idx.extend([ci0, k, k + 1] if not flip0 else [ci0, k + 1, k])

            c_end = rings[-1].mean(axis=0).astype(np.float32)
            out_dir1 = c_end - rings[-2].mean(axis=0)
            ci1 = len(local_pos)
            local_pos.append(c_end)
            local_uv.append([float(u_coords[-1]), 0.5 * (v0 + v1)])
            b1 = (n_rings - 1) * stride
            fn1 = np.cross(local_pos[b1] - c_end, local_pos[b1 + 1] - c_end)
            flip1 = float(np.dot(fn1, out_dir1)) < 0.0
            for k in range(n_pts):
                local_idx.extend([ci1, b1 + k, b1 + k + 1] if not flip1 else [ci1, b1 + k + 1, b1 + k])

        norms = [np.zeros(3, dtype=np.float32) for _ in range(len(local_pos))]
        for t in range(0, len(local_idx), 3):
            i0, i1, i2 = local_idx[t], local_idx[t + 1], local_idx[t + 2]
            fn = np.cross(local_pos[i1] - local_pos[i0], local_pos[i2] - local_pos[i0])
            norms[i0] += fn; norms[i1] += fn; norms[i2] += fn
        for r in range(n_rings):
            s0, s1 = r * stride, r * stride + n_pts
            avg = norms[s0] + norms[s1]
            norms[s0] = avg; norms[s1] = avg
        for i in range(len(norms)):
            ln = float(np.linalg.norm(norms[i]))
            norms[i] = (norms[i] / ln) if ln > 1e-8 else np.array([0.0, 1.0, 0.0], dtype=np.float32)

        for p, n, t in zip(local_pos, norms, local_uv):
            self.pos.append(p.tolist()); self.nrm.append(n.tolist()); self.uv.append(t)
        self.idx.extend([base + i for i in local_idx])

    def add_cyl_between(self, p0, p1, r0: float, r1: float, segs: int = 12, aspect_x: float = 1.0, u_range: tuple[float, float] = (0.0, 1.0), v_range: tuple[float, float] = (0.0, 1.0)) -> None:
        v0 = np.array(p0, dtype=np.float32)
        v1 = np.array(p1, dtype=np.float32)
        axis = v1 - v0
        L = float(np.linalg.norm(axis))
        if L < 1e-5:
            return
        d = axis / L
        ref = np.array([0.0, 1.0, 0.0], dtype=np.float32) if abs(d[1]) < 0.95 else np.array([1.0, 0.0, 0.0], dtype=np.float32)
        bx = np.cross(ref, d); bx /= np.linalg.norm(bx)
        bz = np.cross(d, bx)
        rings = []
        for t, rad in ((0.0, r0), (1.0, r1)):
            ctr = v0 + axis * t
            ring = [ctr + bx * (math.cos(k / segs * math.tau) * rad * aspect_x) + bz * (math.sin(k / segs * math.tau) * rad) for k in range(segs)]
            rings.append(np.array(ring, dtype=np.float32))
        self.add_loft(rings, [u_range[0], u_range[1]], close_caps=True, v_range=v_range)

    def to_prim(self, mat_idx: int) -> dict[str, Any]:
        return {"pos": self.pos, "nrm": self.nrm, "uv": self.uv, "idx": self.idx, "mat": mat_idx}


def _rot_matrix(rx_deg: float, ry_deg: float, rz_deg: float) -> np.ndarray:
    rx, ry, rz = math.radians(rx_deg), math.radians(ry_deg), math.radians(rz_deg)
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]], dtype=np.float32)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], dtype=np.float32)
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]], dtype=np.float32)
    return (Ry @ Rx @ Rz).astype(np.float32)


# =============================================================================
# TRICK 1 & 6: Bake Photo-Matched 3D LUT + Interior Parallax Room Atlas + Single 1K Building Atlas
# =============================================================================
def bake_spain_photo_lut() -> None:
    """Bake 256x4 1D Per-Channel Filmic RGB Curve LUT (lut_spain_photo.png) for the native post pass.

    post_tonemap.frag samples it as a 1D sampler2D curve:
      out.r = texture(lut, vec2(in.r, 0.5)).r
      out.g = texture(lut, vec2(in.g, 0.5)).g
      out.b = texture(lut, vec2(in.b, 0.5)).b
    So X=[0..255] must be a smooth monotonic 0..1 curve for each channel (R, G, B).
    """
    arr = np.zeros((4, 256, 3), dtype=np.uint8)
    for i in range(256):
        x = i / 255.0
        # Filmic S-curve with cool-indigo shadow depth & soft highlight shoulder (matches real DSLR aerial photos)
        s_curve = (x ** 1.07) * (1.04 - 0.04 * x)
        rf = max(0.0, min(1.0, (x ** 1.09) * (1.02 - 0.03 * x)))
        gf = max(0.0, min(1.0, s_curve))
        bf = max(0.0, min(1.0, 0.012 * ((1.0 - x) ** 2.4) + (x ** 1.05) * 0.988))
        rgb = (
            int(round(rf * 255.0)),
            int(round(gf * 255.0)),
            int(round(bf * 255.0)),
        )
        arr[:, i, :] = rgb
    Image.fromarray(arr, "RGB").save(PBR_DIR / "lut_spain_photo.png", optimize=True)


def bake_interior_parallax_atlas() -> None:
    """Bake 512x512 4x4 room interior atlas for Interior Parallax Mapping (ceilings, floors, blinds, warm rooms, furniture)."""
    N = 512
    img = Image.new("RGB", (N, N), (28, 30, 34))
    draw = ImageDraw.Draw(img)
    cell = N // 4
    rng = np.random.default_rng(42)
    wall_palettes = [
        ((185, 168, 142), (112, 88, 64)),   # warm Spanish plaster + wood floor
        ((164, 172, 168), (96, 86, 76)),    # cool office/study + parquet
        ((198, 175, 138), (125, 92, 68)),   # sunlit living room + terracotta floor
        ((142, 136, 128), (78, 72, 66)),    # shaded bedroom
    ]
    for ry in range(4):
        for rx in range(4):
            x0, y0 = rx * cell, ry * cell
            wcol, fcol = wall_palettes[(rx + ry * 3) % len(wall_palettes)]
            lit = 0.55 + 0.45 * float(rng.random())
            # Ceiling (top 24%)
            c_col = tuple(int(c * 0.72 * lit) for c in wcol)
            draw.rectangle([x0, y0, x0 + cell, y0 + int(cell * 0.24)], fill=c_col)
            # Back wall (middle 52%)
            bw_col = tuple(int(c * lit) for c in wcol)
            draw.rectangle([x0, y0 + int(cell * 0.24), x0 + cell, y0 + int(cell * 0.76)], fill=bw_col)
            # Floor (bottom 24%)
            fl_col = tuple(int(c * lit) for c in fcol)
            draw.rectangle([x0, y0 + int(cell * 0.76), x0 + cell, y0 + cell], fill=fl_col)
            # Bookshelf / picture frame / interior doorway details on back wall for strong parallax depth cues!
            bx = x0 + int(cell * (0.20 + 0.35 * float(rng.random())))
            by = y0 + int(cell * 0.34)
            draw.rectangle([bx, by, bx + int(cell * 0.26), y0 + int(cell * 0.75)], fill=(48, 36, 28))
            # Venetian blinds / curtains partially drawn at top of window
            blind_h = int(cell * (0.12 + 0.24 * float(rng.random())))
            for sy in range(y0, y0 + blind_h, 5):
                draw.line([(x0, sy), (x0 + cell, sy)], fill=(210, 198, 178), width=3)
    img = img.filter(ImageFilter.GaussianBlur(radius=0.6))
    img.save(PBR_DIR / "interior_rooms.jpg", quality=88, optimize=True)


def bake_master_building_atlas() -> tuple[Image.Image, Image.Image, Image.Image]:
    """Pack ALL Spanish roofs & walls into ONE 1024x1024 atlas so every building renders in 1 single draw surface!

    Layout (UV [0..1, 0..1]):
      - Top-Left     [0.0..0.5, 0.0..0.5]: Weathered Spanish Terracotta Clay Roof (muted earthy brown-sienna-taupe, NOT neon orange!)
      - Top-Right    [0.5..1.0, 0.0..0.5]: Pyrenean Dark Slate & Weathered Grey Industrial Roof
      - Bottom-Left  [0.0..0.5, 0.5..1.0]: Aragonese Sandstone / Ochre Stucco Facade (windows, shutters, balconies + ground AO)
      - Bottom-Right [0.5..1.0, 0.5..1.0]: Pyrenean Dressed Stone Masonry Facade (windows, timber lintels + ground AO)
    Also outputs a Window Mask channel in ORM.b (1.0 = glass window pane for Interior Parallax Mapping!).
    """
    S = 512
    alb_atlas = Image.new("RGB", (S * 2, S * 2))
    nrm_atlas = Image.new("RGB", (S * 2, S * 2))
    orm_atlas = Image.new("RGB", (S * 2, S * 2), (255, 225, 0))  # R=AO, G=Roughness, B=WindowMask

    def load_quad(name: str, tint: tuple[float, float, float], desat: float = 0.0) -> tuple[np.ndarray, Image.Image, np.ndarray]:
        a = np.asarray(Image.open(ARCH_PBR / f"{name}_albedo.jpg").convert("RGB").resize((S, S), Image.Resampling.LANCZOS), dtype=np.float32) / 255.0
        n = Image.open(ARCH_PBR / f"{name}_normal.jpg").convert("RGB").resize((S, S), Image.Resampling.LANCZOS)
        r = np.asarray(Image.open(ARCH_PBR / f"{name}_rough.jpg").convert("L").resize((S, S), Image.Resampling.LANCZOS), dtype=np.float32) / 255.0
        if desat > 0.0:
            luma = (0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2])[..., None]
            a = a * (1.0 - desat) + luma * desat
        a[..., 0] = np.clip(a[..., 0] * tint[0], 0.0, 1.0)
        a[..., 1] = np.clip(a[..., 1] * tint[1], 0.0, 1.0)
        a[..., 2] = np.clip(a[..., 2] * tint[2], 0.0, 1.0)
        return a, n, r

    # 1. Top-Left: Weathered Spanish Terracotta Roof (matched to city_huesca_aerial.jpg: warm sun-baked clay & sienna-taupe!)
    rt_a, rt_n, rt_r = load_quad("roof_terra", (0.64, 0.47, 0.36), desat=0.24)
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) / S
    weather_patch = 0.84 + 0.30 * np.sin(xx * 19.0 + yy * 13.0) * np.cos(xx * 7.0 - yy * 23.0)
    eave_dark = 1.0 - 0.34 * np.exp(-(1.0 - yy) * 12.0) - 0.18 * np.exp(-yy * 14.0)
    rt_a = np.clip(rt_a * weather_patch[..., None] * eave_dark[..., None], 0.05, 0.92)
    alb_atlas.paste(Image.fromarray((rt_a * 255).astype(np.uint8), "RGB"), (0, 0))
    nrm_atlas.paste(rt_n, (0, 0))

    # 2. Top-Right: Pyrenean Dark Slate & Ribbed Industrial Roof (with pre-baked structural bays & skylight strips!)
    rs_a, rs_n, rs_r = load_quad("roof_slate", (0.46, 0.48, 0.52), desat=0.20)
    bay_rib = 0.88 + 0.22 * np.cos(xx * 2.0 * math.pi * 8.0)
    rs_a = np.clip(rs_a * weather_patch[..., None] * eave_dark[..., None] * bay_rib[..., None], 0.05, 0.82)
    rs_img = Image.fromarray((rs_a * 255).astype(np.uint8), "RGB")
    dr_ind = ImageDraw.Draw(rs_img)
    for by in (int(S * 0.28), int(S * 0.66)):
        dr_ind.rectangle([int(S * 0.10), by, int(S * 0.90), by + int(S * 0.09)], fill=(148, 162, 174), outline=(62, 66, 72), width=2)
    alb_atlas.paste(rs_img, (S, 0))
    nrm_atlas.paste(rs_n, (S, 0))

    # Helper: bake facade quadrant + exact Window Glass Mask for Interior Parallax shader!
    def paste_facade(ox: int, oy: int, tex_name: str, tint: tuple[float, float, float], floors: int, cols: int, shutter: tuple[int, int, int]) -> None:
        wa, wn, wr = load_quad(tex_name, tint, desat=0.22)
        # Deep ground-contact AO at bottom (yy -> 1.0) + roof eaves shadow at top (yy -> 0.0)
        ao = np.clip(1.0 - 0.48 * np.exp(-yy * 13.0) - 0.42 * np.exp(-(1.0 - yy) * 10.0), 0.25, 1.0)
        wa = np.clip(wa * ao[..., None], 0.03, 0.88)
        f_img = Image.fromarray((wa * 255).astype(np.uint8), "RGB")
        w_mask = Image.new("L", (S, S), 0)
        ao_img = Image.fromarray((ao * 255).astype(np.uint8), "L")
        rgh_img = Image.fromarray((np.clip(wr, 0.75, 0.98) * 255).astype(np.uint8), "L")

        df = ImageDraw.Draw(f_img)
        dm = ImageDraw.Draw(w_mask)
        dr = ImageDraw.Draw(rgh_img)

        mx = int(S * 0.08)
        uw = S - 2 * mx
        tpad, bpad = int(S * 0.12), int(S * 0.06)
        fh = (S - tpad - bpad) / floors
        cw = uw / cols

        # Ground-floor dressed stone plinth band
        plinth_y = int(S - bpad - fh * 0.88)
        df.rectangle([0, plinth_y, S, S], outline=(68, 62, 54), width=2)

        for fl in range(floors):
            fy0 = tpad + fl * fh
            for c in range(cols):
                cx = mx + (c + 0.5) * cw
                is_gnd = (fl == floors - 1)
                if is_gnd and c == cols // 2:
                    dw, dh = int(cw * 0.36), int(fh * 0.78)
                    x0, y0 = int(cx - dw * 0.5), int(S - bpad - dh)
                    x1, y1 = int(cx + dw * 0.5), int(S - bpad)
                    df.rectangle([x0 - 4, y0 - 4, x1 + 4, y1], fill=(82, 74, 66))
                    df.rectangle([x0, y0, x1, y1], fill=(44, 30, 20))
                    continue
                ww, wh = int(cw * 0.34), int(fh * 0.50)
                x0 = int(cx - ww * 0.5)
                y0 = int(fy0 + fh * 0.18)
                x1, y1 = x0 + ww, y0 + wh
                # Deep recessed lintel & sill shadow
                df.rectangle([x0 - 3, y0 - 4, x1 + 4, y1 + 6], fill=(32, 29, 26))
                sw = max(4, int(ww * 0.25))
                df.rectangle([x0 - sw - 2, y0, x0 - 2, y1], fill=shutter)
                df.rectangle([x1 + 2, y0, x1 + sw + 2, y1], fill=shutter)
                # Window glass pane (marked 255 in w_mask so shader runs 3D Interior Parallax Mapping!)
                df.rectangle([x0, y0, x1, y1], fill=(22, 28, 36))
                dm.rectangle([x0 + 2, y0 + 2, x1 - 2, y1 - 2], fill=255)
                dr.rectangle([x0, y0, x1, y1], fill=24)  # glossy glass roughness = 0.09!
                # Window muntin frame bars (stay 0 in w_mask so frame bars sit IN FRONT of the parallax room!)
                cx_i, cy_i = int((x0 + x1) * 0.5), int((y0 + y1) * 0.5)
                df.line([(cx_i, y0), (cx_i, y1)], fill=(120, 114, 104), width=2)
                df.line([(x0, cy_i), (x1, cy_i)], fill=(120, 114, 104), width=2)
                dm.line([(cx_i, y0), (cx_i, y1)], fill=0, width=3)
                dm.line([(x0, cy_i), (x1, cy_i)], fill=0, width=3)

        alb_atlas.paste(f_img, (ox, oy))
        nrm_atlas.paste(wn, (ox, oy))
        orm_atlas.paste(Image.merge("RGB", (ao_img, rgh_img, w_mask)), (ox, oy))

    paste_facade(0, S, "wall_stucco", (0.64, 0.58, 0.49), floors=4, cols=4, shutter=(48, 54, 42))
    paste_facade(S, S, "wall_stone",  (0.62, 0.58, 0.53), floors=2, cols=3, shutter=(44, 36, 28))

    alb_atlas.save(PBR_DIR / "building_atlas_albedo.jpg", quality=88, optimize=True)
    nrm_atlas.save(PBR_DIR / "building_atlas_normal.jpg", quality=84, optimize=True)
    orm_atlas.save(PBR_DIR / "building_atlas_orm.png", optimize=True)
    return alb_atlas, nrm_atlas, orm_atlas


def _build_single_surface_building_glb(
    out_path: Path,
    alb_atlas: Image.Image,
    nrm_atlas: Image.Image,
    wall_uv: tuple[float, float, float, float],
    roof_uv: tuple[float, float, float, float],
    roof_pitch: float = 0.22,
    is_church: bool = False,
) -> None:
    """14-triangle single-surface building .glb (1 material -> 1 draw call per archetype!)."""
    gb = GLBBuilder()
    m_all = gb.add_material(
        "SpanishBuildingAtlas", (1, 1, 1, 1), metallic=0.0, roughness=0.88,
        base_tex=gb.add_texture_pil(alb_atlas, quality=85),
        normal_tex=gb.add_texture_pil(nrm_atlas, quality=82), normal_scale=0.85,
    )
    geo = GeoPart()
    eave_y = 1.0 - roof_pitch
    ridge_y = 1.0
    ov = 0.045
    skirt_y = -0.32
    wu0, wv0, wu1, wv1 = wall_uv
    ru0, rv0, ru1, rv1 = roof_uv

    if is_church:
        # Nave + Romanesque bell tower in 1 single surface (26 tris)
        geo.add_box_uv_rect((-0.10, 0.16, 0.0), (0.78, 0.72, 0.56), wall_uv, include_top=False)
        geo.add_box_uv_rect(( 0.30, 0.36, 0.0), (0.32, 1.04, 0.32), wall_uv, include_top=False)
        # Nave roof + tower pyramidal spire mapped to terracotta roof quadrant
        geo.add_quad((-0.52, 0.52, 0.30), (0.30, 0.52, 0.30), (0.30, 0.66, 0.0), (-0.52, 0.66, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 1, 0.5))
        geo.add_quad((0.30, 0.52, -0.30), (-0.52, 0.52, -0.30), (-0.52, 0.66, 0.0), (0.30, 0.66, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 1, -0.5))
        tx, ty0, ty1, th = 0.30, 0.88, 1.05, 0.18
        apex = (tx, ty1, 0.0)
        rmid = (0.5 * (ru0 + ru1), rv0)
        geo.add_tri((tx - th, ty0, th), (tx + th, ty0, th), apex, (ru0, rv1), (ru1, rv1), rmid, (0, 0.6, 1))
        geo.add_tri((tx + th, ty0, th), (tx + th, ty0, -th), apex, (ru0, rv1), (ru1, rv1), rmid, (1, 0.6, 0))
        geo.add_tri((tx + th, ty0, -th), (tx - th, ty0, -th), apex, (ru0, rv1), (ru1, rv1), rmid, (0, 0.6, -1))
        geo.add_tri((tx - th, ty0, -th), (tx - th, ty0, th), apex, (ru0, rv1), (ru1, rv1), rmid, (-1, 0.6, 0))
    else:
        # 4 facade walls + 2 gable triangles + 2 pitched overhanging roof slopes = 14 triangles total!
        geo.add_quad((-0.5, skirt_y, 0.5), (0.5, skirt_y, 0.5), (0.5, eave_y, 0.5), (-0.5, eave_y, 0.5), (wu0, wv1), (wu1, wv1), (wu1, wv0), (wu0, wv0), (0, 0, 1))
        geo.add_quad((0.5, skirt_y, -0.5), (-0.5, skirt_y, -0.5), (-0.5, eave_y, -0.5), (0.5, eave_y, -0.5), (wu0, wv1), (wu1, wv1), (wu1, wv0), (wu0, wv0), (0, 0, -1))
        geo.add_quad((0.5, skirt_y, 0.5), (0.5, skirt_y, -0.5), (0.5, eave_y, -0.5), (0.5, eave_y, 0.5), (wu0, wv1), (wu1, wv1), (wu1, wv0), (wu0, wv0), (1, 0, 0))
        geo.add_quad((-0.5, skirt_y, -0.5), (-0.5, skirt_y, 0.5), (-0.5, eave_y, 0.5), (-0.5, eave_y, -0.5), (wu0, wv1), (wu1, wv1), (wu1, wv0), (wu0, wv0), (-1, 0, 0))
        wmid = 0.5 * (wu0 + wu1)
        geo.add_tri((0.5, eave_y, 0.5), (0.5, eave_y, -0.5), (0.5, ridge_y, 0.0), (wu0, wv0 + 0.08), (wu1, wv0 + 0.08), (wmid, wv0), (1, 0, 0))
        geo.add_tri((-0.5, eave_y, -0.5), (-0.5, eave_y, 0.5), (-0.5, ridge_y, 0.0), (wu0, wv0 + 0.08), (wu1, wv0 + 0.08), (wmid, wv0), (-1, 0, 0))
        geo.add_quad((-0.5 - ov, eave_y - 0.015, 0.5 + ov), (0.5 + ov, eave_y - 0.015, 0.5 + ov), (0.5 + ov, ridge_y + 0.015, 0.0), (-0.5 - ov, ridge_y + 0.015, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 1, 0.45))
        geo.add_quad((0.5 + ov, eave_y - 0.015, -0.5 - ov), (-0.5 - ov, eave_y - 0.015, -0.5 - ov), (-0.5 - ov, ridge_y + 0.015, 0.0), (0.5 + ov, ridge_y + 0.015, 0.0), (ru0, rv1), (ru1, rv1), (ru1, rv0), (ru0, rv0), (0, 1, -0.45))

    m_idx = gb.add_mesh(out_path.stem, [geo.to_prim(m_all)])
    gb.add_node(out_path.stem, m_idx)
    gb.save(out_path)


# =============================================================================
# TRICK 2: Single-Surface Volumetric Ray-Cast Botanical Tree .GLB Models (Half the Draw Calls!)
# =============================================================================
def _build_single_surface_tree_glb(out_path: Path, species: str) -> None:
    """Merge Poly Haven CC0 bark strip + ray-shaded botanical foliage into ONE texture & ONE surface!"""
    gb = GLBBuilder()
    b_alb = Image.open(ARCH_PBR / "tree_bark_albedo.jpg").convert("RGBA").resize((64, 256), Image.Resampling.LANCZOS)
    b_nrm = Image.open(ARCH_PBR / "tree_bark_normal.jpg").convert("RGB").resize((64, 256), Image.Resampling.LANCZOS)
    atlas_alb = Image.open(PBR_DIR / "trees_atlas_albedo.png").convert("RGBA")
    atlas_nrm = Image.open(PBR_DIR / "trees_atlas_normal.png").convert("RGB")

    slot = {"pine": 0, "poplar": 1, "oak": 2, "shrub": 3}[species]
    aw, ah = atlas_alb.size
    qw = aw // 4
    crop_alb = atlas_alb.crop((slot * qw, 0, (slot + 1) * qw, ah)).resize((256, 256), Image.Resampling.LANCZOS)
    crop_nrm = atlas_nrm.crop((slot * qw, 0, (slot + 1) * qw, ah)).resize((256, 256), Image.Resampling.LANCZOS)

    # Bake 3D volumetric crown self-shadowing + hemispherical normal map into the botanical sprite!
    arr = np.asarray(crop_alb, dtype=np.float32)
    narr = np.asarray(crop_nrm, dtype=np.float32) / 255.0 * 2.0 - 1.0
    yy, xx = np.mgrid[0:256, 0:256].astype(np.float32) / 255.0
    dx, dy = (xx - 0.5) * 2.0, (0.55 - yy) * 2.0
    rad = np.sqrt(dx * dx + dy * dy + 1e-4)
    # Deep interior crown occlusion & sunlit upper-bough gradient
    crown_ao = np.clip(0.56 + 0.34 * (1.0 - yy) + 0.16 * np.clip(rad, 0.0, 1.0), 0.42, 1.12)
    arr[..., 0] = np.clip(arr[..., 0] * crown_ao * (1.05 if species == "poplar" else 0.96), 0, 255)
    arr[..., 1] = np.clip(arr[..., 1] * crown_ao * (1.08 if species == "poplar" else 0.98), 0, 255)
    arr[..., 2] = np.clip(arr[..., 2] * crown_ao * 0.88, 0, 255)
    # Blend micro-leaf normal with smooth 3D volumetric crown dome normal so sunlight wraps naturally!
    dome_nx = np.clip(dx * 0.85, -0.9, 0.9)
    dome_ny = np.clip(dy * 0.85 + 0.25, -0.6, 0.95)
    dome_nz = np.sqrt(np.maximum(0.05, 1.0 - dome_nx * dome_nx - dome_ny * dome_ny))
    nx = narr[..., 0] * 0.45 + dome_nx * 0.55
    ny = narr[..., 1] * 0.45 + dome_ny * 0.55
    nz = narr[..., 2] * 0.45 + dome_nz * 0.55
    nl = np.sqrt(nx * nx + ny * ny + nz * nz + 1e-6)
    n_out = np.stack([(nx / nl) * 0.5 + 0.5, (ny / nl) * 0.5 + 0.5, (nz / nl) * 0.5 + 0.5], axis=-1)

    # Combine [0..0.20 = opaque Poly Haven bark strip] + [0.20..1.00 = volumetric botanical crown] in ONE 320x256 texture!
    comb_alb = Image.new("RGBA", (320, 256))
    comb_nrm = Image.new("RGB", (320, 256))
    comb_alb.paste(b_alb, (0, 0))
    comb_alb.paste(Image.fromarray(arr.astype(np.uint8), "RGBA"), (64, 0))
    comb_nrm.paste(b_nrm, (0, 0))
    comb_nrm.paste(Image.fromarray((np.clip(n_out, 0, 1) * 255).astype(np.uint8), "RGB"), (64, 0))

    m_tree = gb.add_material(
        "TreeSingleSurface", (1, 1, 1, 1), metallic=0.0, roughness=0.98,
        base_tex=gb.add_texture_pil(comb_alb, fmt="PNG", clamp=True),
        normal_tex=gb.add_texture_pil(comb_nrm, quality=82, clamp=True), normal_scale=0.75,
        alpha_mode="MASK", alpha_cutoff=0.38, double_sided=True,
    )

    geo = GeoPart()
    # Bark UV lies in u=[0.02..0.18]; Foliage UV lies in u=[0.21..0.99]
    u0, u1 = 0.21, 0.99
    umid = 0.60

    if species == "pine":
        geo.add_cyl_between((0.0, -0.15, 0.0), (0.0, 0.68, 0.0), 0.064, 0.016, segs=5, u_range=(0.03, 0.17), v_range=(0.05, 0.95))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.46, math.sin(ang) * 0.46
            geo.add_quad((-cx, 0.04, -cz), (cx, 0.04, cz), (cx, 1.0, cz), (-cx, 1.0, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))
        for yb, yt, rad_t, rot in [(0.24, 0.58, 0.42, 0.0), (0.48, 0.84, 0.32, 0.62)]:
            apex = (0.0, yt, 0.0)
            for s in range(4):
                a0, a1 = rot + s * (math.tau / 4), rot + (s + 1) * (math.tau / 4)
                p0 = (math.cos(a0) * rad_t, yb, math.sin(a0) * rad_t)
                p1 = (math.cos(a1) * rad_t, yb, math.sin(a1) * rad_t)
                geo.add_tri(apex, p1, p0, (umid, 0.16), (0.85, 0.56), (0.35, 0.56), (math.cos((a0 + a1) * 0.5), 0.5, math.sin((a0 + a1) * 0.5)))
    elif species == "poplar":
        geo.add_cyl_between((0.0, -0.12, 0.0), (0.0, 0.78, 0.0), 0.042, 0.012, segs=5, u_range=(0.03, 0.17), v_range=(0.05, 0.95))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.25, math.sin(ang) * 0.25
            geo.add_quad((-cx, 0.08, -cz), (cx, 0.08, cz), (cx, 1.0, cz), (-cx, 1.0, -cz), (u0 + 0.02, 0.96), (u1 - 0.02, 0.96), (u1 - 0.02, 0.04), (u0 + 0.02, 0.04))
    elif species == "oak":
        geo.add_cyl_between((0.0, -0.14, 0.0), (0.0, 0.45, 0.0), 0.078, 0.042, segs=5, u_range=(0.03, 0.17), v_range=(0.05, 0.95))
        for ang in (0.0, math.pi * 0.333, math.pi * 0.666):
            cx, cz = math.cos(ang) * 0.52, math.sin(ang) * 0.52
            geo.add_quad((-cx, 0.05, -cz), (cx, 0.05, cz), (cx, 0.98, cz), (-cx, 0.98, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))
        top = (0.0, 0.96, 0.0)
        for s in range(5):
            a0, a1 = s * (math.tau / 5), (s + 1) * (math.tau / 5)
            p0 = (math.cos(a0) * 0.46, 0.52, math.sin(a0) * 0.46)
            p1 = (math.cos(a1) * 0.46, 0.52, math.sin(a1) * 0.46)
            geo.add_tri(top, p1, p0, (umid, 0.15), (0.85, 0.46), (0.35, 0.46), (math.cos((a0 + a1) * 0.5), 0.65, math.sin((a0 + a1) * 0.5)))
    else:
        for ang in (0.0, math.pi * 0.5):
            cx, cz = math.cos(ang) * 0.60, math.sin(ang) * 0.60
            geo.add_quad((-cx, 0.0, -cz), (cx, 0.0, cz), (cx, 0.86, cz), (-cx, 0.86, -cz), (u0, 0.98), (u1, 0.98), (u1, 0.02), (u0, 0.02))

    m_idx = gb.add_mesh(out_path.stem, [geo.to_prim(m_tree)])
    gb.add_node(out_path.stem, m_idx)
    gb.save(out_path)


# =============================================================================
# TRICK 7: Study-Level Cessna 172S Skyhawk SP (EC-VFR) Exterior & 3D Cockpit .GLB
# =============================================================================
def _load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for p in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    ):
        if Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def _height_to_normal(hmap: np.ndarray, strength: float = 2.6) -> Image.Image:
    dx = (np.roll(hmap, -1, axis=1) - np.roll(hmap, 1, axis=1)) * strength
    dy = (np.roll(hmap, -1, axis=0) - np.roll(hmap, 1, axis=0)) * strength
    inv_l = 1.0 / np.sqrt(dx * dx + dy * dy + 1.0)
    nrm_arr = np.stack([(-dx * inv_l) * 0.5 + 0.5, (-dy * inv_l) * 0.5 + 0.5, inv_l * 0.5 + 0.5], axis=-1)
    return Image.fromarray((np.clip(nrm_arr, 0, 1) * 255).astype(np.uint8), "RGB")


def _build_c172_skin_textures() -> tuple[Image.Image, Image.Image, Image.Image]:
    """2K (2048x1024) glossy Matterhorn White riveted aluminum fuselage with modern Skyhawk SP livery."""
    W, H = 2048, 1024
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = xx / W, yy / H

    h0 = np.mod(np.sin(xx * 12.9898 + yy * 78.233) * 43758.5453, 1.0) - 0.5
    pu, pv = np.mod(u * 14.0, 1.0), np.mod(v * 8.0, 1.0)
    seam = np.maximum((pu < 0.009) | (pu > 0.991), (pv < 0.009) | (pv > 0.991)).astype(np.float32)

    riv_u = (((pu > 0.018) & (pu < 0.032)) | ((pu > 0.968) & (pu < 0.982))) & (np.abs(np.mod(v * 140.0, 1.0) - 0.5) < 0.22)
    riv_v = (((pv > 0.018) & (pv < 0.032)) | ((pv > 0.968) & (pv < 0.982))) & (np.abs(np.mod(u * 190.0, 1.0) - 0.5) < 0.22)
    riv = (riv_u | riv_v).astype(np.float32)

    pid = np.floor(u * 14.0) * 17.0 + np.floor(v * 8.0) * 11.0
    pvar = (np.mod(np.sin(pid * 19.3) * 4375.5, 1.0) - 0.5) * 0.018

    # Bright glossy Matterhorn White aviation enamel (true sRGB -> crisp clean white under AgX!)
    r = np.full((H, W), 0.86, dtype=np.float32) + pvar + h0 * 0.012
    g = np.full((H, W), 0.87, dtype=np.float32) + pvar + h0 * 0.012
    b = np.full((H, W), 0.85, dtype=np.float32) + pvar + h0 * 0.012
    rough = np.full((H, W), 0.24, dtype=np.float32) + h0 * 0.025 + seam * 0.25
    metal = np.full((H, W), 0.22, dtype=np.float32)

    # Modern swept Cessna 172S Skyhawk wave livery (Crimson Red + Midnight Navy + Metallic Champagne Gold)
    side_dist = np.minimum(np.abs(v - 0.23), np.abs(v - 0.77))
    swoop = 0.030 * np.sin(u * math.pi * 1.38 - 0.35) + 0.018 * (u ** 1.6)
    crimson_mask = np.abs(side_dist - (0.022 + swoop)) < (0.018 + 0.012 * np.clip(1.0 - u * 1.4, 0.0, 1.0))
    navy_mask = np.abs(side_dist - (0.056 + swoop * 1.08)) < (0.013 + 0.008 * np.clip(u, 0.0, 1.0))
    gold_pin = np.abs(side_dist - (-0.003 + swoop * 0.94)) < 0.0045
    # Subtle lower-belly navy cheatline accent
    belly_stripe = (np.minimum(v, 1.0 - v) < 0.075) & (u > 0.14) & (u < 0.88)

    r = np.where(belly_stripe, 0.08 + h0 * 0.008, r)
    g = np.where(belly_stripe, 0.15 + h0 * 0.008, g)
    b = np.where(belly_stripe, 0.30 + h0 * 0.010, b)

    r = np.where(crimson_mask, 0.72 + h0 * 0.012, r)
    g = np.where(crimson_mask, 0.09 + h0 * 0.006, g)
    b = np.where(crimson_mask, 0.11 + h0 * 0.006, b)

    r = np.where(navy_mask, 0.07 + h0 * 0.006, r)
    g = np.where(navy_mask, 0.14 + h0 * 0.008, g)
    b = np.where(navy_mask, 0.30 + h0 * 0.010, b)

    r = np.where(gold_pin, 0.80, r)
    g = np.where(gold_pin, 0.64, g)
    b = np.where(gold_pin, 0.24, b)
    metal = np.where(gold_pin, 0.62, metal)
    rough = np.where(gold_pin, 0.18, rough)

    # Matte black cowling anti-glare strip directly in front of windshield
    glare = (u < 0.215) & (np.abs(v - 0.50) < 0.072)
    r = np.where(glare, 0.09, r); g = np.where(glare, 0.095, g); b = np.where(glare, 0.105, b)
    rough = np.where(glare, 0.86, rough)
    metal = np.where(glare, 0.04, metal)

    # Subtle Lycoming exhaust soot streak along lower belly aft of cowling
    soot = np.exp(-((u - 0.30) / 0.16) ** 2) * np.clip((0.08 - np.minimum(v, 1.0 - v)) / 0.08, 0.0, 1.0) * 0.28
    # Baked under-wing, cabin-door & lower belly AO
    under_wing_ao = np.exp(-((u - 0.36) / 0.12) ** 2) * np.exp(-((np.abs(v - 0.50) - 0.14) / 0.09) ** 2) * 0.28
    belly_ao = np.clip((0.11 - np.minimum(v, 1.0 - v)) / 0.11, 0.0, 1.0) * 0.24
    baked_ao = 1.0 - np.clip(under_wing_ao + belly_ao + soot + seam * 0.26 + riv * 0.08, 0.0, 0.58)

    rgb = np.stack([np.clip(r * baked_ao, 0.02, 0.96), np.clip(g * baked_ao, 0.02, 0.96), np.clip(b * baked_ao, 0.02, 0.96)], axis=-1)
    alb = Image.fromarray((rgb * 255).astype(np.uint8), "RGB")

    draw = ImageDraw.Draw(alb)
    font_reg = _load_font(56)
    font_sm = _load_font(22)
    # Cabin door, baggage door, static port & cowling latches on both sides
    for vy in (int(H * 0.16), int(H * 0.72)):
        draw.rectangle([int(W * 0.29), vy, int(W * 0.44), vy + int(H * 0.12)], outline=(48, 52, 58), width=3)
        draw.rectangle([int(W * 0.48), vy + 18, int(W * 0.55), vy + int(H * 0.10)], outline=(52, 56, 62), width=2)
        draw.ellipse([int(W * 0.225), vy + 46, int(W * 0.233), vy + 54], outline=(60, 64, 70), width=2)
    # Crisp Spanish registration EC-VFR + Spanish flag stripe + SKYHAWK SP decal
    draw.text((int(W * 0.60), int(H * 0.155)), "EC-VFR", fill=(18, 32, 66), font=font_reg)
    draw.text((int(W * 0.25), int(H * 0.22)), "SKYHAWK SP", fill=(180, 32, 36), font=font_sm)
    # Tiny Spanish civil flag decal near registration
    fx0, fy0 = int(W * 0.555), int(H * 0.172)
    draw.rectangle([fx0, fy0, fx0 + 54, fy0 + 9], fill=(190, 24, 30))
    draw.rectangle([fx0, fy0 + 9, fx0 + 54, fy0 + 23], fill=(235, 185, 25))
    draw.rectangle([fx0, fy0 + 23, fx0 + 54, fy0 + 32], fill=(190, 24, 30))

    reg_stamp = Image.new("RGBA", (360, 88), (0, 0, 0, 0))
    ImageDraw.Draw(reg_stamp).text((8, 8), "EC-VFR", fill=(18, 32, 66, 255), font=font_reg)
    reg_flip = reg_stamp.transpose(Image.Transpose.FLIP_LEFT_RIGHT).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    alb.paste(reg_flip, (int(W * 0.60), int(H * 0.745)), reg_flip)

    hmap = 0.5 - seam * 0.36 + riv * 0.14 + h0 * 0.025
    nrm = _height_to_normal(hmap, 3.0)
    mr_arr = np.stack([
        (np.clip(baked_ao, 0, 1) * 255).astype(np.uint8),
        (np.clip(rough, 0, 1) * 255).astype(np.uint8),
        (np.clip(metal, 0, 1) * 255).astype(np.uint8),
    ], axis=-1)
    return alb, nrm, Image.fromarray(mr_arr, "RGB")


def _build_c172_wing_and_tail_tex() -> tuple[Image.Image, Image.Image, Image.Image]:
    """1K (1024x1024) glossy Matterhorn White wing/tail skin with Cessna corrugated control-surface ribs."""
    N = 1024
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    u, v = xx / N, yy / N
    h0 = np.mod(np.sin(xx * 19.1 + yy * 53.7) * 43758.5, 1.0) - 0.5
    rib = ((np.mod(u * 14.0, 1.0) < 0.011) | (np.mod(u * 14.0, 1.0) > 0.989)).astype(np.float32)
    spar = (np.abs(v - 0.30) < 0.006).astype(np.float32)
    hinge = (np.abs(v - 0.72) < 0.010).astype(np.float32)
    # Signature Cessna corrugated stiffening beads along trailing control surfaces!
    corrug = (v > 0.74).astype(np.float32) * (0.5 + 0.5 * np.sin(u * math.tau * 36.0))
    seam = np.maximum(rib, np.maximum(spar, hinge))

    r = np.full((N, N), 0.86, dtype=np.float32) + h0 * 0.014
    g = np.full((N, N), 0.87, dtype=np.float32) + h0 * 0.014
    b = np.full((N, N), 0.85, dtype=np.float32) + h0 * 0.014
    rough = np.full((N, N), 0.24, dtype=np.float32) + h0 * 0.02
    metal = np.full((N, N), 0.22, dtype=np.float32)

    tip = u > 0.92
    red_s = (v > 0.16) & (v < 0.235) & (u < 0.91)
    navy_s = (v > 0.24) & (v < 0.275) & (u < 0.91)
    gold_s = (v > 0.148) & (v < 0.158) & (u < 0.91)
    walk = (u < 0.12) & (v > 0.26) & (v < 0.68)

    for m, col in [(tip, (0.72, 0.09, 0.11)), (red_s, (0.72, 0.09, 0.11)), (navy_s, (0.07, 0.14, 0.30)), (gold_s, (0.80, 0.64, 0.24)), (walk, (0.11, 0.115, 0.125))]:
        r = np.where(m, col[0] + h0 * 0.012, r)
        g = np.where(m, col[1] + h0 * 0.012, g)
        b = np.where(m, col[2] + h0 * 0.012, b)
    rough = np.where(walk, 0.88, rough)
    metal = np.where(walk, 0.02, metal)

    root_ao = 1.0 - 0.20 * np.exp(-u * 12.0) - seam * 0.22 - hinge * 0.16 - corrug * 0.06
    rgb = np.stack([np.clip(r * root_ao, 0.02, 0.96), np.clip(g * root_ao, 0.02, 0.96), np.clip(b * root_ao, 0.02, 0.96)], axis=-1)
    alb = Image.fromarray((rgb * 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(alb)
    # Fuel cap & inspection rings on upper wing
    draw.ellipse([int(N * 0.28), int(N * 0.34), int(N * 0.305), int(N * 0.365)], fill=(195, 198, 204), outline=(45, 48, 54), width=2)
    draw.ellipse([int(N * 0.285), int(N * 0.345), int(N * 0.300), int(N * 0.360)], fill=(175, 32, 32))

    hmap = 0.5 - seam * 0.36 + corrug * 0.20 + h0 * 0.02
    nrm = _height_to_normal(hmap, 2.8)
    mr = Image.fromarray(np.stack([
        (np.clip(root_ao, 0, 1) * 255).astype(np.uint8),
        (np.clip(rough, 0, 1) * 255).astype(np.uint8),
        (np.clip(metal, 0, 1) * 255).astype(np.uint8),
    ], axis=-1), "RGB")
    return alb, nrm, mr


def _build_c172_vtail_tex() -> tuple[Image.Image, Image.Image]:
    N = 512
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    u, v = xx / N, yy / N
    ao = 1.0 - 0.18 * np.exp(-u * 9.0)
    corrug = (0.5 + 0.5 * np.sin(v * math.tau * 22.0)) * 0.05
    r = np.full((N, N), 0.86, dtype=np.float32) * (ao - corrug)
    g = np.full((N, N), 0.87, dtype=np.float32) * (ao - corrug)
    b = np.full((N, N), 0.85, dtype=np.float32) * (ao - corrug)
    band = u + 0.28 * np.abs(v - 0.5)
    for m, col in [
        ((band > 0.62) & (band < 0.84), (0.72, 0.09, 0.11)),
        ((band >= 0.52) & (band <= 0.61), (0.07, 0.14, 0.30)),
        ((band >= 0.845) & (band <= 0.875), (0.80, 0.64, 0.24)),
        (u > 0.91, (0.72, 0.09, 0.11)),
    ]:
        r = np.where(m, col[0] * ao, r); g = np.where(m, col[1] * ao, g); b = np.where(m, col[2] * ao, b)
    alb = Image.fromarray((np.clip(np.stack([r, g, b], axis=-1), 0, 1) * 255).astype(np.uint8), "RGB")
    nrm = _height_to_normal(0.5 + corrug * 4.0, 2.2)
    return alb, nrm


def _build_prop_blur_tex() -> Image.Image:
    """512x512 RGBA realistic high-shutter-speed motion-blurred 2-blade McCauley propeller disc."""
    N = 512
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    nx = (xx / (N - 1)) * 2.0 - 1.0
    ny = (yy / (N - 1)) * 2.0 - 1.0
    rad = np.sqrt(nx * nx + ny * ny)
    ang = np.arctan2(ny, nx)
    # Soft feathered dual-blade streaks + subtle translucent yellow safety tip halo (never neon solid!)
    streak = 0.35 + 0.65 * (np.cos(ang * 2.0) ** 6)
    blade_band = np.clip((rad - 0.17) / 0.10, 0.0, 1.0) * np.clip((0.99 - rad) / 0.04, 0.0, 1.0)
    tip_ring = np.clip(1.0 - np.abs(rad - 0.925) / 0.042, 0.0, 1.0) ** 1.5
    r = 0.14 * (1.0 - tip_ring) + 0.88 * tip_ring
    g = 0.15 * (1.0 - tip_ring) + 0.72 * tip_ring
    b = 0.17 * (1.0 - tip_ring) + 0.12 * tip_ring
    alpha = blade_band * (0.09 + 0.22 * streak + 0.16 * tip_ring) * np.clip(1.10 - rad * 0.42, 0.40, 1.0)
    rgba = np.stack([r, g, b, np.clip(alpha, 0.0, 0.48)], axis=-1)
    return Image.fromarray((np.clip(rgba, 0, 1) * 255).astype(np.uint8), "RGBA")


def _build_cockpit_prop_veil_tex() -> Image.Image:
    """512x512 RGBA ultra-subtle smoky-grey spinning propeller veil for interior windshield view (zero yellow rainbow!)."""
    N = 512
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    nx = (xx / (N - 1)) * 2.0 - 1.0
    ny = (yy / (N - 1)) * 2.0 - 1.0
    rad = np.sqrt(nx * nx + ny * ny)
    ang = np.arctan2(ny, nx)
    streak = 0.30 + 0.70 * (np.cos(ang * 2.0) ** 6)
    band = np.clip((rad - 0.16) / 0.14, 0.0, 1.0) * np.clip((0.98 - rad) / 0.08, 0.0, 1.0)
    alpha = band * (0.04 + 0.12 * streak) * np.clip(1.0 - rad * 0.45, 0.35, 1.0)
    rgba = np.stack([
        np.full((N, N), 0.16, dtype=np.float32),
        np.full((N, N), 0.17, dtype=np.float32),
        np.full((N, N), 0.19, dtype=np.float32),
        np.clip(alpha, 0.0, 0.20),
    ], axis=-1)
    return Image.fromarray((np.clip(rgba, 0, 1) * 255).astype(np.uint8), "RGBA")


def _naca2412_section() -> list[tuple[float, float]]:
    return [
        (0.00, 0.000), (0.04, 0.040), (0.12, 0.064), (0.25, 0.078), (0.45, 0.073),
        (0.68, 0.052), (0.72, 0.047), (0.88, 0.024), (1.00, 0.002),
        (0.88, -0.012), (0.72, -0.022), (0.68, -0.025), (0.45, -0.034),
        (0.25, -0.040), (0.12, -0.036), (0.04, -0.024),
    ]


def _control_surface_geo(span_x0: float, span_x1: float, chord_m: float, thick_m: float, y_offset: float = 0.0) -> GeoPart:
    geo = GeoPart()
    prof = [
        (0.00, 0.000), (0.06, thick_m * 0.50), (0.35, thick_m * 0.42), (0.75, thick_m * 0.18),
        (1.00, 0.004), (0.75, -thick_m * 0.14), (0.35, -thick_m * 0.34), (0.06, -thick_m * 0.42),
    ]
    rings = [np.array([[x, y_offset + y, z * chord_m] for (z, y) in prof], dtype=np.float32) for x in (span_x0, span_x1)]
    geo.add_loft(rings, [0.0, 1.0], close_caps=True)
    return geo


def build_c172s_exterior_glb(out_path: Path) -> None:
    gb = GLBBuilder()
    skin_alb, skin_nrm, skin_mr = _build_c172_skin_textures()
    wing_alb, wing_nrm, wing_mr = _build_c172_wing_and_tail_tex()
    vtail_alb, vtail_nrm = _build_c172_vtail_tex()
    prop_blur_img = _build_prop_blur_tex()

    m_skin = gb.add_material(
        "C172Skin", (1, 1, 1, 1), metallic=0.26, roughness=0.26,
        base_tex=gb.add_texture_pil(skin_alb, quality=92),
        normal_tex=gb.add_texture_pil(skin_nrm, quality=88), normal_scale=0.85,
        mr_tex=gb.add_texture_pil(skin_mr, quality=86),
    )
    m_wing = gb.add_material(
        "C172Wing", (1, 1, 1, 1), metallic=0.24, roughness=0.26,
        base_tex=gb.add_texture_pil(wing_alb, quality=92),
        normal_tex=gb.add_texture_pil(wing_nrm, quality=88), normal_scale=0.80,
        mr_tex=gb.add_texture_pil(wing_mr, quality=86),
    )
    m_vtail = gb.add_material(
        "C172VTail", (1, 1, 1, 1), metallic=0.24, roughness=0.26,
        base_tex=gb.add_texture_pil(vtail_alb, quality=92),
        normal_tex=gb.add_texture_pil(vtail_nrm, quality=85), normal_scale=0.70,
    )
    m_glass = gb.add_material("CabinGlass", (0.04, 0.09, 0.14, 0.30), metallic=0.18, roughness=0.04, alpha_mode="BLEND", double_sided=True)
    m_dark = gb.add_material("DarkTrim", (0.075, 0.08, 0.09, 1.0), metallic=0.20, roughness=0.62)
    m_seat = gb.add_material("CabinSeats", (0.34, 0.27, 0.20, 1.0), metallic=0.04, roughness=0.84)
    m_pilot = gb.add_material("PilotShirt", (0.09, 0.15, 0.26, 1.0), metallic=0.02, roughness=0.88)
    m_skin_p = gb.add_material("PilotSkin", (0.72, 0.54, 0.42, 1.0), metallic=0.0, roughness=0.72)
    m_chrome = gb.add_material("PolishedMetal", (0.88, 0.90, 0.94, 1.0), metallic=0.95, roughness=0.10)
    m_prop = gb.add_material("PropBlade", (0.13, 0.14, 0.16, 1.0), metallic=0.68, roughness=0.24)
    m_yellow = gb.add_material("PropTipYellow", (0.96, 0.78, 0.08, 1.0), metallic=0.12, roughness=0.28)
    m_blur = gb.add_material(
        "PropBlur", (1, 1, 1, 0.92), metallic=0.15, roughness=0.35,
        base_tex=gb.add_texture_pil(prop_blur_img, fmt="PNG", clamp=True),
        alpha_mode="BLEND", double_sided=True,
    )
    m_tire = gb.add_material("RubberTire", (0.055, 0.06, 0.065, 1.0), metallic=0.0, roughness=0.92)
    m_cream = gb.add_material("StrutWhite", (0.84, 0.85, 0.83, 1.0), metallic=0.22, roughness=0.26)
    m_nav_red = gb.add_material("NavRed", (1.0, 0.10, 0.08, 1.0), roughness=0.12, emissive=(2.8, 0.18, 0.10))
    m_nav_grn = gb.add_material("NavGreen", (0.08, 1.0, 0.28, 1.0), roughness=0.12, emissive=(0.14, 2.8, 0.45))
    m_strobe = gb.add_material("StrobeWhite", (1.0, 0.98, 0.95, 1.0), roughness=0.10, emissive=(3.2, 3.2, 3.4))
    m_beacon = gb.add_material("BeaconRed", (1.0, 0.08, 0.05, 1.0), roughness=0.12, emissive=(3.0, 0.14, 0.08))
    m_ldg = gb.add_material("LandingLED", (1.0, 0.96, 0.84, 1.0), roughness=0.08, emissive=(3.4, 3.1, 2.4))

    root = gb.add_node("C172S_Exterior")

    # 32-segment fuselage with TRUE OPEN CABIN GREENHOUSE CUTOUT (so tinted CabinGlass shows 3D interior & pilot!)
    # Forward nose/cowling loft (-2.64 .. -1.24), lower cabin tub (-1.24 .. +1.04, below sill), rear tailboom (+1.04 .. +4.88)!
    SEGS = 32
    def make_fuse_ring(z: float, cy: float, rx: float, ry: float, sq: float, y_cap: float | None = None) -> np.ndarray:
        p = 2.0 / (1.0 + sq)
        ring = []
        for k in range(SEGS):
            ang = (k / SEGS) * math.tau - math.pi * 0.5
            ca, sa = math.cos(ang), math.sin(ang)
            yy = cy + math.copysign(abs(sa) ** p, sa) * ry
            if y_cap is not None and yy > y_cap:
                yy = y_cap
            ring.append([0.16 + math.copysign(abs(ca) ** p, ca) * rx, yy, z])
        return np.array(ring, dtype=np.float32)

    fuse_geo = GeoPart()
    fwd_st = [
        (-2.64, -0.50, 0.17, 0.17, 0.05), (-2.56, -0.49, 0.40, 0.30, 0.28),
        (-2.34, -0.46, 0.50, 0.35, 0.32), (-1.86, -0.40, 0.54, 0.39, 0.32),
        (-1.26, -0.32, 0.57, 0.42, 0.32),
    ]
    fuse_geo.add_loft([make_fuse_ring(*st) for st in fwd_st], [(st[0] + 2.64) / 7.52 for st in fwd_st], close_caps=True)

    # Lower cabin hull capped cleanly at window sill (y=-0.02) so front & side windows are genuinely see-through!
    mid_st = [
        (-1.26, -0.32, 0.57, 0.42, 0.32, -0.02), (-0.90, -0.18, 0.59, 0.48, 0.36, -0.02),
        (-0.30, -0.14, 0.61, 0.52, 0.38, -0.02), ( 0.44, -0.15, 0.60, 0.51, 0.36, -0.01),
        ( 1.04, -0.18, 0.54, 0.48, 0.30,  0.02),
    ]
    fuse_geo.add_loft([make_fuse_ring(*st) for st in mid_st], [(st[0] + 2.64) / 7.52 for st in mid_st], close_caps=True)

    aft_st = [
        ( 1.04, -0.16, 0.54, 0.50, 0.30), ( 1.54, -0.18, 0.43, 0.39, 0.22),
        ( 2.35, -0.14, 0.32, 0.31, 0.16), ( 3.40, -0.07, 0.21, 0.22, 0.12),
        ( 4.42, -0.02, 0.11, 0.14, 0.08), ( 4.88,  0.00, 0.045, 0.075, 0.05),
    ]
    fuse_geo.add_loft([make_fuse_ring(*st) for st in aft_st], [(st[0] + 2.64) / 7.52 for st in aft_st], close_caps=True)
    # Cabin roof cap under high wing carry-through!
    fuse_geo.add_box((0.16, 0.40, 0.14), (1.14, 0.08, 1.88))

    # Clean white dorsal fin fillet transitioning fuselage spine smoothly into vertical stabilizer!
    dorsal_geo = GeoPart()
    df_rings = [
        np.array([[0.16 - 0.045, 0.12, 2.75], [0.16 + 0.045, 0.12, 2.75], [0.16, 0.18, 2.75]], dtype=np.float32),
        np.array([[0.16 - 0.055, 0.10, 3.35], [0.16 + 0.055, 0.10, 3.35], [0.16, 0.40, 3.35]], dtype=np.float32),
        np.array([[0.16 - 0.062, 0.08, 3.90], [0.16 + 0.062, 0.08, 3.90], [0.16, 0.65, 3.90]], dtype=np.float32),
    ]
    dorsal_geo.add_loft(df_rings, [0.0, 0.5, 1.0], close_caps=True)

    # Recessed cowling cooling intakes + window posts + dark interior panel silhouette & aviation headset!
    fuse_trim = GeoPart()
    fuse_trim.add_box((0.16 - 0.22, -0.48, -2.56), (0.17, 0.095, 0.09))
    fuse_trim.add_box((0.16 + 0.22, -0.48, -2.56), (0.17, 0.095, 0.09))
    fuse_trim.add_box((0.16, -0.71, -2.35), (0.15, 0.075, 0.24))
    # Dark interior instrument panel, glareshield & pilot aviation headset visible through real open glass!
    fuse_trim.add_box((0.16, -0.10, -0.82), (1.06, 0.08, 0.30))
    fuse_trim.add_box((0.16, -0.26, -0.88), (1.04, 0.30, 0.12))
    fuse_trim.add_cyl_between((-0.14, 0.14, -0.12), (0.04, 0.14, -0.12), 0.036, 0.036, 10)  # Pilot headset earcups
    fuse_trim.add_cyl_between((-0.05, 0.23, -0.12), (-0.05, 0.25, -0.12), 0.085, 0.085, 10)
    for sx in (-1.0, 1.0):
        fuse_trim.add_cyl_between((0.16 + sx * 0.55, -0.06, -1.20), (0.16 + sx * 0.54, 0.38, -0.76), 0.024, 0.024, 10)
        fuse_trim.add_cyl_between((0.16 + sx * 0.60, -0.04,  0.25), (0.16 + sx * 0.58, 0.39,  0.25), 0.022, 0.022, 10)
        fuse_trim.add_cyl_between((0.16 + sx * 0.56, -0.04,  0.98), (0.16 + sx * 0.50, 0.36,  0.98), 0.020, 0.020, 10)
        fuse_trim.add_cyl_between((0.16 + sx * 0.605, -0.02, -0.74), (0.16 + sx * 0.555, 0.00, 1.02), 0.016, 0.016, 8)
        fuse_trim.add_box((0.16 + sx * 0.61, -0.18, -0.58), (0.04, 0.03, 0.11))
    fuse_trim.add_cyl_between((0.16, -0.06, -1.24), (0.16, 0.39, -0.80), 0.012, 0.012, 8)
    fuse_trim.add_cyl_between((0.16 - 0.10, 0.45, 0.35), (0.16 - 0.24, 0.94, 0.58), 0.006, 0.003, 6)
    fuse_trim.add_cyl_between((0.16 + 0.10, 0.45, 0.35), (0.16 + 0.24, 0.94, 0.58), 0.006, 0.003, 6)
    fuse_trim.add_cyl_between((0.16, 0.26, 2.05), (0.16, 0.68, 2.28), 0.007, 0.003, 6)
    fuse_trim.add_box((0.16, -0.74, 0.65), (0.012, 0.14, 0.07))

    fuse_chrome = GeoPart()
    fuse_chrome.add_cyl_between((0.16 - 0.12, -0.74, -1.78), (0.16 - 0.14, -0.94, -1.50), 0.028, 0.025, 10)
    fuse_chrome.add_cyl_between((0.16 + 0.12, -0.74, -1.78), (0.16 + 0.14, -0.94, -1.50), 0.028, 0.025, 10)
    for fi in range(3):
        fuse_chrome.add_box((0.16 - 0.22, -0.50 + fi * 0.024, -2.59), (0.14, 0.006, 0.02))
        fuse_chrome.add_box((0.16 + 0.22, -0.50 + fi * 0.024, -2.59), (0.14, 0.006, 0.02))
    for sx in (-1.0, 1.0):
        fuse_chrome.add_box((0.16 + sx * 0.63, -0.78, -0.42), (0.16, 0.02, 0.08))
        fuse_chrome.add_box((0.16 + sx * 0.615, -0.16, 0.12), (0.015, 0.025, 0.09))

    # Visible Tan Leather Cabin Seats + 3D Pilot Figure sitting in left seat!
    seats_geo, pilot_torso, pilot_head = GeoPart(), GeoPart(), GeoPart()
    for sx in (-0.22, 0.22):
        seats_geo.add_box((0.16 + sx, -0.16, -0.02), (0.36, 0.36, 0.09), rot_deg=(12, 0, 0))
        seats_geo.add_box((0.16 + sx,  0.09,  0.02), (0.20, 0.15, 0.07), rot_deg=(8, 0, 0))
        seats_geo.add_box((0.16 + sx, -0.18,  0.64), (0.34, 0.32, 0.08), rot_deg=(8, 0, 0))
    pilot_torso.add_box((-0.05, -0.05, -0.12), (0.32, 0.26, 0.18), rot_deg=(8, 0, 0))
    pilot_head.add_cyl_between((-0.05, 0.07, -0.12), (-0.05, 0.23, -0.12), 0.078, 0.068, 12)

    gb.add_node("Fuselage", gb.add_mesh("Fuselage", [
        fuse_geo.to_prim(m_skin), dorsal_geo.to_prim(m_cream), fuse_trim.to_prim(m_dark),
        fuse_chrome.to_prim(m_chrome), seats_geo.to_prim(m_seat),
        pilot_torso.to_prim(m_pilot), pilot_head.to_prim(m_skin_p),
    ]), parent=root)

    glass_geo = GeoPart()
    ws_rings = []
    for t in (0.0, 0.5, 1.0):
        z, y, rx = -1.26 + t * 0.48, -0.10 + t * 0.49, 0.54 + t * 0.03
        ws_rings.append(np.array([[0.16 + math.sin(math.radians(-82.0 + k * (164.0 / 16.0))) * rx, y - (1.0 - math.cos(math.radians(-82.0 + k * (164.0 / 16.0)))) * 0.12, z + (1.0 - math.cos(math.radians(-82.0 + k * (164.0 / 16.0)))) * 0.24] for k in range(17)], dtype=np.float32))
    for r in range(len(ws_rings) - 1):
        for k in range(len(ws_rings[0]) - 1):
            glass_geo.add_quad(ws_rings[r][k], ws_rings[r][k + 1], ws_rings[r + 1][k + 1], ws_rings[r + 1][k], (0, 0), (1, 0), (1, 1), (0, 1), (0, 0.5, -1.0))
    for sx in (-1.0, 1.0):
        x_w = 0.16 + sx * 0.608
        glass_geo.add_quad((x_w, -0.02, -0.74), (x_w, -0.02, 0.22), (x_w - sx * 0.03, 0.36, 0.22), (x_w - sx * 0.03, 0.36, -0.68), (0, 0), (1, 0), (1, 1), (0, 1), (sx, 0.1, 0))
        glass_geo.add_quad((x_w - sx * 0.01, -0.02, 0.28), (x_w - sx * 0.06, 0.00, 1.02), (x_w - sx * 0.09, 0.31, 0.96), (x_w - sx * 0.03, 0.36, 0.28), (0, 0), (1, 0), (1, 1), (0, 1), (sx, 0.1, 0))
    glass_geo.add_quad((0.16 - 0.44, 0.04, 1.08), (0.16 + 0.44, 0.04, 1.08), (0.16 + 0.36, 0.26, 1.44), (0.16 - 0.36, 0.26, 1.44), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0.6, 0.8))
    gb.add_node("CabinGlass", gb.add_mesh("CabinGlass", [glass_geo.to_prim(m_glass)]), parent=root)

    # High-aspect NACA 2412 Wing + Sculpted Drooped Hoerner Wingtips + Strut Fairings + Pitot + Flap Tracks!
    wing_geo = GeoPart()
    foil_full = _naca2412_section()
    span_st = [
        (-5.66, 0.535, 0.62, -0.22), (-5.50, 0.562, 0.96, -0.37), (-5.26, 0.552, 1.14, -0.45),
        (-2.45, 0.470, 1.50, -0.58), ( 0.00, 0.430, 1.54, -0.58),
        ( 2.45, 0.470, 1.50, -0.58), ( 5.26, 0.552, 1.14, -0.45), ( 5.50, 0.562, 0.96, -0.37), ( 5.66, 0.535, 0.62, -0.22),
    ]
    w_rings = [np.array([[0.16 + ox, y0 + py * ch, zle + px * ch] for (px, py) in foil_full], dtype=np.float32) for (ox, y0, ch, zle) in span_st]
    wing_geo.add_loft(w_rings, [abs(ox) / 5.66 for (ox, _, _, _) in span_st], close_caps=True)

    strut_geo = GeoPart()
    strut_geo.add_cyl_between((-0.44, -0.58, -0.12), (-2.55, 0.45, -0.05), 0.024, 0.020, segs=12, aspect_x=2.6)
    strut_geo.add_cyl_between(( 0.76, -0.58, -0.12), ( 2.87, 0.45, -0.05), 0.024, 0.020, segs=12, aspect_x=2.6)
    # Strut root/upper aerodynamic cuffs + flap track fairings
    for sx in (-1.0, 1.0):
        strut_geo.add_box((0.16 + sx * 2.71, 0.44, -0.05), (0.09, 0.07, 0.16))
        for fx in (1.05, 2.25):
            strut_geo.add_box((0.16 + sx * fx, 0.43, 0.54), (0.035, 0.055, 0.24))

    pitot_geo = GeoPart()
    pitot_geo.add_cyl_between((-2.15, 0.37, -0.25), (-2.15, 0.25, -0.25), 0.009, 0.007, 8)
    pitot_geo.add_cyl_between((-2.15, 0.25, -0.25), (-2.15, 0.25, -0.50), 0.007, 0.005, 8)
    pitot_geo.add_box((-1.85, 0.47, -0.59), (0.26, 0.078, 0.04))
    # Tie-down rings & static wicks
    for sx in (-1.0, 1.0):
        pitot_geo.add_cyl_between((0.16 + sx * 5.60, 0.54, 0.36), (0.16 + sx * 5.60, 0.54, 0.56), 0.003, 0.002, 6)
    gb.add_node("Wing", gb.add_mesh("Wing", [wing_geo.to_prim(m_wing), strut_geo.to_prim(m_cream), pitot_geo.to_prim(m_chrome)]), parent=root)

    # Aviation Exterior Lights (Port Red Nav, Stbd Green Nav, Wingtip Strobes, Tail Beacon, Dual Landing LEDs!)
    nav_l = GeoPart(); nav_l.add_box((0.16 - 5.66, 0.545, -0.18), (0.06, 0.048, 0.09))
    nav_r = GeoPart(); nav_r.add_box((0.16 + 5.66, 0.545, -0.18), (0.06, 0.048, 0.09))
    strb = GeoPart()
    strb.add_box((0.16 - 5.67, 0.545, -0.06), (0.05, 0.042, 0.07))
    strb.add_box((0.16 + 5.67, 0.545, -0.06), (0.05, 0.042, 0.07))
    bcn = GeoPart(); bcn.add_cyl_between((0.16, 1.66, 4.62), (0.16, 1.75, 4.62), 0.036, 0.028, 10)
    ldg = GeoPart()
    ldg.add_cyl_between((-1.92, 0.47, -0.61), (-1.92, 0.47, -0.58), 0.034, 0.034, 12)
    ldg.add_cyl_between((-1.78, 0.47, -0.61), (-1.78, 0.47, -0.58), 0.034, 0.034, 12)
    gb.add_node("NavLightRed",   gb.add_mesh("NavLightRed",   [nav_l.to_prim(m_nav_red)]), parent=root)
    gb.add_node("NavLightGreen", gb.add_mesh("NavLightGreen", [nav_r.to_prim(m_nav_grn)]), parent=root)
    gb.add_node("StrobeWhite",   gb.add_mesh("StrobeWhite",   [strb.to_prim(m_strobe)]),   parent=root)
    gb.add_node("BeaconRed",     gb.add_mesh("BeaconRed",     [bcn.to_prim(m_beacon)]),    parent=root)
    gb.add_node("LandingLight",  gb.add_mesh("LandingLight",  [ldg.to_prim(m_ldg)]),       parent=root)

    # Named Animated Control Surfaces (exact hinge pivots!)
    gb.add_node("Flap_L", gb.add_mesh("Flap_L", [_control_surface_geo(-2.05, -0.02, 0.44, 0.075).to_prim(m_wing)]), translation=(0.16 - 0.62, 0.46, 0.52), parent=root)
    gb.add_node("Flap_R", gb.add_mesh("Flap_R", [_control_surface_geo( 0.02,  2.05, 0.44, 0.075).to_prim(m_wing)]), translation=(0.16 + 0.62, 0.46, 0.52), parent=root)
    gb.add_node("Aileron_L", gb.add_mesh("Aileron_L", [_control_surface_geo(-2.45, 0.0, 0.36, 0.062).to_prim(m_wing)]), translation=(0.16 - 2.75, 0.51, 0.46), parent=root)
    gb.add_node("Aileron_R", gb.add_mesh("Aileron_R", [_control_surface_geo( 0.0, 2.45, 0.36, 0.062).to_prim(m_wing)]), translation=(0.16 + 2.75, 0.51, 0.46), parent=root)

    sym_foil = [(0.0, 0.0), (0.18, 0.05), (0.48, 0.054), (0.82, 0.028), (1.0, 0.004), (0.82, -0.028), (0.48, -0.054), (0.18, -0.05)]
    h_st = [(-1.74, 0.07, 0.48, 4.24), (-1.52, 0.07, 0.56, 4.18), (0.0, 0.06, 0.72, 3.96), (1.52, 0.07, 0.56, 4.18), (1.74, 0.07, 0.48, 4.24)]
    hstab_geo = GeoPart()
    hstab_geo.add_loft([np.array([[0.16 + ox, y0 + py * ch, zle + px * ch] for (px, py) in sym_foil], dtype=np.float32) for (ox, y0, ch, zle) in h_st], [abs(ox) / 1.74 for (ox, _, _, _) in h_st], close_caps=True)
    gb.add_node("HStab", gb.add_mesh("HStab", [hstab_geo.to_prim(m_wing)]), parent=root)

    elev_node = gb.add_node("Elevator", gb.add_mesh("Elevator", [_control_surface_geo(-1.68, 1.68, 0.36, 0.055).to_prim(m_wing)]), translation=(0.16, 0.07, 4.66), parent=root)
    gb.add_node("TrimTab", gb.add_mesh("TrimTab", [_control_surface_geo(0.18, 0.92, 0.11, 0.018).to_prim(m_vtail)]), translation=(0.0, 0.0, 0.34), parent=elev_node)

    # Vertical Stabilizer with straight trailing hinge edge at z=4.66 meeting Rudder cleanly (zero gap!)
    fin_st = [(0.06, 3.22, 1.44, 1.05), (0.36, 3.64, 1.02, 0.95), (1.06, 4.02, 0.64, 0.80), (1.58, 4.22, 0.44, 0.62), (1.66, 4.30, 0.36, 0.45)]
    vstab_geo = GeoPart()
    vstab_geo.add_loft([np.array([[0.16 + py * ch * ts, y, zle + px * ch] for (px, py) in sym_foil], dtype=np.float32) for (y, zle, ch, ts) in fin_st], [i / (len(fin_st) - 1) for i in range(len(fin_st))], close_caps=True)
    gb.add_node("VStab", gb.add_mesh("VStab", [vstab_geo.to_prim(m_vtail)]), parent=root)

    rud_geo = GeoPart()
    rud_st = [(0.10, 0.46, 0.058), (0.85, 0.42, 0.048), (1.62, 0.34, 0.032)]
    rud_geo.add_loft([np.array([[px, y, pz * ch] for (pz, px) in [(0.0, 0.0), (0.12, th), (0.55, th * 0.72), (1.0, 0.004), (0.55, -th * 0.72), (0.12, -th)]], dtype=np.float32) for (y, ch, th) in rud_st], [0.0, 0.5, 1.0], close_caps=True)
    gb.add_node("Rudder", gb.add_mesh("Rudder", [rud_geo.to_prim(m_vtail)]), translation=(0.16, 0.0, 4.655), parent=root)

    def add_wheel_spat(geo: GeoPart, cx: float, cy: float, cz: float) -> None:
        spat_st = [(-0.38, 0.02, 0.02, 0.02), (-0.25, 0.0, 0.115, 0.125), (-0.04, 0.0, 0.135, 0.145), (0.19, 0.01, 0.105, 0.115), (0.44, 0.03, 0.03, 0.04)]
        geo.add_loft([np.array([[cx + math.cos(k / 16 * math.tau) * rx, cy + dy + math.sin(k / 16 * math.tau) * ry, cz + z] for k in range(16)], dtype=np.float32) for (z, dy, rx, ry) in spat_st], [i / 4.0 for i in range(5)], close_caps=True)

    main_leg, main_spat, main_tire = GeoPart(), GeoPart(), GeoPart()
    main_leg.add_cyl_between((-0.36, -0.58, 0.10), (-1.04, -1.24, 0.10), 0.028, 0.024, 10, aspect_x=2.2)
    main_leg.add_cyl_between(( 0.68, -0.58, 0.10), ( 1.36, -1.24, 0.10), 0.028, 0.024, 10, aspect_x=2.2)
    add_wheel_spat(main_spat, -1.04, -1.25, 0.10)
    add_wheel_spat(main_spat,  1.36, -1.25, 0.10)
    main_tire.add_cyl_between((-1.10, -1.32, 0.10), (-0.98, -1.32, 0.10), 0.16, 0.16, 18)
    main_tire.add_cyl_between(( 1.30, -1.32, 0.10), ( 1.42, -1.32, 0.10), 0.16, 0.16, 18)
    gb.add_node("GearMain", gb.add_mesh("GearMain", [main_leg.to_prim(m_cream), main_spat.to_prim(m_skin), main_tire.to_prim(m_tire)]), parent=root)

    ng_chrome, ng_spat, ng_tire = GeoPart(), GeoPart(), GeoPart()
    ng_chrome.add_cyl_between((0.0, 0.0, 0.0), (0.0, -0.62, -0.06), 0.032, 0.026, 12)
    ng_chrome.add_cyl_between((0.0, -0.26, -0.02), (0.0, -0.44, 0.09), 0.012, 0.012, 6)
    ng_chrome.add_cyl_between((0.0, -0.44, 0.09), (0.0, -0.56, -0.04), 0.012, 0.012, 6)
    add_wheel_spat(ng_spat, 0.0, -0.62, -0.06)
    ng_tire.add_cyl_between((-0.055, -0.68, -0.06), (0.055, -0.68, -0.06), 0.145, 0.145, 18)
    gb.add_node("NoseGear", gb.add_mesh("NoseGear", [ng_chrome.to_prim(m_chrome), ng_spat.to_prim(m_skin), ng_tire.to_prim(m_tire)]), translation=(0.16, -0.64, -1.62), parent=root)

    # Polished Chrome Spinner + 2-Blade McCauley Twisted Propeller + Separate Spinning PropBlurDisc!
    spin_geo, blade_geo, tip_geo = GeoPart(), GeoPart(), GeoPart()
    sp_st = [(0.00, 0.165), (-0.12, 0.140), (-0.25, 0.092), (-0.35, 0.028), (-0.38, 0.004)]
    spin_geo.add_loft([np.array([[math.cos(k / 24 * math.tau) * rad, math.sin(k / 24 * math.tau) * rad, z] for k in range(24)], dtype=np.float32) for (z, rad) in sp_st], [i / 4.0 for i in range(5)], close_caps=True)
    for sign in (-1.0, 1.0):
        b_st = [(0.13, 0.042, 0.018, 34.0), (0.32, 0.064, 0.014, 26.0), (0.58, 0.070, 0.010, 19.0), (0.79, 0.056, 0.007, 14.0)]
        br = []
        for rad, ch, th, twist in b_st:
            tw = math.radians(twist * sign); ct, st_ = math.cos(tw), math.sin(tw)
            prof = [(-0.5 * ch, 0.0), (-0.2 * ch, th), (0.3 * ch, th * 0.7), (0.5 * ch, 0.0), (0.3 * ch, -th * 0.4), (-0.2 * ch, -th * 0.5)]
            br.append(np.array([[px * ct - pz * st_, sign * rad, px * st_ + pz * ct - 0.08] for (px, pz) in prof], dtype=np.float32))
        blade_geo.add_loft(br, [i / 3.0 for i in range(4)], close_caps=True)
        tr = []
        for rad, ch, th, twist in [(0.79, 0.056, 0.007, 14.0), (0.91, 0.042, 0.005, 11.0), (0.95, 0.022, 0.003, 10.0)]:
            tw = math.radians(twist * sign); ct, st_ = math.cos(tw), math.sin(tw)
            prof = [(-0.5 * ch, 0.0), (0.0, th), (0.5 * ch, 0.0), (0.0, -th * 0.5)]
            tr.append(np.array([[px * ct - pz * st_, sign * rad, px * st_ + pz * ct - 0.08] for (px, pz) in prof], dtype=np.float32))
        tip_geo.add_loft(tr, [0.0, 0.5, 1.0], close_caps=True)
    # Polished Chrome Spinner (always 100% solid!) + Child PropBlades (softens smoothly into PropBlurDisc at cruise RPM!)
    prop_hub = gb.add_node("PropHub", gb.add_mesh("PropHub", [spin_geo.to_prim(m_chrome)]), translation=(0.16, -0.50, -2.64), parent=root)
    gb.add_node("PropBlades", gb.add_mesh("PropBlades", [blade_geo.to_prim(m_prop), tip_geo.to_prim(m_yellow)]), parent=prop_hub)

    disc_geo = GeoPart()
    R_P = 0.96
    disc_geo.add_quad((-R_P, -R_P, -0.08), (R_P, -R_P, -0.08), (R_P, R_P, -0.08), (-R_P, R_P, -0.08), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0, -1))
    gb.add_node("PropBlurDisc", gb.add_mesh("PropBlurDisc", [disc_geo.to_prim(m_blur)]), translation=(0.16, -0.50, -2.64), parent=root)

    gb.save(out_path)


# =============================================================================
# STUDY-LEVEL 3D COCKPIT INSTRUMENT & AVIONICS TEXTURE BAKERS (CESSNA 172S)
# =============================================================================
def _bake_gauge_dial_atlas() -> tuple[Image.Image, Image.Image, Image.Image]:
    """
    Bake 3 crisp high-resolution cockpit textures:
      1. 1536x1536 3x3 Steam Gauge Face Atlas (512px/dial: ASI, AI_Frame, ALT, TC, HDG_Frame, VSI, RPM, VOR, FUEL_OIL)
      2. 1024x512 Rotating Card Atlas (512px/card: Left=Attitude Horizon Ball Card, Right=Directional Gyro Compass Rose)
      3. 1024x1536 Center Avionics Stack (Annunciator, GMA340 Audio, Garmin GNS430 GPS/NAV/COM, KX155A, GTX327)
    """
    S = 512
    K = S / 340.0
    def sc(v: float) -> float: return v * K
    def si(v: float) -> int: return max(1, int(round(v * K)))

    atlas = Image.new("RGB", (S * 3, S * 3), (18, 20, 24))
    draw = ImageDraw.Draw(atlas)
    f_lg = _load_font(44)
    f_md = _load_font(32)
    f_sm = _load_font(24)
    f_xs = _load_font(21)

    def cell_center(col: int, row: int) -> tuple[int, int]:
        return col * S + S // 2, row * S + S // 2

    def draw_base_dial(cx: int, cy: int, rad: float = 154.0) -> None:
        r = sc(rad)
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(16, 18, 22), outline=(72, 76, 84), width=si(4))

    def tick(cx: int, cy: int, deg_cw_from_top: float, r0: float, r1: float, col=(238, 240, 234), w: int = 3) -> None:
        a = math.radians(deg_cw_from_top - 90.0)
        draw.line([(cx + math.cos(a) * sc(r0), cy + math.sin(a) * sc(r0)),
                   (cx + math.cos(a) * sc(r1), cy + math.sin(a) * sc(r1))], fill=col, width=si(w))

    def label_r(cx: int, cy: int, deg_cw_from_top: float, rad: float, txt: str, font, col=(244, 246, 240)) -> None:
        a = math.radians(deg_cw_from_top - 90.0)
        x = cx + math.cos(a) * sc(rad)
        y = cy + math.sin(a) * sc(rad)
        draw.text((x, y), txt, fill=col, font=font, anchor="mm")

    # (0,0) AIRSPEED INDICATOR (40..160 KT -> 25..335 deg CW)
    cx, cy = cell_center(0, 0)
    draw_base_dial(cx, cy)
    def asi_ang(kt: float) -> float: return 25.0 + (kt - 40.0) / 120.0 * 310.0
    for kt0, kt1, col, rad_a, w_a in [
        (40, 85, (235, 240, 245), 130, 8),
        (48, 129, (40, 195, 85), 138, 9),
        (129, 160, (238, 196, 36), 138, 9),
    ]:
        pts = [(cx + math.cos(math.radians(asi_ang(k) - 90)) * sc(rad_a),
                cy + math.sin(math.radians(asi_ang(k) - 90)) * sc(rad_a)) for k in range(kt0, kt1 + 1, 2)]
        if len(pts) > 1: draw.line(pts, fill=col, width=si(w_a))
    tick(cx, cy, asi_ang(158), 116, 146, (235, 42, 36), 6)  # Vne Redline
    for kt in range(40, 161, 10):
        a = asi_ang(kt)
        tick(cx, cy, a, 122 if kt % 20 == 0 else 132, 145, w=4 if kt % 20 == 0 else 2)
        if kt % 20 == 0: label_r(cx, cy, a, 98, str(kt), f_lg)
    draw.text((cx, cy - sc(38)), "AIRSPEED", fill=(218, 224, 232), font=f_sm, anchor="mm")
    draw.text((cx, cy + sc(44)), "KNOTS", fill=(218, 224, 232), font=f_sm, anchor="mm")

    # (1,0) ATTITUDE INDICATOR BEZEL OVERLAY (transparent cutout ring / fixed orange aircraft reference)
    cx, cy = cell_center(1, 0)
    draw_base_dial(cx, cy)
    draw.pieslice([cx - sc(142), cy - sc(142), cx + sc(142), cy + sc(142)], 180, 360, fill=(44, 128, 210))
    draw.pieslice([cx - sc(142), cy - sc(142), cx + sc(142), cy + sc(142)], 0, 180, fill=(116, 74, 38))
    draw.line([(cx - sc(142), cy), (cx + sc(142), cy)], fill=(248, 248, 245), width=si(4))
    for deg in (-60, -45, -30, -20, -10, 0, 10, 20, 30, 45, 60):
        tick(cx, cy, deg, 122 if deg % 30 == 0 else 132, 146, (250, 220, 75) if deg == 0 else (240, 244, 248), 4 if deg % 30 == 0 else 2)
    draw.line([(cx - sc(64), cy), (cx - sc(16), cy), (cx - sc(10), cy + sc(12))], fill=(252, 168, 28), width=si(7))
    draw.line([(cx + sc(64), cy), (cx + sc(16), cy), (cx + sc(10), cy + sc(12))], fill=(252, 168, 28), width=si(7))
    draw.ellipse([cx - sc(6), cy - sc(6), cx + sc(6), cy + sc(6)], fill=(252, 168, 28))

    # (2,0) ALTIMETER (0..9 -> 0..360 deg CW)
    cx, cy = cell_center(2, 0)
    draw_base_dial(cx, cy)
    for n in range(50):
        a = n * 7.2
        is_maj = (n % 5 == 0)
        tick(cx, cy, a, 120 if is_maj else 134, 146, w=4 if is_maj else 2)
        if is_maj: label_r(cx, cy, a, 98, str(n // 5), f_lg)
    draw.rectangle([cx + sc(46), cy - sc(16), cx + sc(116), cy + sc(16)], fill=(24, 28, 34), outline=(145, 150, 158), width=si(2))
    draw.text((cx + sc(81), cy), "1018", fill=(248, 250, 242), font=f_sm, anchor="mm")
    draw.text((cx, cy - sc(42)), "ALT", fill=(224, 228, 234), font=f_md, anchor="mm")
    draw.text((cx, cy + sc(44)), "100 FT", fill=(208, 214, 222), font=f_sm, anchor="mm")

    # (0,1) TURN COORDINATOR
    cx, cy = cell_center(0, 1)
    draw_base_dial(cx, cy)
    for a in (-90, -110, 90, 110):
        tick(cx, cy, a, 116, 144, (245, 245, 240), 5)
    draw.text((cx - sc(94), cy + sc(42)), "L", fill=(244, 246, 242), font=f_md, anchor="mm")
    draw.text((cx + sc(94), cy + sc(42)), "R", fill=(244, 246, 242), font=f_md, anchor="mm")
    draw.rounded_rectangle([cx - sc(64), cy + sc(56), cx + sc(64), cy + sc(84)], radius=si(12), fill=(224, 228, 214), outline=(90, 95, 102), width=si(2))
    draw.line([(cx - sc(14), cy + sc(56)), (cx - sc(14), cy + sc(84))], fill=(25, 28, 32), width=si(3))
    draw.line([(cx + sc(14), cy + sc(56)), (cx + sc(14), cy + sc(84))], fill=(25, 28, 32), width=si(3))
    draw.ellipse([cx - sc(11), cy + sc(59), cx + sc(11), cy + sc(81)], fill=(20, 22, 26))
    draw.text((cx, cy - sc(64)), "TURN COORDINATOR", fill=(216, 222, 228), font=f_xs, anchor="mm")
    draw.text((cx, cy + sc(106)), "2 MIN", fill=(212, 218, 224), font=f_sm, anchor="mm")

    # (1,1) HEADING INDICATOR FRAME (subtle lubber mark; rotating Card_HDG sits just in front!)
    cx, cy = cell_center(1, 1)
    draw_base_dial(cx, cy)
    draw.polygon([(cx, cy - sc(120)), (cx - sc(12), cy - sc(146)), (cx + sc(12), cy - sc(146))], fill=(252, 132, 28))

    # (2,1) VERTICAL SPEED INDICATOR (0 at 9 o'clock = 270 deg; +2000 at 3 o'clock = 90 deg!)
    cx, cy = cell_center(2, 1)
    draw_base_dial(cx, cy)
    def vsi_ang(hfpm: float) -> float: return 270.0 + (hfpm / 20.0) * 165.0
    for v in range(-20, 21, 1):
        a = vsi_ang(v)
        is_maj = (v % 5 == 0)
        tick(cx, cy, a, 122 if is_maj else 134, 146, w=4 if is_maj else 2)
        if is_maj and abs(v) <= 20:
            lbl = "0" if v == 0 else str(abs(v))
            label_r(cx, cy, a, 98, lbl, f_lg if abs(v) != 20 else f_md)
    draw.text((cx, cy - sc(42)), "UP", fill=(224, 230, 236), font=f_sm, anchor="mm")
    draw.text((cx, cy + sc(44)), "DN", fill=(224, 230, 236), font=f_sm, anchor="mm")
    draw.text((cx + sc(22), cy), "VERT\nSPEED", fill=(208, 214, 222), font=f_xs, anchor="mm", align="center")

    # (0,2) TACHOMETER RPM (0..35 x100 RPM -> 230..130 deg CW = 260 deg sweep)
    cx, cy = cell_center(0, 2)
    draw_base_dial(cx, cy)
    def rpm_ang(r100: float) -> float: return 230.0 + (r100 / 35.0) * 260.0
    pts = [(cx + math.cos(math.radians(rpm_ang(r) - 90)) * sc(136),
            cy + math.sin(math.radians(rpm_ang(r) - 90)) * sc(136)) for r in range(21, 28)]
    draw.line(pts, fill=(40, 200, 88), width=si(10))
    tick(cx, cy, rpm_ang(27), 116, 146, (238, 42, 36), 6)
    for r100 in range(0, 36, 1):
        a = rpm_ang(r100)
        is_maj = (r100 % 5 == 0)
        tick(cx, cy, a, 122 if is_maj else 134, 146, w=4 if is_maj else 2)
        if is_maj: label_r(cx, cy, a, 98, str(r100), f_md)
    draw.text((cx, cy - sc(38)), "RPM x100", fill=(222, 228, 234), font=f_sm, anchor="mm")
    draw.rectangle([cx - sc(50), cy + sc(48), cx + sc(50), cy + sc(76)], fill=(10, 12, 15), outline=(120, 126, 134), width=si(2))
    draw.text((cx, cy + sc(62)), "01482.6", fill=(244, 246, 238), font=f_sm, anchor="mm")

    # (1,2) VOR1 / CDI NAV INDICATOR
    cx, cy = cell_center(1, 2)
    draw_base_dial(cx, cy)
    for d in range(0, 360, 10):
        tick(cx, cy, d, 126 if d % 30 == 0 else 136, 146, w=3 if d % 30 == 0 else 2)
    for d, t in [(0, "N"), (90, "E"), (180, "S"), (270, "W"), (30, "3"), (60, "6"), (120, "12"), (150, "15"), (210, "21"), (240, "24"), (300, "30"), (330, "33")]:
        label_r(cx, cy, d, 108, t, f_sm)
    for k in range(-4, 5):
        draw.ellipse([cx + sc(k * 18 - 4), cy - sc(4), cx + sc(k * 18 + 4), cy + sc(4)], fill=(238, 240, 234))
    draw.line([(cx + sc(6), cy - sc(78)), (cx + sc(6), cy + sc(78))], fill=(248, 248, 242), width=si(5))
    draw.polygon([(cx + sc(34), cy - sc(28)), (cx + sc(22), cy - sc(12)), (cx + sc(46), cy - sc(12))], fill=(238, 238, 228))
    draw.text((cx - sc(42), cy - sc(20)), "NAV", fill=(218, 224, 232), font=f_sm, anchor="mm")

    # (2,2) DUAL ENGINE CLUSTER (FUEL L/R + OIL TEMP/PRESS)
    cx, cy = cell_center(2, 2)
    draw_base_dial(cx, cy)
    draw.line([(cx, cy - sc(138)), (cx, cy + sc(138))], fill=(82, 88, 96), width=si(3))
    draw.text((cx - sc(64), cy - sc(68)), "FUEL", fill=(234, 238, 244), font=f_md, anchor="mm")
    draw.text((cx + sc(64), cy - sc(68)), "OIL", fill=(234, 238, 244), font=f_md, anchor="mm")
    for sx, val_y in [(-64, -22), (64, -18)]:
        draw.rectangle([cx + sc(sx - 13), cy - sc(44), cx + sc(sx + 13), cy + sc(56)], fill=(28, 32, 38), outline=(105, 110, 118), width=si(2))
        draw.rectangle([cx + sc(sx - 9), cy + sc(val_y), cx + sc(sx + 9), cy + sc(52)], fill=(42, 204, 94))
    draw.text((cx - sc(64), cy + sc(84)), "L / R", fill=(224, 230, 236), font=f_sm, anchor="mm")
    draw.text((cx + sc(64), cy + sc(84)), "T / P", fill=(224, 230, 236), font=f_sm, anchor="mm")

    # --- 2. ROTATING CARDS TEXTURE (1024x512: Left=AI Horizon Ball, Right=HDG Compass Rose) ---
    cards = Image.new("RGB", (1024, 512), (18, 20, 24))
    cd = ImageDraw.Draw(cards)
    fc_md = _load_font(42)
    fc_sm = _load_font(22)
    # Left 512x512: Attitude Horizon Card (sky top, brown earth bottom, crisp white pitch ladder!)
    cd.rectangle([0, 0, 511, 256], fill=(38, 134, 218))
    cd.rectangle([0, 256, 511, 511], fill=(122, 74, 34))
    cd.line([(0, 256), (511, 256)], fill=(252, 252, 248), width=7)
    for dp, w_bar in [(-20, 76), (-10, 52), (-5, 28), (5, 28), (10, 52), (20, 76)]:
        y_l = 256 - int(dp * 6.0)
        cd.line([(256 - w_bar, y_l), (256 + w_bar, y_l)], fill=(248, 248, 242), width=5)
        if abs(dp) in (10, 20):
            cd.text((256 - w_bar - 20, y_l), str(abs(dp)), fill=(246, 246, 240), font=fc_sm, anchor="mm")
            cd.text((256 + w_bar + 20, y_l), str(abs(dp)), fill=(246, 246, 240), font=fc_sm, anchor="mm")
    # Right 512x512: Directional Gyro Rotating Compass Rose!
    hcx, hcy = 768, 256
    cd.ellipse([hcx - 244, hcy - 244, hcx + 244, hcy + 244], fill=(16, 18, 22), outline=(88, 94, 102), width=6)
    for d in range(0, 360, 5):
        a = math.radians(d - 90.0)
        r0 = 192 if d % 30 == 0 else (210 if d % 10 == 0 else 222)
        cd.line([(hcx + math.cos(a) * r0, hcy + math.sin(a) * r0), (hcx + math.cos(a) * 236, hcy + math.sin(a) * 236)],
                fill=(244, 246, 240), width=6 if d % 30 == 0 else 4)
    for d, t in [(0, "N"), (30, "3"), (60, "6"), (90, "E"), (120, "12"), (150, "15"), (180, "S"), (210, "21"), (240, "24"), (270, "W"), (300, "30"), (330, "33")]:
        a = math.radians(d - 90.0)
        cd.text((hcx + math.cos(a) * 150, hcy + math.sin(a) * 150), t,
                fill=(252, 204, 74) if t in ("N", "E", "S", "W") else (244, 246, 242), font=fc_md, anchor="mm")

    # --- 3. CENTER AVIONICS STACK TEXTURE (1024x1536: Annunciator + GMA340 + GNS430 + KX155A + GTX327) ---
    av = Image.new("RGB", (1024, 1536), (22, 25, 30))
    ad = ImageDraw.Draw(av)
    fa_lg = _load_font(56)
    fa_md = _load_font(40)
    fa_sm = _load_font(30)
    fa_xs = _load_font(26)
    # Top Annunciator Bar (0..128)
    ad.rectangle([16, 16, 1008, 116], fill=(14, 16, 20), outline=(76, 82, 92), width=6)
    for i, (lbl, col) in enumerate([("VOLTS", (62, 66, 72)), ("OIL P", (62, 66, 72)), ("FUEL", (62, 66, 72)), ("VAC", (62, 66, 72)), ("VFR OK", (52, 232, 118))]):
        x0 = 36 + i * 192
        ad.rectangle([x0, 32, x0 + 176, 100], fill=(10, 12, 16), outline=(62, 68, 76), width=2)
        ad.text((x0 + 88, 66), lbl, fill=col, font=fa_sm, anchor="mm")

    # GMA 340 Audio Panel (144..288)
    ad.rectangle([16, 144, 1008, 288], fill=(28, 32, 38), outline=(82, 88, 98), width=6)
    ad.text((132, 184), "GMA 340", fill=(222, 228, 236), font=fa_sm, anchor="mm")
    for i, lbl in enumerate(["COM1", "COM2", "NAV1", "MKR", "SPKR", "ICS"]):
        bx = 264 + i * 120
        ad.rectangle([bx, 168, bx + 100, 260], fill=(18, 22, 26), outline=(98, 104, 114), width=4)
        if i in (0, 2, 5):
            ad.rectangle([bx + 16, 180, bx + 84, 196], fill=(56, 242, 122))
        ad.text((bx + 50, 228), lbl, fill=(222, 228, 236), font=fa_xs, anchor="mm")

    # GARMIN GNS 430 GPS / NAV / COM (312..876)
    ad.rectangle([16, 312, 1008, 876], fill=(26, 30, 36), outline=(92, 100, 114), width=6)
    ad.text((156, 348), "GNS 430", fill=(236, 242, 248), font=fa_sm, anchor="mm")
    ad.rectangle([48, 384, 824, 792], fill=(8, 18, 28), outline=(112, 124, 140), width=6)
    ad.rectangle([56, 392, 312, 784], fill=(10, 22, 36), outline=(52, 86, 116), width=4)
    ad.text((184, 424), "COM", fill=(122, 206, 252), font=fa_sm, anchor="mm")
    ad.text((184, 480), "123.50", fill=(80, 250, 150), font=fa_md, anchor="mm")
    ad.text((184, 532), "118.40", fill=(184, 216, 236), font=fa_sm, anchor="mm")
    ad.line([(64, 572), (304, 572)], fill=(52, 86, 116), width=4)
    ad.text((184, 608), "VLOC", fill=(122, 206, 252), font=fa_sm, anchor="mm")
    ad.text((184, 664), "114.50", fill=(80, 250, 150), font=fa_md, anchor="mm")
    ad.text((184, 720), "LECI", fill=(252, 212, 82), font=fa_sm, anchor="mm")
    # Right Moving Map Terrain + Magenta Pyrenees VFR Track!
    ad.rectangle([324, 392, 816, 784], fill=(14, 34, 24))
    ad.polygon([(340, 740), (436, 580), (550, 640), (680, 500), (804, 570), (804, 776), (340, 776)], fill=(34, 64, 38))
    ad.line([(420, 750), (564, 584), (710, 448)], fill=(248, 72, 238), width=10)
    ad.polygon([(564, 564), (544, 604), (584, 604)], fill=(252, 252, 252))
    ad.text((688, 424), "ES-0205", fill=(252, 222, 88), font=fa_sm, anchor="mm")
    ad.text((568, 754), "RNG 15 NM  GPS", fill=(92, 238, 252), font=fa_sm, anchor="mm")
    # Softkeys on right bezel (shifted slightly up so lower 3D rotary knob stays clear of ENT)
    for ki, kl in enumerate(["RNG", "D->", "MENU", "CLR", "ENT"]):
        ky = 388 + ki * 72
        ad.rounded_rectangle([848, ky, 984, ky + 56], radius=10, fill=(38, 42, 50), outline=(118, 124, 134), width=4)
        ad.text((916, ky + 28), kl, fill=(236, 240, 246), font=fa_sm, anchor="mm")

    # KX 155A NAV/COM 2 (904..1176)
    ad.rectangle([16, 904, 1008, 1176], fill=(26, 30, 36), outline=(86, 92, 104), width=6)
    ad.rectangle([52, 944, 972, 1080], fill=(22, 10, 8), outline=(96, 66, 60), width=4)
    ad.text((276, 1012), "121.50   119.30", fill=(252, 114, 48), font=fa_md, anchor="mm")
    ad.text((756, 1012), "113.00   108.90", fill=(252, 114, 48), font=fa_md, anchor="mm")
    ad.text((512, 1128), "KX 155A   COM 2  /  NAV 2", fill=(214, 220, 230), font=fa_sm, anchor="mm")

    # GTX 327 Digital Transponder (1204..1512)
    ad.rectangle([16, 1204, 1008, 1512], fill=(26, 30, 36), outline=(86, 92, 104), width=6)
    ad.rectangle([56, 1244, 676, 1392], fill=(20, 16, 10), outline=(112, 98, 66), width=4)
    ad.text((156, 1316), "ALT", fill=(252, 208, 68), font=fa_md, anchor="mm")
    ad.text((448, 1316), "7000", fill=(255, 222, 78), font=fa_lg, anchor="mm")
    ad.text((840, 1316), "GTX 327\nVFR XPDR", fill=(218, 224, 234), font=fa_sm, anchor="mm", align="center")
    for i in range(8):
        bx = 56 + i * 116
        ad.rectangle([bx, 1416, bx + 96, 1484], fill=(42, 46, 54), outline=(118, 124, 134), width=2)
        ad.text((bx + 48, 1450), str(i), fill=(242, 244, 248), font=fa_sm, anchor="mm")

    return atlas, cards, av


def _bake_cockpit_panel_face_pbr(
    x_min: float, x_max: float, y_min: float, y_max: float,
    gauges: list[tuple[float, float, float]],
    av_rect: tuple[float, float, float, float],
    brow_y_fn,
) -> tuple[Image.Image, Image.Image, Image.Image]:
    """
    2560x768 (square-texel 3.33:1 matching the 1.44x0.405m panel!) Cessna 172S Skyhawk SP main faceplate PBR baker:
      - Authentic two-tone molded aviation interior (warm dove/taupe surround + crinkle-coat charcoal sub-panel)
      - Real baked curved glareshield cast shadow + radial contact AO around every bezel, switch & avionics bay
      - Crisp unstretched engraved white aviation labels, EC-VFR limitations placard, Davtron OAT/Volts & VFR checklist!
    """
    W, H = 2560, 768
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    wx = x_min + (xx / (W - 1)) * (x_max - x_min)
    wy = y_max - (yy / (H - 1)) * (y_max - y_min)  # v=0 top -> y_max, v=1 bottom -> y_min

    def px(x_m: float) -> int: return int(round((x_m - x_min) / (x_max - x_min) * (W - 1)))
    def py(y_m: float) -> int: return int(round((y_max - y_m) / (y_max - y_min) * (H - 1)))

    # Fine powder-coated aviation crinkle-coat micro-grain (high-frequency + medium grain)
    g1 = np.mod(np.sin(xx * 13.7 + yy * 29.3) * 43758.5, 1.0) - 0.5
    g2 = np.mod(np.sin((xx // 2) * 23.1 + (yy // 2) * 17.9) * 19384.2, 1.0) - 0.5
    crinkle = g1 * 0.55 + g2 * 0.45

    # Warm Cessna dove-taupe textured cabin surround (#4B4945) vs dark charcoal molded instrument fascia (#1E2126)
    r = np.full((H, W), 0.34, dtype=np.float32) + crinkle * 0.025
    g = np.full((H, W), 0.33, dtype=np.float32) + crinkle * 0.024
    b = np.full((H, W), 0.31, dtype=np.float32) + crinkle * 0.022
    rough = np.full((H, W), 0.62, dtype=np.float32) + crinkle * 0.06
    metal = np.full((H, W), 0.14, dtype=np.float32)
    hmap = np.full((H, W), 0.50, dtype=np.float32) + crinkle * 0.05

    # Molded Cessna Pilot + Avionics Upper Fascia Overlay (smoothly raised dark-charcoal sub-panel)
    in_fascia = (wx > -0.455) & (wx < 0.495) & (wy > -0.112) & (wy < 0.165)
    in_lower_sw = (wx > -0.455) & (wx < 0.495) & (wy > -0.195) & (wy <= -0.112)
    r = np.where(in_fascia, 0.145 + crinkle * 0.020, r)
    g = np.where(in_fascia, 0.155 + crinkle * 0.020, g)
    b = np.where(in_fascia, 0.170 + crinkle * 0.020, b)
    rough = np.where(in_fascia, 0.48 + crinkle * 0.05, rough)
    metal = np.where(in_fascia, 0.28, metal)
    hmap = np.where(in_fascia, hmap + 0.14, hmap)

    r = np.where(in_lower_sw, 0.18 + crinkle * 0.020, r)
    g = np.where(in_lower_sw, 0.19 + crinkle * 0.020, g)
    b = np.where(in_lower_sw, 0.205 + crinkle * 0.020, b)
    hmap = np.where(in_lower_sw, hmap + 0.06, hmap)

    # REAL BAKED AMBIENT OCCLUSION:
    # 1. Curved under-glareshield cast shadow following exact S-curve brow_y_fn(wx)!
    brow_y = np.vectorize(brow_y_fn)(wx[0, :])[None, :]
    dist_below_brow = np.maximum(0.0, brow_y - wy)
    glareshield_ao = 1.0 - 0.54 * np.exp(-dist_below_brow / 0.048)

    # 2. Soft radial contact AO shadow well around EVERY circular 3D gauge bezel!
    ao_map = glareshield_ao.copy()
    for gx, gy, gr in gauges:
        dd = np.sqrt((wx - gx) ** 2 + (wy - gy) ** 2)
        ring_ao = np.exp(-np.maximum(0.0, dd - gr * 1.05) / 0.015) * 0.48
        ao_map *= (1.0 - ring_ao)
    # 3. Avionics bay & yoke collar AO shadows
    ax0, ax1, ay0, ay1 = av_rect
    adx = np.maximum(0.0, np.maximum(ax0 - wx, wx - ax1))
    ady = np.maximum(0.0, np.maximum(ay0 - wy, wy - ay1))
    ao_map *= (1.0 - 0.44 * np.exp(-np.hypot(adx, ady) / 0.016))
    for yx in (-0.108, 0.455):
        dyk = np.sqrt((wx - yx) ** 2 + (wy - (-0.175)) ** 2)
        ao_map *= (1.0 - 0.55 * np.exp(-np.maximum(0.0, dyk - 0.028) / 0.022))

    rgb = np.stack([np.clip(r * ao_map, 0.02, 0.95), np.clip(g * ao_map, 0.02, 0.95), np.clip(b * ao_map, 0.02, 0.95)], axis=-1)
    alb = Image.fromarray((rgb * 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(alb)
    f_sw = _load_font(14)
    f_xs = _load_font(17)
    f_sm = _load_font(20)
    f_md = _load_font(24)
    f_lg = _load_font(32)

    # Molded bevel outlines around sub-panels (clean highlight + shadow groove)
    draw.rounded_rectangle([px(-0.455), py(0.155), px(0.195), py(-0.106)], radius=14, outline=(72, 78, 88), width=3)
    draw.rounded_rectangle([px(-0.452), py(0.152), px(0.192), py(-0.103)], radius=12, outline=(18, 20, 24), width=2)
    draw.rounded_rectangle([px(0.206), py(0.146), px(0.490), py(-0.106)], radius=10, outline=(72, 78, 88), width=3)
    draw.line([(px(-0.455), py(-0.112)), (px(0.490), py(-0.112))], fill=(88, 94, 104), width=2)

    # Top-left engraved red/black/white Aircraft Registration & V-Speeds Limitations Placard (clear of brow & bezels!)
    pl_x0, pl_y0, pl_x1, pl_y1 = px(-0.440), py(0.139), px(0.155), py(0.120)
    draw.rounded_rectangle([pl_x0, pl_y0, pl_x1, pl_y1], radius=4, fill=(18, 20, 24), outline=(156, 162, 170), width=2)
    draw.rectangle([pl_x0 + 4, pl_y0 + 4, pl_x0 + 146, pl_y1 - 4], fill=(172, 28, 30))
    draw.text((pl_x0 + 75, (pl_y0 + pl_y1) // 2), "EC-VFR", fill=(250, 250, 246), font=f_md, anchor="mm")
    draw.text(((pl_x0 + 152 + pl_x1) // 2, (pl_y0 + pl_y1) // 2), "CESSNA 172S SKYHAWK SP  •  VNE 163  VA 105  VFE 85 KT", fill=(236, 240, 246), font=f_sm, anchor="mm")

    # Lower Switch Bar Silkscreen Labels (cleanly spaced over 3D Ignition, Master, Toggles & Circuit Breakers!)
    draw.text((px(-0.422), py(-0.118)), "MAGS", fill=(228, 234, 240), font=f_sw, anchor="mm")
    draw.text((px(-0.362), py(-0.118)), "ALT BAT", fill=(248, 126, 118), font=f_sw, anchor="mm")
    sw_labels = ["AVN", "PUMP", "BCN", "LDG", "TAXI", "NAV", "STRB", "PITOT"]
    for i, lbl in enumerate(sw_labels):
        sx = -0.312 + i * 0.032
        draw.text((px(sx), py(-0.118)), lbl, fill=(230, 236, 244), font=f_sw, anchor="mm")
    draw.text((px(0.350), py(-0.120)), "CIRCUIT BREAKERS  (28V DC)", fill=(214, 222, 232), font=f_xs, anchor="mm")

    # Throttle / Mixture / Flaps Engraved Collar Labels!
    draw.text((px(0.050), py(-0.146)), "ALT AIR", fill=(232, 238, 244), font=f_xs, anchor="mm")
    draw.text((px(0.120), py(-0.146)), "THROTTLE", fill=(246, 248, 252), font=f_xs, anchor="mm")
    draw.text((px(0.190), py(-0.146)), "MIXTURE", fill=(252, 134, 126), font=f_xs, anchor="mm")
    draw.text((px(0.268), py(-0.146)), "FLAPS", fill=(242, 246, 250), font=f_xs, anchor="mm")
    for fi, flbl in enumerate(["UP", "10", "20", "30"]):
        draw.text((px(0.296), py(-0.160 - fi * 0.014)), flbl, fill=(230, 236, 244), font=f_xs, anchor="lm")

    # Co-Pilot Side Panel: Brushed Skyhawk SP Emblem (positioned cleanly below curved glareshield brow!)
    draw.rounded_rectangle([px(0.515), py(0.112), px(0.825), py(0.064)], radius=8, fill=(26, 30, 36), outline=(128, 136, 148), width=3)
    draw.text((px(0.670), py(0.094)), "SKYHAWK  SP", fill=(240, 244, 250), font=f_lg, anchor="mm")
    draw.text((px(0.670), py(0.074)), "180 HP  •  LYCOMING IO-360", fill=(200, 208, 220), font=f_sm, anchor="mm")
    # Digital Davtron-style OAT / Volts / UTC Mini Display
    draw.rounded_rectangle([px(0.515), py(0.054), px(0.648), py(0.006)], radius=6, fill=(10, 14, 18), outline=(104, 112, 124), width=3)
    draw.text((px(0.581), py(0.039)), "OAT +16C  28.2V", fill=(255, 194, 64), font=f_sm, anchor="mm")
    draw.text((px(0.581), py(0.019)), "UTC 10:42  1018", fill=(248, 210, 88), font=f_sm, anchor="mm")
    # Clipped Cream Northern Spain VFR Checklist Card on Co-Pilot Side
    ck_x0, ck_y0, ck_x1, ck_y1 = px(0.665), py(0.054), px(0.830), py(-0.092)
    draw.rounded_rectangle([ck_x0, ck_y0, ck_x1, ck_y1], radius=6, fill=(220, 214, 200), outline=(78, 74, 66), width=3)
    draw.rectangle([ck_x0 + 4, ck_y0 + 4, ck_x1 - 4, ck_y0 + 38], fill=(24, 48, 88))
    draw.text(((ck_x0 + ck_x1) // 2, ck_y0 + 21), "ARAGON VFR CHECK", fill=(248, 248, 242), font=f_sm, anchor="mm")
    for ci, cline in enumerate(["MIX ..... RICH", "FUEL .... BOTH", "FLAPS ... 10-30", "FINAL ... 65 KT", "LECI .... 123.5", "ES-0205 . 900m"]):
        draw.text((ck_x0 + 20, ck_y0 + 50 + ci * 36), cline, fill=(22, 24, 28), font=f_md)
    # Recessed Co-Pilot Glovebox Door Contour
    draw.rounded_rectangle([px(0.515), py(-0.014), px(0.648), py(-0.134)], radius=10, fill=(48, 47, 45), outline=(22, 23, 26), width=4)
    draw.rounded_rectangle([px(0.519), py(-0.018), px(0.644), py(-0.130)], radius=8, outline=(96, 94, 88), width=2)

    nrm = _height_to_normal(hmap, 2.4)
    # ao_map is already baked into alb; keep orm.r gentle (0.68..1.0) so PBR doesn't double-darken labels
    orm = Image.fromarray(np.stack([
        (np.clip(0.68 + 0.32 * ao_map, 0, 1) * 255).astype(np.uint8),
        (np.clip(rough, 0, 1) * 255).astype(np.uint8),
        (np.clip(metal, 0, 1) * 255).astype(np.uint8),
    ], axis=-1), "RGB")
    return alb, nrm, orm


def build_c172s_cockpit_glb(out_path: Path) -> None:
    """Photorealistic Study-Level Cessna 172S 3D Cockpit: Curved Stitched Leather Glareshield, 2K Engraved Panel,
    3D Machined Bezels + Glass Lenses + Corner Knobs, Organic Lofted Yokes, Enclosed Cabin & Visible High Wings!"""
    gb = GLBBuilder()

    # 1. Pebbled Charcoal Stitched Leather Glareshield Texture (512x512 Albedo + Normal)
    L = 512
    yy, xx = np.mgrid[0:L, 0:L].astype(np.float32)
    h1 = np.mod(np.sin(xx * 19.3 + yy * 43.7) * 9182.3, 1.0)
    h2 = np.mod(np.sin((xx // 3) * 31.1 + (yy // 3) * 19.9) * 5182.7, 1.0)
    pebble = h1 * 0.45 + h2 * 0.55
    # Double contrast French-stitch seam running neatly along front leather bolster (v ~ 0.36)
    stitch_band = (np.abs(yy / L - 0.36) < 0.006) | (np.abs(yy / L - 0.39) < 0.006)
    stitch_dash = np.mod(xx / 10.0, 1.0) < 0.55
    stitch = (stitch_band & stitch_dash).astype(np.float32)
    ao_l = 0.72 + 0.28 * np.sin((yy / L) * math.pi)
    gl = (0.115 + pebble * 0.048) * ao_l
    lr = np.clip(gl * (1.0 - stitch) + 0.42 * stitch, 0.02, 0.85)
    lg = np.clip(gl * 1.02 * (1.0 - stitch) + 0.41 * stitch, 0.02, 0.85)
    lb = np.clip(gl * 1.06 * (1.0 - stitch) + 0.39 * stitch, 0.02, 0.85)
    leather_img = Image.fromarray((np.stack([lr, lg, lb], axis=-1) * 255).astype(np.uint8), "RGB")
    leather_nrm = _height_to_normal(pebble * 0.65 + stitch * 0.35, 2.8)

    dial_atlas, cards_tex, av_tex = _bake_gauge_dial_atlas()
    wing_alb, wing_nrm, wing_mr = _build_c172_wing_and_tail_tex()
    prop_veil_img = _build_cockpit_prop_veil_tex()

    # Exact 6-Pack + Engine/Nav Layout inside PanelGroup (tight, large 3-1/8" gauges filling pilot's field cleanly!)
    G_R = 0.047
    ROW_TOP, ROW_BOT = 0.068, -0.042
    COL_L, COL_C, COL_R, COL_E = -0.228, -0.114, 0.000, 0.114
    all_gauges = [
        (COL_L, ROW_TOP, G_R), (COL_C, ROW_TOP, G_R), (COL_R, ROW_TOP, G_R), (COL_E, ROW_TOP, G_R),
        (COL_L, ROW_BOT, G_R), (COL_C, ROW_BOT, G_R), (COL_R, ROW_BOT, G_R), (COL_E, ROW_BOT, G_R),
        (-0.352, ROW_TOP, 0.039), (-0.352, ROW_BOT, 0.039),
    ]
    av_rect = (0.214, 0.482, -0.102, 0.140)
    PX0, PX1, PY0, PY1 = -0.58, 0.86, -0.23, 0.175

    def glareshield_brow_y_world(x_m: float) -> float:
        """Signature Cessna 172S Skyhawk S-curve glareshield dome: higher over pilot 6-pack, swooping smoothly down right."""
        pilot_dome = 0.022 / (1.0 + math.exp((x_m - 0.19) * 14.0))
        edge_tuck = -0.012 * ((max(0.0, abs(x_m - 0.14) - 0.52) / 0.25) ** 2)
        return -0.168 + pilot_dome + edge_tuck

    panel_alb, panel_nrm, panel_orm = _bake_cockpit_panel_face_pbr(
        PX0, PX1, PY0, PY1, all_gauges, av_rect,
        lambda x_m: glareshield_brow_y_world(x_m) - (-0.312),  # relative to PanelGroup y=-0.312
    )

    m_leather = gb.add_material(
        "LeatherDash", (1, 1, 1, 1), metallic=0.04, roughness=0.58,
        base_tex=gb.add_texture_pil(leather_img, quality=92),
        normal_tex=gb.add_texture_pil(leather_nrm, quality=86), normal_scale=0.80,
    )
    m_panel = gb.add_material(
        "PanelFace2K", (1, 1, 1, 1), metallic=0.22, roughness=0.56,
        base_tex=gb.add_texture_pil(panel_alb, fmt="PNG", clamp=True),
        mr_tex=gb.add_texture_pil(panel_orm.resize((512, 192), Image.Resampling.LANCZOS), quality=90, clamp=True),
        emissive=(0.07, 0.07, 0.07),
    )
    m_bezel = gb.add_material("GaugeBezel", (0.068, 0.072, 0.080, 1.0), metallic=0.62, roughness=0.28)
    m_oatmeal = gb.add_material("CessnaOatmealTrim", (0.58, 0.55, 0.50, 1.0), metallic=0.02, roughness=0.84)
    m_rubber = gb.add_material("WindowRubberSeal", (0.048, 0.050, 0.055, 1.0), metallic=0.02, roughness=0.88)
    m_carpet = gb.add_material("CabinCarpet", (0.085, 0.088, 0.095, 1.0), metallic=0.0, roughness=0.95)
    m_headliner = gb.add_material("Headliner", (0.64, 0.62, 0.58, 1.0), metallic=0.0, roughness=0.90)
    m_metal = gb.add_material("BrushedMetal", (0.84, 0.86, 0.90, 1.0), metallic=0.92, roughness=0.16)
    m_yoke = gb.add_material(
        "YokeLeather", (0.72, 0.72, 0.74, 1.0), metallic=0.08, roughness=0.46,
        base_tex=gb.add_texture_pil(leather_img, quality=88),
        normal_tex=gb.add_texture_pil(leather_nrm, quality=82), normal_scale=0.55,
    )
    m_red = gb.add_material("KnobRed", (0.82, 0.11, 0.10, 1.0), metallic=0.14, roughness=0.26)
    m_needle = gb.add_material("GaugeNeedle", (0.98, 0.96, 0.90, 1.0), roughness=0.24, emissive=(0.48, 0.45, 0.38))
    m_orange = gb.add_material("GaugeOrange", (0.98, 0.56, 0.08, 1.0), roughness=0.24, emissive=(0.72, 0.40, 0.05))
    m_cream = gb.add_material("CowlWhite", (0.86, 0.87, 0.85, 1.0), metallic=0.24, roughness=0.24)
    m_glare_blk = gb.add_material("CowlGlareBlack", (0.075, 0.080, 0.088, 1.0), metallic=0.04, roughness=0.88)
    m_visor = gb.add_material("SunVisor", (0.04, 0.13, 0.07, 0.34), metallic=0.12, roughness=0.08, alpha_mode="BLEND")
    m_gauge_glass = gb.add_material("GaugeGlass", (0.70, 0.82, 0.92, 0.09), metallic=0.10, roughness=0.03, alpha_mode="BLEND")
    m_compass = gb.add_material("CompassDial", (0.92, 0.94, 0.86, 1.0), roughness=0.28, emissive=(0.38, 0.44, 0.32))
    m_dials = gb.add_material(
        "SteamDials", (1, 1, 1, 1), metallic=0.05, roughness=0.24,
        base_tex=gb.add_texture_pil(dial_atlas, fmt="PNG", clamp=True),
        emissive=(0.20, 0.20, 0.18),
    )
    m_cards = gb.add_material(
        "RotatingCards", (1, 1, 1, 1), metallic=0.05, roughness=0.24,
        base_tex=gb.add_texture_pil(cards_tex, fmt="PNG", clamp=True),
        emissive=(0.22, 0.22, 0.20),
    )
    m_avionics = gb.add_material(
        "AvionicsStack", (1, 1, 1, 1), metallic=0.08, roughness=0.20,
        base_tex=gb.add_texture_pil(av_tex, fmt="PNG", clamp=True),
        emissive=(0.45, 0.45, 0.45),
    )
    m_wing_in = gb.add_material(
        "CockpitHighWing", (1, 1, 1, 1), metallic=0.24, roughness=0.26,
        base_tex=gb.add_texture_pil(wing_alb, quality=88),
        normal_tex=gb.add_texture_pil(wing_nrm, quality=84), normal_scale=0.75,
        mr_tex=gb.add_texture_pil(wing_mr, quality=84),
    )
    m_blur = gb.add_material(
        "CockpitPropVeil", (1, 1, 1, 0.88), metallic=0.05, roughness=0.45,
        base_tex=gb.add_texture_pil(prop_veil_img, fmt="PNG", clamp=True),
        alpha_mode="BLEND", double_sided=True,
    )

    root = gb.add_node("C172S_Cockpit")

    # 1. SCULPTURAL 32-STATION LOFTED CURVED LEATHER GLARESHIELD BROW (authentic Cessna S-curve hood!)
    dash = GeoPart()
    # Cross-section profile in (dy, dz) around brow lip: rolled front bolster -> domed top -> windshield base -> recessed lip underside!
    brow_prof = [
        ( 0.000,  0.000), ( 0.012, -0.004), ( 0.022, -0.018), ( 0.026, -0.052),
        ( 0.016, -0.140), ( 0.000, -0.255), (-0.016, -0.320), (-0.030, -0.300),
        (-0.024, -0.140), (-0.016, -0.032), (-0.014, -0.006), (-0.008,  0.003),
    ]
    gs_rings, gs_u = [], []
    N_GS = 32
    for i in range(N_GS):
        t = i / (N_GS - 1)
        x_m = -0.61 + t * 1.50
        y0 = glareshield_brow_y_world(x_m)
        z_curve = -0.695 - 0.045 * ((abs(x_m - 0.14) / 0.75) ** 2)  # Gentle wrap-around curve toward doors!
        gs_rings.append(np.array([[x_m, y0 + dy, z_curve + dz] for (dy, dz) in brow_prof], dtype=np.float32))
        gs_u.append(t * 3.5)
    dash.add_loft(gs_rings, gs_u, close_caps=True)

    # Molded lower center console & dark carpeted enclosed cabin footwell / firewall!
    carpet = GeoPart()
    carpet.add_box((0.14, -0.82, -0.28), (1.58, 0.04, 1.50), uv_scale=(4, 4))
    carpet.add_box((0.14, -0.58, -0.98), (1.58, 0.56, 0.04), rot_deg=(-8, 0, 0), uv_scale=(4, 2))
    carpet.add_box((0.15, -0.68, -0.58), (0.24, 0.27, 0.46), rot_deg=(10, 0, 0))
    # Lower side kick panels below door sill
    carpet.add_box((-0.62, -0.60, -0.30), (0.04, 0.42, 1.35))
    carpet.add_box(( 0.90, -0.60, -0.30), (0.04, 0.42, 1.35))

    # 2. WARM OATMEAL-VINYL CURVED A-PILLARS, DOOR ARMRESTS, HEADLINER & BLACK RUBBER WINDOW SEALS!
    oatmeal, rubber, metal_shell, headliner, visor = GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart()
    # Sculpted tapered A-pillars (curved sweep hugging windshield) with inner black rubber glass gaskets!
    for sx, x_bot, x_top in [(-1.0, -0.59, -0.52), (1.0, 0.87, 0.80)]:
        oatmeal.add_cyl_between((x_bot, -0.17, -0.89), (x_top, 0.355, -0.56), 0.032, 0.026, 12, aspect_x=0.78)
        rubber.add_cyl_between((x_bot - sx * 0.022, -0.17, -0.885), (x_top - sx * 0.020, 0.352, -0.565), 0.011, 0.010, 8)
        # Upper side window header rail & molded door sill + padded armrest!
        oatmeal.add_cyl_between((x_top, 0.355, -0.56), (x_top + sx * 0.02, 0.355, 0.36), 0.024, 0.022, 10)
        oatmeal.add_box((x_bot + sx * 0.02, -0.31, -0.25), (0.055, 0.24, 1.22))
        dash.add_box((x_bot - sx * 0.005, -0.36, -0.18), (0.075, 0.052, 0.56))  # Padded charcoal armrest!
        metal_shell.add_box((x_bot - sx * 0.012, -0.26, -0.34), (0.018, 0.024, 0.11))  # Chrome door latch!
        # Overhead cabin air-vent eyeball nozzles
        metal_shell.add_cyl_between((x_top - sx * 0.04, 0.30, -0.50), (x_top - sx * 0.08, 0.26, -0.44), 0.016, 0.014, 10)

    # Sleek curved windshield upper brow frame & thin center windshield spline (never blocks runway!)
    oatmeal.add_cyl_between((-0.52, 0.355, -0.56), (0.80, 0.355, -0.56), 0.028, 0.028, 12)
    rubber.add_cyl_between((-0.52, 0.336, -0.57), (0.80, 0.336, -0.57), 0.010, 0.010, 8)
    rubber.add_cyl_between((0.18, -0.17, -0.94), (0.18, 0.345, -0.58), 0.0065, 0.0065, 8)
    # Compact rounded overhead wet magnetic compass housing tucked neatly near top of windshield!
    rubber.add_box((0.18, 0.282, -0.605), (0.062, 0.046, 0.050))
    rubber.add_cyl_between((0.18, 0.305, -0.605), (0.18, 0.348, -0.575), 0.007, 0.007, 8)

    # Curved upholstered cabin headliner ceiling & overhead speaker/map-light console
    headliner.add_box((0.14, 0.382, -0.06), (1.38, 0.042, 1.05))
    rubber.add_box((0.14, 0.358, -0.18), (0.28, 0.018, 0.32))
    # Chrome-hinged smoky-green translucent Cessna sun visors
    for vx, vyaw in [(-0.18, 4.0), (0.46, -4.0)]:
        visor.add_box((vx, 0.316, -0.53), (0.42, 0.082, 0.005), rot_deg=(-24, vyaw, 0))
        metal_shell.add_cyl_between((vx - 0.16, 0.350, -0.55), (vx + 0.16, 0.350, -0.55), 0.0045, 0.0045, 8)

    # 3. AUTHENTIC SCULPTED EXTERIOR NOSE COWLING + HIGH WINGS & WHITE STRUTS VISIBLE OUT SIDE WINDOWS!
    cowl, glare_strip, wing_in = GeoPart(), GeoPart(), GeoPart()
    # Smooth 24-segment curved upper cowling dome visibly sloping from windshield base out to chrome spinner!
    cw_st = [(-0.92, -0.55, 0.52, 0.42), (-1.45, -0.53, 0.50, 0.38), (-2.05, -0.50, 0.46, 0.33), (-2.46, -0.48, 0.36, 0.26), (-2.62, -0.47, 0.16, 0.16)]
    cowl.add_loft([np.array([[0.16 + math.cos(k / 24 * math.tau) * rx, cy + math.sin(k / 24 * math.tau) * ry, z] for k in range(24)], dtype=np.float32) for (z, cy, rx, ry) in cw_st], [i / 4.0 for i in range(5)], close_caps=True)
    # Matte charcoal-black anti-glare cowling center strip hugging exact curved hood arc!
    for si in range(3):
        z0, cy0, rx0, ry0 = cw_st[si]
        z1, cy1, rx1, ry1 = cw_st[si + 1]
        for ai in range(6):
            a0 = math.radians(68.0 + ai * 7.33)
            a1 = math.radians(68.0 + (ai + 1) * 7.33)
            p00 = (0.16 + math.cos(a0) * rx0, cy0 + math.sin(a0) * ry0 + 0.0025, z0)
            p10 = (0.16 + math.cos(a1) * rx0, cy0 + math.sin(a1) * ry0 + 0.0025, z0)
            p11 = (0.16 + math.cos(a1) * rx1, cy1 + math.sin(a1) * ry1 + 0.0025, z1)
            p01 = (0.16 + math.cos(a0) * rx1, cy1 + math.sin(a0) * ry1 + 0.0025, z1)
            glare_strip.add_quad(p00, p10, p11, p01, (0, 0), (1, 0), (1, 1), (0, 1), (0, 1, 0))
    sp_st = [(-2.60, 0.165), (-2.72, 0.135), (-2.84, 0.085), (-2.94, 0.025), (-2.97, 0.004)]
    metal_shell.add_loft([np.array([[0.16 + math.cos(k / 20 * math.tau) * rad, -0.47 + math.sin(k / 20 * math.tau) * rad, z] for k in range(20)], dtype=np.float32) for (z, rad) in sp_st], [i / 4.0 for i in range(5)], close_caps=True)

    # Real Left & Right Cessna High Wings + Diagonal White Wing Struts visible when banking or looking out side windows!
    foil_full = _naca2412_section()
    for sign in (-1.0, 1.0):
        w_st = [(sign * 0.68, 0.40, 1.48, -0.58), (sign * 2.55, 0.44, 1.46, -0.58), (sign * 5.20, 0.52, 1.12, -0.45), (sign * 5.56, 0.53, 0.72, -0.26)]
        if sign < 0: w_st = list(reversed(w_st))
        wing_in.add_loft([np.array([[0.16 + ox, y0 + py * ch, zle + px * ch] for (px, py) in foil_full], dtype=np.float32) for (ox, y0, ch, zle) in w_st], [abs(ox) / 5.56 for (ox, _, _, _) in w_st], close_caps=True)
        cowl.add_cyl_between((0.16 + sign * 0.62, -0.55, -0.12), (0.16 + sign * 2.70, 0.42, -0.05), 0.024, 0.020, 10, aspect_x=2.4)

    gb.add_node("CockpitShell", gb.add_mesh("CockpitShell", [
        dash.to_prim(m_leather), carpet.to_prim(m_carpet), oatmeal.to_prim(m_oatmeal),
        rubber.to_prim(m_rubber), headliner.to_prim(m_headliner), visor.to_prim(m_visor),
        cowl.to_prim(m_cream), glare_strip.to_prim(m_glare_blk), metal_shell.to_prim(m_metal),
        wing_in.to_prim(m_wing_in),
    ]), parent=root)

    # Ultra-subtle smoky-grey spinning propeller motion veil (zero neon yellow rainbow!)
    pdisc = GeoPart()
    R_CP = 0.88
    pdisc.add_quad((-R_CP, -R_CP, 0.0), (R_CP, -R_CP, 0.0), (R_CP, R_CP, 0.0), (-R_CP, R_CP, 0.0), (0, 0), (1, 0), (1, 1), (0, 1), (0, 0, 1))
    gb.add_node("PropBlurDisc", gb.add_mesh("PropBlurDisc", [pdisc.to_prim(m_blur)]), translation=(0.16, -0.47, -2.66), parent=root)

    comp = GeoPart()
    comp.add_cyl_between((0.0, -0.011, 0.0), (0.0, 0.011, 0.0), 0.018, 0.018, 16)
    gb.add_node("CompassCard", gb.add_mesh("CompassCard", [comp.to_prim(m_compass)]), translation=(0.18, 0.282, -0.584), parent=root)

    # 4. 2K ENGRAVED & AO-BAKED 3D INSTRUMENT PANEL GROUP (Tilted -4 deg, facing pilot directly!)
    tilt_rad = math.radians(-4.0)
    panel_rot_xyzw = (math.sin(tilt_rad * 0.5), 0.0, 0.0, math.cos(tilt_rad * 0.5))
    panel_node = gb.add_node("PanelGroup", translation=(0.0, -0.312, -0.785), rotation_xyzw=panel_rot_xyzw, parent=root)

    p_plate, p_back, p_bezel, p_metal, p_red, p_orange, p_dials, p_av, p_glass = (
        GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart(), GeoPart()
    )
    # 2K textured front instrument faceplate (exact 1:1 UV match to _bake_cockpit_panel_face_pbr!)
    p_plate.add_quad((PX0, PY0, 0.0), (PX1, PY0, 0.0), (PX1, PY1, 0.0), (PX0, PY1, 0.0), (0, 1), (1, 1), (1, 0), (0, 0), (0, 0, 1))
    p_back.add_box(((PX0 + PX1) * 0.5, (PY0 + PY1) * 0.5, -0.024), (PX1 - PX0, PY1 - PY0, 0.044))

    def add_gauge_3d(gx: float, gy: float, rad: float, col_i: int, row_i: int, draw_static_face: bool = True, corner_knob: str = "") -> None:
        # 2-Tier Machined Chamfered Circular Bezel + Recessed Shadow Depth + Glossy Glass Cover Lens + 4 Screws!
        segs = 28
        r_in, r_mid, r_out = rad * 0.96, rad * 1.04, rad * 1.12
        z0, z1, z2 = 0.0005, 0.0085, 0.0135
        for i in range(segs):
            a0, a1 = (i / segs) * math.tau, ((i + 1) / segs) * math.tau
            c0, s0, c1, s1 = math.cos(a0), math.sin(a0), math.cos(a1), math.sin(a1)
            # Chamfered outer step
            p_bezel.add_quad((gx + c0 * r_out, gy + s0 * r_out, z0), (gx + c1 * r_out, gy + s1 * r_out, z0),
                             (gx + c1 * r_mid, gy + s1 * r_mid, z2), (gx + c0 * r_mid, gy + s0 * r_mid, z2),
                             (0, 0), (1, 0), (1, 1), (0, 1), (c0 * 0.7, s0 * 0.7, 0.7))
            # Top machined bezel face
            p_bezel.add_quad((gx + c0 * r_in, gy + s0 * r_in, z2), (gx + c0 * r_mid, gy + s0 * r_mid, z2),
                             (gx + c1 * r_mid, gy + s1 * r_mid, z2), (gx + c1 * r_in, gy + s1 * r_in, z2),
                             (0, 0), (1, 0), (1, 1), (0, 1), (0, 0, 1))
            # Deep inner recessed barrel wall
            p_bezel.add_quad((gx + c1 * r_in, gy + s1 * r_in, z0), (gx + c0 * r_in, gy + s0 * r_in, z0),
                             (gx + c0 * r_in, gy + s0 * r_in, z2), (gx + c1 * r_in, gy + s1 * r_in, z2),
                             (0, 0), (1, 0), (1, 1), (0, 1), (-c0, -s0, 0))
            # Thin glossy circular glass cover disc recessed just inside the bezel rim!
            p_glass.add_tri((gx, gy, z1 + 0.003), (gx + c0 * r_in, gy + s0 * r_in, z1 + 0.003), (gx + c1 * r_in, gy + s1 * r_in, z1 + 0.003),
                            (0.5, 0.5), (0.5 + c0 * 0.5, 0.5 + s0 * 0.5), (0.5 + c1 * 0.5, 0.5 + s1 * 0.5), (0, 0, 1))
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                p_metal.add_cyl_between((gx + sx * rad * 0.96, gy + sy * rad * 0.96, 0.001), (gx + sx * rad * 0.96, gy + sy * rad * 0.96, 0.007), 0.0032, 0.0032, 6)
        # Signature Cessna 3D knurled corner adjustment knobs (Baro / Cage / Heading Bug!)
        if corner_knob:
            kx = gx + (-rad * 0.92 if corner_knob == "bl" else (rad * 0.92 if corner_knob == "br" else 0.0))
            ky = gy - (rad * 0.92 if corner_knob in ("bl", "br") else rad * 1.02)
            p_bezel.add_cyl_between((kx, ky, 0.003), (kx, ky, 0.022), 0.0075, 0.0070, 10)
            p_metal.add_cyl_between((kx, ky, 0.022), (kx, ky, 0.025), 0.0045, 0.0045, 8)
        if draw_static_face:
            u0, v0 = col_i / 3.0, row_i / 3.0
            u1, v1 = (col_i + 1) / 3.0, (row_i + 1) / 3.0
            p_dials.add_quad((gx - rad, gy - rad, 0.0025), (gx + rad, gy - rad, 0.0025), (gx + rad, gy + rad, 0.0025), (gx - rad, gy + rad, 0.0025),
                             (u0, v1), (u1, v1), (u1, v0), (u0, v0), (0, 0, 1))

    add_gauge_3d(COL_L, ROW_TOP, G_R, 0, 0)                      # Airspeed Indicator
    add_gauge_3d(COL_C, ROW_TOP, G_R, 1, 0, False, "bc")         # Attitude Indicator + bottom pitch trim knob
    add_gauge_3d(COL_R, ROW_TOP, G_R, 2, 0, True,  "bl")         # Altimeter + bottom-left Kollsman baro knob!
    add_gauge_3d(COL_E, ROW_TOP, G_R, 1, 2, True,  "bl")         # VOR1 CDI + OBS knob!
    add_gauge_3d(COL_L, ROW_BOT, G_R, 0, 1)                      # Turn Coordinator
    add_gauge_3d(COL_C, ROW_BOT, G_R, 1, 1, False, "bl")         # Heading Indicator + gyro slave knob!
    add_gauge_3d(COL_R, ROW_BOT, G_R, 2, 1)                      # VSI
    add_gauge_3d(COL_E, ROW_BOT, G_R, 0, 2)                      # RPM Tachometer
    add_gauge_3d(-0.352, ROW_TOP, 0.039, 2, 2)                   # Fuel L/R & Oil
    add_gauge_3d(-0.352, ROW_BOT, 0.039, 2, 2)                   # EGT/FF & Vac/Amps

    # Fixed orange Attitude Indicator gull-wing symbol & Heading lubber triangle right behind glass
    p_orange.add_box((COL_C - 0.019, ROW_TOP, 0.010), (0.017, 0.0032, 0.002))
    p_orange.add_box((COL_C + 0.019, ROW_TOP, 0.010), (0.017, 0.0032, 0.002))
    p_orange.add_box((COL_C, ROW_TOP, 0.010), (0.0046, 0.0046, 0.002))
    p_orange.add_box((COL_C, ROW_BOT + G_R * 0.88, 0.010), (0.006, 0.009, 0.002))

    # Center Avionics Rack (GMA340 + Garmin GNS430 + KX155A + GTX327) with beveled dark housing & rotary dual knobs!
    ax0, ax1, ay0, ay1 = av_rect
    p_bezel.add_box(((ax0 + ax1) * 0.5, (ay0 + ay1) * 0.5, 0.005), (ax1 - ax0 + 0.016, ay1 - ay0 + 0.016, 0.011))
    p_av.add_quad((ax0, ay0, 0.011), (ax1, ay0, 0.011), (ax1, ay1, 0.011), (ax0, ay1, 0.011), (0, 1), (1, 1), (1, 0), (0, 0), (0, 0, 1))
    for kx, ky in [(0.236, 0.012), (0.460, 0.012), (0.236, -0.040), (0.460, -0.040), (0.460, -0.088)]:
        p_bezel.add_cyl_between((kx, ky, 0.011), (kx, ky, 0.026), 0.009, 0.008, 12)
        p_metal.add_cyl_between((kx, ky, 0.026), (kx, ky, 0.031), 0.005, 0.005, 10)

    # Lower Switch Bar: Key Ignition + Red Split Master Rocker + 8 Chrome Toggle Switches + 10 White-Ring Breakers!
    p_metal.add_cyl_between((-0.422, -0.136, 0.001), (-0.422, -0.136, 0.013), 0.011, 0.010, 12)
    p_bezel.add_box((-0.422, -0.136, 0.021), (0.004, 0.022, 0.016))
    p_red.add_box((-0.370, -0.136, 0.010), (0.013, 0.024, 0.016))  # ALT half
    p_red.add_box((-0.354, -0.136, 0.010), (0.013, 0.024, 0.016))  # BAT half
    for i in range(8):
        sx = -0.312 + i * 0.032
        p_metal.add_cyl_between((sx, -0.136, 0.001), (sx, -0.136, 0.008), 0.0055, 0.0050, 8)  # Hex nut collar
        p_metal.add_cyl_between((sx, -0.136, 0.007), (sx, -0.128, 0.022), 0.0028, 0.0022, 8)  # Bat toggle lever
    for i in range(10):
        bx = 0.236 + (i % 5) * 0.028
        by = -0.138 - (i // 5) * 0.024
        p_bezel.add_cyl_between((bx, by, 0.001), (bx, by, 0.013), 0.0062, 0.0060, 10)
        p_metal.add_cyl_between((bx, by, 0.013), (bx, by, 0.016), 0.0042, 0.0042, 8)
    # Machined throttle quadrant bezel + chrome vernier collars + slotted flap track + co-pilot glovebox latch!
    p_bezel.add_box((0.120, -0.172, 0.003), (0.215, 0.056, 0.007))
    for cx in (0.050, 0.120, 0.190):
        p_metal.add_cyl_between((cx, -0.174, 0.005), (cx, -0.174, 0.016), 0.012, 0.011, 12)
    p_bezel.add_box((0.268, -0.176, 0.004), (0.028, 0.068, 0.008))  # Flap detent slot plate
    p_metal.add_cyl_between((0.581, -0.026, 0.002), (0.581, -0.026, 0.011), 0.0085, 0.0085, 10)  # Glovebox chrome latch
    # Black accordion rubber steering column dust boots behind Pilot & Co-Pilot yokes!
    for yx in (-0.114, 0.455):
        boot_st = [(0.002, 0.032), (0.014, 0.026), (0.024, 0.030), (0.036, 0.022), (0.048, 0.018)]
        p_bezel.add_loft([np.array([[yx + math.cos(k / 14 * math.tau) * r_b, -0.175 + math.sin(k / 14 * math.tau) * r_b, zb] for k in range(14)], dtype=np.float32) for (zb, r_b) in boot_st], [i / 4.0 for i in range(5)], close_caps=True)

    gb.add_node("PanelStatic", gb.add_mesh("PanelStatic", [
        p_plate.to_prim(m_panel), p_back.to_prim(m_carpet), p_bezel.to_prim(m_bezel), p_metal.to_prim(m_metal),
        p_red.to_prim(m_red), p_orange.to_prim(m_orange), p_dials.to_prim(m_dials), p_av.to_prim(m_avionics),
        p_glass.to_prim(m_gauge_glass),
    ]), parent=panel_node)

    # 5. LIVE ANIMATED 3D GAUGE NEEDLES & 28-SEG CIRCULAR ROTATING CARDS (zero square corners!)
    def make_needle(name: str, gx: float, gy: float, length: float, width: float, z_off: float = 0.0085, tail: float = 0.009) -> None:
        ng = GeoPart()
        ng.add_box((0.0, (length - tail) * 0.5, 0.0), (width, length + tail, 0.0020))
        hub = GeoPart()
        hub.add_cyl_between((0.0, 0.0, -0.001), (0.0, 0.0, 0.003), width * 1.45, width * 1.35, 10)
        gb.add_node(name, gb.add_mesh(name, [ng.to_prim(m_needle), hub.to_prim(m_bezel)]), translation=(gx, gy, z_off), parent=panel_node)

    def make_circular_card(u_center: float, u_rad: float, v_center: float, v_rad: float, disc_rad: float, segs: int = 28) -> GeoPart:
        gp = GeoPart()
        for i in range(segs):
            a0, a1 = (i / segs) * math.tau, ((i + 1) / segs) * math.tau
            c0, s0, c1, s1 = math.cos(a0), math.sin(a0), math.cos(a1), math.sin(a1)
            gp.add_tri((0.0, 0.0, 0.0), (c0 * disc_rad, s0 * disc_rad, 0.0), (c1 * disc_rad, s1 * disc_rad, 0.0),
                       (u_center, v_center), (u_center + c0 * u_rad, v_center - s0 * v_rad), (u_center + c1 * u_rad, v_center - s1 * v_rad), (0, 0, 1))
        return gp

    make_needle("Needle_ASI",      COL_L, ROW_TOP, G_R * 0.80, 0.0032, 0.0085)
    make_needle("Needle_ALT_100",  COL_R, ROW_TOP, G_R * 0.82, 0.0030, 0.0095)
    make_needle("Needle_ALT_1000", COL_R, ROW_TOP, G_R * 0.54, 0.0052, 0.0075)
    make_needle("Needle_VSI",      COL_R, ROW_BOT, G_R * 0.78, 0.0032, 0.0085)
    make_needle("Needle_RPM",      COL_E, ROW_BOT, G_R * 0.78, 0.0032, 0.0085)

    ai_card = make_circular_card(0.25, 0.23, 0.50, 0.46, G_R * 0.96, 28)
    gb.add_node("Card_AI", gb.add_mesh("Card_AI", [ai_card.to_prim(m_cards)]), translation=(COL_C, ROW_TOP, 0.004), parent=panel_node)

    hdg_card = make_circular_card(0.75, 0.23, 0.50, 0.46, G_R * 0.96, 28)
    gb.add_node("Card_HDG", gb.add_mesh("Card_HDG", [hdg_card.to_prim(m_cards)]), translation=(COL_C, ROW_BOT, 0.005), parent=panel_node)

    tc_ac = GeoPart()
    tc_ac.add_box((0.0, 0.0, 0.0), (G_R * 1.15, 0.0042, 0.002))
    tc_ac.add_box((0.0, 0.006, 0.0), (0.0042, 0.015, 0.002))
    tc_ac.add_box((0.0, 0.004, 0.0), (0.018, 0.0032, 0.002))
    gb.add_node("Card_TC", gb.add_mesh("Card_TC", [tc_ac.to_prim(m_needle)]), translation=(COL_L, ROW_BOT + 0.004, 0.0085), parent=panel_node)

    # 6. TRUE ORGANIC CONTINUOUS-LOFTED CESSNA 172S RAM'S-HORN CONTROL YOKES (zero blocky pipes!)
    def build_yoke_node(name: str, x_pos: float) -> None:
        yk_shaft, yk_grip, yk_red, yk_badge = GeoPart(), GeoPart(), GeoPart(), GeoPart()
        yk_shaft.add_cyl_between((0.0, 0.0, -0.26), (0.0, 0.0, 0.0), 0.014, 0.014, 16)
        # Sculpted rounded oval center hub cushion (lofted soft pillow, NOT a hard cube!)
        hub_st = [(-0.016, 0.036, 0.024), (-0.004, 0.044, 0.030), (0.012, 0.042, 0.028), (0.020, 0.030, 0.020)]
        yk_grip.add_loft([np.array([[math.cos(k / 16 * math.tau) * rx, math.sin(k / 16 * math.tau) * ry, zh] for k in range(16)], dtype=np.float32) for (zh, rx, ry) in hub_st], [i / 3.0 for i in range(4)], close_caps=True)
        # Beveled brushed-aluminum Cessna wing medallion emblem on front of hub!
        yk_badge.add_cyl_between((0.0, 0.0, 0.019), (0.0, 0.0, 0.023), 0.022, 0.020, 16, aspect_x=1.45)

        # Continuous 17-station smooth swept loft spanning Left Horn -> Crossbar -> Right Horn!
        spine = [
            (-0.132, 0.112, 0.014, 0.012, 0.011), (-0.150, 0.090, 0.013, 0.016, 0.014),
            (-0.156, 0.056, 0.011, 0.019, 0.016), (-0.152, 0.022, 0.008, 0.019, 0.016),
            (-0.136, -0.004, 0.004, 0.018, 0.015), (-0.104, -0.014, 0.001, 0.017, 0.014),
            (-0.064, -0.012, 0.000, 0.017, 0.015), (-0.028, -0.006, 0.000, 0.019, 0.016),
            ( 0.000, -0.004, 0.000, 0.020, 0.017),
            ( 0.028, -0.006, 0.000, 0.019, 0.016), ( 0.064, -0.012, 0.000, 0.017, 0.015),
            ( 0.104, -0.014, 0.001, 0.017, 0.014), ( 0.136, -0.004, 0.004, 0.018, 0.015),
            ( 0.152, 0.022, 0.008, 0.019, 0.016), ( 0.156, 0.056, 0.011, 0.019, 0.016),
            ( 0.150, 0.090, 0.013, 0.016, 0.014), ( 0.132, 0.112, 0.014, 0.012, 0.011),
        ]
        y_rings = []
        for idx_s, (sx, sy, sz, ra, rb) in enumerate(spine):
            p_prev = np.array(spine[max(0, idx_s - 1)][:3], dtype=np.float32)
            p_next = np.array(spine[min(len(spine) - 1, idx_s + 1)][:3], dtype=np.float32)
            tang = p_next - p_prev
            tang /= max(1e-6, float(np.linalg.norm(tang)))
            fwd = np.array([0.0, 0.0, 1.0], dtype=np.float32)
            n1 = np.cross(fwd, tang)
            n1 /= max(1e-6, float(np.linalg.norm(n1)))
            n2 = np.cross(tang, n1)
            c_pt = np.array([sx, sy, sz], dtype=np.float32)
            y_rings.append(np.array([c_pt + n1 * (math.cos(k / 14 * math.tau) * ra) + n2 * (math.sin(k / 14 * math.tau) * rb) for k in range(14)], dtype=np.float32))
        yk_grip.add_loft(y_rings, [i / (len(spine) - 1) * 4.0 for i in range(len(spine))], close_caps=True)
        # Red PTT button + electric pitch trim switch on left horn!
        yk_red.add_cyl_between((-0.148, 0.088, 0.022), (-0.148, 0.088, 0.030), 0.006, 0.0055, 10)
        yk_badge.add_box((-0.144, 0.068, 0.025), (0.012, 0.016, 0.008))
        gb.add_node(name, gb.add_mesh(name, [yk_shaft.to_prim(m_metal), yk_grip.to_prim(m_yoke), yk_red.to_prim(m_red), yk_badge.to_prim(m_metal)]),
                    translation=(x_pos, -0.486, -0.535), parent=root)

    build_yoke_node("Yoke", -0.114)
    build_yoke_node("YokeCopilot", 0.455)

    # 7. MACHINED VERNIER THROTTLE / MIXTURE / ALT-AIR KNOBS, SERRATED TRIM WHEEL, FLAP LEVER & RUDDER PEDALS!
    for name, x_pos, mat_k, rad in [("CarbHeatKnob", 0.050, m_bezel, 0.0125), ("ThrottleKnob", 0.120, m_yoke, 0.0175), ("MixtureKnob", 0.190, m_red, 0.0155)]:
        s_g, k_g = GeoPart(), GeoPart()
        s_g.add_cyl_between((0.0, 0.0, -0.095), (0.0, 0.0, 0.002), 0.0050, 0.0050, 10)
        # Sculpted knurled vernier knob head + silver center vernier release button!
        k_st = [(0.000, rad * 0.62), (0.006, rad * 0.96), (0.022, rad * 1.00), (0.027, rad * 0.82)]
        k_g.add_loft([np.array([[math.cos(k / 16 * math.tau) * rk, math.sin(k / 16 * math.tau) * rk, zk] for k in range(16)], dtype=np.float32) for (zk, rk) in k_st], [i / 3.0 for i in range(4)], close_caps=True)
        s_g.add_cyl_between((0.0, 0.0, 0.026), (0.0, 0.0, 0.031), rad * 0.42, rad * 0.38, 10)
        gb.add_node(name, gb.add_mesh(name, [s_g.to_prim(m_metal), k_g.to_prim(mat_k)]), translation=(x_pos, -0.486, -0.715), parent=root)

    tw_g = GeoPart(); tw_g.add_cyl_between((-0.016, 0.0, 0.0), (0.016, 0.0, 0.0), 0.056, 0.056, 22)
    gb.add_node("TrimWheel", gb.add_mesh("TrimWheel", [tw_g.to_prim(m_bezel)]), translation=(0.11, -0.585, -0.575), parent=root)

    fs_g = GeoPart(); fs_g.add_box((0.0, 0.012, -0.015), (0.018, 0.016, 0.068))
    gb.add_node("FuelSelector", gb.add_mesh("FuelSelector", [fs_g.to_prim(m_red)]), translation=(0.16, -0.68, -0.48), parent=root)

    fl_g, fl_tip = GeoPart(), GeoPart()
    fl_g.add_cyl_between((0.0, 0.0, 0.0), (0.0, 0.0, 0.068), 0.004, 0.004, 8)
    fl_tip.add_box((0.0, 0.0, 0.072), (0.036, 0.010, 0.024))
    gb.add_node("FlapLever", gb.add_mesh("FlapLever", [fl_g.to_prim(m_metal), fl_tip.to_prim(m_cream)]), translation=(0.268, -0.488, -0.705), parent=root)

    fso_g = GeoPart(); fso_g.add_cyl_between((0.0, 0.0, -0.04), (0.0, 0.0, 0.015), 0.012, 0.012, 12)
    gb.add_node("FuelShutoff", gb.add_mesh("FuelShutoff", [fso_g.to_prim(m_red)]), translation=(0.16, -0.62, -0.53), parent=root)

    pb_g = GeoPart(); pb_g.add_box((0.0, 0.0, 0.0), (0.064, 0.014, 0.016))
    gb.add_node("ParkingBrake", gb.add_mesh("ParkingBrake", [pb_g.to_prim(m_bezel)]), translation=(-0.22, -0.54, -0.71), parent=root)

    for pname, px in [("RudderPedal_L", -0.18), ("RudderPedal_R", -0.02)]:
        rp = GeoPart(); rp.add_box((0.0, 0.0, 0.0), (0.075, 0.14, 0.014), rot_deg=(-18, 0, 0))
        gb.add_node(pname, gb.add_mesh(pname, [rp.to_prim(m_metal)]), translation=(px, -0.72, -0.82), parent=root)

    gb.save(out_path)


def verify_glb(path: Path, expected_nodes: list[str] | None = None) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    magic, ver, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and ver == 2 and length == len(data), f"Bad GLB header: {path}"
    jlen, jtype = struct.unpack_from("<II", data, 12)
    assert jtype == 0x4E4F534A, f"Missing JSON chunk: {path}"
    gltf = json.loads(data[20:20 + jlen].decode("utf-8"))
    node_names = {nd.get("name", "") for nd in gltf.get("nodes", [])}
    if expected_nodes:
        missing = [n for n in expected_nodes if n not in node_names]
        assert not missing, f"Missing expected nodes in {path.name}: {missing}"
    n_nodes = len(gltf.get("nodes", []))
    n_meshes = len(gltf.get("meshes", []))
    n_prims = sum(len(m.get("primitives", [])) for m in gltf.get("meshes", []))
    n_tris = sum(gltf["accessors"][pr["indices"]]["count"] // 3 for m in gltf.get("meshes", []) for pr in m.get("primitives", []))
    assert n_meshes > 0 and n_tris > 0, f"Empty mesh in {path}"
    return n_nodes, n_meshes, n_prims, n_tris


def main() -> None:
    import sys
    aircraft_only = "--aircraft-only" in sys.argv
    cockpit_only = "--cockpit-only" in sys.argv
    ac = MODELS_DIR / "aircraft"

    if not aircraft_only and not cockpit_only:
        bake_spain_photo_lut()
        bake_interior_parallax_atlas()
        alb_atlas, nrm_atlas, _orm_atlas = bake_master_building_atlas()

        arch = MODELS_DIR / "architecture"
        veg = MODELS_DIR / "vegetation"

        r_terra = (0.02, 0.02, 0.48, 0.48)
        r_slate = (0.52, 0.02, 0.98, 0.48)
        w_stucco_2f = (0.02, 0.72, 0.48, 0.98)
        w_stucco_4f = (0.02, 0.52, 0.48, 0.98)
        w_stone_2f  = (0.52, 0.52, 0.98, 0.98)

        _build_single_surface_building_glb(arch / "aragon_house_stone.glb", alb_atlas, nrm_atlas, w_stone_2f, r_slate, roof_pitch=0.26)
        _build_single_surface_building_glb(arch / "aragon_house_terra.glb", alb_atlas, nrm_atlas, w_stucco_2f, r_terra, roof_pitch=0.22)
        _build_single_surface_building_glb(arch / "aragon_apartment.glb",   alb_atlas, nrm_atlas, w_stucco_4f, r_terra, roof_pitch=0.11)
        _build_single_surface_building_glb(arch / "aragon_church_tower.glb",alb_atlas, nrm_atlas, w_stone_2f, r_terra, roof_pitch=0.24, is_church=True)
        _build_single_surface_building_glb(arch / "industrial_hangar.glb",  alb_atlas, nrm_atlas, r_slate,    r_slate, roof_pitch=0.07)
        _build_single_surface_building_glb(arch / "rural_barn.glb",         alb_atlas, nrm_atlas, w_stone_2f, r_terra, roof_pitch=0.25)

        for sp, fname in [
            ("pine", "tree_scots_pine.glb"),
            ("poplar", "tree_poplar.glb"),
            ("oak", "tree_holm_oak.glb"),
            ("shrub", "shrub_maquis.glb"),
        ]:
            _build_single_surface_tree_glb(veg / fname, sp)

    ext_path = ac / "c172s_exterior.glb"
    ckp_path = ac / "c172s_cockpit.glb"
    if not cockpit_only:
        build_c172s_exterior_glb(ext_path)
    build_c172s_cockpit_glb(ckp_path)

    ext_req = ["Fuselage", "Wing", "PropHub", "PropBlurDisc", "Flap_L", "Flap_R", "Aileron_L", "Aileron_R", "Elevator", "TrimTab", "Rudder", "NoseGear", "NavLightRed", "NavLightGreen", "StrobeWhite", "BeaconRed", "LandingLight"]
    ckp_req = ["CockpitShell", "PanelGroup", "PanelStatic", "PropBlurDisc", "Yoke", "ThrottleKnob", "MixtureKnob", "FlapLever", "TrimWheel", "CompassCard", "Needle_ASI", "Card_AI", "Needle_ALT_100", "Needle_ALT_1000", "Card_TC", "Card_HDG", "Needle_VSI", "Needle_RPM"]
    checks = [(ckp_path, ckp_req)] if cockpit_only else [(ext_path, ext_req), (ckp_path, ckp_req)]
    for p, req in checks:
        nn, nm, npr, nt = verify_glb(p, req)
        print(f"  [VERIFIED] {str(p.relative_to(ROOT)):46s} | {p.stat().st_size // 1024:4d} KB | nodes={nn:2d} surfs={npr:2d} tris={nt:5d}")


if __name__ == "__main__":
    main()
