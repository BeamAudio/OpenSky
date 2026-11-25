# Modding OpenSky

OpenSky mods are installed content folders. The runtime currently supports two content types: aircraft packages and regional scenery packs. Both can be built without editing the C++ source, but the aircraft systems API is not scriptable yet.

Keep generated packs, downloaded data, caches and temporary bakes outside tracked source. Include attribution and redistribution rights for all content in a mod.

## Aircraft packages

### Install or select an aircraft

The simulator scans these locations:

- `game/aircraft/<id>/` for content shipped with the game or a developer checkout.
- `~/.config/opensky-vfr/aircraft/<id>/` for a user-installed package on Linux.

List packages and start one explicitly:

```bash
./run_sim.sh --list-aircraft
./run_sim.sh --aircraft sample-aircraft
```

Interactive selection is saved in the user's fleet profile. If a saved package is missing, select the factory aircraft with `--aircraft c172s`.

### Package layout

```text
sample-aircraft/
├── aircraft.ini
├── flight/
│   ├── flight.xml
│   └── engine/
│       ├── sample-engine.xml
│       └── sample-propeller.xml
└── models/
    ├── exterior.glb
    └── cockpit.glb
```

`flight/` is the JSBSim model directory. Its main XML file has the same base name as the directory. Add every engine, propeller and other XML dependency referenced from that file. The loader requires a piston engine with a propeller, two fuel tanks and at least three gear units.

The manifest is strict and uses paths relative to the package. Start from the working [C172S manifest](../game/aircraft/c172s/aircraft.ini):

```ini
format = opensky-aircraft
version = 1
id = sample-aircraft
name = Sample Aircraft
manufacturer = Example Works
model = Trainer 100
registration = EC-ABC
systems_profile = single_piston_v1
flight_model_dir = flight
exterior_model = models/exterior.glb
cockpit_model = models/cockpit.glb
cockpit_eye_height_m = 0.62
max_flaps_deg = 25
left_tank_capacity_gal = 18
right_tank_capacity_gal = 18
oil_capacity_quarts = 6
bus_voltage = 14
node.exterior_Flap_L = LeftFlap
node.exterior_Flap_R = RightFlap
```

Keep only supported manifest keys. The package loader rejects absolute paths, path traversal, missing files, duplicate IDs and unknown keys. `@shared/...` resolves against shared game assets and is intended for built-in content; community packages should include the assets they need.

### Models and cockpit nodes

Export exterior and cockpit models as GLB with textures embedded. The runtime animates recognized canonical nodes for control surfaces, flaps, doors and aircraft parts. Map a canonical role to your GLTF node with `node.exterior_<CanonicalName>` or `node.cockpit_<CanonicalName>` in the manifest. Unmapped model nodes remain static. The shared systems profile still owns core instruments and systems behavior.

Test the manifest and package loader with `--list-aircraft`, then launch the aircraft with a fresh or isolated profile. Check that the startup model loads, control nodes animate, the engine can be started, and the package can taxi, take off, land and park. Do not test with your only saved profile if the package ID differs; saves retain the selected aircraft ID.

### Aircraft system scripting status

Format 1 packages do **not** run aircraft-specific scripts. Uploading a 3D model and Lua/JavaScript files will not make a new system profile work. The supported package uses the shared `single_piston_v1` behavior and one piston engine. Twins, turboprops, jets, retractable-gear procedures and package-specific electrical/avionics logic are outside this format.

The planned modding direction is a small embedded script API with read-only flight/system snapshots and bounded outputs. Lua is a candidate because it is familiar and lightweight. A safe API, execution budget, persistence rules and runtime error handling must be implemented and reviewed before scripts are accepted. Until then, do not add `systems.lua` or `cockpit.lua` to a format 1 package expecting them to run.

## Regional content

The full game release is intended to contain both Aragón and Catalunya as baseline scenery. Compatible user regions remain add-ons and do not replace that baseline.

Build a region using the World Editor or `builder/build_region.py`. Run the editor from the repository root:

```bash
./run_builder.sh
```

Or use a supported builder preset:

```bash
python3 builder/build_region.py --preset zaragoza-pyrenees-quick
```

For a custom region, provide licensed XYZ imagery and Terrarium elevation tiles, or choose an elevation-only build. Spanish presets use the approved IGN/OurAirports/OSM sources. OSM roads and settlements outside the bundled Spain extracts need a local `.pbf` extract. Keep source attribution in the pack manifest and preserve ODbL share-alike obligations for OSM-derived output.

The runtime loads `.nspack` and optional companions such as `.relief.png`, `_cities.nscity`, `_city_sh.nsshd`, `_scenery.nsscn` and `.nswx`. Keep companions beside the pack when sharing it. Install user region packs under `~/.local/share/opensky-vfr/packs/`; the application also loads its shipped data directory. A user pack with the same filename is considered before the shipped copy.

## Redistribute a mod

Publish the complete package folder, installation instructions, license/attribution file and a version number. State which game/package format the mod uses. Keep third-party licenses with the assets. Do not include Esri imagery, GPL aircraft models/configurations/textures, or any content without clear redistribution permission. See [third-party licenses](../THIRD_PARTY_LICENSES.md) before sharing.

## Current limits

- Aircraft packages use `single_piston_v1`; they are not a general aircraft-systems scripting environment.
- The runtime requires a JSBSim-compatible flight model and the minimum aircraft hardware listed above.
- Region packs must match the versioned shared protocol; the builder and C++ reader are separate programs.
- Mods may replace or add content, but they cannot change the player-owned aircraft, saved-base or no-relocation rules without changing the game source.
- The active supported native runtime is Linux/X11/GLX. Windows 11 package support is not available yet.
