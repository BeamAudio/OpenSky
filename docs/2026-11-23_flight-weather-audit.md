# OpenSky flight and weather audit · 23 November 2026

## Flight model and Earth geometry

OpenSky now loads upstream JSBSim 1.3.4 as a separate shared library. The project-authored C172S configuration supplies the airframe, propeller and engine data. The adapter steps JSBSim at 120 Hz, passes regional wind, air temperature, humidity and QNH to the aircraft, and resolves wheel contact against the terrain and runway elevation. The smoke checks cover cruise trim at about 100 kt and 1,500 m, response to control input, and ground contact and braking. The aerodynamic and propeller coefficients still need comparison with measured C172S performance through the full flight envelope. [Adapter](../engine/src/flight_model_jsb.cpp) · [C172S data](../game/aircraft/c172s/c172s.xml) · [JSBSim ground callback](https://jsbsim-team.github.io/jsbsim/classJSBSim_1_1FGGroundCallback.html)

JSBSim propagates geodetic position on WGS84. The renderer stores regional scenery in a local east/north projection. Terrain, buildings, roads and runways curve away from the camera using a spherical approximation. The drop is about 8 m at 10 km, 31 m at 20 km, and 196 m at 50 km. Cloud rays use a curved atmospheric shell. Mountain ridges obscure the geometric horizon at typical valley flying heights. A continuous globe with exact geodesic placement and region transitions would require a common ECEF transform and rebasing. [Terrain vertex shader](../sim_engine/shaders/cdlod_terrain.vert) · [Cloud shader](../sim_engine/shaders/clouds_qres.frag)

## Weather

The editor bakes a 220-state regional climatology from 2024 ERA5 archive data and five corridor locations. Live mode fetches model-based current weather from Open-Meteo at the aircraft's request location. Successful requests repeat after ten minutes and failures retry after three minutes. A fetch older than 30 minutes falls back to the installed climatology. The local 2 m temperature and dew point enter the solver at its reference elevation so the terrain lapse is applied once. The HUD identifies the weather source. The live boundary is one model point across the region, and the baked precipitation rate is an estimate from a normalized value. [Bake](../builder/bake_weather.py) · [Open-Meteo variables](https://open-meteo.com/en/docs) · [ERA5](https://confluence.ecmwf.int/pages/viewpage.action?navigatingVersions=true&pageId=111152818)

The CPU model advances a **384 × 288 × 4** grid in 0.25 s steps. In the shipped corridor, that is approximately **400 × 425 m** horizontally with samples at 10, 550, 1,800 and 4,500 m above each terrain column. The new terrain exposure index increases uplift and condensation on moist windward ridges. Sheltered basins damp mixing and turbulence under light winds at night, allowing mist if the air is moist. The cloud renderer uses higher-frequency detail within each cell. That detail does not give the CPU model metre resolution. F6 shows the four layers and the terrain indices. [Weather solver](../sim_engine/src/regional_atmosphere.cpp)

| Limitation | Verification and next implementation |
| --- | --- |
| Regional forcing can miss real fronts or differences across the Pyrenees | Replay a valley-to-ridge traverse using identical boundary input, then compare wind, temperature, cloud base and rain traces with time-matched observations and published forecast fields. Evaluate multiple observation/model points and vertical profiles, such as the regional HARMONIE-AROME product. [AEMET model](https://www.aemet.es/en/eltiempo/prediccion/modelosnumericos/harmonie_arome_ccaa/ayuda) |
| Minimal vertical exchange and approximate pressure and precipitation | Use vertical profiles and fixed forcing cases for saturated upslope flow, calm nocturnal basins, and the Ebro Cierzo. Check continuity of temperature, pressure and wind across layer boundaries. Keep finer physics local to the route and benchmark the cost against the current worker. |
| A 4-layer worker uses about 67.5 MiB for four CPU cell buffers | Track solve time, publication backlog and upload stalls before adding cells. The 5-slice GPU weather texture is about 4.2 MiB. Favor smaller publish buffers and adaptive local resolution. Use a full regional NWP solve for offline boundary/reference data. [WRF dynamics](https://www2.mmm.ucar.edu/wrf/site/documentation/users_guide/dynamics.html) |
| Study-level validation and 60 FPS pacing remain open | Run the C172S configuration through stall, climb, cruise, glide, takeoff and landing cases at several weights. Record weather and flight inputs alongside a 30-minute 1080p High Renoir trace. Gate all routes at p95 ≤16.7 ms and record p99 and terrain residency. |

## Rendering and streaming findings

The previous BC7 partition search took 294–369 ms of worker wall time per built page in the short Renoir routes, finishing at L3/L4. The mode-6 encoder reached L9 in both routes at 80–83 ms per page in the comparison and uses the same 183 MiB of logical terrain array storage. It is now the default. Both comparisons predate the fixed-size window presentation change and the final cockpit asset. At matched LOD, check compression quality on distant field edges and rooftops. The full visual sweep also blocks in synchronous warmup drains, so its wall time and frame intervals include deliberate capture waits.

Building direct and specular gains previously exceeded the terrain's daylight response, making facades look too bright at a distance. The near and intermediate building shaders now use lower gains. The terrain shaders still rely on material shading for close grass and forest volume. The close Castejón capture and regional flight photos show the remaining difference in vegetation silhouette, scale and continuity. [Castejón flight photographs](https://pirineoh.com/es/servicio/volar-en-pirineos/39.html)

## Reproduction

```bash
make -C sim_engine flight-test
./run_sim.sh --selftest
./run_sim.sh -- --autotest /tmp/opensky-cabin --cockpit-qa /tmp/opensky-cabin
./run_sim.sh -- --quick-profile /tmp/opensky-flight --online-ortho pnoa
```

The cockpit demo saves five views at 1920 × 1080 from the resolved game image. Flight defaults to a normal window around 90% of the active monitor, sized to fit the work area. High shades the scene internally at 1280 × 720 and reconstructs it with temporal antialiasing.
