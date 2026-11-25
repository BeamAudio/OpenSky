#!/usr/bin/env python3
"""build_region_gui.py — Visual regional pack compiler (GUI only).

Select a region (preset or custom area), choose detail level, and build packs
with live logs and visual previews of generated data (map + heatmaps/vectors).
No command-line steps required to use this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import queue
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any

import tkinter as tk
from tkinter import ttk, messagebox, filedialog

try:
    from PIL import Image, ImageTk
except Exception:  # pragma: no cover
    Image = None
    ImageTk = None

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
PYTHON = sys.executable


def _run(cmd: list[str], on_line, on_done, env: dict[str, str] | None = None) -> None:
    def worker():
        try:
            proc = subprocess.Popen(
                cmd,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                on_line(line)
            proc.wait()
            on_done(proc.returncode)
        except Exception as exc:
            on_line(f"[ERROR] {exc}\n")
            on_done(1)

    t = threading.Thread(target=worker, daemon=True)
    t.start()


class PackCompilerGUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("OpenSky World Editor — Region Builder")
        root.geometry("1420x980")

        from builder.build_region import PRESETS, QUALITY_PROFILES

        self.PRESETS = PRESETS
        self.QUALITY_PROFILES = QUALITY_PROFILES
        self.preview_imgtk = None
        self.preview_path: Path | None = None
        self.cov_imgtk = None
        self.cov_pil = None
        self.cov_view_bbox: list[float] = [-9.8, 39.8, 4.6, 44.2]
        self.cov_draw_rect: tuple[float, float, float, float] | None = None
        self.cov_pin_latlon: tuple[float, float] | None = None
        self._modules_cache: dict[str, Any] = {}
        self._cov_busy = False

        nb = ttk.Notebook(root)
        nb.pack(fill="both", expand=True, padx=8, pady=8)

        self.tab_pack = ttk.Frame(nb)
        self.tab_weather = ttk.Frame(nb)
        nb.add(self.tab_pack, text="Module Manager + Compiler")
        nb.add(self.tab_weather, text="Weather library (.nswx)")

        self._build_pack_tab()
        self._build_weather_tab()
        self._build_log(root)

        self.log_q: queue.Queue[tuple[str, Any]] = queue.Queue()
        root.after(100, self._poll_log)
        self._refresh_pack_contents()
        self.root.after(50, self._refresh_coverage_map)

    # ----- UI builders -----
    def _build_pack_tab(self) -> None:
        f = self.tab_pack
        paned = ttk.Panedwindow(f, orient="horizontal")
        paned.pack(fill="both", expand=True, padx=4, pady=4)

        left = ttk.Frame(paned, width=560)
        right = ttk.Frame(paned)
        paned.add(left, weight=1)
        paned.add(right, weight=2)

        top = ttk.LabelFrame(left, text="Area of interest (select or set bbox)")
        top.pack(fill="x", padx=4, pady=4)

        ttk.Label(top, text="Preset").grid(row=0, column=0, sticky="w", padx=4, pady=4)
        self.preset_var = tk.StringVar(value="zaragoza-pyrenees-quick")
        cb = ttk.Combobox(top, textvariable=self.preset_var, values=["all-spain"] + sorted(self.PRESETS.keys()), width=34, state="readonly")
        cb.grid(row=0, column=1, sticky="w", padx=4, pady=4)
        cb.bind("<<ComboboxSelected>>", self._on_preset)

        self.custom_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text="Custom bbox (lon_min lat_min lon_max lat_max)", variable=self.custom_var, command=self._toggle_custom).grid(row=0, column=2, sticky="w", padx=6)

        ttk.Label(top, text="bbox").grid(row=1, column=0, sticky="w", padx=4, pady=4)
        self.bbox_entry = ttk.Entry(top, width=44)
        self.bbox_entry.insert(0, "-1.20 41.55 0.65 42.65")
        self.bbox_entry.grid(row=1, column=1, columnspan=2, sticky="w", padx=4, pady=4)
        self._toggle_custom()

        mid = ttk.LabelFrame(left, text="Detail / quality")
        mid.pack(fill="x", padx=4, pady=4)
        ttk.Label(mid, text="Quality").grid(row=0, column=0, sticky="w", padx=4, pady=4)
        self.quality_var = tk.StringVar(value="high")
        qcb = ttk.Combobox(mid, textvariable=self.quality_var, values=list(self.QUALITY_PROFILES.keys()), width=14, state="readonly")
        qcb.grid(row=0, column=1, sticky="w", padx=4, pady=4)
        self.quality_desc = ttk.Label(mid, text="", foreground="#444", wraplength=320, justify="left")
        self.quality_desc.grid(row=0, column=2, sticky="w", padx=6)
        qcb.bind("<<ComboboxSelected>>", self._on_quality)
        self._on_quality()

        src = ttk.LabelFrame(left, text="Global data sources · XYZ tiles use EPSG:3857")
        src.pack(fill="x", padx=4, pady=4)
        self.imagery_source_var = tk.StringVar(value="pnoa")
        self.elevation_source_var = tk.StringVar(value="ign+terrarium")
        ttk.Label(src, text="Imagery").grid(row=0, column=0, sticky="w", padx=4, pady=2)
        ttk.Combobox(src, textvariable=self.imagery_source_var,
                     values=("pnoa", "local-xyz", "none"), width=17, state="readonly").grid(row=0, column=1, sticky="w", padx=4, pady=2)
        self.imagery_dir_entry = ttk.Entry(src, width=32)
        self.imagery_dir_entry.grid(row=0, column=2, sticky="ew", padx=4, pady=2)
        ttk.Button(src, text="Browse…", command=lambda: self._pick_data_dir(self.imagery_dir_entry)).grid(row=0, column=3, padx=4, pady=2)
        ttk.Label(src, text="Imagery attribution").grid(row=1, column=0, sticky="w", padx=4, pady=2)
        self.imagery_attr_entry = ttk.Entry(src, width=70)
        self.imagery_attr_entry.grid(row=1, column=1, columnspan=3, sticky="ew", padx=4, pady=2)
        ttk.Label(src, text="Elevation").grid(row=2, column=0, sticky="w", padx=4, pady=2)
        ttk.Combobox(src, textvariable=self.elevation_source_var,
                     values=("ign+terrarium", "terrarium", "local-terrarium"), width=17, state="readonly").grid(row=2, column=1, sticky="w", padx=4, pady=2)
        self.elevation_dir_entry = ttk.Entry(src, width=32)
        self.elevation_dir_entry.grid(row=2, column=2, sticky="ew", padx=4, pady=2)
        ttk.Button(src, text="Browse…", command=lambda: self._pick_data_dir(self.elevation_dir_entry)).grid(row=2, column=3, padx=4, pady=2)
        ttk.Label(src, text="Elevation attribution").grid(row=3, column=0, sticky="w", padx=4, pady=2)
        self.elevation_attr_entry = ttk.Entry(src, width=70)
        self.elevation_attr_entry.grid(row=3, column=1, columnspan=3, sticky="ew", padx=4, pady=2)
        ttk.Label(src, text="OSM extract (.pbf)").grid(row=4, column=0, sticky="w", padx=4, pady=2)
        self.pbf_entry = ttk.Entry(src, width=60)
        self.pbf_entry.grid(row=4, column=1, columnspan=2, sticky="ew", padx=4, pady=2)
        ttk.Button(src, text="Browse…", command=self._pick_pbf).grid(row=4, column=3, padx=4, pady=2)
        src.columnconfigure(2, weight=1)

        opt = ttk.LabelFrame(left, text="Baking steps (optional post-processing)")
        opt.pack(fill="x", padx=4, pady=4)
        self.bake_weather_var = tk.BooleanVar(value=True)
        self.bake_cities_var = tk.BooleanVar(value=False)
        self.bake_scenery_var = tk.BooleanVar(value=False)
        self.bake_vegetation_var = tk.BooleanVar(value=True)
        self.no_night_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(opt, text="Bake weather library (.nswx) first", variable=self.bake_weather_var).grid(row=0, column=0, sticky="w", padx=4, pady=1)
        ttk.Checkbutton(opt, text="Pre-bake cities + 10-TOD shadows/night RT", variable=self.bake_cities_var).grid(row=1, column=0, sticky="w", padx=4, pady=1)
        ttk.Checkbutton(opt, text="Pre-bake OSM roads/buildings (.nsscn)", variable=self.bake_scenery_var).grid(row=2, column=0, sticky="w", padx=4, pady=1)
        ttk.Checkbutton(opt, text="Skip night lights bake", variable=self.no_night_var).grid(row=3, column=0, sticky="w", padx=4, pady=1)
        ttk.Checkbutton(opt, text="Bake LiDAR canopy across the whole region", variable=self.bake_vegetation_var).grid(row=4, column=0, sticky="w", padx=4, pady=1)

        btns = ttk.Frame(left)
        btns.pack(fill="x", padx=4, pady=4)
        self.btn_plan = ttk.Button(btns, text="Preview plan (dry run)", command=self._dry_run)
        self.btn_plan.pack(side="left", padx=2)
        self.btn_build = ttk.Button(btns, text="Build pack", command=self._build_pack)
        self.btn_build.pack(side="left", padx=2)
        ttk.Button(btns, text="Fly in Sim", command=self._launch_sim).pack(side="left", padx=4)
        ttk.Button(btns, text="Open packs/", command=self._open_packs).pack(side="left", padx=4)

        pc = ttk.LabelFrame(left, text="Installed Modules, Zoom Quality & Files (expand region)")
        pc.pack(fill="both", expand=True, padx=4, pady=4)
        cols = ("quality", "tiles", "size")
        self.pack_tree = ttk.Treeview(pc, columns=cols, show="tree headings", height=11)
        self.pack_tree.heading("#0", text="Module / Layer / File")
        self.pack_tree.heading("quality", text="Quality / Res")
        self.pack_tree.heading("tiles", text="Tiles / Info")
        self.pack_tree.heading("size", text="Size")
        self.pack_tree.column("#0", width=230, anchor="w")
        self.pack_tree.column("quality", width=105, anchor="w")
        self.pack_tree.column("tiles", width=120, anchor="w")
        self.pack_tree.column("size", width=70, anchor="e")
        self.pack_tree.pack(side="left", fill="both", expand=True)
        psb = ttk.Scrollbar(pc, command=self.pack_tree.yview)
        psb.pack(side="right", fill="y")
        self.pack_tree.configure(yscrollcommand=psb.set)
        self.pack_tree.bind("<<TreeviewSelect>>", self._on_pack_select)
        pbtn = ttk.Frame(left)
        pbtn.pack(fill="x", padx=4, pady=2)
        ttk.Button(pbtn, text="Refresh", command=lambda: (self._refresh_pack_contents(), self._refresh_coverage_map())).pack(side="left")
        ttk.Button(pbtn, text="Verify Selected", command=self._verify_selected_region).pack(side="left", padx=4)
        ttk.Button(pbtn, text="Remove Selected", command=self._remove_selected_region).pack(side="left", padx=4)
        ttk.Button(pbtn, text="Render Heatmaps", command=self._generate_visual_previews).pack(side="left", padx=4)

        # Right side: interactive tile coverage map + coordinate inspector
        map_frame = ttk.LabelFrame(right, text="Tile Coverage & Quality Map (click anywhere to inspect tiles)")
        map_frame.pack(fill="both", expand=True, padx=4, pady=4)
        bar = ttk.Frame(map_frame)
        bar.pack(fill="x", padx=4, pady=2)
        self.cov_region_var = tk.StringVar(value="")
        self.cov_region_cb = ttk.Combobox(bar, textvariable=self.cov_region_var, width=22, state="readonly")
        self.cov_region_cb.pack(side="left", padx=2)
        self.cov_region_cb.bind("<<ComboboxSelected>>", lambda _e: self._refresh_coverage_map())
        self.layer_vars = {
            "imagery": tk.BooleanVar(value=True),
            "elevation": tk.BooleanVar(value=False),
            "surface": tk.BooleanVar(value=False),
            "cities": tk.BooleanVar(value=True),
            "online": tk.BooleanVar(value=True),
        }
        for lk, lbl in (("imagery", "Ortho"), ("elevation", "DEM"), ("surface", "Surface"), ("cities", "Cities 500m"), ("online", "Online")):
            ttk.Checkbutton(bar, text=lbl, variable=self.layer_vars[lk], command=self._refresh_coverage_map).pack(side="left", padx=3)
        self.cov_zoom_var = tk.StringVar(value="All Z")
        zcb = ttk.Combobox(bar, textvariable=self.cov_zoom_var, values=["All Z"] + [f"Z{z}" for z in range(10, 21)], width=7, state="readonly")
        zcb.pack(side="left", padx=4)
        zcb.bind("<<ComboboxSelected>>", lambda _e: self._refresh_coverage_map())

        self.map_canvas = tk.Canvas(map_frame, bg="#06101b", highlightthickness=0, cursor="crosshair")
        self.map_canvas.pack(fill="both", expand=True)
        self.map_canvas.bind("<Configure>", lambda _e: self._draw_world_map())
        self.map_canvas.bind("<Button-1>", self._on_map_click)

        pv_frame = ttk.LabelFrame(right, text="Tile & Quality Inspector (clicked coordinate or selected .png)")
        pv_frame.pack(fill="both", expand=True, padx=4, pady=4)
        self.preview_info = ttk.Label(pv_frame, text="Click anywhere on the coverage map above to inspect Ortho / DEM / Baked City Night / Online tiles.", foreground="#2a6a8a", anchor="w")
        self.preview_info.pack(fill="x", padx=6, pady=2)
        self.preview_label = ttk.Label(pv_frame, text="", anchor="center")
        self.preview_label.pack(fill="both", expand=True)
        self._refresh_pack_contents()

    def _build_weather_tab(self) -> None:
        f = self.tab_weather
        info = ttk.LabelFrame(f, text="ERA5-calibrated mesoscale state library (.nswx)")
        info.pack(fill="x", padx=6, pady=6)
        ttk.Label(
            info,
            text=(
                "Builds packs/es_aragon_weather.nswx from real ERA5 reanalysis (Open-Meteo archive),\n"
                "solved offline (terrain flow, waves, slope circulation, LCL clouds).\n"
                "All actions below run from this GUI (no terminal needed)."
            ),
            justify="left",
        ).pack(anchor="w", padx=8, pady=6)
        btns = ttk.Frame(f)
        btns.pack(fill="x", padx=6, pady=8)
        ttk.Button(btns, text="Self-test weather solvers", command=self._wx_selftest).pack(side="left", padx=4)
        ttk.Button(btns, text="Bake weather library (fetch/cache ERA5)", command=lambda: self._wx_bake(offline=False)).pack(side="left", padx=4)
        ttk.Button(btns, text="Bake weather library (offline/cache only)", command=lambda: self._wx_bake(offline=True)).pack(side="left", padx=4)
        ttk.Button(btns, text="Generate weather previews", command=lambda: self._generate_visual_previews()).pack(side="left", padx=8)

    def _build_log(self, root: tk.Tk) -> None:
        lf = ttk.LabelFrame(root, text="Build log (step by step)")
        lf.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log_text = tk.Text(lf, height=18, wrap="word", bg="#0c0c0c", fg="#d0d0d0", insertbackground="white")
        self.log_text.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(lf, command=self.log_text.yview)
        sb.pack(side="right", fill="y")
        self.log_text.configure(yscrollcommand=sb.set)

    def _on_preset(self, _evt=None) -> None:
        p = self.PRESETS.get(self.preset_var.get())
        if p and "bbox" in p:
            self.bbox_entry.delete(0, "end")
            self.bbox_entry.insert(0, " ".join(str(x) for x in p["bbox"]))
        self._draw_world_map()

    def _on_quality(self, _evt=None) -> None:
        q = self.quality_var.get()
        desc = self.QUALITY_PROFILES.get(q, {}).get("desc", "")
        self.quality_desc.configure(text=desc)
        self._draw_world_map()

    def _get_bbox(self):
        try:
            if self.custom_var.get():
                parts = self.bbox_entry.get().strip().split()
                if len(parts) != 4:
                    return None
                return tuple(float(x) for x in parts)
            if self.preset_var.get() == "all-spain":
                return (-9.35, 35.95, 4.35, 43.80)
            p = self.PRESETS.get(self.preset_var.get())
            if p and "bbox" in p:
                return tuple(p["bbox"])
        except Exception:
            return None
        return None

    def _draw_world_map(self) -> None:
        if not hasattr(self, "map_canvas"):
            return
        c = self.map_canvas
        c.delete("all")
        w = max(240, c.winfo_width())
        h = max(200, c.winfo_height())
        c.create_rectangle(0, 0, w, h, fill="#06101b", outline="")
        vx0, vy0, vx1, vy1 = self.cov_view_bbox

        if self.cov_pil is not None and ImageTk is not None:
            iw, ih = self.cov_pil.size
            scale = min(w / max(1, iw), h / max(1, ih))
            rw, rh = max(1, int(iw * scale)), max(1, int(ih * scale))
            ox, oy = (w - rw) / 2.0, (h - rh) / 2.0
            self.cov_draw_rect = (ox, oy, rw, rh)
            resized = self.cov_pil.resize((rw, rh), Image.Resampling.BILINEAR)
            self.cov_imgtk = ImageTk.PhotoImage(resized)
            c.create_image(ox, oy, anchor="nw", image=self.cov_imgtk)
        else:
            ox, oy, rw, rh = 16.0, 16.0, float(w - 32), float(h - 32)
            self.cov_draw_rect = (ox, oy, rw, rh)
            c.create_rectangle(ox, oy, ox + rw, oy + rh, outline="#2a6a8a", width=2)
            c.create_text(w / 2, h / 2, text="Rendering coverage map...", fill="#5f8ea3")

        def to_xy(lon: float, lat: float) -> tuple[float, float]:
            px = ox + (lon - vx0) / max(1e-6, vx1 - vx0) * rw
            py = oy + (1.0 - (lat - vy0) / max(1e-6, vy1 - vy0)) * rh
            return px, py

        bb = self._get_bbox()
        if bb:
            lon0, lat0, lon1, lat1 = bb
            px0, py0 = to_xy(lon0, lat1)
            px1, py1 = to_xy(lon1, lat0)
            px0, px1 = max(ox, min(ox + rw, px0)), max(ox, min(ox + rw, px1))
            py0, py1 = max(oy, min(oy + rh, py0)), max(oy, min(oy + rh, py1))
            if px1 > px0 + 2 and py1 > py0 + 2:
                c.create_rectangle(px0, py0, px1, py1, outline="#ffd166", width=2, dash=(6, 4))
                c.create_text((px0 + px1) / 2, max(oy + 12, py0 - 8), text=f"Selected build region [{self.quality_var.get().upper()}]", fill="#ffd166", font=("TkDefaultFont", 8, "bold"))

        if self.cov_pin_latlon:
            plat, plon = self.cov_pin_latlon
            px, py = to_xy(plon, plat)
            if ox <= px <= ox + rw and oy <= py <= oy + rh:
                c.create_oval(px - 6, py - 6, px + 6, py + 6, outline="#22d3ee", width=2)
                c.create_line(px - 10, py, px + 10, py, fill="#22d3ee", width=1)
                c.create_line(px, py - 10, px, py + 10, fill="#22d3ee", width=1)

    def _refresh_coverage_map(self) -> None:
        if self._cov_busy:
            return
        self._cov_busy = True
        try:
            from builder.pack_preview import render_tile_quality_map
            layers = tuple(k for k, v in self.layer_vars.items() if v.get()) or ("imagery",)
            zf_s = self.cov_zoom_var.get()
            zf = int(zf_s[1:]) if zf_s.startswith("Z") and zf_s[1:].isdigit() else None
            rid = self.cov_region_var.get() or None
            out = ROOT / "packs" / "preview_demo" / "tile_coverage_map.png"
            _p, inv = render_tile_quality_map(
                out, ROOT / "packs", ROOT / "cache" / "online_tiles",
                region_id=rid, show_layers=layers, zoom_filter=zf,
            )
            self.cov_view_bbox = list(inv.get("view_bbox", [-9.8, 39.8, 4.6, 44.2]))
            if Image is not None and out.is_file():
                with Image.open(out) as im:
                    self.cov_pil = im.convert("RGB")
            self._draw_world_map()
        except Exception as exc:
            self._append_log(f"[coverage] {exc}\n")
        finally:
            self._cov_busy = False

    def _on_map_click(self, event) -> None:
        if not self.cov_draw_rect:
            return
        ox, oy, rw, rh = self.cov_draw_rect
        if rw <= 1 or rh <= 1:
            return
        fx = (event.x - ox) / rw
        fy = (event.y - oy) / rh
        if fx < 0 or fx > 1 or fy < 0 or fy > 1:
            return
        vx0, vy0, vx1, vy1 = self.cov_view_bbox
        lon = vx0 + fx * (vx1 - vx0)
        lat = vy1 - fy * (vy1 - vy0)
        self.cov_pin_latlon = (lat, lon)
        self._draw_world_map()
        try:
            from builder.pack_preview import inspect_point_tiles
            out = ROOT / "packs" / "preview_demo" / "point_inspector.png"
            rid = self.cov_region_var.get() or None
            info = inspect_point_tiles(lat, lon, out, ROOT / "packs", ROOT / "cache" / "online_tiles", region_id=rid)
            parts = [f"({info['lat']}, {info['lon']})"]
            if info.get("covering_preset"):
                parts.append(f"Preset '{info['covering_preset']}' (not built)")
            if info.get("ortho_z"):
                parts.append(f"Ortho Z{info['ortho_z']} ({info['ortho_gsd_m']} m/px)")
            if info.get("dem_z"):
                er = f" {info['elev_range_m'][0]}..{info['elev_range_m'][1]}m" if info.get("elev_range_m") else ""
                parts.append(f"DEM Z{info['dem_z']} ({info['dem_gsd_m']} m/px{er})")
            if info.get("city_chunk"):
                parts.append(f"City {info['city_chunk']}")
            if info.get("online_z"):
                parts.append(f"Online {info['online_provider']} Z{info['online_z']} ({info['online_gsd_m']} m/px)")
            self.preview_info.configure(text=" | ".join(parts))
            if out.exists():
                self._show_preview_image(out)
        except Exception as exc:
            self.preview_info.configure(text=f"Inspect failed at ({lat:.4f}, {lon:.4f}): {exc}")

    def _toggle_custom(self) -> None:
        state = "normal" if self.custom_var.get() else "disabled"
        self.bbox_entry.configure(state=state)
        self._draw_world_map()

    def _append_log(self, text: str) -> None:
        self.log_text.insert("end", text)
        self.log_text.see("end")

    def _poll_log(self) -> None:
        try:
            while True:
                kind, payload = self.log_q.get_nowait()
                if kind == "line":
                    self._append_log(payload)
                elif kind == "done":
                    self._append_log(f"\n[done] exit code {payload}\n")
                    self._set_busy(False)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_log)

    def _set_busy(self, busy: bool) -> None:
        state = "disabled" if busy else "normal"
        self.btn_plan.configure(state=state)
        self.btn_build.configure(state=state)

    def _open_packs(self) -> None:
        p = ROOT / "packs"
        p.mkdir(exist_ok=True)
        subprocess.Popen(["xdg-open", str(p)])

    def _refresh_pack_contents(self) -> None:
        packs_dir = ROOT / "packs"
        packs_dir.mkdir(parents=True, exist_ok=True)
        for iid in self.pack_tree.get_children():
            self.pack_tree.delete(iid)
        try:
            from builder.pack_preview import inspect_installed_modules
            mod = inspect_installed_modules(packs_dir, ROOT / "cache" / "online_tiles")
        except Exception as exc:
            self._append_log(f"[modules] {exc}\n")
            return
        self._modules_cache = mod
        rids = ["__all__"] + [r["region_id"] for r in mod.get("regions", [])]
        if hasattr(self, "cov_region_cb"):
            self.cov_region_cb.configure(values=rids)
            if self.cov_region_var.get() not in rids:
                self.cov_region_var.set(rids[1] if len(rids) == 2 else "__all__")

        for r in mod.get("regions", []):
            rid = r["region_id"]
            node = self.pack_tree.insert(
                "", "end", iid=f"reg:{rid}", open=True,
                text=f"{rid} ({r['airports_count']} apts)",
                values=(f"[{r['quality'].upper()}] O:Z{r['max_ortho_z']} D:Z{r['max_dem_z']}", f"{r['tile_count']} tiles", f"{r['total_mb']} MB"),
            )
            for lkey, lname in (("imagery", "Ortho"), ("elevation", "DEM"), ("surface", "Surface")):
                for zs in r["layers"][lkey]["zooms"]:
                    self.pack_tree.insert(
                        node, "end",
                        text=f"  {lname} Z{zs['z']}",
                        values=(f"{zs['gsd_m']} m/px", f"{zs['tiles']} tiles", f"{zs['mb']} MB"),
                    )
            if r.get("city_shadows"):
                cs = r["city_shadows"]
                self.pack_tree.insert(
                    node, "end",
                    text="  Baked Cities / 3L-Night (.nsshd)",
                    values=(f"{cs['gsd_m']} m/px (v{cs['version']})", f"{cs['chunks']} chunks", f"{cs['size_mb']} MB"),
                )
            if r.get("cities"):
                ct = r["cities"]
                self.pack_tree.insert(node, "end", text="  City Polygons (.nscity)", values=(f"v{ct['version']}", f"{ct['chunks']} chunks", f"{ct['size_mb']} MB"))
            if r.get("weather"):
                wx = r["weather"]
                self.pack_tree.insert(node, "end", text="  Weather (.nswx)", values=("ERA5 3D", f"{wx['states']} states", f"{wx['size_mb']} MB"))

        oc = mod.get("online_cache", {})
        if oc.get("total_tiles", 0) > 0:
            onode = self.pack_tree.insert(
                "", "end", open=False, text="Online Stream Cache",
                values=("Dynamic PNOA", f"{oc['total_tiles']} tiles", f"{oc['total_mb']} MB"),
            )
            for oz in oc.get("by_provider_zoom", []):
                self.pack_tree.insert(onode, "end", text=f"  {oz['provider']} Z{oz['z']}", values=(f"{oz['gsd_m']} m/px", f"{oz['tiles']} tiles", f"{oz['mb']} MB"))

        prev = packs_dir / "preview_demo"
        if prev.is_dir():
            pnode = self.pack_tree.insert("", "end", open=False, text="Preview Images (preview_demo/)", values=("PNG", "Click to view", ""))
            for p in sorted(prev.rglob("*.png")):
                rel = p.relative_to(packs_dir)
                size_s = f"{p.stat().st_size / 1024:.0f} KB" if p.exists() else "?"
                self.pack_tree.insert(pnode, "end", iid=f"png:{rel}", text=str(rel), values=("PNG", "Visual preview", size_s))

    def _selected_region_id(self) -> str | None:
        sel = self.pack_tree.selection()
        if sel and sel[0].startswith("reg:"):
            return sel[0][4:]
        v = self.cov_region_var.get()
        return None if v in ("", "__all__") else v

    def _launch_sim(self) -> None:
        cmd = [str(ROOT / "run_custom_sim.sh")]
        rid = self._selected_region_id()
        if rid:
            cmd += ["--region", rid, "--only-region"]
        subprocess.Popen(cmd, cwd=ROOT, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._append_log(f"\n[SIM] Launched: {' '.join(cmd)}\n")

    def _verify_selected_region(self) -> None:
        rid = self._selected_region_id()
        if not rid:
            messagebox.showinfo("Verify Module", "No installed region module selected.")
            return
        reg = next((r for r in self._modules_cache.get("regions", []) if r["region_id"] == rid), None)
        pack_path = ROOT / "packs" / (reg["pack_file"] if reg else f"{rid}.nspack")
        self._append_log(f"\n=== VERIFY INTEGRITY: {rid} ({pack_path.name}) ===\n")
        self._set_busy(True)
        _run([PYTHON, str(ROOT / "builder" / "test_pack.py"), str(pack_path)],
             lambda s: self.log_q.put(("line", s)), lambda rc: self.log_q.put(("done", rc)))

    def _remove_selected_region(self) -> None:
        rid = self._selected_region_id()
        if not rid:
            messagebox.showinfo("Remove Module", "No installed region module selected.")
            return
        if not messagebox.askyesno("Remove Module", f"Uninstall region '{rid}' and delete its .nspack / pre-baked files?"):
            return
        self._append_log(f"\n=== REMOVE MODULE: {rid} ===\n")
        self._set_busy(True)
        _run([PYTHON, str(ROOT / "builder" / "build_region.py"), "remove", rid, "--yes"],
             lambda s: self.log_q.put(("line", s)),
             lambda rc: (self.log_q.put(("done", rc)), self.root.after(0, self._refresh_pack_contents), self.root.after(50, self._refresh_coverage_map)))

    def _on_pack_select(self, _evt=None) -> None:
        sel = self.pack_tree.selection()
        if not sel:
            return
        iid = sel[0]
        if iid.startswith("reg:"):
            rid = iid[4:]
            self.cov_region_var.set(rid)
            self._refresh_coverage_map()
            return
        if iid.startswith("png:"):
            rel = iid[4:]
            p = ROOT / "packs" / rel
            if p.exists():
                self.preview_info.configure(text=f"Preview file: {rel}")
                self._show_preview_image(p)
                return
        item = self.pack_tree.item(iid)
        self._append_log(f"\n[module] {item.get('text', '').strip()} | {item.get('values', [])}\n")

    def _show_preview_image(self, path: Path) -> None:
        if Image is None or ImageTk is None:
            self.preview_label.configure(text=f"Pillow not available.\nImage: {path}", image="")
            return
        try:
            im = Image.open(path)
            pw = max(240, self.preview_label.winfo_width())
            ph = max(240, self.preview_label.winfo_height())
            im2 = im.copy()
            im2.thumbnail((pw - 10, ph - 10), Image.Resampling.LANCZOS)
            self.preview_imgtk = ImageTk.PhotoImage(im2)
            self.preview_label.configure(image=self.preview_imgtk, text=path.name)
        except Exception as e:
            self.preview_label.configure(text=f"Could not load preview: {e}", image="")

    def _generate_visual_previews(self) -> None:
        self._append_log("\n=== GENERATE VISUAL PREVIEWS ===\n")
        self._set_busy(True)
        env = dict(__import__("os").environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + ":" + str(ROOT)
        script = (
            "from pathlib import Path\n"
            "from builder.pack_preview import render_pack_elevation_heatmap, render_nswx_previews, render_tile_quality_map\n"
            "ROOT = Path.cwd()\n"
            "packs = ROOT / 'packs'\n"
            "out = packs / 'preview_demo'\n"
            "out.mkdir(parents=True, exist_ok=True)\n"
            "gen = []\n"
            "for p in sorted(packs.glob('*.nspack')):\n"
            "    try:\n"
            "        r = render_pack_elevation_heatmap(p, out / 'elevation_heatmap.png')\n"
            "        if r:\n"
            "            gen.append(str(r)); print('GEN', r)\n"
            "        c, _ = render_tile_quality_map(out / 'tile_coverage_map.png', packs, ROOT / 'cache' / 'online_tiles', region_id=p.stem)\n"
            "        gen.append(str(c)); print('GEN', c)\n"
            "    except Exception as e:\n"
            "        print('ELEVERR', e)\n"
            "for w in sorted(packs.glob('*.nswx')):\n"
            "    try:\n"
            "        outs = render_nswx_previews(w, out / 'nswx', season=2, slot=5, wtype=0)\n"
            "        for x in outs:\n"
            "            gen.append(str(x)); print('GEN', x)\n"
            "    except Exception as e:\n"
            "        print('WXERR', e)\n"
            "print('GENERATED_COUNT', len(gen))\n"
        )
        _run([PYTHON, "-c", script], lambda s: self.log_q.put(("line", s)),
             lambda rc: (self.log_q.put(("done", rc)), self.root.after(0, self._refresh_pack_contents), self.root.after(50, self._refresh_coverage_map), self.root.after(900, self._auto_show_latest_preview)),
             env=env)

    def _auto_show_latest_preview(self) -> None:
        prev = ROOT / "packs" / "preview_demo"
        if not prev.is_dir():
            return
        imgs = sorted(prev.rglob("*.png"), key=lambda p: p.stat().st_mtime, reverse=True)
        if imgs:
            self._show_preview_image(imgs[0])

    def _pick_data_dir(self, entry: ttk.Entry) -> None:
        path = filedialog.askdirectory(parent=self.root, title="Select XYZ tile source")
        if path:
            entry.delete(0, "end")
            entry.insert(0, path)

    def _pick_pbf(self) -> None:
        path = filedialog.askopenfilename(parent=self.root, title="Select OpenStreetMap extract", filetypes=(("OSM PBF", "*.pbf"), ("All files", "*")))
        if path:
            self.pbf_entry.delete(0, "end")
            self.pbf_entry.insert(0, path)

    def _build_common_args(self, dry_run: bool) -> list[str]:
        preset = "custom-world" if self.custom_var.get() else self.preset_var.get()
        args = [PYTHON, str(ROOT / "builder" / "build_region.py"), "build", preset,
                "--quality", self.quality_var.get()]
        args += ["--imagery-source", self.imagery_source_var.get(),
                 "--elevation-source", self.elevation_source_var.get()]
        imagery_dir = self.imagery_dir_entry.get().strip()
        elevation_dir = self.elevation_dir_entry.get().strip()
        if imagery_dir:
            args += ["--imagery-dir", imagery_dir]
        if elevation_dir:
            args += ["--elevation-dir", elevation_dir]
        if self.imagery_source_var.get() == "local-xyz":
            args += ["--imagery-attribution", self.imagery_attr_entry.get().strip()]
        if self.elevation_source_var.get() == "local-terrarium":
            args += ["--elevation-attribution", self.elevation_attr_entry.get().strip()]
        pbf = self.pbf_entry.get().strip()
        if pbf:
            args += ["--pbf", pbf]
        if self.custom_var.get():
            parts = self.bbox_entry.get().strip().split()
            if len(parts) != 4:
                raise ValueError("bbox must be 4 numbers: lon_min lat_min lon_max lat_max")
            try:
                bounds = tuple(float(x) for x in parts)
            except ValueError as exc:
                raise ValueError("bbox values must all be numbers") from exc
            rid = "world_" + hashlib.sha256(",".join(f"{v:.6f}" for v in bounds).encode("ascii")).hexdigest()[:12]
            args += ["--bbox", *parts, "--region-id", rid,
                     "--name", f"World region {bounds[0]:.3f},{bounds[1]:.3f} to {bounds[2]:.3f},{bounds[3]:.3f}"]
        if dry_run:
            args.append("--dry-run")
        if self.bake_weather_var.get():
            args.append("--bake-weather")
        if self.bake_cities_var.get():
            args.append("--bake-cities")
        if self.bake_scenery_var.get():
            args.append("--bake-scenery")
        if self.bake_vegetation_var.get():
            args.append("--bake-vegetation")
        else:
            args.append("--no-bake-vegetation")
        if self.no_night_var.get():
            args.append("--no-night-lights")
        return args

    def _dry_run(self) -> None:
        try:
            cmd = self._build_common_args(dry_run=True)
        except ValueError as e:
            messagebox.showerror("Invalid input", str(e))
            return
        self._append_log("\n=== DRY RUN ===\n")
        self._set_busy(True)
        env = dict(__import__("os").environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + ":" + str(ROOT)
        _run(cmd, lambda s: self.log_q.put(("line", s)), lambda rc: self.log_q.put(("done", rc)), env=env)

    def _build_pack(self) -> None:
        try:
            pack_cmd = self._build_common_args(dry_run=False)
        except ValueError as e:
            messagebox.showerror("Invalid input", str(e))
            return
        if not messagebox.askyesno("Start build", "Start the GUI build pipeline (all steps run here)? This can take a long time."):
            return
        self._set_busy(True)
        self._append_log("\n=== ECOSYSTEM BUILD ===\n")
        env = dict(__import__("os").environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + ":" + str(ROOT)
        _run(
            pack_cmd,
            lambda s: self.log_q.put(("line", s)),
            lambda rc: (self.log_q.put(("done", rc)), self.root.after(0, self._refresh_pack_contents), self.root.after(50, self._refresh_coverage_map)),
            env=env,
        )

    def _wx_selftest(self) -> None:
        self._append_log("\n=== WEATHER SELFTEST ===\n")
        self._set_busy(True)
        env = dict(__import__("os").environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + ":" + str(ROOT)
        _run([PYTHON, str(ROOT / "builder" / "bake_weather.py"), "--selftest"],
             lambda s: self.log_q.put(("line", s)), lambda rc: self.log_q.put(("done", rc)), env=env)

    def _wx_bake(self, offline: bool = False) -> None:
        self._append_log("\n=== WEATHER BAKE ===\n")
        self._set_busy(True)
        env = dict(__import__("os").environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + ":" + str(ROOT)
        cmd = [PYTHON, str(ROOT / "builder" / "bake_weather.py")]
        if offline:
            cmd.append("--offline")
        _run(cmd, lambda s: self.log_q.put(("line", s)), lambda rc: self.log_q.put(("done", rc)), env=env)


def _run_selftest(render_check: str | None = None) -> int:
    from builder.pack_preview import inspect_installed_modules, inspect_point_tiles, render_tile_quality_map

    packs_dir = ROOT / "packs"
    cache_dir = ROOT / "cache" / "online_tiles"
    inv = inspect_installed_modules(packs_dir, cache_dir)
    assert isinstance(inv, dict) and "regions" in inv and "online_cache" in inv
    out_cov = packs_dir / "preview_demo" / "tile_coverage_map.png"
    _c, inv2 = render_tile_quality_map(out_cov, packs_dir, cache_dir)
    assert out_cov.is_file() and len(inv2.get("view_bbox", [])) == 4
    out_pt = None
    if inv["regions"]:
        out_pt = packs_dir / "preview_demo" / "point_inspector.png"
        pt_info = inspect_point_tiles(41.6538, -0.8806, out_pt, packs_dir, cache_dir)
        assert out_pt.is_file() and pt_info.get("pack")

    if render_check and Image is not None:
        im_c = Image.open(out_cov).convert("RGB")
        if out_pt and out_pt.is_file():
            im_p = Image.open(out_pt).convert("RGB")
            pw = im_c.width
            ph = int(im_p.height * (pw / max(1, im_p.width)))
            im_p = im_p.resize((pw, ph), Image.Resampling.BILINEAR)
            board = Image.new("RGB", (pw, im_c.height + ph + 8), (6, 16, 27))
            board.paste(im_c, (0, 0))
            board.paste(im_p, (0, im_c.height + 8))
        else:
            board = im_c
        Path(render_check).parent.mkdir(parents=True, exist_ok=True)
        board.save(render_check)

    try:
        root = tk.Tk()
        root.withdraw()
        app = PackCompilerGUI(root)
        app._refresh_coverage_map()
        root.update_idletasks()
        assert len(app.pack_tree.get_children()) >= len(inv["regions"])
        assert app.cov_pil is not None
        root.destroy()
    except tk.TclError as exc:
        print(f"[GUI SELFTEST] headless skip Tk widgets ({exc})")

    print(
        f"[GUI SELFTEST OK] regions={len(inv['regions'])} online_tiles={inv['online_cache']['total_tiles']} "
        f"view_bbox={inv2['view_bbox']}"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true", help="verify Module Manager inspection & GUI state cleanly")
    ap.add_argument("--render-check", default=None, help="write combined coverage + point inspector PNG for visual check")
    args = ap.parse_args()
    if args.selftest or args.render_check:
        return _run_selftest(args.render_check)
    root = tk.Tk()
    PackCompilerGUI(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
