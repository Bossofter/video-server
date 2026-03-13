#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

if [[ ! -d "${BUILD_DIR}" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "build directory not found; running ./build.sh first"
  ./build.sh
fi

ctest --test-dir "${BUILD_DIR}" --output-on-failure
