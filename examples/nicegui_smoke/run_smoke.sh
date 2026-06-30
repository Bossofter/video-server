#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

PRODUCER_ONLY=0
ARGS=()
for arg in "$@"; do
  if [[ "$arg" == "--producer-only" ]]; then
    PRODUCER_ONLY=1
  else
    ARGS+=("$arg")
  fi
done

SMOKE_SERVER=""
for candidate in \
  build-dynamic/video_server_nicegui_smoke_server \
  build/video_server_nicegui_smoke_server; do
  if [[ -x "$candidate" ]]; then
    SMOKE_SERVER="$candidate"
    break
  fi
done

if [[ -z "$SMOKE_SERVER" ]]; then
  echo "Smoke server binary missing; running ./build.sh"
  ./build.sh
  SMOKE_SERVER="build/video_server_nicegui_smoke_server"
fi

if [[ "$PRODUCER_ONLY" -eq 1 ]]; then
  exec "$SMOKE_SERVER" "${ARGS[@]}"
fi

# python -m venv .venv
# source .venv/bin/activate
# pip install -r examples/nicegui_smoke/requirements.txt
python3 -m py_compile examples/nicegui_smoke/app.py
python3 -m ruff check examples/nicegui_smoke/app.py
python3 examples/nicegui_smoke/app.py --start-server "${ARGS[@]}"
