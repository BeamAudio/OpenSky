#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$ROOT/.tools/pydeps:$ROOT${PYTHONPATH:+:$PYTHONPATH}"
cd "$ROOT"
exec python3 "$ROOT/builder/build_region_gui.py" "$@"
