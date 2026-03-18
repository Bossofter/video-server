#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
PIPELINE_FILTER="WebRtcPipelineTest.DISABLED_EndToEndCurrentPath"

if [[ ! -d "${BUILD_DIR}" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "build directory not found; running ./build.sh first"
  ./build.sh
fi

"${BUILD_DIR}/video_server_tests" \
  --gtest_color=yes \
  --gtest_also_run_disabled_tests \
  --gtest_filter="${PIPELINE_FILTER}"
