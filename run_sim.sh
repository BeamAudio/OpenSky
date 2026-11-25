#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Stable gameplay entry point used by the editor, QA captures and desktop builds.
# Keep -- as an optional separator for scripts that wrap another launcher.
if [[ "${1:-}" == "--" ]]; then
  shift
fi
exec "$ROOT/run_custom_sim.sh" "$@"
