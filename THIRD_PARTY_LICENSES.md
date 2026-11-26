# Licenses & Attribution

The project's own source code and original art assets are released under the **MIT License** (see `LICENSE`).
Third-party code, data and assets keep their own licenses, summarized below.
Redistribution follows each component's license, attribution duties, and any
separate API service terms. Do not treat the project MIT license as a license
for third-party scenery, models, or live data services.

## Own Code
- `sim_engine/`, `engine/src/`, `builder/`, `protocol/`, `tools/`: MIT (see `LICENSE`).
- `game/aircraft/c172s/*`: OpenSky-authored aircraft manifest/configuration, CC0 1.0.
  No FlightGear GPL flight models, configs or textures are used.
- Project-generated architecture, vegetation, cockpit geometry and supporting atlases are produced by the checked-in `tools/export_glb_models.py`, `tools/build_c172s_cockpit.py` and related scripts. These project-authored source and generated assets use the repository MIT license unless a source is listed separately below.

## Libraries
| Component | License | Notes |
|---|---|---|
| JSBSim | LGPL-2.1-or-later | Upstream source pinned in `engine/thirdparty/jsbsim/UPSTREAM.md`, built as a separate `libJSBSim.so.1` by `sim_engine/Makefile`. `nspain_sim` links dynamically with `$ORIGIN` rpath; no static LGPL linking. License text: `engine/thirdparty/jsbsim/COPYING.LGPL-2.1`. |
| cgltf, stb_image, stb_image_write, stb_easy_font, miniz_tinfl, KHR/glcorearb | MIT / Public Domain | Headers under `sim_engine/third_party/`. |
| Open-Meteo | CC BY 4.0 (data) | Optional live boundary input; attribution is shown in-game. Data licensing and API service terms are separate. The live feed can be disabled with `./run_sim.sh --no-live-weather`. Check the current [Open-Meteo API terms](https://open-meteo.com/en/terms) and [data license](https://open-meteo.com/en/licence). |

## Geospatial Data
| Source | License | Obligations |
|---|---|---|
| IGN Spain PNOA / MDT05 / MDT25 / INSPIRE Hidrografía | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Attribution "© Instituto Geográfico Nacional de España" must appear in generated pack manifests **and** in the runtime UI. `builder/test_pack.py` enforces this. |
| OpenStreetMap buildings & roads | ODbL 1.0 | **Share-alike**: any distributed `.nsscn` / `.nscity` derived database must be offered under ODbL with © OpenStreetMap contributors attribution. Do not place it under MIT. |
| OurAirports | Public Domain | None. |
| Natural Earth (1:50m land) | [Public Domain](https://www.naturalearthdata.com/about/terms-of-use/) | The globe uses a rasterized land mask from the Natural Earth 5.1.2 vector release. |
| Poly Haven PBR textures | CC0 1.0 | None. |
| AWS Terrarium (border DEM fallback) | Open data (Copernicus/SRTM) | Attribution outside Spain only. |

Generated packs (`packs/`), download caches (`builder/.cache/`, `cache/`)
and build artifacts are gitignored, so no unlicensed imagery is committed.

## Frontend Typography

The interface bitmap atlas was rendered from Inter Medium by the Inter Project Authors. The source font is distributed under the [SIL Open Font License 1.1](https://github.com/rsms/inter/blob/master/LICENSE.txt).

## 3D Aircraft Model
- **Cessna 172 Exterior (`game/assets/models/aircraft/c172s_exterior.glb`)**
  - Original Work: "Cessna 172" by [e737](https://sketchfab.com/e0057537) on [Sketchfab](https://sketchfab.com/3d-models/cessna-172-64cddaee5aff470682659a8c08525046)
  - Intermediate Optimization: `c172.glb` from [veyron007/AUREL](https://github.com/veyron007/AUREL/blob/main/public/models/README.md)
  - Original model license recorded here: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
  - Modifications: re-oriented to `-Z` forward / `+X` right / `+Y` up, textures
    converted to standard Metallic-Roughness PNG/JPEG, propeller/spinner split
    into animated `PropHub`/`PropBlades` nodes, navigation/strobe/beacon lights added.
  - The [AUREL model attribution record](https://github.com/veyron007/AUREL/blob/main/public/models/README.md)
    identifies its `c172.glb` as the e737 Sketchfab Cessna 172 under CC BY 4.0.
    OpenSky reoriented and modified that model; this bundled copy retains the
    creator, source, license and modification notice above.
- **C172S Cockpit (`game/assets/models/aircraft/c172s_cockpit.glb`)**
  - Generated from the project-authored procedural Blender source in
    `tools/build_c172s_cockpit.py` and `tools/export_glb_models.py`; the editable
    `.blend` and generated GLB are included. They contain no imported third-party
    aircraft mesh or texture. The C172 name identifies the simulated aircraft type.

## Online Imagery Policy
Runtime and offline baking stream **IGN Spain PNOA only** (CC BY 4.0), at
distance-tiered zoom levels Z20..Z14 with baked autogen beyond 20 km.
Bing Maps VirtualEarth and Google Maps Satellite endpoints were removed from
`sim_engine/src/pack_reader.cpp`, `builder/build_region.py` and
`tools/compare_topdown_satellite.py`: their terms prohibit redistributable
tile caching and are incompatible with an open-source release. Cached tiles
from those providers were deleted. The QA comparison tool now uses IGN PNOA
WMS or user-supplied local captures.

## Explicitly Forbidden
Esri World Imagery / ArcGIS tiles, GPL aircraft models, configs or textures,
and any unlicensed copyrighted assets. The legacy Phase 0 Esri tile cache was
deleted and must never be reintroduced.
