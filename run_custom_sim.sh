#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$ROOT/game/bin:${LD_LIBRARY_PATH:-}"
cd "$ROOT"
if [[ ! -x "$ROOT/sim_engine/OpenSky" ]]; then
  echo "OpenSky has not been built yet. Build it with: make -C sim_engine -j\$(nproc)" >&2
  exit 127
fi
exec "$ROOT/sim_engine/OpenSky" "$@"
