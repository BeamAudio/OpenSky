# Contributing to OpenSky

Thanks for helping improve OpenSky VFR. Read [the modding guide](docs/MODDING.md) before preparing an aircraft or region package.

## Code changes

- Keep the runtime on C++20, OpenGL 4.6, Linux/X11/GLX until the platform plan changes.
- Preserve the default C172S, saved-base ownership loop, v1-v3 fleet profile readers, and the 120 Hz JSBSim step.
- Keep generated scenery packs, downloaded data, caches, captures, and temporary bakes outside tracked source.
- Keep JSBSim dynamically linked and retain its LGPL notices.
- Include a clear attribution and license trail for any new code, model, texture, sound, or data.
- Do not commit secrets, credentials, local profile files, generated packs, or unlicensed imagery.

## Build

See [Installation](README.md#installation) for required build tools, X11/OpenGL/ALSA libraries, and Python scenery-builder dependencies, then run from the repository root:

```bash
make -C sim_engine -j"$(nproc)"
./run_sim.sh
```

Use `CMAKE=/path/to/cmake` if CMake is not on `PATH`. Describe behavior changes and the checks you ran in the pull request. Visual changes should include captures and comparable performance measurements when available.

By submitting a code contribution, you agree that it may be distributed under the repository's MIT License. Contributions that include third-party material must identify its source and license.
