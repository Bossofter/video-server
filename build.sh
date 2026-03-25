#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  if [[ -d "/opt/vcpkg" ]]; then
    export VCPKG_ROOT="/opt/vcpkg"
    echo "info: VCPKG_ROOT was unset; defaulting to ${VCPKG_ROOT}"
  else
    echo "error: VCPKG_ROOT is not set"
    echo "set it to your vcpkg checkout, e.g. export VCPKG_ROOT=/opt/vcpkg"
    exit 1
  fi
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
