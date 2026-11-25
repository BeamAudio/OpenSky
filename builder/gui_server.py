#!/usr/bin/env python3
"""gui_server.py — Local HTTP backend + Module Manager API for build_region_gui.html."""
from __future__ import annotations

import http.server
import json
import os
import subprocess
import sys
import threading
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from builder.build_region import PRESETS, QUALITY_PROFILES  # noqa: E402
from builder.pack_preview import (  # noqa: E402
    inspect_installed_modules,
    inspect_point_tiles,
    render_nswx_previews,
    render_pack_elevation_heatmap,
    render_tile_quality_map,
)
from protocol.pack_format import PackReader  # noqa: E402

PYTHON = sys.executable
PACKS = ROOT / "packs"
CACHE = ROOT / "cache" / "online_tiles"
PREVIEW = PACKS / "preview_demo"
HTML_PATH = ROOT / "builder" / "build_region_gui.html"

LOG_Q: list[str] = []
LOG_SEQ = 0
TASK_BUSY = False
LOG_LOCK = threading.Lock()


def log_line(text: str) -> None:
    global LOG_SEQ
    with LOG_LOCK:
        for line in text.splitlines():
            LOG_Q.append(line)
            LOG_SEQ += 1
        if len(LOG_Q) > 2000:
            del LOG_Q[:-2000]


def run_cmd(cmd: list[str], env: dict[str, str] | None = None, on_done=None) -> None:
    def worker() -> None:
        global TASK_BUSY
        rc = 1
        try:
            with LOG_LOCK:
                TASK_BUSY = True
            proc = subprocess.Popen(
                cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env
            )
            assert proc.stdout is not None
            for line in iter(proc.stdout.readline, ""):
                log_line(line)
            proc.stdout.close()
            rc = proc.wait()
            log_line(f"[done] exit code {rc}")
        except Exception as exc:
            log_line(f"[ERROR] {exc}")
        finally:
            if on_done:
                on_done(rc)
            else:
                with LOG_LOCK:
                    TASK_BUSY = False

    threading.Thread(target=worker, daemon=True).start()


def pack_contents_text(path: Path) -> str:
    name = path.name
    if name.endswith(".nspack"):
        try:
            with PackReader(path) as r:
                m = r.manifest
                return (
                    f"Terrain+ortho pack: {len(m.get('airports', []))} airports, {r.index_count} tiles | "
                    f"elev {m.get('elevation_zooms')} | ortho {m.get('imagery_zooms')}"
                )
        except Exception as e:
            return f".nspack (read error: {e})"
    if name.endswith(".nscity"):
        return "Cities (.nscity): exact OSM polygons + 10-TOD GI + 3L NightRT"
    if name.endswith(".nsshd"):
        return "City shadows (.nsshd): 10-TOD shadows + 3L ray-traced night tiles"
    if name.endswith(".nsscn"):
        return "Scenery (.nsscn): OSM roads/buildings/elevation"
    if name.endswith(".nswx"):
        return "Weather (.nswx): 220 ERA5-calibrated baked 3D atmospheric states"
    return "File"


def list_packs() -> list[dict[str, str]]:
    items = []
    if not PACKS.is_dir():
        return items
    for p in sorted(PACKS.iterdir()):
        if not p.is_file() or p.suffix.lower() not in (".nspack", ".nscity", ".nsshd", ".nsscn", ".nswx"):
            continue
        size_mb = p.stat().st_size / (1024 * 1024)
        size_s = f"{size_mb:.1f} MB" if size_mb >= 0.1 else f"{p.stat().st_size / 1024:.0f} KB"
        items.append({"artifact": p.name, "kind": pack_contents_text(p), "size": size_s})
    if PREVIEW.is_dir():
        for p in sorted(PREVIEW.rglob("*.png")):
            rel = p.relative_to(PACKS)
            items.append({"artifact": str(rel), "kind": "Visual preview PNG", "size": f"{p.stat().st_size / 1024:.0f} KB"})
    return items


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args) -> None:
        pass

    def _send_json(self, obj, code: int = 200) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path: Path, content_type: str) -> None:
        resolved = path.resolve()
        if not (resolved.is_relative_to(PACKS.resolve()) or resolved == HTML_PATH.resolve()) or not resolved.is_file():
            self.send_response(404)
            self.end_headers()
            return
        data = resolved.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        url = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if url in ("/", "/build_region_gui.html"):
            self._send_file(HTML_PATH, "text/html; charset=utf-8")
        elif url == "/api/presets":
            out = {"all-spain": {"name": "ALL SPAIN (modular build across Spain)", "bbox": [-9.35, 35.95, 4.35, 43.80]}}
            for k, v in PRESETS.items():
                out[k] = {"name": v.get("name"), "bbox": v.get("bbox")}
            self._send_json(out)
        elif url == "/api/quality":
            self._send_json({k: {"desc": v.get("desc", "")} for k, v in QUALITY_PROFILES.items()})
        elif url.startswith("/api/preset/"):
            key = urllib.parse.unquote(url.rsplit("/", 1)[-1])
            if key == "all-spain":
                self._send_json({"name": "ALL SPAIN (modular build across Spain)", "bbox": [-9.35, 35.95, 4.35, 43.80]})
            else:
                self._send_json(PRESETS.get(key, {}))
        elif url.startswith("/api/quality/"):
            key = urllib.parse.unquote(url.rsplit("/", 1)[-1])
            self._send_json(QUALITY_PROFILES.get(key, {}))
        elif url == "/api/packs":
            self._send_json(list_packs())
        elif url == "/api/modules":
            self._send_json(inspect_installed_modules(PACKS, CACHE))
        elif url == "/api/coverage":
            rid = (qs.get("region") or [None])[0]
            layers_s = (qs.get("layers") or ["imagery,cities,online"])[0]
            layers = tuple(x.strip() for x in layers_s.split(",") if x.strip()) or ("imagery",)
            zf_s = (qs.get("z") or [""])[0]
            zf = int(zf_s) if zf_s.isdigit() else None
            bb_s = (qs.get("bbox") or [""])[0]
            hl = None
            if bb_s:
                try:
                    pts = [float(x) for x in bb_s.split(",")]
                    if len(pts) == 4:
                        hl = (pts[0], pts[1], pts[2], pts[3])
                except ValueError:
                    hl = None
            out = PREVIEW / "tile_coverage_map.png"
            try:
                _p, inv = render_tile_quality_map(
                    out, PACKS, CACHE, region_id=rid, show_layers=layers, zoom_filter=zf, highlight_bbox=hl
                )
                self._send_json({
                    "image": "preview_demo/tile_coverage_map.png",
                    "regions": len(inv["regions"]),
                    "online_tiles": inv["online_cache"]["total_tiles"],
                    "view_bbox": inv.get("view_bbox", [-9.8, 39.8, 4.6, 44.2]),
                })
            except Exception as e:
                self._send_json({"error": str(e)}, code=400)
        elif url == "/api/inspect_point":
            try:
                lat = float((qs.get("lat") or ["41.6538"])[0])
                lon = float((qs.get("lon") or ["-0.8806"])[0])
                rid = (qs.get("region") or [None])[0]
                out = PREVIEW / "point_inspector.png"
                info = inspect_point_tiles(lat, lon, out, PACKS, CACHE, region_id=rid)
                info["image"] = "preview_demo/point_inspector.png"
                self._send_json(info)
            except Exception as e:
                self._send_json({"error": str(e)}, code=400)
        elif url == "/api/logs":
            since = int((qs.get("since") or ["0"])[0])
            with LOG_LOCK:
                start = max(0, len(LOG_Q) - max(0, LOG_SEQ - since))
                self._send_json({"seq": LOG_SEQ, "busy": TASK_BUSY, "lines": LOG_Q[start:]})
        elif url.startswith("/api/preview/img/"):
            rel = urllib.parse.unquote(url[len("/api/preview/img/"):])
            self._send_file(PACKS / rel, "image/png")
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self) -> None:
        url = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        env = dict(os.environ)
        env["PYTHONPATH"] = str(ROOT / ".tools" / "pydeps") + os.pathsep + str(ROOT)

        if url == "/api/build":
            args = [PYTHON, str(ROOT / "builder" / "build_region.py"), "build",
                    body.get("preset", "zaragoza-pyrenees-quick"),
                    "--quality", body.get("quality", "high")]
            if body.get("custom") and body.get("bbox"):
                bb = [str(x) for x in body["bbox"]]
                rid = f"custom_{bb[0].replace('.', 'p')}_{bb[2].replace('.', 'p')}"
                args += ["--bbox", *bb, "--region-id", rid]
            if body.get("dry_run"):
                args.append("--dry-run")
            if body.get("bake_weather"):
                args.append("--bake-weather")
            if body.get("bake_cities"):
                args.append("--bake-cities")
            if body.get("bake_scenery"):
                args.append("--bake-scenery")
            if body.get("no_night"):
                args.append("--no-night-lights")
            log_line("=== " + ("DRY RUN" if body.get("dry_run") else "ECOSYSTEM BUILD") + " ===")
            run_cmd(args, env=env)
            self._send_json({"status": "started"})
        elif url == "/api/launch_sim":
            cmd = [str(ROOT / "run_custom_sim.sh")]
            rid = str(body.get("region_id") or "")
            if rid and rid != "__all__":
                cmd += ["--region", rid]
                if body.get("only_region"):
                    cmd.append("--only-region")
            subprocess.Popen(cmd, cwd=ROOT, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            log_line(f"=== LAUNCH SIM: {' '.join(cmd)} ===")
            self._send_json({"status": "launched", "region_id": rid or "__all__"})
        elif url == "/api/weather":
            cmd = [PYTHON, str(ROOT / "builder" / "bake_weather.py")]
            if body.get("selftest"):
                cmd.append("--selftest")
            if body.get("offline"):
                cmd.append("--offline")
            log_line("=== " + ("WEATHER SELFTEST" if body.get("selftest") else "WEATHER BAKE") + " ===")
            run_cmd(cmd, env=env)
            self._send_json({"status": "started"})
        elif url == "/api/verify":
            rid = str(body.get("region_id", "es_aragon_pyrenees"))
            pack_path = PACKS / str(body.get("pack_file") or f"{rid}.nspack")
            if not pack_path.is_file():
                inv = inspect_installed_modules(PACKS, CACHE)
                match = next((r["pack_file"] for r in inv["regions"] if r["region_id"] == rid), None)
                if match:
                    pack_path = PACKS / match
            log_line(f"=== VERIFY INTEGRITY: {rid} ({pack_path.name}) ===")
            run_cmd([PYTHON, str(ROOT / "builder" / "test_pack.py"), str(pack_path)], env=env)
            self._send_json({"status": "started"})
        elif url == "/api/remove":
            rid = str(body.get("region_id", ""))
            if not rid:
                self._send_json({"error": "missing region_id"}, code=400)
                return
            log_line(f"=== REMOVE MODULE: {rid} ===")
            run_cmd([PYTHON, str(ROOT / "builder" / "build_region.py"), "remove", rid, "--yes"], env=env)
            self._send_json({"status": "started"})
        elif url == "/api/preview":
            def job() -> None:
                global TASK_BUSY
                with LOG_LOCK:
                    TASK_BUSY = True
                try:
                    PREVIEW.mkdir(parents=True, exist_ok=True)
                    for p in sorted(PACKS.glob("*.nspack")):
                        r = render_pack_elevation_heatmap(p, PREVIEW / "elevation_heatmap.png")
                        if r:
                            log_line(f"GEN {r}")
                        c_png, _ = render_tile_quality_map(PREVIEW / "tile_coverage_map.png", PACKS, CACHE, region_id=p.stem)
                        log_line(f"GEN {c_png}")
                    for w in sorted(PACKS.glob("*.nswx")):
                        for x in render_nswx_previews(w, PREVIEW / "nswx", season=2, slot=5, wtype=0):
                            log_line(f"GEN {x}")
                    log_line("[done] exit code 0")
                except Exception as exc:
                    log_line(f"[ERROR] {exc}")
                finally:
                    with LOG_LOCK:
                        TASK_BUSY = False

            threading.Thread(target=job, daemon=True).start()
            self._send_json({"status": "started"})
        elif url == "/api/openpacks":
            PACKS.mkdir(parents=True, exist_ok=True)
            try:
                if os.name == "nt":
                    os.startfile(str(PACKS))  # type: ignore[attr-defined]
                else:
                    subprocess.Popen(["xdg-open", str(PACKS)])
            except Exception:
                pass
            self._send_json({"status": "ok"})
        else:
            self.send_response(404)
            self.end_headers()


def main() -> None:
    PACKS.mkdir(parents=True, exist_ok=True)
    port = int(os.environ.get("NSPAIN_GUI_PORT", "8765"))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"Module Manager backend listening on http://127.0.0.1:{port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
