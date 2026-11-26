# Changelog

This file records user-visible changes. No GitHub release version has been assigned yet.

## Unreleased

- Added folder-based aircraft packages with a manifest, package-local JSBSim data, exterior/cockpit GLB assets, configurable two-tank capacities, oil capacity, flap travel, electrical bus, and GLTF node mappings.
- Added aircraft discovery with `--list-aircraft`, selection with `--aircraft <id>`, and fleet profile v4 persistence while retaining v1-v3 readers.
- Generalized the shared light-piston systems class while preserving the factory C172S profile and saves.
- Updated the start menu, aircraft card, and Credits page for package-based aircraft and open-source attribution.
- Prepared Debian/Ubuntu desktop and region packages for Aragón and both Catalonia regions, with an app-menu icon, portable CPU flags, and per-user mod/cache paths.
- Added modding and contribution guides and an open-source release checklist. Removed the obsolete delivery plan.

### Current limitations

- Aircraft format 1 supports one piston engine, two fuel tanks, and the shared light-piston systems profile. Multi-engine, turbine, aircraft-specific systems, and Lua behavior are not implemented.
- The current Renoir benchmark does not meet the 60 FPS average and p95 frame-time targets.
- C172 model attribution is documented; the region builder uses OurAirports and OpenStreetMap airport data and does not import ENAIRE AIP records. Open-Meteo remains an optional service with separate terms and can be disabled at launch. No Debian package has been generated, and baseline region packs are not in the source checkout. See the [GitHub publication status](docs/OPEN_SOURCE_RELEASE.md).
- Windows 11 is not supported until the platform APIs are ported. See the [open-source release checklist](docs/OPEN_SOURCE_RELEASE.md).
