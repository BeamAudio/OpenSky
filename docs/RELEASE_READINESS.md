# Release Readiness

## Decision

The source can be prepared for commercial use under the repository's MIT license, but the current product is **not ready for a commercial release**. The unresolved items below affect redistributed content, live services, aircraft completeness, and the advertised performance target.

## Release blockers

1. **C172S model rights chain.** `THIRD_PARTY_LICENSES.md` records the exterior model's original Sketchfab attribution, but the intermediate AUREL model and embedded texture rights are not evidenced. The cockpit GLB and its source also lack recorded provenance and redistribution terms. Obtain and archive permission covering the exact distributed files, or replace them with verified assets.
2. **ENAIRE AIP source.** `builder/build_region.py` currently records ENAIRE AIP among airport sources, but no reuse license/permission is recorded. Identify exactly which fields came from AIP, document the applicable terms, and remove or replace data that cannot be redistributed commercially.
3. **Open-Meteo service terms.** The weather data license and API service terms are distinct. The current free API service is non-commercial; use a plan that permits the intended commercial use or disable the live feed in a commercial build. Keep source attribution visible.
4. **Performance and platform scope.** The target is 60 FPS on Radeon Renoir, but the dated engine audit records High-setting scenarios below target. Keep the product's measured performance statement narrow until current comparable 1080p captures meet the target with p95 frame time reported. The current supported runtime is Linux/X11/GLX. Windows 11 is not supported: the game has no Win32/WGL window backend, and several systems still use POSIX APIs.
5. **Aircraft package completeness.** Format 1 makes the current single-piston/two-tank aircraft data and visuals package-based. It does not yet provide Lua scripts, multi-engine aircraft, jets/turboprops, or per-aircraft avionics. Do not advertise a complete general-purpose aircraft mod SDK until the script runtime and broader system API are implemented.

## Release preparation checklist

- Build the player-facing Debian/Ubuntu packages from an Ubuntu 22.04 release environment with `python3 tools/package_deb.py --version VERSION --maintainer 'Name <email>' --content-approved`. It creates the app + Aragón package and an `Architecture: all` Catalonia package containing both coast and Pyrenees regions. Players install both packages and launch the app from the Games menu; the packaging helper is for release maintainers only. The player build omits the developer Region Tools button; users can still add regions in their user data folder, while the editor remains part of the source checkout. The builder refuses any individual artifact at or above GitHub's [2 GiB release-asset limit](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases). Verify clean installs on the exact Debian and Ubuntu versions claimed in the release notes.
- The game stores configuration in `~/.config/opensky-vfr`, user aircraft in `~/.config/opensky-vfr/aircraft`, user regions in `~/.local/share/opensky-vfr/packs`, and downloaded tiles in `~/.cache/opensky-vfr`.
- Do not publish a Windows 11 installer until native Win32/WGL window/input support and replacements for POSIX filesystem, socket, and audio integrations are implemented and packaged.
- Keep this repository's MIT license and include separate notices for JSBSim, Lua if it is added, fonts, assets, and every regional data source.
- Build and package `libJSBSim.so.1` dynamically; ship the LGPL license and relinking information as required by the distribution model.
- Ship only redistributable scenery and aircraft content. Offer OSM-derived distributed data under ODbL and include its attribution.
- Configure the commercial weather API plan or ship the live feed disabled.
- Test a clean install, first launch, offline launch, region discovery, aircraft discovery, save upgrade, and removal/reinstall paths.
- Capture visual and performance evidence for the exact release build, quality settings, GPU, region residency, mean FPS, and p95 frame time.
- Publish supported platform, controls, known limitations, accessibility notes, and mod installation instructions.

Review this document against the exact release artifact; upstream service terms and external asset permissions can change.
