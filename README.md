# OpenSky VFR Simulator

OpenSky is an iGPU-first VFR simulator project built around a native **C++20 / OpenGL 4.6** renderer. The first curated flight region is the Pyrenees in Aragón, with a Cessna 172S and an **OpenSky World Editor** in the source checkout. The current runnable target is Linux with X11/GLX; Windows packaging and measured 1080p High support remain release milestones.

The product target is 1920×1080 output at 60 FPS on integrated graphics, with the renderer allowed a bounded internal reconstruction scale. The documented Renoir baseline is below that target in several High scenarios. See the dated [engine audit](docs/2026-11-23_opensky-engine-audit.md) and [flight/weather audit](docs/2026-11-23_flight-weather-audit.md) for measurements and known gaps.

## Install and launch the game

The Debian/Ubuntu release is packaged as `opensky-vfr_<version>_amd64.deb` (the app and Aragón) plus `opensky-vfr-catalonia_<version>_all.deb` (Catalonia Coast and Pyrenees). Install both with the graphical Software app, or run `sudo apt install ./opensky-vfr_<version>_amd64.deb ./opensky-vfr-catalonia_<version>_all.deb`, then launch **OpenSky VFR** from the Games menu. Players do not compile the source or start the game from a script. The package recipe is in `tools/package_deb.py`; these downloads are not published yet because the asset and data rights listed in [release readiness](docs/RELEASE_READINESS.md) still need to be cleared. Windows 11 is not supported yet.

Add-on regions can be installed in `~/.local/share/opensky-vfr/packs/`. Saves and aircraft packages stay in the user's config folder; downloaded imagery stays in the user's cache folder.

---

## Architecture
- `protocol/` — Shared `.nspack` binary pack specification ([pack_format.h](protocol/pack_format.h) / [pack_format.py](protocol/pack_format.py)).
- `builder/` — Offline regional pack baker ([build_region.py](builder/build_region.py)) ingesting **IGN Spain PNOA**, **IGN MDT05/MDT25**, **OurAirports**, and **OpenStreetMap**. The World Editor also accepts user-licensed XYZ imagery and Terrarium elevation tiles outside Spain, or builds explicit elevation-only regions.
- `sim_engine/` — C++20/OpenGL renderer with CDLOD terrain, instanced and baked city meshes, vegetation shading, reduced-resolution volumetric clouds, baked solar and night lighting, and reverse-Z terrain depth.
- `engine/` — WGS84 geometry, light-piston aircraft systems, OpenTrack UDP and the adapter to upstream JSBSim. Aircraft packages keep project aircraft data separate from the LGPL library.
- `game/` — Runtime textures, GLB models, the factory aircraft package, and native libraries.
- [Engine audit](docs/2026-11-23_opensky-engine-audit.md) and [flight/weather audit](docs/2026-11-23_flight-weather-audit.md) — Dated performance and implementation evidence.

---

## Build from source (developers)

These steps are for contributors building the source checkout. Players use the installable app package described above when a release is published.

### 1. Bake a regional pack (Zaragoza LEZG – Huesca LEHC – Pyrenees corridor)
```bash
python3 builder/build_region.py --preset zaragoza-pyrenees-quick
python3 builder/test_pack.py packs/es_aragon_pyrenees.nspack
```
For higher detail (`z15–z16` insets, full cruise `z10–z13`):
```bash
python3 builder/build_region.py --preset zaragoza-pyrenees
```

### 2. Build the native C++20 renderer
```bash
cd sim_engine && make -j"$(nproc)"
```
The build creates upstream JSBSim as a separate LGPL shared library in
`game/bin/libJSBSim.so.1` and links the simulator to it dynamically. A clean
build needs CMake 3.18 or newer. `CMAKE=/path/to/cmake` selects a non-default
installation.

### 3. Run the simulator
```bash
./run_sim.sh --online-ortho pnoa
```
Flights and visual QA start with online-first IGN PNOA imagery, distance-tiered
(Z20 within 200 m, Z19 within 500 m, down to Z14 at 20 km). A cached online tile
replaces the matching pack imagery; the installed imagery remains visible while
downloads complete and remains the fallback offline. Downloads are asynchronous
and the decoded tile cache is bounded. `O` toggles online imagery. Use
`--online-ortho off` to force installed imagery for an offline comparison. The
four-season review is `./run_sim.sh --season-qa /tmp/opensky-seasons`; a single
season can be selected with `--season winter|spring|summer|autumn`.

Run `./run_sim.sh --selftest` for the headless verification suite and
`make -C sim_engine flight-test` for JSBSim trim, controls, terrain contacts,
and braking checks. For GPU screenshots run
`./run_sim.sh -- --autotest /tmp/opensky-visual-qa --online-ortho pnoa`.
`./run_sim.sh -- --quick-profile /tmp/opensky-profile` captures two short High routes
with startup, frame, GPU and resource traces. Use `--profile <dir>` to record while flying.
See the [engine audit](docs/2026-11-23_opensky-engine-audit.md) for the compression comparison
and verification cases.
See the [current flight and weather audit](docs/2026-11-23_flight-weather-audit.md)
for the JSBSim wiring, Earth curvature review, weather limitations and next gates.

### Runtime verification

Run `./run_sim.sh --runtime-check --online-ortho off` with Aragón and Catalonia
installed. `--runtime-check-gpu` runs the same replay with GPU weather uploads and
checks stale terrain jobs. `--outlook-check` also waits for all three forecast leads.
These checks use their own temporary aircraft profile. Capture the World menu
with `./run_sim.sh --world-qa /tmp/world.png --airport ES-0098 --online-ortho off`.

### Start a flight

Open **World** to find the C172S at its home airport, Castejón de Sos, on a new save. Select an installed
airfield and add it to your route, or use **Add map point** for a visual
waypoint. The route appears on the globe and gives a true bearing and distance
to the active waypoint in flight.

Choose **Enter airport on foot**, then use `WASD` to walk and drag either mouse
button or use the arrow keys to look around. Press `E` near the aircraft to refuel, repair, or
board it. **Board at parking** starts from the parking spot. Taxi to the runway for departure. After landing, stop
near an installed airfield and choose **Park and step outside** from the pause
menu. That airfield becomes your temporary base, and the next session resumes
from there. Castejón de Sos remains home. The aircraft's parked location, fuel,
condition, engine hours, and route are saved locally. The World menu shows
the regional weather model's current conditions and recent history. **Outlook**
runs a separate projection at +1, +3, and +5 simulation minutes using the
current model inputs.

### World Editor

New packs include a shaded elevation image for the World menu. Bake images for
already installed packs with `python3 builder/bake_map_relief.py packs/*.nspack`.
The images are loaded at startup and travel with their packs.

Start it with `./run_builder.sh`. Choose a Northern Spain preset or draw custom bounds. For global regions, choose `local-xyz` and an imagery directory in `<root>/<z>/<x>/<y>.jpg|png|webp` layout, or choose `none` for terrain-only presentation. Elevation can use Terrarium XYZ PNG tiles in the same directory layout. Imported layers require an attribution string; the pack manifest records it. Custom region IDs derive from all four bounds so separate areas do not collide.

For OSM roads and baked settlements outside the bundled Spain extracts, provide a local `.pbf` extract. Spanish region builds bake IGN LiDAR canopy across the full selected region at Z15 by default and derive coarser levels for distance. OSM taxiways and apron polygons are drawn with the airport runway surfaces in every region. In flight, canopy heights share the fixed terrain page cache with the orthophoto and ground elevation.

### Aircraft packages and source mods

List installed aircraft with `./run_sim.sh --list-aircraft`. Install a package folder in `game/aircraft/<id>/` or `~/.config/opensky-vfr/aircraft/<id>/`, then select it with `./run_sim.sh --aircraft <id>`. An interactive selection is saved in the fleet profile. Format 1 packages include an `aircraft.ini`, JSBSim aircraft/engine data, and exterior/cockpit GLB models. The current systems profile supports one piston engine and two tanks; Lua aircraft scripts and multi-engine/jet systems are future work.

See [docs/MODDING.md](docs/MODDING.md) for the package format, model node mappings, region content workflow, and current limits. See [CONTRIBUTING.md](CONTRIBUTING.md) for code contribution rules and [docs/RELEASE_READINESS.md](docs/RELEASE_READINESS.md) for commercial release blockers.

### Controls
| Input | Action |
|---|---|
| `W` / `S` | Throttle up / down |
| `A` / `D` | Roll / bank |
| `↑` / `↓` | Pitch down / up |
| `Q` / `E` | Rudder left / right |
| Right-drag mouse | Look around in all camera views (`R` centers view and mouse yoke) |
| Left-drag mouse | Look around when mouse yoke is off |
| `Y` | Toggle mouse yoke (off by default); move the mouse to steer and hold the deflection |
| `R` | Center the mouse yoke and cockpit view |
| `C` | Cycle camera: Cockpit → Chase → Free Cam |
| `Page Up` / `Page Down` | Raise / lower the cockpit seat |
| `Home` / `End` | Move the cockpit seat forward / back |
| `Backspace` | Reset the cockpit seat position |
| `Esc` / `Space` | Pause to the World screen |
| `X` | Release the parking brake, or hold for wheel brakes |
| Settings | Adjust terrain LOD, online imagery, aircraft and weather detail, super-resolution, AA, and native cockpit resolution |
| `K` | Toggle cold & dark / running engine |
| `F` / `V` | Flaps down / up |
| `F1`–`F4` | Graphics tier: Low (iGPU) → Medium → High → Ultra |
| `F5` | Live CPU, GPU, memory and frame-time profiler |
| `F6` | Cycle the four terrain-following weather debug layers |
| `F7` | Aircraft physics telemetry and projected vectors |
| `L` | Toggle real UTC sun vs fixed pre-baked TOD |
| `P` | Cycle 10 pre-baked TODs + night |
| `B` | Advance weather |
| `M` | Cycle mesoscale wind regime |
| `O` | Toggle distance-tiered PNOA online ortho |
| `H` | Toggle diagnostic overlay |

---

## Current status

The supported runtime is Linux/X11/GLX. The target is 1080p at 60 FPS on AMD Renoir, but the dated [Renoir benchmark](docs/benchmarks/2026-11-23-renoir-linux.md) does not meet the average-frame or p95 gates. Aircraft packages currently support one piston engine and two tanks; scripted and multi-engine systems are not implemented. See [release readiness](docs/RELEASE_READINESS.md) for the remaining product and redistribution blockers.

## Attribution (Required)
- **Orthophoto:** PNOA cedido por © Instituto Geográfico Nacional de España (CC-BY 4.0).
- **Elevation & Hydrography:** MDT05 / MDT25 & INSPIRE Hidrografía © Instituto Geográfico Nacional de España (CC-BY 4.0); border fallback AWS Terrain Tiles.
- **Airports & Scenery:** OurAirports (Public Domain), OpenStreetMap contributors (ODbL); ENAIRE AIP reuse terms are under review.
- **PBR Ground & Material Textures:** Poly Haven (CC0 1.0 Public Domain).

## License
Project source is MIT (`LICENSE`). Third-party code, data and assets keep their
own licenses — see `THIRD_PARTY_LICENSES.md`. Generated OSM-derived packs are
ODbL share-alike; do not redistribute them under MIT.
