#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

if [[ ! -d "${BUILD_DIR}" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "build directory not found; running ./build.sh first"
  ./build.sh
fi

FFMPEG_BIN="${VIDEO_SERVER_TEST_FFMPEG:-ffmpeg}"
if command -v "${FFMPEG_BIN}" >/dev/null 2>&1; then
  echo "[test.sh] ffmpeg integration tests enabled via ${FFMPEG_BIN}"
else
  echo "[test.sh] WARNING: ffmpeg integration tests unavailable; raw->H264 tests will report GTEST_SKIP."
  echo "[test.sh] Set VIDEO_SERVER_TEST_FFMPEG=/path/to/ffmpeg (or install ffmpeg on PATH) to enable them."
fi

"${BUILD_DIR}/video_server_tests" --gtest_color=yes
