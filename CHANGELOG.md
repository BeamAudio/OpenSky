# Changelog

## 0.3.2 (2026-11-27)

- Fixed wind seams between regional weather fields and preserved solved wind when the aircraft crosses into a new terrain frame. The World map now has separate Wind and Cloud & Rain overlays, and cloud initialization no longer adds artificial bands.
- Expanded `weather_detail` to 7 gradual volumetric cloud quality tiers (`Very Low` through `Ultra (Max Clouds)`, plus `Follow preset`) with `cloud_res_percent` scaling across 1080p–4K displays.
- Rebuilt volumetric cloud shading in `clouds_qres.frag` around a unified 3D Perlin-Worley/Worley density field coupled directly to the solved mesoscale weather field (`0.56*L1 + 0.44*L2` mid-cloud steering wind with 60 Hz continuous field-advection interpolation, downwind vertical shear, dynamic `2/3/4`-tap directional sun light marching driven by Settings & `FlightFrameBudget`, opaque zero-visibility IMC interiors, and smooth mountain-crest depth fusion in `post_tonemap.frag`).
- Added a real-time 2D `256x160` `GL_RGBA16F` shallow-water windshield fluid simulation (`windshield_rain_sim.frag`) driven by 3D body-frame airspeed, ambient crosswind/headwind, gravity projection, and clockwise propeller slipstream wash, plus articulated 3D conformal pantograph windshield wipers (`KEY_J` / panel switch) and metric object-space rain/fluid parity across lateral and rear cockpit windows.
- Updated `FlightFrameBudget` adaptive pixel scaling floor (`44%` / `36%` thermal) for Ryzen/Renoir 60 FPS consistency while preserving native output resolution for cockpit instruments.
- Added 5 `1080×1920` vertical @ 60 FPS engine showcase promo clips (`engine_load_balancing`, `engine_tile_loading`, `engine_weather_cloud_terrain`, `engine_aircraft_systems`, `engine_aircraft_physics`).
- Fixed acoustic CFL instability, Davies nested boundary coupling, and debug pathline visualization in local weather refinement (`192×192` / `384×384`), added dividing-streamline orographic terrain flow deflection, and updated the 9:16 vertical weather promo videos.
- Replaced World map wind arrows with an animated, zoom-adaptive GPU streamline flowmap across all installed `.nswx` regions.
- Replaced scrolling raindrop LUTs on cockpit glass with procedural optical convex-lens water beads and meandering stick-slip rivulets.
- Added live VSync and FPS limiter controls in **Settings** (`graphics.ini`).
- Added plug-and-play Linux `evdev` USB flight yoke, rudder pedal (with differential toe brakes), and throttle/mixture quadrant support with Direct/Assisted mode, deadzone, S-curve, axis invert, and Center calibration in **Settings** and **Controls**.
- Added folder-based aircraft packages with a manifest, package-local JSBSim data, exterior/cockpit GLB assets, configurable two-tank capacities, oil capacity, flap travel, electrical bus, and GLTF node mappings.
- Added aircraft discovery with `--list-aircraft`, selection with `--aircraft <id>`, and fleet profile v4 persistence while retaining v1-v3 readers.
- Generalized the shared light-piston systems class while preserving the factory C172S profile and saves.
- Updated the start menu, aircraft card, and Credits page for package-based aircraft and open-source attribution.
- Prepared Debian/Ubuntu desktop and region packages for Aragón and both Catalonia regions, with an app-menu icon, portable CPU flags, and per-user mod/cache paths.
- Added modding and contribution guides and an open-source release checklist. Removed the obsolete delivery plan.

### Current limitations

- Aircraft format 1 supports one piston engine, two fuel tanks, and the shared light-piston systems profile. Multi-engine, turbine, aircraft-specific systems, and Lua behavior are not implemented.
- C172 model attribution is documented; the region builder uses OurAirports and OpenStreetMap airport data and does not import ENAIRE AIP records. Open-Meteo remains an optional service with separate terms and can be disabled at launch. Baseline region packs are not in the source checkout. See the [GitHub publication status](docs/OPEN_SOURCE_RELEASE.md).
