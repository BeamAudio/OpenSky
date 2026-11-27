# Renoir frame pacing and clouds, 27 November 2026

Tested the native Windows WGL build on a Ryzen 7 5700U with AMD Radeon Graphics, driver 31.0.14076.3, Balanced Windows power plan, and 1920×1080 output on a 60 Hz display. Each route has 239 measured intervals after warmup. The flight dynamics run at 120 Hz. The 350-knot leg uses scripted slew. These routes stay within Aragón. In the separate crossing attempt, the scripted pilot made terrain contact before reaching Catalunya, so continuous boundary frame pacing remains unmeasured.

| Route | F2 pack imagery FPS | p95 / p99 ms | F2 online PNOA storm FPS | p95 / p99 ms | Final LOD, pack / online |
| --- | ---: | ---: | ---: | ---: | ---: |
| Castejón de Sos | 60.0 | 16.69 / 16.88 | 60.0 | 16.71 / 16.88 | 9 / 9 |
| Zaragoza | 60.0 | 16.71 / 16.76 | 60.0 | 16.73 / 16.81 | 9 / 9 |
| Embalse de Mediano | 60.0 | 16.70 / 16.72 | 60.0 | 17.18 / 17.65 | 9 / 5 |
| 350-knot streaming | 60.0 | 16.70 / 16.74 | 60.0 | 16.79 / 17.53 | 9 / 9 |

The initial Castejón capture with the machine's saved custom settings averaged 46.5 FPS with a 26.32 ms p95. It ended at L9. The original Zaragoza capture stayed at L3 because the test jumped to Zaragoza while retaining a Pyrenees terrain anchor. The updated quick profile reanchors before warming each route. Under WGL VSync, Zaragoza dropped to 54.0 FPS on an earlier Balanced run. The Windows laptop presets now use a 60 FPS software deadline before the swap, with an explicit VSync setting available.

Balanced shades the world at 40% of output pixel area, keeps the cockpit instruments at output resolution, and preserves near-aircraft L7–L9 imagery. It budgets one terrain upload per frame in steady flight. Terrain and online imagery workers sleep when idle. The city streamer reads new chunks off the render thread, uploads them in 2 MiB steps and keeps the previous chunks visible until the replacement is ready. Balanced uses the regional weather grid and the baked windshield relief, animated by the live airflow across the panes. High and Ultra increase cloud sample count and world detail.

The High GPU captures over Mediano show smoother cloud silhouettes and less stippling along the ridges. In the Castejón and Mediano captures, distant woodland still reads flatter than in the aerial references. The cloud check is a qualitative comparison across different lighting and poses. [Castejón aerial reference](https://www.heraldo.es/multimedia/imagenes/viajes/fotos-parapente-pirineos-castejon-sos-aragon-extraordinario/), [Mediano aerial reference](https://jesustejel.com/aragon-desde-el-aire/)

Windows exposed one ACPI thermal zone with a constant reported temperature and zero throttle flags during spot checks. CPU and GPU junction temperatures and sustained thermal behavior remain unverified.

## Reproduce on Windows

```powershell
.\run_sim.bat --quick-profile "$env:TEMP\opensky-balanced" --graphics-profile balanced --online-ortho off
.\run_sim.bat --quick-profile "$env:TEMP\opensky-storm" --graphics-profile balanced --online-ortho pnoa --weather storm
.\run_sim.bat --autotest "$env:TEMP\opensky-clouds" --cloud-qa --graphics-profile high --online-ortho off
python tools/summarize_profile.py "$env:TEMP\opensky-balanced" "$env:TEMP\opensky-storm"
```

For throughput profiling add `--uncapped-profile`. For a swap-interval comparison add `--vsync-profile`. Quick profiles and visual QA use isolated, in-memory fleet state.
