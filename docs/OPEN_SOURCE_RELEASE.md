# GitHub publication status

## Source repository

**Ready for public source hosting.** OpenSky's project code and original assets are under the repository MIT license. JSBSim remains dynamically linked under LGPL-2.1-or-later, and the third-party notices identify the external model, font, data and library terms. The repository is already public at [BeamAudio/OpenSky](https://github.com/BeamAudio/OpenSky).

The cockpit and the architecture/vegetation models are generated from checked-in project source. The C172 exterior model retains its creator/source attribution, CC BY 4.0 license link and modification notice. Current airport-pack generation uses OurAirports and OpenStreetMap; ENAIRE AIP is reference material only and is not imported. The README credits IGN for PNOA imagery in promotional captures. See [third-party notices](../THIRD_PARTY_LICENSES.md).

## Player-facing game release

**Not ready as a complete desktop release.** This does not block publishing the source repository.

- The source checkout does not contain the generated Aragón and Catalunya region packs. A clean clone therefore needs region data built or installed before the game can show its baseline world.
- Linux/X11/GLX is the only supported runtime. Windows 11 has no native backend yet.
- No Debian/Ubuntu installer or Windows installer has been generated and checked from a clean install.
- The release build still needs reproducible first-launch, offline, region discovery, save upgrade and uninstall/reinstall verification.
- Do not advertise a performance target until the exact packaged build has mean FPS and p95 frame time evidence on the named hardware.

## Runtime services and data

Open-Meteo is an optional live boundary input. Its data attribution appears in-game; its data license and API service terms are separate. Use `./run_sim.sh --no-live-weather` to disable live weather. Online PNOA imagery can be disabled separately with Settings or `./run_sim.sh --online-ortho off`.

Any distributed OSM-derived region pack must carry ODbL attribution and its share-alike terms. Keep the required IGN attribution with PNOA and elevation data. Recheck provider terms against the actual release contents.
