#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

if [[ ! -x build/video_server_nicegui_smoke_server ]]; then
  echo "Smoke server binary missing; running ./build.sh"
  ./build.sh
fi

# python -m venv .venv
# source .venv/bin/activate
# pip install -r examples/nicegui_smoke/requirements.txt
python examples/nicegui_smoke/app.py --start-server "$@"
