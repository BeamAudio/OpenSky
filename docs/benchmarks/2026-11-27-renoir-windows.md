# Renoir frame pacing and clouds, 27 November 2026

Tested the native Windows WGL build on a Ryzen 7 5700U with AMD Radeon Graphics, driver 31.0.14076.3, Balanced Windows power plan, and 1920×1080 output on a 60 Hz display. Each route has 239 measured intervals after warmup. The flight dynamics run at 120 Hz. The 350-knot leg uses scripted slew. These routes stay within Aragón. In the separate crossing attempt, the scripted pilot made terrain contact before reaching Catalunya, so continuous boundary frame pacing remains unmeasured.

| Route | F2 pack imagery FPS | p95 / p99 ms | F2 online PNOA storm FPS | p95 / p99 ms | Final LOD, pack / online |
| --- | ---: | ---: | ---: | ---: | ---: |
| Castejón de Sos | 60.0 | 16.69 / 16.88 | 60.0 | 16.71 / 16.88 | 9 / 9 |
| Zaragoza | 60.0 | 16.71 / 16.76 | 60.0 | 16.73 / 16.81 | 9 / 9 |
| Embalse de Mediano | 60.0 | 16.70 / 16.72 | 60.0 | 17.18 / 17.65 | 9 / 5 |
| 350-knot streaming | 60.0 | 16.70 / 16.74 | 60.0 | 16.79 / 17.53 | 9 / 9 |

The initial Castejón capture with the machine's saved custom settings averaged 46.5 FPS with a 26.32 ms p95. It ended at L9. The original Zaragoza capture stayed at L3 because the test jumped to Zaragoza while retaining a Pyrenees terrain anchor. The updated quick profile reanchors before warming each route. Under WGL VSync, Zaragoza dropped to 54.0 FPS on an earlier Balanced run. The Windows laptop presets now use a 60 FPS software deadline before the swap, with an explicit VSync setting available.

Balanced starts the world at 40% of output pixel area. It adjusts between 35% and the configured ceiling (48% by default) using frame times, keeps the cockpit instruments at output resolution, and preserves near-aircraft L7–L9 imagery. It budgets one terrain upload per frame in steady flight. Terrain and online imagery workers sleep when idle. The city streamer reads new chunks off the render thread, uploads them in 2 MiB steps and keeps the previous chunks visible until the replacement is ready. Balanced uses the regional weather grid and the baked windshield relief, animated by the live airflow across the panes. High and Ultra increase cloud sample count and world detail.

The High GPU captures over Mediano show smoother cloud silhouettes and less stippling along the ridges. In the Castejón and Mediano captures, distant woodland still reads flatter than in the aerial references. The cloud check is a qualitative comparison across different lighting and poses. [Castejón aerial reference](https://www.heraldo.es/multimedia/imagenes/viajes/fotos-parapente-pirineos-castejon-sos-aragon-extraordinario/), [Mediano aerial reference](https://jesustejel.com/aragon-desde-el-aire/)

## Windowed storm and region crossing

The 35-second 350-knot storm run used a decorated 1280×720 window, 1920×1080 output, Balanced, and IGN PNOA streaming. Across 2,099 measured intervals, it averaged **59.8 FPS** with a **16.70 ms p95** and a **16.76 ms p99**. One frame exceeded 20 ms. It took 105.79 ms and coincided with the terrain anchor handoff from Aragón to the Catalan Pyrenees. The run recorded 1,028 imagery downloads and finished drawing L9.

The storm visual check captured 24 consecutive frames each with fixed and evolving weather at the same camera pose. Across adjacent frames in both sets, 99% of sampled sky pixels changed by no more than one 8-bit level in any RGB channel. The sample used x=180–1739 and y=0–499 of the 1920×1080 captures.

## Reproduce on Windows

```powershell
.\run_sim.bat --quick-profile "$env:TEMP\opensky-storm" --soak-seconds 35 --graphics-profile balanced --profile-windowed --online-ortho pnoa --weather storm
.\run_sim.bat --autotest "$env:TEMP\opensky-clouds" --cloud-stability --graphics-profile balanced --profile-windowed --online-ortho pnoa --weather storm
python tools/summarize_profile.py "$env:TEMP\opensky-storm"
```

For throughput profiling add `--uncapped-profile`. For a swap-interval comparison add `--vsync-profile`. To hold the scene resolution preset fixed, add `--fixed-profile`. Quick profiles and visual QA use isolated, in-memory fleet state.
