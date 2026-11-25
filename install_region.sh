#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$ROOT/.tools/pydeps:$ROOT${PYTHONPATH:+:$PYTHONPATH}"
cd "$ROOT"

# Fast plug-and-play Spanish region installer.
# Examples:
#   ./install_region.sh presets
#   ./install_region.sh list
#   ./install_region.sh catalonia-pyrenees catalonia-coast
#   ./install_region.sh madrid-central --quality medium
if [[ $# -eq 0 ]]; then
  exec python3 "$ROOT/builder/build_region.py" presets
fi
case "$1" in
  list|presets|info|remove|selftest)
    exec python3 "$ROOT/builder/build_region.py" "$@"
    ;;
  *)
    exec python3 "$ROOT/builder/build_region.py" install --quality fast --full "$@"
    ;;
esac
