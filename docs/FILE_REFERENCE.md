# File reference

This catalog documents the responsibilities of the OpenSky-maintained files checked into the repository. It intentionally does not duplicate 307 vendored JSBSim source-file comments: those files remain upstream library code under `engine/thirdparty/jsbsim/`, with their own license and [upstream notes](../engine/thirdparty/jsbsim/UPSTREAM.md). The repository root, builder, runtime, protocols, shaders, aircraft data and shipped runtime assets are covered below.

In grouped asset rows, filenames following the first path use the same parent directory unless a different path is written explicitly.

## Repository and documentation

| File | Responsibility |
|---|---|
| `.gitignore` | Excludes generated packs, caches, captures, builds and local artifacts from source control. |
| `AGENTS.md` | Project invariants for gameplay, simulation, architecture, performance, data and visual verification. |
| `README.md` | GitHub landing page: concept, status, build entry point, architecture and links to guides. |
| `CHANGELOG.md` | Human-readable release and development changes. |
| `CONTRIBUTING.md` | Contributor setup, build workflow, code rules and verification entry points. |
| `LICENSE` | MIT terms for OpenSky-authored source; third-party terms remain separate. |
| `THIRD_PARTY_LICENSES.md` | Notices and license requirements for bundled code, data and assets. |
| `NorthernSpainBuilder.desktop` | Linux desktop launcher for the World Editor. |
| `install_region.sh` | Helper for installing a built or downloaded region into the user data directory. |
| `run_sim.sh` | Stable player entry point that forwards arguments to the native simulator launcher. |
| `run_custom_sim.sh` | Sets the project runtime library path, changes to the repository root and launches the native binary. |
| `run_builder.sh` | Starts the local World Editor from the repository root. |
| `packaging/linux/opensky-vfr.desktop` | Linux desktop metadata for the player application. |
| `packaging/linux/opensky-vfr.svg` | Linux application icon. |
| `docs/ARCHITECTURE.md` | Component boundaries, data flow, thread ownership and change guidance. |
| `docs/SYSTEMS.md` | Runtime system responsibilities and primary implementation files. |
| `docs/WEATHER.md` | Weather inputs, worker, snapshots, player-facing views and model limits. |
| `docs/MODDING.md` | Current aircraft/region package workflow and script-system limitations. |
| `docs/FILE_REFERENCE.md` | This maintained file catalog. |
| `docs/OPEN_SOURCE_RELEASE.md` | Open-source release platform, rights, packaging and verification checklist. |
| `docs/airport-data-sources.md` | Provenance and reuse notes for airport data. |
| `docs/images/promo_cockpit_barcelona_beaches.png` | Ultra quality cockpit capture above Barcelona's coast. |
| `docs/images/promo_chase_barcelona_beaches.png` | Ultra quality chase capture above Barcelona and its harbor. |
| `docs/images/promo_cockpit_costa_brava.png` | Ultra quality cockpit capture above the Costa Brava near Begur. |
| `docs/images/promo_chase_costa_brava.png` | Ultra quality chase capture above the cliffs and coves near Begur. |
| `docs/images/promo_cockpit_catalan_pyrenees.png` | Ultra quality cockpit capture over the Catalan Pyrenees. |
| `docs/images/promo_chase_catalan_pyrenees.png` | Ultra quality chase capture over the Catalan Pyrenees. |
| `docs/images/promo_cockpit_castejon.png` | Ultra quality cockpit capture above the Castejón de Sos valley. |
| `docs/images/promo_chase_castejon.png` | Ultra quality chase capture above the Castejón de Sos valley. |
| `docs/images/promo_cockpit_mediano.png` | Ultra quality cockpit capture above Mediano reservoir. |
| `docs/images/promo_chase_mediano.png` | Ultra quality chase capture above Mediano reservoir. |
| `docs/images/promo_weather_clouds.png` | In-game showcase of volumetric clouds over the Pyrenees. |
| `docs/images/promo_weather_rain.png` | In-game showcase of cockpit rain near Castejón de Sos. |
| `docs/2026-11-23_opensky-engine-audit.md` | Dated engine architecture and performance review. |
| `docs/2026-11-23_flight-weather-audit.md` | Dated flight model and weather review. |
| `docs/benchmarks/2026-11-23-renoir-linux.md` | Renoir frame-time measurements and capture settings. |

## Builder and tooling

| File | Responsibility |
|---|---|
| `builder/build_region.py` | Main regional pack build command and preset/custom-region orchestration. |
| `builder/build_region_gui.py` | Local GUI entry point for selecting region sources and build options. |
| `builder/build_region_gui.html` | World Editor browser interface. |
| `builder/gui_server.py` | Local HTTP service that bridges the editor UI to builder actions. |
| `builder/build_scenery.py` | Bakes terrain, imagery and core region scenery into the pack format. |
| `builder/airports.py` | Imports and normalizes airport and runway inputs. |
| `builder/bake_weather.py` | Creates regional `.nswx` weather libraries with provenance. |
| `builder/bake_map_relief.py` | Produces `.relief.png` sidecars for World-menu elevation shading. |
| `builder/bake_static_cities.py` | Bakes OSM building, road and city chunks for runtime streaming. |
| `builder/bake_vegetation.py` | Processes vegetation/canopy inputs for region packs. |
| `builder/water_mask.py` | Builds water masks used by scenery and terrain shading. |
| `builder/fetch_pbr_assets.py` | Fetches approved material inputs and records/validates source metadata. |
| `builder/pack_preview.py` | Inspects and previews a generated region pack. |
| `builder/test_pack.py` | Validates pack structure, indexes and associated data. |
| `tools/package_deb.py` | Creates the current Debian package artifacts; consult release-readiness before publishing. |
| `tools/bake_foliage_cluster_atlas.py` | Builds the foliage cluster texture atlas. |
| `tools/bake_satellite_detail_atlas.py` | Builds subtle satellite micro-detail atlas inputs. |
| `tools/build_c172s_cockpit.py` | Generates the C172S cockpit model from maintained source inputs. |
| `tools/build_regional_architecture_blender.py` | Builds regional architectural meshes in Blender. |
| `tools/compare_topdown_satellite.py` | Compares rendered top-down scenery with image references. |
| `tools/export_glb_models.py` | Exports prepared models to GLB runtime assets. |
| `tools/generate_windscreen_rain.py` | Generates rain lookup/texture assets for the cockpit. |
| `tools/import_ccby_c172.py` | Imports the appropriately licensed C172 source asset. |
| `tools/summarize_profile.py` | Summarizes runtime profiler captures. |
| `tools/upgrade_building_atlas.py` | Updates the baked building material atlas. |
| `tools/upgrade_pnoa_cities.py` | Updates PNOA city/scenery inputs. |
| `tools/upgrade_vegetation.py` | Updates generated vegetation resources. |

## Pack protocols

| File | Responsibility |
|---|---|
| `protocol/pack_format.h` | C++ definitions for the `.nspack` binary format. |
| `protocol/pack_format.py` | Python writer/reader definitions matching the C++ pack format. |
| `protocol/weather_format.h` | C++ definitions for the `.nswx` weather library format. |
| `protocol/weather_format.py` | Python writer/reader definitions matching the C++ weather format. |

Change the paired Python and C++ format implementations together. Preserve the runtime reader for installed packs when changing builder outputs.

## Aircraft and engine source

| File | Responsibility |
|---|---|
| `engine/src/c172_systems.hpp` | Piston-system inputs, outputs, state and supported aircraft configuration. |
| `engine/src/c172_systems.cpp` | Fuel, battery, starter, oil, icing, wear and power-state behavior. |
| `engine/src/flight_model_jsb.hpp` | Flight model, environment, control and engine interfaces. |
| `engine/src/flight_model_jsb.cpp` | JSBSim package loading, fixed-step integration, control transfer and ground callback. |
| `engine/src/flight_yoke.hpp` | Linux `evdev` multi-device USB flight yoke, pedal, and throttle calibration interfaces. |
| `engine/src/flight_yoke.cpp` | Plug-and-play USB yoke/pedal/throttle scanning, role mapping, tare calibration, and selftests. |
| `engine/src/geo_math.hpp` | WGS84, local-distance and geodetic helper functions. |
| `engine/src/opentrack_udp.hpp` | OpenTrack UDP input interface. |
| `engine/src/opentrack_udp.cpp` | Head-pose UDP listener and state conversion. |
| `engine/tests/test_flight_and_systems.cpp` | Engine and flight-model test cases. |
| `engine/tests/test_geo_and_pack.cpp` | Geodesy and pack/protocol test cases. |
| `engine/thirdparty/jsbsim/` | Vendored upstream JSBSim and support source; see its CMake files, `UPSTREAM.md` and LGPL notices rather than treating it as OpenSky-owned code. |

## Simulator source

| File | Responsibility |
|---|---|
| `sim_engine/Makefile` | Native simulator build and link rules. |
| `sim_engine/src/main.cpp` | Startup, pack selection, main loop, fixed-step updates, render ordering, region transitions and QA entry points. |
| `sim_engine/src/platform_window.hpp` | Window, event, key and presentation interface. |
| `sim_engine/src/platform_window.cpp` | Linux X11/GLX window, decorated/borderless/fullscreen display modes, OpenGL context, input events and screenshots. |
| `sim_engine/src/gl_loader.hpp` | OpenGL function loader interface. |
| `sim_engine/src/gl_loader.cpp` | OpenGL function resolution for supported window backends. |
| `sim_engine/src/math3d.hpp` | Small vector, matrix, camera and geometric math types. |
| `sim_engine/src/app_paths.hpp` | User config, data and cache directory selection. |
| `sim_engine/src/graphics_settings.hpp` | Graphics settings defaults, validation and persistence. |
| `sim_engine/src/simulation_clock.hpp` | Fixed 120 Hz simulation-time accumulator. |
| `sim_engine/src/pack_reader.hpp` | Region pack, tile, airport and immutable world-frame interfaces. |
| `sim_engine/src/pack_reader.cpp` | Pack loading, tile access, geospatial queries and caches. |
| `sim_engine/src/async_streamer.hpp` | Terrain worker, staging queue, fixed page cache and GPU residency interface. |
| `sim_engine/src/async_streamer.cpp` | Priority requests, asynchronous page preparation and budgeted uploads. |
| `sim_engine/src/cdlod_terrain.hpp` | CDLOD terrain renderer interface and quality controls. |
| `sim_engine/src/cdlod_terrain.cpp` | Terrain page selection, draw calls, atmosphere and material bindings. |
| `sim_engine/src/aircraft_package.hpp` | Aircraft package data model and discovery API. |
| `sim_engine/src/aircraft_package.cpp` | Strict aircraft manifest parsing, validation and package discovery. |
| `sim_engine/src/aircraft_condition.hpp` | Persisted aircraft care snapshot fields. |
| `sim_engine/src/aircraft_session.hpp` | Ownership, boarding, parking, route and profile interface. |
| `sim_engine/src/aircraft_session.cpp` | Aircraft lifecycle, service actions, flight logging and base persistence. |
| `sim_engine/src/fleet_profile.hpp` | Fleet profile types and load/save interface. |
| `sim_engine/src/fleet_profile.cpp` | Profile-version parsing, validation, safe writes and preservation of invalid saves. |
| `sim_engine/src/aircraft_hud.hpp` | Aircraft state, cockpit rendering, controls and HUD interface. |
| `sim_engine/src/aircraft_hud.cpp` | Aircraft input, flight-state integration, cockpit/exterior rendering and flight overlays. |
| `sim_engine/src/game_frontend.hpp` | World/menu/walkaround/controls/settings interface. |
| `sim_engine/src/game_frontend.cpp` | World map, relief, route planning, airports, weather UI and menu rendering. |
| `sim_engine/src/frontend_font_metrics.hpp` | Glyph advance metrics for the bitmap frontend font. |
| `sim_engine/src/regional_atmosphere.hpp` | Weather model, input, snapshot, outlook and worker interfaces. |
| `sim_engine/src/regional_atmosphere.cpp` | Regional weather solver, terrain exposure, domain generations and snapshot publication. |
| `sim_engine/src/weather_library.hpp` | Baked weather records and provenance interface. |
| `sim_engine/src/weather_library.cpp` | `.nswx` loading, baked boundary inputs and source metadata. |
| `sim_engine/src/volumetric_clouds.hpp` | Sky/cloud renderer, weather textures and post-processing interface. |
| `sim_engine/src/volumetric_clouds.cpp` | Atmosphere textures, volumetric cloud rendering and scene resolve. |
| `sim_engine/src/world_objects.hpp` | City, building, road and landmark renderer interface. |
| `sim_engine/src/world_objects.cpp` | Baked scenery indexing, visibility and local GPU streaming. |
| `sim_engine/src/airfield_vegetation.hpp` | Airport vegetation renderer interface. |
| `sim_engine/src/airfield_vegetation.cpp` | Local airport tree and shrub placement/rendering. |
| `sim_engine/src/living_traffic.hpp` | Live aircraft and maritime traffic state. |
| `sim_engine/src/living_traffic.cpp` | Traffic simulation and rendering. |
| `sim_engine/src/audio_engine.hpp` | Audio state and engine interface. |
| `sim_engine/src/audio_engine.cpp` | Procedural aircraft, wind, rain, stall and ground audio. |
| `sim_engine/src/flight_profiler.hpp` | CPU/GPU profile structures and query interface. |
| `sim_engine/src/flight_profiler.cpp` | Timing collection, startup markers and profiler summaries. |
| `sim_engine/src/debug_overlays.cpp` | Optional weather, physics and performance overlays. |
| `sim_engine/src/flight_replay.hpp` | Replay, recording and route persistence interfaces. |
| `sim_engine/src/flight_replay.cpp` | Flight recording, replay and persistence checks. |
| `sim_engine/src/runtime_checks.hpp` | Runtime invariant-check interface. |
| `sim_engine/src/runtime_checks.cpp` | Save, world-frame, stale-work and other runtime checks. |

## Shader inventory

| Files | Responsibility |
|---|---|
| `sim_engine/shaders/fullscreen.vert`, `frontend.frag` | Fullscreen triangle and World globe/map, region relief and weather overlay. |
| `sim_engine/shaders/sky.frag`, `clouds_qres.frag`, `post_tonemap.frag`, `env_reflect.frag` | Sky, reduced-resolution volumetric clouds, final tonemapping and reflections. |
| `sim_engine/shaders/cdlod_terrain.vert`, `cdlod_terrain.frag` | Terrain geometry and terrain materials. |
| `sim_engine/shaders/building.vert`, `building.frag`, `bldg_lod1.vert`, `bldg_lod1.frag`, `bldg_shadow.vert`, `bldg_shadow.frag` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/city_baked.vert`, `city_baked.frag`, `city_baked_far.frag` | Baked city chunks at near and far distances. |
| `sim_engine/shaders/landmark.vert`, `landmark.frag` | Regional landmark geometry. |
| `sim_engine/shaders/road.vert`, `road.frag`, `sim_engine/shaders/runway.vert`, `runway.frag` | Roads and runway surfaces. |
| `sim_engine/shaders/airfield_trees.vert`, `airfield_trees.frag` | Airport tree/vegetation rendering. |
| `sim_engine/shaders/aircraft.vert`, `aircraft.frag` | Exterior and cockpit GLB materials. |
| `sim_engine/shaders/living_traffic.vert`, `living_traffic.frag`, `traffic.vert`, `traffic.frag` | Dynamic traffic silhouettes and materials. |
| `sim_engine/shaders/precip.vert`, `precip.frag` | Screen/camera precipitation effects. |
| `sim_engine/shaders/hud.vert`, `hud.frag` | Bitmap HUD and interface geometry. |
| `sim_engine/shaders/cull_instances.comp` | GPU instance visibility/culling compute pass. |

## Aircraft data and runtime assets

| Files | Responsibility |
|---|---|
| `game/aircraft/c172s/aircraft.ini` | C172S package manifest and node mappings. |
| `game/aircraft/c172s/c172s.xml` | JSBSim airframe and flight-control definition. |
| `game/aircraft/c172s/engine/IO-360-L2A.xml` | Lycoming engine data. |
| `game/aircraft/c172s/engine/mccauley_76.xml` | McCauley propeller data. |
| `game/assets/models/aircraft/c172s_cockpit.glb`, `c172s_exterior.glb` | Runtime cockpit and exterior aircraft models. |
| `game/assets/models/aircraft/c172s_cockpit_source.blend` | Editable Blender source for the cockpit model. |
| `game/assets/models/architecture/aragon_apartment.glb`, `aragon_church_tower.glb`, `aragon_house_stone.glb`, `aragon_house_terra.glb`, `industrial_hangar.glb`, `rural_barn.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/vegetation/shrub_maquis.glb`, `tree_holm_oak.glb`, `tree_poplar.glb`, `tree_scots_pine.glb` | Regional shrub and tree meshes. |
| `game/assets/pbr/arch/roof_slate_albedo.jpg`, `roof_slate_normal.jpg`, `roof_slate_rough.jpg`, `roof_terra_albedo.jpg`, `roof_terra_normal.jpg`, `roof_terra_rough.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/tree_bark_albedo.jpg`, `tree_bark_normal.jpg`, `tree_bark_rough.jpg` | Architecture/vegetation bark material maps. |
| `game/assets/pbr/arch/wall_metal_albedo.jpg`, `wall_metal_normal.jpg`, `wall_metal_rough.jpg`, `wall_stone_albedo.jpg`, `wall_stone_normal.jpg`, `wall_stone_rough.jpg`, `wall_stucco_albedo.jpg`, `wall_stucco_normal.jpg`, `wall_stucco_rough.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/asphalt_albedo.jpg`, `asphalt_normal.jpg`, `gravel_albedo.jpg`, `gravel_normal.jpg`, `rock_albedo.jpg`, `rock_arm.jpg`, `rock_normal.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/farm_albedo.jpg`, `farm_arm.jpg`, `farm_normal.jpg` | Farmland surface maps. |
| `game/assets/pbr/grass_albedo.jpg`, `grass_arm.jpg`, `grass_normal.jpg`, `forest_albedo.jpg`, `forest_arm.jpg`, `forest_normal.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/foliage_cluster_albedo.png`, `foliage_cluster_normal.png`, `trees_atlas_albedo.png`, `trees_atlas_normal.png`, `bark_src_albedo.jpg`, `leaf_src_albedo.jpg`, `leaf_src_normal.jpg` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/building_atlas_albedo.jpg`, `building_atlas_normal.jpg`, `building_atlas_orm.png` | Baked building material atlas. |
| `game/assets/pbr/hangar_albedo.jpg`, `hangar_normal.jpg`, `terminal_albedo.jpg`, `terminal_normal.jpg`, `interior_rooms.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/c172_windscreen_rain_front.png`, `c172_windscreen_rain_side.png` | C172 windscreen rain masks/lookups. |
| `game/assets/pbr/cloud_vol_3d.png` | Cloud-volume noise resource. |
| `game/assets/pbr/sat_detail_atlas.jpg` | Subtle orthophoto detail atlas. |
| `game/assets/pbr/lut_spain_photo.png` | Photographic color-grading lookup. |
| `game/assets/pbr/hud_font.png`, `game/assets/ui/frontend_font.png` | Cockpit and menu bitmap fonts. |
| `game/assets/ui/globe_land.png` | World globe land mask. |
| `game/bin/.gitkeep` | Keeps the runtime library directory present in a clean checkout; generated JSBSim binaries are not tracked here. |

## Third-party support files

| Path | Responsibility |
|---|---|
| `sim_engine/third_party/GL/glcorearb.h`, `KHR/khrplatform.h` | Khronos OpenGL definitions and platform types. |
| `sim_engine/third_party/bc7enc.cpp`, `bc7enc.h`, `bc7enc_LICENSE.txt` | BC7 texture encoder and its license notice. |
| `sim_engine/third_party/cgltf.h` | Single-header glTF/GLB parser. |
| `sim_engine/third_party/miniz_common.h`, `miniz_export.h`, `miniz_tinfl.c`, `miniz_tinfl.h` | Minimal DEFLATE/inflate support for pack data. |
| `sim_engine/third_party/stb_easy_font.h`, `stb_image.h`, `stb_image_write.h` | Small font, image-loading and screenshot-writing dependencies. |
| `engine/thirdparty/jsbsim/` | Upstream JSBSim 1.3.4 source tree; its nested CMake and source files are maintained by upstream and carry upstream licenses. |

If a file's role changes, update this catalog and the relevant system guide in the same change. New generated output should be described as an output of its builder, not committed as source data.

## Complete tracked-file index

The tables above explain subsystem responsibilities. This expanded index names every source-controlled project file, including vendored JSBSim and third-party libraries. Upstream files remain governed by their own comments and licenses.

<details><summary>Show all tracked files</summary>

| Path | Per-file role |
|---|---|
| `.gitignore` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `AGENTS.md` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `CHANGELOG.md` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `CONTRIBUTING.md` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `LICENSE` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `NorthernSpainBuilder.desktop` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `README.md` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `THIRD_PARTY_LICENSES.md` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `builder/airports.py` | Imports and normalizes airport and runway inputs. |
| `builder/bake_map_relief.py` | Produces `.relief.png` sidecars for World-menu elevation shading. |
| `builder/bake_static_cities.py` | Bakes OSM building, road and city chunks for runtime streaming. |
| `builder/bake_vegetation.py` | Processes vegetation/canopy inputs for region packs. |
| `builder/bake_weather.py` | Creates regional `.nswx` weather libraries with provenance. |
| `builder/build_region.py` | Main regional pack build command and preset/custom-region orchestration. |
| `builder/build_region_gui.html` | World Editor browser interface. |
| `builder/build_region_gui.py` | Local GUI entry point for selecting region sources and build options. |
| `builder/build_scenery.py` | Bakes terrain, imagery and core region scenery into the pack format. |
| `builder/fetch_pbr_assets.py` | Fetches approved material inputs and records/validates source metadata. |
| `builder/gui_server.py` | Local HTTP service that bridges the editor UI to builder actions. |
| `builder/pack_preview.py` | Inspects and previews a generated region pack. |
| `builder/test_pack.py` | Validates pack structure, indexes and associated data. |
| `builder/water_mask.py` | Builds water masks used by scenery and terrain shading. |
| `docs/2026-11-23_flight-weather-audit.md` | Dated flight model and weather review. |
| `docs/2026-11-23_opensky-engine-audit.md` | Dated engine architecture and performance review. |
| `docs/ARCHITECTURE.md` | Component boundaries, data flow, thread ownership and change guidance. |
| `docs/FILE_REFERENCE.md` | This maintained file catalog. |
| `docs/MODDING.md` | Current aircraft/region package workflow and script-system limitations. |
| `docs/OPEN_SOURCE_RELEASE.md` | Open-source release platform, rights, packaging and verification checklist. |
| `docs/SYSTEMS.md` | Runtime system responsibilities and primary implementation files. |
| `docs/WEATHER.md` | Weather inputs, worker, snapshots, player-facing views and model limits. |
| `docs/airport-data-sources.md` | Provenance and reuse notes for airport data. |
| `docs/benchmarks/2026-11-23-renoir-linux.md` | Renoir frame-time measurements and capture settings. |
| `engine/src/c172_systems.cpp` | Fuel, battery, starter, oil, icing, wear and power-state behavior. |
| `engine/src/c172_systems.hpp` | Piston-system inputs, outputs, state and supported aircraft configuration. |
| `engine/src/flight_model_jsb.cpp` | JSBSim package loading, fixed-step integration, control transfer and ground callback. |
| `engine/src/flight_model_jsb.hpp` | Flight model, environment, control and engine interfaces. |
| `engine/src/geo_math.hpp` | WGS84, local-distance and geodetic helper functions. |
| `engine/src/opentrack_udp.cpp` | Head-pose UDP listener and state conversion. |
| `engine/src/opentrack_udp.hpp` | OpenTrack UDP input interface. |
| `engine/tests/test_flight_and_systems.cpp` | Engine and flight-model test cases. |
| `engine/tests/test_geo_and_pack.cpp` | Geodesy and pack/protocol test cases. |
| `engine/thirdparty/jsbsim/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/COPYING.LGPL-2.1` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/JSBSimConfig.cmake.in` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/UPSTREAM.md` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/FGFDMExec.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/FGFDMExec.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/FGJSBBase.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/FGJSBBase.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Config.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Constants.hpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Geodesic.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Geodesic.hpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/GeodesicLine.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/GeodesicLine.hpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/LICENSE.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Math.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/GeographicLib/Math.hpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/JSBSim.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/JSBSim.minimal.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/JSBSim_API.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGInitialCondition.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGInitialCondition.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGLinearization.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGLinearization.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGSimplexTrim.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGSimplexTrim.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrim.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrim.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAnalysis.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAnalysis.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAnalysisControl.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAnalysisControl.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAxis.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimAxis.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimmer.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/initialization/FGTrimmer.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGGroundCallback.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGGroundCallback.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGInputSocket.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGInputSocket.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGInputType.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGInputType.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGLog.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGLog.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGModelLoader.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGModelLoader.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputFG.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputFG.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputFile.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputFile.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputSocket.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputSocket.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputTextFile.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputTextFile.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputType.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGOutputType.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGPropertyManager.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGPropertyManager.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGPropertyReader.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGPropertyReader.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGScript.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGScript.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGUDPInputSocket.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGUDPInputSocket.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLElement.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLElement.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLFileRead.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLFileRead.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLParse.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGXMLParse.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGfdmSocket.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/FGfdmSocket.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/net_fdm.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/string_utilities.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/input_output/string_utilities.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGColumnVector3.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGColumnVector3.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGCondition.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGCondition.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGFunction.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGFunction.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGFunctionValue.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGLocation.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGLocation.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGMatrix33.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGMatrix33.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGModelFunctions.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGModelFunctions.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGNelderMead.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGNelderMead.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGParameter.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGParameterValue.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGPropertyValue.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGPropertyValue.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGQuaternion.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGQuaternion.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGRealValue.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGRealValue.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGRungeKutta.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGRungeKutta.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGStateSpace.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGStateSpace.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGTable.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGTable.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGTemplateFunc.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/FGTemplateFunc.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/math/LagrangeMultiplier.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAccelerations.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAccelerations.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAerodynamics.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAerodynamics.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAircraft.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAircraft.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAtmosphere.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAtmosphere.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAuxiliary.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGAuxiliary.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGBuoyantForces.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGBuoyantForces.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGExternalForce.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGExternalForce.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGExternalReactions.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGExternalReactions.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGFCS.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGFCS.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGFCSChannel.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGGasCell.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGGasCell.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGGroundReactions.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGGroundReactions.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGInertial.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGInertial.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGInput.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGInput.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGLGear.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGLGear.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGMassBalance.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGMassBalance.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGModel.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGModel.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGOutput.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGOutput.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGPropagate.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGPropagate.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGPropulsion.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGPropulsion.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGSurface.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/FGSurface.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGMSIS.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGMSIS.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGMars.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGMars.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGStandardAtmosphere.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGStandardAtmosphere.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGWinds.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/FGWinds.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/DOCUMENTATION` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/nrlmsise-00.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/nrlmsise-00.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/nrlmsise-00_data.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/atmosphere/MSIS/nrlmsise-00_test.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGAccelerometer.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGAccelerometer.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGActuator.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGActuator.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGAngles.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGAngles.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGDeadBand.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGDeadBand.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGDistributor.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGDistributor.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFCSComponent.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFCSComponent.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFCSFunction.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFCSFunction.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFilter.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGFilter.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGGain.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGGain.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGGyro.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGGyro.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGKinemat.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGKinemat.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGLinearActuator.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGLinearActuator.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGMagnetometer.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGMagnetometer.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGPID.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGPID.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSensor.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSensor.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSensorOrientation.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSummer.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSummer.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSwitch.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGSwitch.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGWaypoint.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/flight_control/FGWaypoint.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGBrushLessDCMotor.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGBrushLessDCMotor.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGElectric.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGElectric.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGEngine.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGEngine.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGForce.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGForce.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGNozzle.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGNozzle.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGPiston.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGPiston.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGPropeller.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGPropeller.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGRocket.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGRocket.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGRotor.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGRotor.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTank.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTank.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGThruster.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGThruster.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTransmission.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTransmission.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTurbine.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTurbine.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTurboProp.cpp` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/models/propulsion/FGTurboProp.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/compiler.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/io/iostreams/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/io/iostreams/sgstream.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/io/iostreams/sgstream.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/magvar/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/magvar/coremag.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/magvar/coremag.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/sg_path.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/sg_path.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/stdint.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/strutils.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/misc/strutils.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/props/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/props/props.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/props/props.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/structure/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/structure/SGReferenced.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/structure/SGSharedPtr.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/CMakeLists.txt` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/COPYING` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/ConfigureChecks.cmake` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/ascii.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/asciitab.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/easyxml.cxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/easyxml.hxx` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/expat.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/expat_config.h.cmake` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/expat_external.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/fallthrough.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/hash_table.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/iasciitab.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/internal.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/latin1tab.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/memory_sanitizer.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/msvc/expat_config.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/nametab.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_arc4random.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_arc4random.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_arc4random_buf.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_arc4random_buf.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_dev_urandom.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_dev_urandom.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_getentropy.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_getentropy.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_getrandom.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_getrandom.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_rand_s.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/random_rand_s.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/siphash.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/utf8tab.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/winconfig.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xcsinc.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmlparse.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmlrole.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmlrole.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmltok.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmltok.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmltok_impl.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmltok_impl.h` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `engine/thirdparty/jsbsim/src/simgear/xml/xmltok_ns.c` | Vendored upstream JSBSim source or support file; consult inline comments and upstream docs. LGPL-2.1. |
| `game/aircraft/c172s/aircraft.ini` | C172S package manifest and node mappings. |
| `game/aircraft/c172s/c172s.xml` | JSBSim airframe and flight-control definition. |
| `game/aircraft/c172s/engine/IO-360-L2A.xml` | Lycoming engine data. |
| `game/aircraft/c172s/engine/mccauley_76.xml` | McCauley propeller data. |
| `game/assets/models/aircraft/c172s_cockpit.glb` | Runtime cockpit and exterior aircraft models. |
| `game/assets/models/aircraft/c172s_cockpit_source.blend` | Editable Blender source for the cockpit model. |
| `game/assets/models/aircraft/c172s_exterior.glb` | Runtime cockpit and exterior aircraft models. |
| `game/assets/models/architecture/aragon_apartment.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/architecture/aragon_church_tower.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/architecture/aragon_house_stone.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/architecture/aragon_house_terra.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/architecture/industrial_hangar.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/architecture/rural_barn.glb` | Regional building and airport architecture meshes. |
| `game/assets/models/vegetation/shrub_maquis.glb` | Regional shrub and tree meshes. |
| `game/assets/models/vegetation/tree_holm_oak.glb` | Regional shrub and tree meshes. |
| `game/assets/models/vegetation/tree_poplar.glb` | Regional shrub and tree meshes. |
| `game/assets/models/vegetation/tree_scots_pine.glb` | Regional shrub and tree meshes. |
| `game/assets/pbr/arch/roof_slate_albedo.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/roof_slate_normal.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/roof_slate_rough.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/roof_terra_albedo.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/roof_terra_normal.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/roof_terra_rough.jpg` | Slate and terracotta roof material maps. |
| `game/assets/pbr/arch/tree_bark_albedo.jpg` | Architecture/vegetation bark material maps. |
| `game/assets/pbr/arch/tree_bark_normal.jpg` | Architecture/vegetation bark material maps. |
| `game/assets/pbr/arch/tree_bark_rough.jpg` | Architecture/vegetation bark material maps. |
| `game/assets/pbr/arch/wall_metal_albedo.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_metal_normal.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_metal_rough.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stone_albedo.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stone_normal.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stone_rough.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stucco_albedo.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stucco_normal.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/arch/wall_stucco_rough.jpg` | Metal, stone and stucco material maps. |
| `game/assets/pbr/asphalt_albedo.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/asphalt_normal.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/bark_src_albedo.jpg` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/building_atlas_albedo.jpg` | Baked building material atlas. |
| `game/assets/pbr/building_atlas_normal.jpg` | Baked building material atlas. |
| `game/assets/pbr/building_atlas_orm.png` | Baked building material atlas. |
| `game/assets/pbr/c172_windscreen_rain_front.png` | C172 windscreen rain masks/lookups. |
| `game/assets/pbr/c172_windscreen_rain_side.png` | C172 windscreen rain masks/lookups. |
| `game/assets/pbr/cloud_vol_3d.png` | Cloud-volume noise resource. |
| `game/assets/pbr/farm_albedo.jpg` | Farmland surface maps. |
| `game/assets/pbr/farm_arm.jpg` | Farmland surface maps. |
| `game/assets/pbr/farm_normal.jpg` | Farmland surface maps. |
| `game/assets/pbr/foliage_cluster_albedo.png` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/foliage_cluster_normal.png` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/forest_albedo.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/forest_arm.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/forest_normal.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/grass_albedo.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/grass_arm.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/grass_normal.jpg` | Grass and forest surface maps. |
| `game/assets/pbr/gravel_albedo.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/gravel_normal.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/hangar_albedo.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/hangar_normal.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/hud_font.png` | Cockpit and menu bitmap fonts. |
| `game/assets/pbr/interior_rooms.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/leaf_src_albedo.jpg` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/leaf_src_normal.jpg` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/lut_spain_photo.png` | Photographic color-grading lookup. |
| `game/assets/pbr/rock_albedo.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/rock_arm.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/rock_normal.jpg` | Runway, road, gravel and rock surfaces. |
| `game/assets/pbr/sat_detail_atlas.jpg` | Subtle orthophoto detail atlas. |
| `game/assets/pbr/terminal_albedo.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/terminal_normal.jpg` | Hangar, terminal and cockpit/interior surfaces. |
| `game/assets/pbr/trees_atlas_albedo.png` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/pbr/trees_atlas_normal.png` | Foliage/leaf source maps and runtime atlases. |
| `game/assets/ui/frontend_font.png` | Cockpit and menu bitmap fonts. |
| `game/assets/ui/globe_land.png` | World globe land mask. |
| `game/bin/.gitkeep` | Keeps the runtime library directory present in a clean checkout; generated JSBSim binaries are not tracked here. |
| `install_region.sh` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `packaging/linux/opensky-vfr.desktop` | Linux desktop metadata for the player application. |
| `packaging/linux/opensky-vfr.svg` | Linux application icon. |
| `protocol/pack_format.h` | C++ definitions for the `.nspack` binary format. |
| `protocol/pack_format.py` | Python writer/reader definitions matching the C++ pack format. |
| `protocol/weather_format.h` | C++ definitions for the `.nswx` weather library format. |
| `protocol/weather_format.py` | Python writer/reader definitions matching the C++ weather format. |
| `run_builder.sh` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `run_custom_sim.sh` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `run_sim.sh` | Project build, launcher, package metadata or configuration; see README and the relevant guide. |
| `sim_engine/Makefile` | Native simulator build and link rules. |
| `sim_engine/shaders/aircraft.frag` | Exterior and cockpit GLB materials. |
| `sim_engine/shaders/aircraft.vert` | Exterior and cockpit GLB materials. |
| `sim_engine/shaders/airfield_trees.frag` | Airport tree/vegetation rendering. |
| `sim_engine/shaders/airfield_trees.vert` | Airport tree/vegetation rendering. |
| `sim_engine/shaders/bldg_lod1.frag` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/bldg_lod1.vert` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/bldg_shadow.frag` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/bldg_shadow.vert` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/building.frag` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/building.vert` | Near/far buildings and baked building shadows. |
| `sim_engine/shaders/cdlod_terrain.frag` | Terrain geometry and terrain materials. |
| `sim_engine/shaders/cdlod_terrain.vert` | Terrain geometry and terrain materials. |
| `sim_engine/shaders/city_baked.frag` | Baked city chunks at near and far distances. |
| `sim_engine/shaders/city_baked.vert` | Baked city chunks at near and far distances. |
| `sim_engine/shaders/city_baked_far.frag` | Baked city chunks at near and far distances. |
| `sim_engine/shaders/clouds_qres.frag` | Sky, reduced-resolution volumetric clouds, final tonemapping and reflections. |
| `sim_engine/shaders/cull_instances.comp` | GPU instance visibility/culling compute pass. |
| `sim_engine/shaders/env_reflect.frag` | Sky, reduced-resolution volumetric clouds, final tonemapping and reflections. |
| `sim_engine/shaders/frontend.frag` | Fullscreen triangle and World globe/map, region relief and weather overlay. |
| `sim_engine/shaders/fullscreen.vert` | Fullscreen triangle and World globe/map, region relief and weather overlay. |
| `sim_engine/shaders/hud.frag` | Bitmap HUD and interface geometry. |
| `sim_engine/shaders/hud.vert` | Bitmap HUD and interface geometry. |
| `sim_engine/shaders/landmark.frag` | Regional landmark geometry. |
| `sim_engine/shaders/landmark.vert` | Regional landmark geometry. |
| `sim_engine/shaders/living_traffic.frag` | Dynamic traffic silhouettes and materials. |
| `sim_engine/shaders/living_traffic.vert` | Dynamic traffic silhouettes and materials. |
| `sim_engine/shaders/post_tonemap.frag` | Sky, reduced-resolution volumetric clouds, final tonemapping and reflections. |
| `sim_engine/shaders/precip.frag` | Screen/camera precipitation effects. |
| `sim_engine/shaders/precip.vert` | Screen/camera precipitation effects. |
| `sim_engine/shaders/road.frag` | Roads and runway surfaces. |
| `sim_engine/shaders/road.vert` | Roads and runway surfaces. |
| `sim_engine/shaders/runway.frag` | Roads and runway surfaces. |
| `sim_engine/shaders/runway.vert` | Roads and runway surfaces. |
| `sim_engine/shaders/sky.frag` | Sky, reduced-resolution volumetric clouds, final tonemapping and reflections. |
| `sim_engine/shaders/traffic.frag` | Dynamic traffic silhouettes and materials. |
| `sim_engine/shaders/traffic.vert` | Dynamic traffic silhouettes and materials. |
| `sim_engine/src/aircraft_condition.hpp` | Persisted aircraft care snapshot fields. |
| `sim_engine/src/aircraft_hud.cpp` | Aircraft input, flight-state integration, cockpit/exterior rendering and flight overlays. |
| `sim_engine/src/aircraft_hud.hpp` | Aircraft state, cockpit rendering, controls and HUD interface. |
| `sim_engine/src/aircraft_package.cpp` | Strict aircraft manifest parsing, validation and package discovery. |
| `sim_engine/src/aircraft_package.hpp` | Aircraft package data model and discovery API. |
| `sim_engine/src/aircraft_session.cpp` | Aircraft lifecycle, service actions, flight logging and base persistence. |
| `sim_engine/src/aircraft_session.hpp` | Ownership, boarding, parking, route and profile interface. |
| `sim_engine/src/airfield_vegetation.cpp` | Local airport tree and shrub placement/rendering. |
| `sim_engine/src/airfield_vegetation.hpp` | Airport vegetation renderer interface. |
| `sim_engine/src/app_paths.hpp` | User config, data and cache directory selection. |
| `sim_engine/src/async_streamer.cpp` | Priority requests, asynchronous page preparation and budgeted uploads. |
| `sim_engine/src/async_streamer.hpp` | Terrain worker, staging queue, fixed page cache and GPU residency interface. |
| `sim_engine/src/audio_engine.cpp` | Procedural aircraft, wind, rain, stall and ground audio. |
| `sim_engine/src/audio_engine.hpp` | Audio state and engine interface. |
| `sim_engine/src/cdlod_terrain.cpp` | Terrain page selection, draw calls, atmosphere and material bindings. |
| `sim_engine/src/cdlod_terrain.hpp` | CDLOD terrain renderer interface and quality controls. |
| `sim_engine/src/debug_overlays.cpp` | Optional weather, physics and performance overlays. |
| `sim_engine/src/fleet_profile.cpp` | Profile-version parsing, validation, safe writes and preservation of invalid saves. |
| `sim_engine/src/fleet_profile.hpp` | Fleet profile types and load/save interface. |
| `sim_engine/src/flight_profiler.cpp` | Timing collection, startup markers and profiler summaries. |
| `sim_engine/src/flight_profiler.hpp` | CPU/GPU profile structures and query interface. |
| `sim_engine/src/flight_replay.cpp` | Flight recording, replay and persistence checks. |
| `sim_engine/src/flight_replay.hpp` | Replay, recording and route persistence interfaces. |
| `sim_engine/src/frontend_font_metrics.hpp` | Glyph advance metrics for the bitmap frontend font. |
| `sim_engine/src/game_frontend.cpp` | World map, relief, route planning, airports, weather UI and menu rendering. |
| `sim_engine/src/game_frontend.hpp` | World/menu/walkaround/controls/settings interface. |
| `sim_engine/src/gl_loader.cpp` | OpenGL function resolution for supported window backends. |
| `sim_engine/src/gl_loader.hpp` | OpenGL function loader interface. |
| `sim_engine/src/graphics_settings.hpp` | Graphics settings defaults, validation and persistence. |
| `sim_engine/src/living_traffic.cpp` | Traffic simulation and rendering. |
| `sim_engine/src/living_traffic.hpp` | Live aircraft and maritime traffic state. |
| `sim_engine/src/main.cpp` | Startup, pack selection, main loop, fixed-step updates, render ordering, region transitions and QA entry points. |
| `sim_engine/src/math3d.hpp` | Small vector, matrix, camera and geometric math types. |
| `sim_engine/src/pack_reader.cpp` | Pack loading, tile access, geospatial queries and caches. |
| `sim_engine/src/pack_reader.hpp` | Region pack, tile, airport and immutable world-frame interfaces. |
| `sim_engine/src/platform_window.cpp` | Linux X11/GLX window, decorated/borderless/fullscreen display modes, OpenGL context, input events and screenshots. |
| `sim_engine/src/platform_window.hpp` | Window, event, key and presentation interface. |
| `sim_engine/src/regional_atmosphere.cpp` | Regional weather solver, terrain exposure, domain generations and snapshot publication. |
| `sim_engine/src/regional_atmosphere.hpp` | Weather model, input, snapshot, outlook and worker interfaces. |
| `sim_engine/src/runtime_checks.cpp` | Save, world-frame, stale-work and other runtime checks. |
| `sim_engine/src/runtime_checks.hpp` | Runtime invariant-check interface. |
| `sim_engine/src/simulation_clock.hpp` | Fixed 120 Hz simulation-time accumulator. |
| `sim_engine/src/volumetric_clouds.cpp` | Atmosphere textures, volumetric cloud rendering and scene resolve. |
| `sim_engine/src/volumetric_clouds.hpp` | Sky/cloud renderer, weather textures and post-processing interface. |
| `sim_engine/src/weather_library.cpp` | `.nswx` loading, baked boundary inputs and source metadata. |
| `sim_engine/src/weather_library.hpp` | Baked weather records and provenance interface. |
| `sim_engine/src/world_objects.cpp` | Baked scenery indexing, visibility and local GPU streaming. |
| `sim_engine/src/world_objects.hpp` | City, building, road and landmark renderer interface. |
| `sim_engine/third_party/GL/glcorearb.h` | Khronos OpenGL definitions and platform types. |
| `sim_engine/third_party/KHR/khrplatform.h` | Bundled third-party support file; consult its license notice and upstream terms. |
| `sim_engine/third_party/bc7enc.cpp` | BC7 texture encoder and its license notice. |
| `sim_engine/third_party/bc7enc.h` | BC7 texture encoder and its license notice. |
| `sim_engine/third_party/bc7enc_LICENSE.txt` | BC7 texture encoder and its license notice. |
| `sim_engine/third_party/cgltf.h` | Single-header glTF/GLB parser. |
| `sim_engine/third_party/miniz_common.h` | Minimal DEFLATE/inflate support for pack data. |
| `sim_engine/third_party/miniz_export.h` | Minimal DEFLATE/inflate support for pack data. |
| `sim_engine/third_party/miniz_tinfl.c` | Minimal DEFLATE/inflate support for pack data. |
| `sim_engine/third_party/miniz_tinfl.h` | Minimal DEFLATE/inflate support for pack data. |
| `sim_engine/third_party/stb_easy_font.h` | Small font, image-loading and screenshot-writing dependencies. |
| `sim_engine/third_party/stb_image.h` | Small font, image-loading and screenshot-writing dependencies. |
| `sim_engine/third_party/stb_image_write.h` | Small font, image-loading and screenshot-writing dependencies. |
| `tools/bake_foliage_cluster_atlas.py` | Builds the foliage cluster texture atlas. |
| `tools/bake_satellite_detail_atlas.py` | Builds subtle satellite micro-detail atlas inputs. |
| `tools/build_c172s_cockpit.py` | Generates the C172S cockpit model from maintained source inputs. |
| `tools/build_regional_architecture_blender.py` | Builds regional architectural meshes in Blender. |
| `tools/compare_topdown_satellite.py` | Compares rendered top-down scenery with image references. |
| `tools/export_glb_models.py` | Exports prepared models to GLB runtime assets. |
| `tools/generate_windscreen_rain.py` | Generates rain lookup/texture assets for the cockpit. |
| `tools/import_ccby_c172.py` | Imports the appropriately licensed C172 source asset. |
| `tools/package_deb.py` | Creates the current Debian package artifacts; consult release-readiness before publishing. |
| `tools/summarize_profile.py` | Summarizes runtime profiler captures. |
| `tools/upgrade_building_atlas.py` | Updates the baked building material atlas. |
| `tools/upgrade_pnoa_cities.py` | Updates PNOA city/scenery inputs. |
| `tools/upgrade_vegetation.py` | Updates generated vegetation resources. |

</details>
