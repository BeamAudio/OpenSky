# OpenSky engine handoff

**Snapshot:** 25 November 2026. Work is on the local `codex/engine-foundation` branch. The engine refactor checkpoint is `47b61a8`; the handoff cleanup follows it. `master` remains at `4582eea`. This checkout has no configured Git remote.

OpenSky is a Cessna ownership and exploration sandbox. Start at Castejón de Sos, fly to another airfield, park temporarily, and return to the same aircraft on restart. [AGENTS.md](../AGENTS.md) defines the active rules.

## Pick up the project

Use the Linux C++20/OpenGL 4.6 build with GCC, make, and CMake 3.18 or newer. From the repository root:

```bash
make -C sim_engine -j"$(nproc)"
./run_sim.sh --online-ortho off
```

If CMake is outside PATH, append `CMAKE=/absolute/path/to/cmake` to the make command. The builder entry point is `./run_builder.sh`. The verified workspace had the Aragón Pyrenees, Catalonia Pyrenees, and Catalonia Coast packs installed. Packs and captures are local artifacts outside Git. On a fresh checkout, install the regions before running QA. Bake missing menu relief images for existing packs with `python3 builder/bake_map_relief.py packs/*.nspack`.

Normal play uses `fleet.ini` in the OpenSky config directory. Use a temporary `XDG_CONFIG_HOME` for interactive experiments involving parking or saves.

## Find the code

| Area | Entry point | Contract |
| --- | --- | --- |
| Aircraft ownership | [Session](../sim_engine/src/aircraft_session.cpp), [profile store](../sim_engine/src/fleet_profile.cpp) | Parking owns the saved location; profile reads validate complete records before applying them, and writes replace files atomically. |
| Flight timing | [Simulation clock](../sim_engine/src/simulation_clock.hpp), [aircraft step](../sim_engine/src/aircraft_hud.cpp) | Sample controls before advancing JSBSim at 120 Hz. Scripted and interactive flight share the aircraft step. |
| World transitions | [Runtime loop](../sim_engine/src/main.cpp), [terrain streamer](../sim_engine/src/async_streamer.cpp) | Terrain jobs retain the immutable world frame they start with. Discard jobs that belong to a superseded generation. |
| Weather | [Regional atmosphere](../sim_engine/src/regional_atmosphere.cpp) | Consumers read the published snapshot. An outlook advances an isolated copy with fixed inputs and prioritizes the live solver. |
| World map | [Relief baker](../builder/bake_map_relief.py), [frontend](../sim_engine/src/game_frontend.cpp) | Bake relief offline. The frontend uploads and mipmaps the sidecars once. |
| QA | [Runtime checks](../sim_engine/src/runtime_checks.cpp) | Use isolated saves. Capture results with the expected region packs and graphics settings. |

The cleanup removed the airport roof detector and its pack build step. It emitted guessed structures that the runtime already ignored. The reader still skips those entries in existing packs. Three temporary cockpit bake outputs were also removed. The maintained `.blend` and runtime `.glb` remain.

## Verification and limits

The refactor branch passed a clean build, flight tests, pack integrity checks, headless and GPU runtime checks, and the full outlook check. The seeded CPU and GPU replays reported identical state. The live World menu capture shows matching Aragón and Catalonia relief. These checks were recorded during the refactor; the handoff cleanup did not rerun the suites.

Useful commands from the repository root:

```bash
make -C sim_engine flight-test
python3 builder/test_pack.py packs/es_aragon_pyrenees.nspack
./run_sim.sh --selftest --online-ortho off
./run_sim.sh --runtime-check --online-ortho off
./run_sim.sh --runtime-check-gpu --online-ortho off
./run_sim.sh --outlook-check --online-ortho off
./run_sim.sh --world-qa /tmp/opensky-world.png --airport ES-0098 --online-ortho off
./run_sim.sh --quick-profile /tmp/opensky-quick-profile --online-ortho off --no-audio
```

The Castejón departure and Santa Cilia touchdown were separate scripted segments. Parking and restart passed with a staged arrival. **A continuous departure, region crossing, landing, taxi, park, and restart remains unverified.**

On Renoir at 1920×1080 High with installed imagery, the quick profiles improved p95 from **19.31 to 18.33 ms** in the Pyrenees and **19.12 to 17.94 ms** in Zaragoza. Average FPS was **59.6–59.7**. These short routes do not measure the handover between packs. Local evidence is under `gameplay_captures/engine-foundation/`: `baseline/perf/`, `after/final-perf/`, `after/flight-demo.log`, and `after/catalonia-world.png`.

## Next milestone

1. Record a continuous Castejón-to-Catalonia flight and save replayable inputs. Log frame times around a region boundary and assert the final parked coordinates, heading, fuel, and logbook after restart.
2. Make the crossing asynchronous. The in-flight path still calls `invalidate_all_and_prewarm()`, and the prewarm helper waits on the main thread for coarse pages with an upper limit of three seconds. Prepare the next frame and its coarse pages while drawing the old frame, then switch once the new coverage is ready. Use a bounded memory budget on Renoir.
3. Compare crossing frame spikes, p99, and time to sharp terrain against the same replay. Once it passes, profile baked or cached terrain pages and budget clouds separately. Extend the weather outlook along the planned route once the basic engine gates are repeatable.
