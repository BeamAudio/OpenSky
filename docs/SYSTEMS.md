# Runtime systems

This reference maps player-visible behavior to the code that owns it. For the component diagram and data boundaries, see [architecture](ARCHITECTURE.md). For a complete list of maintained project files, see [file reference](FILE_REFERENCE.md).

## Aircraft, flight and ownership

| System | Responsibility | Main implementation |
|---|---|---|
| Aircraft package loader | Discovers manifests, validates IDs and relative paths, resolves models and JSBSim data | `sim_engine/src/aircraft_package.*` |
| Flight dynamics | Applies controls, engine/environment state, fixed 120 Hz integration and terrain contacts | `engine/src/flight_model_jsb.*` |
| Piston aircraft systems | Fuel, electrical, starter, mixture, oil, carb heat, icing and engine condition | `engine/src/c172_systems.*` |
| Aircraft presentation | Cockpit/exterior models, instruments, animation, flight state and scene rendering | `sim_engine/src/aircraft_hud.*` |
| Aircraft session | Walkaround, boarding, active flight, landing and parking ownership rules | `sim_engine/src/aircraft_session.*` |
| Aircraft care | Typed persisted values for condition, fuel, electrical state and wear | `sim_engine/src/aircraft_condition.hpp` |
| Fleet profile | Reads supported save versions, validates fields, atomically writes valid saves, preserves unreadable originals | `sim_engine/src/fleet_profile.*` |
| Simulation clock | Converts wall-clock deltas to whole JSBSim steps; render calls do not advance flight time | `sim_engine/src/simulation_clock.hpp` |

Browsing the map and adding or editing route points does not relocate the aircraft. Boarding begins at the saved stand. Landing alone does not establish a new base: the player must taxi clear and park successfully. The next start uses that parked location and saved heading, condition, fuel and logbook. Leaving before a successful park preserves the prior base.

The game also models a walkaround with service points, chocks/tie-downs/pitot covers, fuel, battery, oil and some engine-condition effects. A cockpit caution can identify modeled issues such as carburetor icing or water in the fuel sumps. A rendered frame stall should not itself reduce engine power; see the note on sump-water behavior in [weather and exposure](WEATHER.md#parked-exposure-and-engine-symptoms).

## World and scenery

| System | Responsibility | Main implementation |
|---|---|---|
| Pack reader | Loads region metadata, indexes tiles, airports, runways, roads, places and static data | `sim_engine/src/pack_reader.*` |
| Async terrain streamer | Prioritizes page requests, worker staging, bounded GPU uploads and fixed residency | `sim_engine/src/async_streamer.*` |
| CDLOD terrain | Renders height/surface pages, imagery, atmosphere interactions and LOD transitions | `sim_engine/src/cdlod_terrain.*` and `sim_engine/shaders/cdlod_terrain.*` |
| City/world objects | Streams baked city chunks, buildings, landmarks and OSM scenery | `sim_engine/src/world_objects.*` |
| Airfield vegetation | Draws close airport foliage and local ground vegetation | `sim_engine/src/airfield_vegetation.*` |
| Traffic | Updates regional aircraft and maritime traffic | `sim_engine/src/living_traffic.*` |
| Map relief & flowmap | Loads `.relief.png` region sidecars once, creates mipmaps, and renders an animated GPU weather flowmap across installed regions | `sim_engine/src/game_frontend.cpp`, `sim_engine/shaders/frontend.frag` |

Pack readers and terrain jobs share immutable `PackReader::WorldFrame` data. Region transitions are centralized. Completed work carries its world generation and is rejected if it belongs to an older region. Keep cache and staging limits bounded; generated packs and caches are not source artifacts.

## Atmosphere, clouds and weather

`WeatherLibrary` reads baked `.nswx` records and provenance. `RegionalAtmosphere` chooses and evolves the active regional state, consumes live or manual input, advances its grid on a worker and publishes coherent snapshots. `volumetric_clouds.*` and `regional_atmosphere.*` textures feed the scene renderer. `GameFrontend` reads the published state for airport briefs, map history and outlook.

The World screen shows input provenance and reports when an airport is outside active model coverage. Current and historical UI values must come from published model snapshots. The outlook uses the current model and fixed boundary inputs at +1, +3 and +5 simulation minutes. For resolution, cadence, source fallbacks and validation gaps, read [weather system](WEATHER.md).

## Rendering and input

`PlatformWindow` owns X11/GLX on the active Linux runtime. It supports decorated windowed mode, desktop fullscreen, and borderless fullscreen-windowed mode (applied after restart), plus live VSync and frame-rate limiting in Settings. `gl_loader` resolves OpenGL functions. `main.cpp` orders the sky, terrain, world, clouds, aircraft, cockpit and interface passes. `volumetric_clouds.*` includes cloud rendering and post processing. `game_frontend.*` renders menus and map overlays; `aircraft_hud.*` renders flight-state, cockpit interaction, procedural optical cockpit rain, and in-flight interface overlays. Flight overlays are hidden by default for an immersive view; the player can enable them in Settings or toggle the controls guide with `T`.

The first-flight guide appears for a new profile, explains map browsing, walkaround, basic flight and saved-base parking, and can be reopened from the World screen. Its completion marker is stored separately from the fleet profile.

Graphics settings persist in the user's config directory (`graphics.ini`). Internal scene resolution follows quality settings. Avoid full-resolution depth copies and SSAO on low tiers, throttle expensive atmosphere/reflection work, and keep CPU/GPU uploads within per-frame budgets targeting 60 FPS or better on AMD Renoir.

Input translation lives in `PlatformWindow`, with multi-device plug-and-play USB flight yoke, rudder pedal, and throttle quadrant input handled via Linux `evdev` in `engine/src/flight_yoke.*`. Hardware deadzone, S-curve sensitivity, inversion, Direct vs Assisted mode, and center calibration live in `Settings`. Aircraft input is applied to the physical simulation at the next fixed step. OpenTrack receives head-tracking input through UDP in its own adapter.

## Audio

`audio_engine.*` owns the audio worker and procedural engine, wind, stall, rain and ground sounds. The main thread publishes a compact `AudioState`; the audio callback does not query GL or call into the JSBSim executive. Volume and mute settings are runtime controls.

## Builder and data protocols

Python tools under `builder/` fetch or process permitted source data and bake packs, relief, cities, vegetation, water and weather. The World Editor is a local UI over those build functions. `protocol/pack_format.h` and `.py` must remain compatible; `protocol/weather_format.h` and `.py` do the same for weather libraries.

Pack sidecars travel with their source region. Keep the C++ reader capable of opening installed formats when changing a producer. Add attribution and source-license records to generated pack metadata. The user-facing release baseline contains both Aragón and Catalunya, regardless of how future add-on regions are distributed.

## QA and profiling tools

`runtime_checks.*` exercises save, world-frame, stale-result and other runtime invariants. `flight_replay.*` records and replays fixed-step flight data. `flight_profiler.*` measures startup, frame and GPU stages. `debug_overlays.cpp` draws optional diagnostics. `builder/test_pack.py` validates generated packs; engine tests live under `engine/tests/`.

The repository's [contributing guide](../CONTRIBUTING.md) lists build and verification commands. Visual changes should use the project capture flow and be compared at matched location, quality and terrain residency; report p95 frame time alongside average FPS.
