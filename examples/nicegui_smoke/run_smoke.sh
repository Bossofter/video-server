#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

# vcpkg's libva may not include the host distribution's DRI driver directory
# in its compiled search path. Prefer an explicit caller override, otherwise
# discover the common system locations used by Debian/Ubuntu and RPM systems.
if [[ -z "${LIBVA_DRIVERS_PATH:-}" ]]; then
  MULTIARCH="$(gcc -print-multiarch 2>/dev/null || true)"
  for DRIVER_DIR in "/usr/lib/${MULTIARCH}/dri" /usr/lib64/dri /usr/lib/dri; do
    if [[ -d "$DRIVER_DIR" ]]; then
      export LIBVA_DRIVERS_PATH="$DRIVER_DIR"
      break
    fi
  done
fi

if [[ ! -x build/video_server_nicegui_smoke_server ]]; then
  echo "Smoke server binary missing; running ./build.sh"
  ./build.sh
fi

SERVER_ARGS=()
HAS_STREAM_SELECTION=false
for ARG in "$@"; do
  case "$ARG" in
    --config|--stream|--stream-id|--multi-stream-demo)
      HAS_STREAM_SELECTION=true
      ;;
  esac
done
if [[ "$HAS_STREAM_SELECTION" == false ]]; then
  SERVER_ARGS+=(--multi-stream-demo)
fi

exec build/video_server_nicegui_smoke_server "${SERVER_ARGS[@]}" "$@"
