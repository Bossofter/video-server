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

BUILD_MODE="${1:-static}"
case "${BUILD_MODE}" in
  static)
    BUILD_DIR="build"
    CMAKE_BUILD_SHARED_LIBS="OFF"
    CMAKE_BUILD_TYPE_ARGS=()
    VCPKG_TRIPLET_ARGS=()
    ;;
  dynamic)
    BUILD_DIR="build-dynamic"
    CMAKE_BUILD_SHARED_LIBS="ON"
    CMAKE_BUILD_TYPE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
    VCPKG_TRIPLET_ARGS=(-DVCPKG_TARGET_TRIPLET=x64-linux-dynamic)
    ;;
  -h|--help|help)
    echo "usage: ./build.sh [static|dynamic]"
    echo "  static   default: build static libraries with vcpkg's default triplet"
    echo "  dynamic  build shared libraries with VCPKG_TARGET_TRIPLET=x64-linux-dynamic"
    exit 0
    ;;
  *)
    echo "error: unknown build mode: ${BUILD_MODE}"
    echo "usage: ./build.sh [static|dynamic]"
    exit 1
    ;;
esac

TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "error: vcpkg toolchain file not found: ${TOOLCHAIN_FILE}"
  exit 1
fi

INSTALL_DIR="${BUILD_DIR}/install"

cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  "${VCPKG_TRIPLET_ARGS[@]}" \
  "${CMAKE_BUILD_TYPE_ARGS[@]}" \
  -DBUILD_SHARED_LIBS="${CMAKE_BUILD_SHARED_LIBS}" \
  -DENABLE_VIDEO_SERVER=ON \
  -DENABLE_WEBRTC_BACKEND=ON \
  -DBUILD_TESTING=ON

cmake --build "${BUILD_DIR}" -j
cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
