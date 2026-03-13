#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  echo "error: VCPKG_ROOT is not set"
  echo "hint: export VCPKG_ROOT=/path/to/vcpkg"
  exit 1
fi

TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "error: vcpkg toolchain file not found: ${TOOLCHAIN_FILE}"
  exit 1
fi

BUILD_DIR="build"

cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DENABLE_VIDEO_SERVER=ON \
  -DENABLE_WEBRTC_BACKEND=ON \
  -DBUILD_TESTING=ON

cmake --build "${BUILD_DIR}" -j
