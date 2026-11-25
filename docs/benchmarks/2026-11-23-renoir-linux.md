# OpenSky baseline · AMD Renoir · 23 November 2026

## Capture setup

- Output: 1920×1080, OpenGL 4.6 core, High / Medium / Low tiers, uncapped benchmark.
- GPU: AMD Radeon Graphics, Renoir, Mesa 25.0.7, radeonsi, ACO.
- OS kernel: Linux 6.12.116.
- World: installed Aragón/Pyrenees pack; 16,817 terrain tiles, 17 airfields, 103,109 OSM buildings, 51,714 roads, and 94,974 baked city buildings.
- Command: `./run_sim.sh -- --autotest /tmp/opensky-m0-baseline` (captured before the editor and region-selection changes in this delivery pass).

## High tier results

| Flight segment | Average | Mean frame time | p95 frame time | 60 FPS gate |
| --- | ---: | ---: | ---: | --- |
| Pyrenees, 100 kt | 54.9 FPS | 18.23 ms | 25.89 ms | Fail |
| Zaragoza, dense city | 56.7 FPS | 17.64 ms | 29.44 ms | Fail |
| Mediano, water | 64.0 FPS | 15.62 ms | 23.18 ms | Average passes; pacing fails |
| Streaming stress, 350 kt | 62.0 FPS | 16.12 ms | 24.39 ms | Average passes; pacing fails |

The release gate remains 60 FPS average per standard route, p95 ≤16.7 ms and p99 ≤20 ms. This baseline does not meet it. Low tier averaged 67.6–82.7 FPS across these four routes, with p95 spikes as high as 25.85 ms. Uploads reported by the benchmark stayed below 0.55 ms; this does not identify the dominant GPU pass.

## Visual review findings

Captured at 1080p with the fixed autotest camera poses. The cockpit shows readable instruments but the close ground and distant ortho remain soft. The dedicated cloud captures show little or no visible cloud structure under the intended cloudy preset. The close woodland camera shows a broad green terrain surface without recognizable canopy form. The diagnostic says `0 trees`; the renderer's current canopy is a terrain-volume pass, so this counter is misleading and does not measure that pass.

Captures are in `/tmp/opensky-m0-baseline/`. Review the exact views `shot_ES-0205.png`, `shot_clouds_mediano.png`, `shot_lidar_forest_close.png`, and `shot_cockpit_3d_panel.png`.

## Canopy integration fix candidate

The follow-up renderer candidate is captured in `/tmp/opensky-m1-m2-fixed/`. A foliage experiment that lifted the terrain mesh by 6–13 m and added crown domes produced faceted raised patches and broadened satellite texture derivatives. The fix removes canopy geometry from the CDLOD height field and keeps its response in the terrain material pass. The fixed Castejón valley capture no longer has those patches. The forest close-up still lacks recognizable crown silhouettes.

High output is 1920×1080, but the scene input is 1280×720. The close-up offline view uses the packaged Z17 imagery; PNOA Z20–Z14 is only available through online streaming. The remaining softness therefore includes a pre-existing internal-scale and source-LOD limit. It does not meet the requested near-ground 0.25 m/px offline detail target.

High results from the fixed candidate run:

| Flight segment | Average | Mean frame time | p95 frame time | 60 FPS gate |
| --- | ---: | ---: | ---: | --- |
| Pyrenees, 100 kt | 55.9 FPS | 17.89 ms | 24.39 ms | Fail |
| Zaragoza, dense city | 54.0 FPS | 18.52 ms | 27.28 ms | Fail |
| Mediano, water | 62.7 FPS | 15.96 ms | 21.35 ms | Average passes; pacing fails |
| Streaming stress, 350 kt | 70.8 FPS | 14.12 ms | 21.59 ms | Average passes; pacing fails |

These results are another short candidate run, not a thermally settled qualification. GPU per-pass timing and p99 are still missing.

## Online-first ortho and reverse-Z cloud mask

The interactive simulator now checks cached PNOA Z20/Z19 imagery before the
installed pack at matching distance tiers. It shows the pack tile until the
online tile is ready, then rebuilds close pages from PNOA; the pack remains the
offline fallback. The final dedicated 1920×1080 Castejón capture used PNOA
(`76` new downloads and `32,792` cache uses during the run). The autotest still
starts offline except for this explicit online capture.

The same review found the High-tier post pass used a reversed depth mask for
low-resolution clouds. Reverse-Z sky is near zero, while terrain depth is
larger, so the old threshold allowed cloud reconstruction to wash over terrain.
The resolve now composites cloud color only on sky and applies bounded
neighborhood sharpening without adding texture reads.

The final close online capture still reached only CDLOD L7 (2.5 m per page
texel), so it does not yet prove the required 0.25 m/px landing detail. The
sharper sky and stronger terrain contrast are visible, but high-detail
close-page residency still needs work. This short, non-thermally-settled run
averaged 53.3 FPS in the Pyrenees, 50.2 in Zaragoza, 62.3 at Mediano and 59.7
in the 350 kt streaming stress route. p95 ranged from 21.64 to 27.82 ms, so
neither the 60 FPS route gate nor frame-pacing gate is met. See
`/tmp/opensky-final-online-terrain/` for the captures and GPU-pass CSV.

## Asynchronous GPU pass sample

The follow-up `--bench-only` run sampled one in every four frames and wrote `/tmp/opensky-gpu-profile/gpu_pass_times.csv` (408 samples, 136 per quality). This profile-enabled run averaged 59.7 FPS in the Pyrenees, 59.1 in Zaragoza, 78.1 at Mediano and 61.9 in the streaming stress route. p95 ranged from 21.34 to 27.84 ms. The query sampling adds a small measurement cost, so these are diagnostic figures rather than qualification results.

| High pass | Mean | p95 |
| --- | ---: | ---: |
| Sky | 0.43 ms | 0.52 ms |
| Terrain | 2.95 ms | 5.12 ms |
| Buildings and roads | 1.08 ms | 4.66 ms |
| Aircraft | 2.13 ms | 2.48 ms |
| Clouds and post | 3.97 ms | 4.94 ms |
| HUD | 0.09 ms | 0.12 ms |

These pass distributions combine route types and must not be added as if their p95 samples came from the same frame. The next profile change should preserve route labels and include CPU simulation, uploads, swap/present wait and p99 so the remaining frame-time gap can be assigned accurately.

## Milestone status

- **M0 build:** Linux renderer rebuilt successfully with the local compiler. Windows has no validated platform backend or package yet.
- **M0 baseline:** reproducible scene names, output dimensions, GPU/driver identity, per-route FPS and p95 captured. Asynchronous GPU pass timing now writes to the autotest output. Per-route profile labels, CPU simulation/upload timing, p99 and a thermally settled 30-minute soak are still missing.
- **M1 playable flow:** flight menu and mouse yoke exist; region selection is being connected to the editor. Controls persistence, a complete flight setup, pause/restart/debrief flow and clean-install timing remain open.
- **M2 performance:** not qualified. The fixed candidate misses 60 FPS average on two of four routes and misses the p95 gate on all four.
- **M4 scenery / M5 weather:** the raised-canopy terrain regression is fixed. Near-ground terrain remains soft and canopy silhouettes, cloud detail, and moving visual review remain open.
- **M6 world editor:** global local-XYZ/Terrarium import and isolated region launch are implemented. Global dry-run validation passed. Two tiny non-Spain pack builds passed integrity checks: an offline imported imagery/elevation fixture and a real Terrarium Alps terrain-only pack. Representative high-resolution scenery and offline flight validation remain open.
