#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${1:-}"
if [[ -z "${BUILD_DIR}" ]]; then
  if [[ -x "${REPO_DIR}/build-dynamic/video_server_vcpkg_example" ]]; then
    BUILD_DIR="${REPO_DIR}/build-dynamic"
  else
    BUILD_DIR="${REPO_DIR}/build"
  fi
elif [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${REPO_DIR}/${BUILD_DIR}"
fi

EXE="${BUILD_DIR}/video_server_vcpkg_example"
if [[ ! -x "${EXE}" ]]; then
  echo "error: example executable not found: ${EXE}"
  echo "run ./build.sh or ./build.sh dynamic first"
  exit 1
fi

LIB_PATH="${BUILD_DIR}"
if [[ -d "${BUILD_DIR}/vcpkg_installed/x64-linux/lib" ]]; then
  LIB_PATH="${LIB_PATH}:${BUILD_DIR}/vcpkg_installed/x64-linux/lib"
fi
if [[ -d "${BUILD_DIR}/vcpkg_installed/x64-linux-dynamic/lib" ]]; then
  LIB_PATH="${LIB_PATH}:${BUILD_DIR}/vcpkg_installed/x64-linux-dynamic/lib"
fi

if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
  export LD_LIBRARY_PATH="${LIB_PATH}:${LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="${LIB_PATH}"
fi

exec "${EXE}"
