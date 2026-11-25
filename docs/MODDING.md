# Community Modding

OpenSky content is installed as folders. The loader currently supports regional packs and aircraft packages. Keep downloaded data, generated packs, and caches outside the tracked source tree.

## Aircraft packages (format 1)

Create one directory per aircraft under either:

- `game/aircraft/<id>/` for a package shipped with a checkout or release.
- `~/.config/opensky-vfr/aircraft/<id>/` for a user-installed package on Linux.

The directory must contain `aircraft.ini`, a JSBSim model directory, an exterior GLB, and a cockpit GLB. The built-in [C172S manifest](../game/aircraft/c172s/aircraft.ini) is the working example.

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

The example package also needs `flight/flight.xml`, `flight/engine/`, and any other JSBSim files referenced by the aircraft model. The model directory's XML file must have the same base name as the directory. Exterior and cockpit files must be GLB files with their textures embedded. Paths are relative to the package. `@shared/...` resolves against the shared game asset tree and is intended for built-in packages; user aircraft should include their own assets.

The package loader rejects absolute paths, path traversal, missing files, duplicate IDs, and unknown manifest keys. IDs may contain letters, digits, `_`, and `-`. `--list-aircraft` lists discovered packages. Start with `./run_sim.sh --aircraft sample-aircraft`; an interactive selection is stored in the fleet profile. A missing saved package is reported at startup; use `--aircraft c172s` to select the factory plane again.

### Format 1 systems support

The flight adapter currently requires one piston engine with a propeller, two fuel tanks, and at least three JSBSim gear units. `single_piston_v1` configures fuel capacity, oil capacity, maximum flap travel, and a 14V or 28V electrical bus. The gameplay systems use a shared light-aircraft piston profile. This does not yet cover twins, turboprops, jets, retractable-gear procedures, or aircraft-specific electrical and avionics logic.

GLTF control and cockpit nodes use the built-in canonical node names by default. A `node.exterior_<CanonicalName>` or `node.cockpit_<CanonicalName>` manifest entry maps one canonical role to a package's GLTF node. Unmapped nodes remain static. Core cockpit instruments and systems behavior still use the shared profile.

## Scripted aircraft systems: next milestone

Aircraft-specific scripts are not executed by format 1. The recommended direction is embedded Lua rather than a new language: it is familiar to modders, small to ship, and available under the MIT license with an attribution notice. See the [Lua 5.5.1 reference manual](https://www.lua.org/manual/5.5/) and [license](https://www.lua.org/license.html).

The proposed package adds `systems.lua` and `cockpit.lua`. Scripts should receive read-only snapshots and return bounded values through a documented API; they must not access files, network, native libraries, or the JSBSim executive. Run them at the fixed simulation step with per-call instruction and memory budgets. Invalid output should be clamped or rejected, and repeated script errors should disable the package script without stopping the simulator. Keep JavaScript-free and Lua-disabled packages loadable during migration.

This scripting runtime and its API need implementation and review before authors can rely on those files. Do not add `systems.lua` or `cockpit.lua` to a format 1 manifest and expect the game to load them.

## Regional content

Use the World Editor or `builder/build_region.py` to create a region pack. On Linux, install downloaded `.nspack` files and their companion files in `~/.local/share/opensky-vfr/packs/`; the game also loads the release's built-in `packs/` directory. A user pack with the same filename is loaded first, allowing it to replace a shipped region. Keep each pack's `.relief.png`, `_cities.nscity`, `_city_sh.nsshd`, `_scenery.nsscn`, and `_weather.nswx` companions together when present.

Spanish presets provide the approved IGN/OurAirports/OSM pipeline. For another region, provide licensed XYZ imagery and Terrarium elevation, or build elevation-only scenery. Add source and attribution metadata to the pack, and retain any ODbL share-alike obligations for OSM-derived data.

## Sharing a mod

Include the complete package folder, a short installation/readme note, and a license/attribution file. You must have redistribution rights for every model, texture, sound, data set, and script you include. Do not include Esri imagery or GPL aircraft content in an OpenSky-compatible package.
