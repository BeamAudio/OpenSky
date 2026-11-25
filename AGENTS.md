# OpenSky VFR project rules

## Product invariants

- OpenSky is a free-roam VFR exploration sandbox. The player owns one Cessna 172S, starts at Castejón de Sos (`ES-0205`), and discovers the region by flying.
- Landing and parking clear of the runway can establish a temporary base. Only successful parking changes the saved base. Boarding starts at the saved stand. Preserve location, heading, aircraft condition, fuel, and logbook on restart. Exiting before parking keeps the last saved base.
- Browsing the map, selecting airfields, and planning routes never move the aircraft. Keep instant relocations and slew in explicit QA modes. Keep missions, jobs, and permanent parking purchases out of the ownership loop.
- The World menu uses **baked elevation relief** with one palette across regions. Bake `.relief.png` sidecars with region packs, upload them once with mipmaps, and reuse them while panning or zooming. Satellite imagery is for the flight scene.
- Weather shown as current or historical must come from a published in-game model snapshot. Label the source of the model inputs. Open-Meteo supplies current boundary conditions when live input is available. The outlook projects the in-game state at +1, +3, and +5 simulation minutes under fixed inputs. Show when a selected airfield is outside model coverage.

## Engine and compatibility

- The active runtime is **C++20 / OpenGL 4.6 on Linux/X11/GLX**. Build with `make -C sim_engine -j"$(nproc)"` and run from the repository root with `./run_sim.sh`. Use `CMAKE=/path/to/cmake` when CMake is outside PATH.
- `AircraftSession` owns the fleet rules. `fleet_profile` validates and atomically writes the save. Keep v1/v2 readers and preserve unreadable user files. Scripted runs use isolated profiles. Keep JSBSim at 120 Hz and keep render calls from advancing simulation time.
- Terrain jobs use an immutable `PackReader::WorldFrame`. Keep region transitions centralized and reject stale terrain and weather results before publication. Weather consumers read a coherent snapshot with its domain, source, generation, and solved time.
- `builder/` produces scenery packs and sidecars. `protocol/` defines the C++ and Python pack formats. Keep readers for installed data when retiring obsolete producers. Store generated packs, caches, captures, and temporary bakes outside tracked source.
- See the [engine handoff](docs/ENGINE_HANDOFF.md) for current verification, performance evidence, and the next engine milestone.

## Visual and performance rules

- Target **60 FPS or better on AMD Radeon Renoir**, with room for higher tiers on discrete GPUs. Prefer baking, light shader approximations, and offline or startup texture synthesis. Keep internal 3D resolution controlled by the active quality tier (`F1`–`F4`) and size the native window to the monitor work area with a comfortable 720p or larger logical footprint on HiDPI displays.
- Avoid full-resolution depth copies and SSAO on `F1`/`F2`. Avoid real-time shadow maps for large populations of alpha-tested foliage. Use baked or analytic contact occlusion and terrain shadow data. Throttle expensive sky and reflection updates to weather changes or at most 0.5 Hz.
- Keep flight scenery driven by IGN PNOA imagery. Prioritize `L7–L9` streaming close to the aircraft. Keep micro-detail under 1 km to subtle zero-mean luminance grain within ±12%. Avoid procedural blotches, synthetic urban paver grids, and heavy rock stripes.
- Keep the natural sense of scale with about 50° vertical cockpit FOV and trees 6–13.5 m tall. Use continuous, interlocking forest canopies and soft under-canopy shadows. Terrain, vegetation, and runways must share the same atmospheric haze and value balance.
- Keep road traffic compact at about 3.2 m per vehicle, with simple silhouettes and density capped by road segment length. Bake night lighting across DSM slices at 0, 5, and 10 m. Preserve evolving cloud, wind, turbulence, and precipitation across the weather model's vertical layers.
- Preserve the UTC astronomical solar cycle and the short night. The sun still sets and rises.
- For visual changes, capture live GPU screenshots using `./run_sim.sh -- --autotest <dir>` and compare against aerial or cockpit photos of the exact location. Useful reference sites include Castejón de Sos, Río Ésera, Santa Cilia (`LECI`), Huesca (`LEHC`), and Embalse de Mediano. Compare Renoir performance at the same settings and comparable terrain residency. Report frame time p95 alongside mean FPS and note whether the capture crossed a region boundary.

## Asset and data rules

Use commercially reusable sources and retain attribution. Keep upstream JSBSim dynamically linked under LGPL. Approved sources include IGN Spain PNOA / MDT05 / MDT25 / INSPIRE Hidrografía (CC BY 4.0), OurAirports (public domain), OpenStreetMap (ODbL), Poly Haven (CC0), and Open-Meteo subject to the project's data attribution and feed terms. Follow [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Esri imagery, GPL aircraft models/configurations/textures, and unlicensed assets are prohibited.
