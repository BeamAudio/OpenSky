# Northern Spain VFR Flight Sim — Project Rules

## 1. Visual Immersion via iGPU-Friendly Visual Tricks (Never Heavy Modern Rendering)
- Target **60+ FPS on integrated GPUs** (baseline: AMD Radeon Renoir iGPU) while scaling cleanly to high-end RTX GPUs.
- Achieve photorealistic immersion through **visual tricks, baking, and texture synthesis** — NOT heavy modern post-processing/rendering passes that crush iGPU bandwidth:
  - Prefer offline/startup texture super-resolution and synthesized detail maps (even partially faked/hallucinated high-frequency detail derived from ortho tiles) over stacked real-time anisotropic fragment lookups.
  - Never cast real-time shadow maps from tens of thousands of alpha-tested foliage billboards; use baked/analytic contact AO, heightmap shadow rays, and terrain-shader shadow tricks instead.
  - Never mutate `PhysicalSkyMaterial` properties every frame on Vulkan (triggers per-frame sky radiance cubemap rebakes); throttle sky updates to <= 0.5 Hz or on weather change.
  - Avoid full-screen `hint_depth_texture` resolves and full-resolution HiDPI SSAO on iGPU presets (`F1`/`F2`).

## 2. Ruthless Visual Verification Against Real-World Photos
- Never judge visuals from code alone. Capture live GPU screenshots (`./run_sim.sh -- --autotest <dir>`) and compare them directly against real-world aerial/cockpit photographs of the exact Northern Spain locations (e.g., Castejón de Sos, Río Ésera, Santa Cilia `LECI`, Huesca `LEHC`, Embalse de Mediano).
- Explicitly check for and fix:
  - **Satellite-first texture realism**: never overpower real IGN PNOA or streamed Z17/Z18 orthophotos with harsh procedural autogen blotches, synthetic urban paver grids, or heavy rock stripes. Keep `< 1 km` close-up micro-detail as a subtle zero-mean luminance grain (`<= ±12%`) and prioritize `L7–L9` close-up quad streaming first.
  - **Sense of scale**: keep cockpit camera near natural human-eye perspective (`~50°` vertical FOV), true-to-life tree heights (`6–13.5 m`), and layered elevation-aware aerial perspective.
  - **Seamless blending**: distant trees, bushes, terrain, and runways MUST share the exact same atmospheric haze, sky-haze emission, and color/value balance so nothing pops as a dark sticker or hard rectangle overlay.
  - **Continuous forest canopies (never isolated pin trees)**: render vegetation as broad, interlocking multi-dome 3D canopies and closed-canopy groves that fill in woodland volume continuously, with soft unified under-canopy shadows rather than dark circular blobs under individual trees.
  - **Minimal compact traffic**: keep road vehicles small (`~3.2 m`), minimal (2-box silhouette + flush light quads), and density-capped to road segment length.
  - **3-layer (`0m / 5m / 10m`) ray-traced volumetric night lighting & dynamic multi-layer weather**: bake night lighting across three DSM height slices (`0 m`, `5 m`, `10 m`) combined into volumetric street-canyon light across all cities, towns, and roads; evolve clouds, winds, turbulence, and rain/fog/storms dynamically across vertical atmospheric layers.

## 3. Product & Display Invariants
- **Short-Night VFR Solar Cycle**: Use real-time UTC astronomical solar ephemeris and live weather with a compressed, fast-transit night phase (no Midnight Sun floor clamp) so the sun genuinely sets/rises while keeping the VFR night down-period brief.
- **HiDPI-aware windowing**: Size the desktop window to a comfortable 720p+ logical footprint on HiDPI displays (`DisplayServer.screen_get_usable_rect()` / DPI-scaled) while keeping internal 3D `scaling_3d_scale` anchored to the active quality tier (`F1`–`F4`).
- **Commercially safe licenses only**:
  - Allowed: Godot (MIT), JSBSim dynamically linked (LGPL `.so`/`.dll`), IGN Spain PNOA / MDT05 / MDT25 / INSPIRE Hidrografía (CC-BY 4.0), OurAirports (Public Domain), OpenStreetMap (ODbL), Poly Haven (CC0 1.0), Open-Meteo.
  - Forbidden: Esri imagery, GPL aircraft models/configs/textures, or unlicensed copyrighted assets.
