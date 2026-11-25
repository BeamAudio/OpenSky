# OpenSky VFR Simulator delivery plan

## The launch promise

OpenSky VFR Simulator is a PC flight simulator for players who want the pleasure of flying low over recognizable landscapes on an integrated graphics laptop. The factory aircraft is the Cessna 172S. The shipped world is the Pyrenees and its Aragón approach corridor. The bundled **OpenSky World Editor** lets players build, install, and fly their own regional scenery from available sources, without a publisher having to package each location.

The installation must support a complete offline flight in the shipped region. Each local profile owns its aircraft: the player can park it at an airfield, leave it shut down, and return to the same airframe with its fuel, systems, condition, and service history intact. They can fly VFR between fields, log the flight, and handle repairs. Local ownership and saves work without an account or network connection. A player who prefers to tinker can import or acquire data in the editor, build a region locally, and start a flight in it through the same simulator. The primary input path must work with an ordinary laptop keyboard and mouse. A joystick and pedals are optional.

The initial release is an offline-first personal aircraft simulator. Its save and aircraft identity formats should be versioned so a future online service can synchronize ownership, parking location, maintenance history, and aircraft state. Online accounts, shared parking occupancy, and multiplayer are later service work; a network outage must never prevent a player from flying their locally owned aircraft.

## Seasonal world state

The environment follows the UTC calendar season and its corresponding baked regional weather state. Winter, spring, summer, and autumn affect the snowline and snow coverage, vegetation and field color, canopy response, temperature, and seasonal solar path. Seasonal color stays anchored to the orthophoto so the real field boundaries and land cover remain visible. `N` cycles a development preview through the four seasons and back to automatic; `--season auto|winter|spring|summer|autumn` selects a repeatable state, and `--season-qa <dir>` captures the same canopy and alpine views in all four seasons. The preview is a review control, not a replacement for the calendar-driven default.

Microsoft Flight Simulator 2024 sets the reference for cockpit legibility, low altitude scenery, atmospheric depth, cloud appearance, and the sense of place. Compare equivalent scenes and user journeys. OpenSky 1.0 focuses its content and verification on light aircraft VFR, one aircraft, one curated region, and independent world creation. [MSFS 2024 feature reference](https://www.flightsimulator.com/microsoft-flight-simulator-2024/) · [Published PC requirements](https://www.flightsimulator.com/guide/which-platform-is-right-for-me/)

## Starting point and open gaps

| Area | Evidence in the workspace | Consequence for delivery |
| --- | --- | --- |
| Platform | The active build uses C++20, OpenGL 4.6, X11/GLX, and a Linux launcher. A working Windows package has not been demonstrated. | Bring up Windows on a real iGPU during the first playable milestone. Qualify each OS and driver before listing it as supported. |
| Rendering | CDLOD terrain, regional packs, baked lighting, forest canopy rendering, temporal reconstruction and reduced resolution cloud rays exist. Output stays at 1080p; the main scene pixel count, AA mode, mesh LOD radius, and native-resolution cockpit pass are configurable in the game menu. | Re-profile each pixel-budget tier on Renoir. Keep 60 FPS and thermal headroom as release gates while comparing distant-edge clarity and cockpit detail. |
| Frame pacing | On 23 November 2026, the 1920×1080 Renoir High baseline averaged 54.9 FPS in the Pyrenees, 56.7 in Zaragoza, 64.0 at Mediano and 62.0 during a 350 kt streaming stress run. High p95 ranged from 23.18 to 29.44 ms. | High fails the 60 FPS gate in two standard routes and fails the frame-pacing gate in all four. GPU per-pass timings and p99 are still missing. |
| Visual review | Current autotest captures show little or no cloud structure in the intended cloud view, a close woodland view without recognizable canopy form, and soft cockpit/ground detail. The current `0 trees` diagnostic does not count the terrain canopy pass. | Treat canopy, clouds, near terrain and cockpit as active quality blockers; review moving captures against the named regional photos before signoff. |
| Aircraft | The runtime now dynamically links pinned upstream JSBSim. The OpenSky C172S XML defines its own estimated aerodynamic and propeller data. Trim, control response, 120 Hz stepping, terrain contact and braking have direct integration checks. | Validate the model against licensed POH/TCDS performance tables and flight-test measurements. The current authored coefficients are not yet a study-level validated aircraft. |
| Product | Menus, a mouse yoke and an airfield picker are in development. Persistent airframe ownership, parking, wear, and repair records are not implemented yet. | Qualify a complete new player journey, including acquiring the factory aircraft, parking and resuming it, maintenance actions, pause, reconnecting input, save, and debrief. |
| Editor | The Tk editor has a custom bounds input, pack compiler, installed pack inspector and simulator launcher. The defaults still select Spain specific imagery and scenery. | Treat global, local authoring as a launch feature with its own acceptance tests. A bounding box alone does not prove global coverage. |

See the [runtime entry point](../sim_engine/src/main.cpp), [render configuration](../sim_engine/src/volumetric_clouds.cpp), [regional weather model](../sim_engine/src/regional_atmosphere.cpp), [JSBSim adapter](../engine/src/flight_model_jsb.cpp), [factory aircraft data](../game/aircraft/c172s/c172s.xml), [builder](../builder/build_region.py), and [editor](../builder/build_region_gui.py). The [flight and weather audit](2026-11-23_flight-weather-audit.md) records the current coordinate and forcing paths and validation gaps.

The current machine and per-route results are recorded in the [Renoir baseline report](benchmarks/2026-11-23-renoir-linux.md). M0 has its first Linux build and visual/performance baseline; per-pass GPU timings, p99, clean replay packaging, Windows build validation, and the 30-minute soak remain open.

## 1.0 release gates

These are proposed acceptance targets. Freeze a specific laptop SKU, OS, driver, memory configuration and power profile in M0 before using the numbers in marketing or compatibility claims.

| Gate | Required evidence |
| --- | --- |
| 1080p High | 1920×1080 output in every standard flight scenario on the reference AMD Renoir laptop with 16 GB dual channel memory, supported driver, SSD and AC power in its standard performance profile. High may use bounded internal reconstruction. List the internal scale range and capture the HUD at output resolution. |
| Frame pacing | Each standard route averages at least 60 FPS in an uncapped, thermally settled run. Pre-present frame time p95 ≤16.7 ms and p99 ≤20 ms. In 60 Hz play, no recurring dropped frames and no frames over 33.3 ms during the scripted in-flight streaming windows. Log startup and intentional teleports separately. Report all required scenes individually. |
| Resource use | A 30 minute flight and a repeated region load loop show no sustained memory growth, swaps or residency thrash. Starting budget is ≤8 GB combined attributable game working set and GPU residency on the 16 GB system, counting physically shared allocations once. Record peak, steady state and hardware pressure. Separate editor build budgets from flight budgets. |
| Visual quality | Capture fixed and moving views for takeoff, approach, level cruise, cloud approach, cloud entry, storm rain and sunset. Compare matched views with real photographs of the exact Pyrenean locations. Review several frames in motion for seams, unstable silhouettes, temporal ghosting, disappearing canopy, synthetic texture patterns and weather flicker. Sign off the footage, not only screenshots. |
| Aircraft and gameplay | Start with the profile-owned factory C172S at a saved parking stand. Complete cold start, taxi, takeoff, cruise, approach, landing, shutdown, save, and resume offline. Aircraft fuel, electrical and system state, condition, repairs, and service/logbook records persist across launches. Repeat with mouse and keyboard and with joystick hardware. Pass the flight model and systems gates in M3. Home → setup → aircraft state → flight → parking → debrief is supported without a terminal. |
| Independent world building | On a clean install, a user can select bounds, obtain or import permitted data, build a package, install it and fly it from the UI. Prove this with two regions outside Spain with dissimilar terrain, including one run with no photo source and one offline import. Fly the resulting packs offline. Invalid source data and interrupted jobs must leave a recoverable state. |
| Distribution | A Windows installer and a separately qualified Linux bundle include the aircraft, shipped region and editor. Confirm clean installation without system Python, upgrade, removal, offline first flight, persisted controls, and the licenses and attribution carried by each source layer. |

At qualification, include a Renoir machine on each OS being advertised, one Intel iGPU laptop such as an Iris Xe configuration, and one current AMD iGPU laptop. Use these machines to define support tiers from measured results. Treat battery mode and single channel memory as separately measured profiles. Measure dedicated GPU scaling as a secondary check.

## Milestones and dependencies

Estimates are engineering effort, including integration and verification. They assume access to target laptops and the existing Pyrenees pack. Provider agreements, independent pilot review and release distribution lead times can add calendar time.

| ID | Milestone | Depends on | Estimated effort |
| --- | --- | --- | --- |
| M0 | Reproducible build and baseline | Current workspace | 1–2 engineer weeks |
| M1 | Playable OpenSky vertical slice with local aircraft persistence | M0 | 3–5 engineer weeks |
| M2 | 1080p performance and streaming | M0, integrate into M1 | 4–7 engineer weeks |
| M3 | Factory C172S and flight systems | M1 aircraft interface | 4–7 engineer weeks |
| M4 | Pyrenees scenery and lighting | M2 budgets, M1 flight path | 5–8 engineer weeks |
| M5 | Evolving weather and detailed clouds | M0 baseline, M2 budgets | 4–7 engineer weeks |
| M6 | Global editor and pack interoperability | Provider and format spike after M0, integrate with M1 and M4 | 5–8 engineer weeks |
| M7 | Aircraft ownership loop, flight activities and public beta | M1–M6 feature complete | 3–5 engineer weeks |
| M8 | Release candidate and 1.0 | M7 | 2–4 engineer weeks |

Total planning range **31–53 engineer weeks**. M3, M4, M5 and M6 can overlap where their interfaces are stable. A focused team should schedule Windows validation and a non-Spain build early. Finding either blocker after art polish would force rework.

### M0 · Reproducible build and baseline

- Snapshot the source, assets, toolchain, pack schema and benchmark commands. Make a known-good build available to the team with renderer, OS, GPU driver, power mode, pack hash, quality settings and internal render scale recorded.
- Build a deterministic flight replay input that freezes time and weather when comparing changes. Capture frame timing on the main thread, GPU timings by pass, present interval, terrain and weather upload timing, memory pressure and output resolution. Store captures independently of thermal and network tests.
- Run Castejón de Sos runway final, Santa Cilia circuits, Huesca cross-country, Mediano shoreline, dense Zaragoza, a fast streaming traverse, and broken and rainy weather. Record packaged versus online imagery and warm versus cold caches. Keep screenshots and benchmark logs linked to the build ID.
- Confirm which flight dynamics implementation is present, its license, and how to substitute a pinned upstream JSBSim build if needed. Check which graphics capabilities the Windows and Intel drivers expose. Inventory the editor's data sources and identify one viable non-Spain build path.
- **Exit gate** — Anyone on the team can reproduce the build and replay, distinguish CPU, GPU and streaming stalls, and identify the largest three gaps with evidence. Assign an owner to each release blocker.

### M1 · Playable OpenSky vertical slice

- Brand the executable, installers, launcher, in-game UI and editor as OpenSky. Name the Pyrenees and C172S in the flight setup and first launch. Preserve existing packs, settings and scripts through compatibility aliases or migration.
- Ship a launcher with **Fly**, **World Editor**, **Settings**, and **Quit**. Flight setup selects installed region, airfield, start state, weather, and time. Support startup on the runway, in the air, and at parking. Pause provides resume, restart and return to setup.
- Make the mouse yoke discoverable and configurable. Provide pitch and roll sensitivity, inversion, dead zone, recentering and a separate look action. Add keyboard and joystick bindings, calibration and device hotplug. Preserve the state of the aircraft when paused, including controls and systems.
- Put routine flight information in the cockpit and a compact optional overlay. Keep render and streaming telemetry behind a diagnostic control. Add loading progress, accessible menu navigation, an in-game checklist and clear explanations for offline or missing pack states.
- Create a versioned local pilot profile that owns a stable aircraft instance ID. Persist its parked airfield/stand (or validated world position where no stand exists), shutdown state, fuel, electrical state, component condition, and service/logbook records. Save atomically at shutdown/parking and recover safely after an interrupted write. Migrate save versions without discarding the user's aircraft.
- Make Windows windowing, input, audio, file paths and frame pacing functional on actual iGPU hardware. Prefer a small platform layer that keeps the existing renderer until capability measurements justify a backend change. Validate Linux alongside Windows.
- **Exit gate** — A first-time player receives their profile-owned C172S, starts from a parking stand within two minutes of installation completing, pauses and resumes it, changes input settings, completes a Santa Cilia circuit, parks and shuts down, then relaunches into the same saved airframe state without console commands, an account, or external downloads.

### M2 · 1080p performance and streaming

- Establish a preliminary 14.5 ms GPU budget on Renoir. Allocate 5 ms to terrain, 2.5 ms to buildings, traffic and aircraft, 2 ms to sky, clouds and rain, 3.5 ms to output reconstruction and post, and 1.5 ms to copies, HUD and margin. Keep frame thread CPU work near or below 8 ms. CPU and GPU timings overlap.
- Cap worker concurrency and uploads by measured frame headroom, residency pressure and travel direction. Move texture conversions, weather packing, scenery decoding and prefetch off the frame thread. Use fences and bounded staging queues so a worker or upload never blocks the present path.
- Use an explicit shared memory budget across graphics, terrain and weather. Build depth and imagery LOD ladders, bounded caches and staggered uploads. Confirm that under memory pressure the renderer lowers distant tile residency smoothly and releases memory when regions change.
- Keep cockpit and HUD crisp at 1080p. Tune temporal reconstruction using velocity and depth confidence, stable foliage and mist history, motion rejection and tested exposure changes. Apply gradual fallback under sustained load through cloud sampling, distant object density and internal shading within the disclosed High range. Preserve the output resolution.
- **Exit gate** — All standard and stress routes have recorded timings. Standard routes pass the 1080p gate in flight and remain within the approved image quality floor. A performance regression automatically blocks promotion to the next release channel.

### M3 · Factory Cessna 172S and flight systems

- Specify the factory airframe and avionics fit. Audit the dynamic library, pin the intended implementation, and reconcile the aircraft configuration against licensed performance references. Keep the aircraft and all configurations distributable with the game.
- Match weight and balance, trim, power, flap and stall behavior. Tune ground contact, brake and steer response, crosswind authority, flare, touchdown and go-around. Drive animation, audio and flight instruments from simulation state.
- Build a readable cockpit with mapped controls, useful radio/navigation functions, working gauges, electrical and fuel states, lights, cold start and shutdown, plus real state changes when controls are operated. Bake cabin material wear, contact lighting and normal maps. Keep enhancements within the cockpit budget.
- Model an airframe's service life, with component wear driven by operating time, loads, hard landings, overheating, and system faults. Give the player inspections, repair/replace actions, time and cost estimates, and a durable maintenance log. Repairs must update the same JSBSim and system state used by flight, and a saved airframe must resume with the repaired or unrepaired condition the player left it in.
- Add assisted and full procedure profiles. Provide startup, takeoff, glide, approach, landing, and shutdown checklists. Use validated instrumentation and feedback, while leaving unsupported avionics functions clearly identified in the aircraft manual.
- **Exit gate** — Publish a flight validation matrix stating aircraft loading, density altitude, wind, flap state and reference source. Initial targets are published stall speeds within 3 knots and takeoff distance and climb within 10% under matched conditions. Verify that selected faults and repairs alter the relevant system and flight behavior and survive save/reload. A C172 pilot or instructor reviews the handling, controls and scenarios before the study level label is used.

### M4 · Pyrenees scenery and lighting

- Curate recognizable approaches and landmarks at Santa Cilia, Castejón de Sos, Huesca, Mediano and the Zaragoza corridor. Bake height, runways, terrain shadow visibility, vegetation masks, surface materials and local light data from attributed sources. Review shoreline and runway alignment against reference photographs.
- Retain a stable satellite base down to runway level. Bake or generate close tile detail ahead of flight from high resolution orthophoto and material classification. Apply subtle, zero mean synthesis within the project limit of ±12% luminance. Prioritize close terrain LOD by route and approach direction. Preserve real field edges and road markings.
- Render forests as overlapping continuous canopy volumes derived from actual woodland and height coverage. Use low polygon canopy surfaces, baked transmission tint for backlit leaves, and broad baked contact occlusion. Keep the distance transition continuous and share terrain haze and illumination across the view. Local detail stays inside mapped vegetation.
- Bake directional sun visibility and contact occlusion for terrain, settlements and canopy. Interpolate LUTs and use height-aware parallax in slopes and runways. Use baked 0/5/10 m night light slices for street and building illumination. Use astronomical UTC with compressed night transit as the default and permit fixed conditions for practice.
- Apply the four calendar seasons to snowline, terrain snow, meadow/field and canopy response, with automatic UTC selection and a deterministic preview state. Keep changes subtle against the orthophoto and use the regional weather library's season-specific temperature and snowline. Capture matching winter, spring, summer and autumn flights at the Pyrenees sites, and compare them with real photos from the same area and season.
- **Exit gate** — The full factory flight route passes the visual gate at runway, climb and cruise altitude. Seasonal captures show distinct but regionally plausible states without replacing satellite field patterns or creating seams at tile/vegetation boundaries. Photos and captures document the same visible landmarks and conditions. No extra runtime shadow-map pass is required for canopy or routine world objects.

### M5 · Evolving weather and detailed clouds

- Use a shared CPU weather state for flight forces, clouds, precipitation and terrain cloud shadows. Evolve a terrain-following field for wind, lift, humidity, cloud water and rain. Blend live observations as boundary conditions and include replayable offline presets. Profile AVX2/FMA transport on the reference CPU and qualify the scalar fallback.
- Shape cloud cells at regional scale with condensation, entrainment and shear. Use mip-filtered, world anchored volumetric detail for visible structure on the order of 1–2 m near the aircraft. Allocate fine steps by distance and projected footprint. Track history through movement and allow cloud forms to change as the weather evolves.
- Render local rain as depth-clipped world volumes using precipitation and wind. Use clouds' own optical thickness for sun shafts and a shared evolving coverage field for ground shadows. Reuse illumination LUTs and update shadow or sky fields at a bounded rate. Tune gray, blue and warm scattering separately in rain, broken cloud and golden light.
- **Exit gate** — Replay clear → forming cumulus → broken deck → rain → clearing in multiple seasons. In capture, cells form and deform over time, clouds show visible fine structure at the edge, rain shafts move with wind and disappear behind terrain, mountains affect local airflow, cloud shadows track the cells, seasonal temperatures and snowline agree with the selected season, and the Renoir frame budget holds. A 1–2 m render-detail claim describes visual features close to the aircraft. The simulation grid remains regional.

### M6 · Bundle the global world editor

- Present a map and coordinate entry with build bounds, resolution and estimated tile count, time, disk and coverage. Show per-layer data availability and attribution before the build. Import permitted local imagery and elevation with CRS, vertical datum and nodata validation.
- Split dataset and country selection from the compiler. Add provider adapters for permitted global elevation, airport and vector data, and imagery where a reusable source is available. Reuse the same tile and bake code as the factory world. In areas with no photo source, generate clearly labeled terrain materials using elevation and land cover. Accept appropriate licensed user imagery for higher fidelity.
- Give every region a stable ID and versioned manifest describing CRS, bounds, vertical units, layer coverage, provenance, optional features and checksums. Support region packs that have no airfield and start the aircraft at a chosen point or a safe preview altitude. Detect overlap and expose which pack is active. Handle edges at the accepted projection bounds and either split antimeridian areas automatically or provide a clear build split.
- Preflight resources and dependencies, resume downloads from a bounded cache, cancel safely, build in staging, verify the output, and install atomically. A build leaves the current installed version usable until the replacement passes validation. Use descriptive progress, source errors, and retry controls. Include an offline import path. Ship the needed editor runtime with the game.
- Add **Fly this region**, preview coverage, import/export and uninstall. Make region packages load via the same versioned contract as the shipped Pyrenees. Package exports carry data source notices and only include layers the user can redistribute. Import validates file size, paths, format version and memory limits before runtime allocation.
- **Exit gate** — Build and fly two non-Spain regions from the bundled UI on clean consumer machines. A user with no publishing account or terminal can carry out the full workflow. Test invalid data, offline operation, failure and retry, version upgrade, removal, and how a region without licensed imagery or an airfield appears in the sim.

### M7 · Flight activities and public beta

- Link the product into three loops around the owned airframe. **Free Flight** starts from where the player parked and is saved and resumable. **Aircraft management** shows location, fuel, system condition, damage, due inspections, repairs, and the logbook, with clear service actions before flight. **Training** covers controls, circuits and VFR navigation. **Challenges** offer a Pyrenees sightseeing route, a short field arrival, and a wind or weather exercise. Add briefing, route map, restart and a debrief with useful approach and touchdown feedback.
- Invite laptop sim players and pilots to attempt the first flight and editor flows. Measure time to first flight, task completion, crashes, settings retention and build/install success from observed sessions. Offer English and Spanish interfaces and preserve readable labels and keyboard focus at supported DPI scales.
- Review finished visuals across the target hardware matrix using equal camera settings and route conditions. Resolve misleading lighting, unstable cloud edges, runway artifacts, and lost scenery detail from actual reports. Make the latest build's performance evidence accessible from the beta notes.
- **Exit gate** — At least 9 of 10 new beta participants complete the mouse-and-keyboard first flight flow without team intervention, with at least one successful world build conducted by a first-time editor user. No open issues prevent installation, flying, landing or regional pack creation on the support matrix.

### M8 · Release candidate and 1.0

- Assemble signed installable artifacts, matching game/editor schemas, offline scenario content and data notices. Retain a rollback build and test updates with existing controls, saves, and locally generated regions. Archive reproducible package inputs and build logs.
- Run the full release matrix with final packages. Include cold boot, offline first flight, saved flight resume, clean and upgraded installs, interrupted editor builds, long weather flights, thermally settled performance and supported graphics drivers.
- Publish a clear hardware compatibility table, known limitations, quick start and example editor region. Include a support bundle with build ID, driver, pack IDs and recent frame timings. Make support information opt in and avoid uploading raw location or input history by default.
- **Exit gate** — All 1.0 release gates pass on the final artifact. Publication claims cite the exact tested configuration. Product, simulation, rendering and packaging reviewers sign off from the same release candidate.

## What OpenSky keeps as an engine

Use the simulator as the first shipping client of reusable services. Keep flight scenarios, training and scorecards in the product layer. The interfaces that leave the 1.0 project are the pack format, streaming and residency, rendering capability tiers, input and fixed step simulation, terrain and atmosphere queries, and replay and capture. The editor and simulator share build manifests, terrain metadata and validation. Do not maintain separate regional formats for each.

The independent region build in M6 is the first engine portability proof. For the second, load a distinct scenario and another validated region through the same APIs with no changes to renderer or streaming code. Exercise a small scripted scenario through the headless runner. Version the contracts before promising third-party compatibility.

| Phase | Engine deliverable | Proof |
| --- | --- | --- |
| 1.0 · Engine foundation | Versioned world runtime, scenario instances, pack lifecycle, capability reporting, fixed step scheduling and headless replay. | The Pyrenees and generated regions run through one loader and renderer. The same replay runs with rendering enabled or disabled. |
| E1 · Extensible simulator | Document the world, scenario and aircraft interfaces. Ship a sample scenario, SDK validation tools, and a second aircraft using the same input, camera, audio and world interfaces. | A contributor adds an aircraft and a scenario without changing renderer or streaming source. The factory aircraft still passes its regression suite. |
| E2 · Scene and simulation authoring | Add scene placement, entity lifecycles, reusable components for input, physics, camera, audio and triggers, a compact scripting surface, profiling and asset import with versioned formats. | Build a small non-flight simulation in a generated region using the same large-world streaming, weather and lighting stack. Package and run it on the iGPU baseline without flight game code. |
| E3 · Engine distribution | Package the editor and runtime for standalone applications. Document supported graphics capabilities, extension compatibility and asset migration. Add a rendering backend only if platform data or profiling justifies it. | An independent developer creates, exports and runs a second application from a clean install. The engine update also loads the 1.0 flight game and its existing region packs. |

### Future online aircraft service

After the offline ownership loop is stable, add online accounts and synchronization around the same versioned aircraft records. The service is authoritative for ownership, aircraft condition, repairs, and parking-stand occupancy; clients continue to simulate flight locally and submit validated state transitions and logbook events. Define conflict handling for two devices, reconnect after a network interruption, server backups, and a clean offline mode before adding shared-world traffic or multiplayer. Online sync must never overwrite a newer local aircraft state silently.

Broader career progression, shared-world multiplayer, full ATC, photogrammetry and additional factory regions can follow the core flight and editor release.

## First integration sequence

1. **Baseline captured:** High at 1080p on the available Renoir machine. Add per-pass GPU timings, p99 and thermally settled soak data to close M0; compare this baseline with the next build using the same route/camera script.
2. Fix the top measured frame-time problem and keep its replay as a regression gate. In parallel, package one native Windows build that can start the factory flight.
3. Land the first complete OpenSky menu-to-debrief flight on the shipped pack. Use it to lock the aircraft and scenery interfaces.
4. Build one non-Spain region through the current compiler with verified data sources. Wire editor output to an installed-region flight before polishing the map UI.
5. Use these two flight paths as the weekly review. Record a passing replay, a GPU capture, and the largest remaining release blocker for each.
