# video-server

Portable C++17 video streaming subsystem focused on a stable producer-facing API (`register_stream`, `push_frame`, `push_access_unit`) with minimal-copy, pointer-based frame ingestion. A new bridge pipeline now converts raw producer frames into H264 while reusing the existing encoded-media/WebRTC delivery path.

## Implemented

- Core reusable interfaces and types (`IVideoServer`, `VideoFrameView`, `EncodedAccessUnitView`, stream config/info types).
- Core in-memory stream state management, latest-frame snapshots, latest encoded H264 access-unit snapshots, and stats updates.
- Modular output/display transform stage with runtime display mode configuration and rotation/mirroring support.
- WebRTC backend shell (`WebRtcVideoServer`) with lightweight HTTP/signaling control surface, a real encoded-media bridge path for H264 access units, and a first session-side H264 sender architecture.
- New raw-to-H264 bridge pipeline layer (`IRawVideoPipeline` / `RawVideoPipelineConfig`) that feeds encoded Annex-B access units into the existing `push_access_unit(...)` server path through an ffmpeg subprocess backend.
- Internal synthetic frame generator for server-side validation without external producers.
- CMake + vcpkg manifest + tests.

## Dependencies

Managed via vcpkg manifest (`vcpkg.json`):

- libdatachannel with the `srtp` feature enabled for media track support
- gtest
- libjuice
- openssl
- libsrtp
- spdlog

## Build and test with vcpkg (supported flow)

The supported build flow requires `VCPKG_ROOT` and explicitly uses the vcpkg CMake toolchain file.

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh

cd /workspace/video-server
export VCPKG_ROOT=/path/to/vcpkg
./build.sh
./test.sh
./test_pipeline.sh
```

`build.sh` configures with:

- `-DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"`
- `-DENABLE_VIDEO_SERVER=ON`
- `-DENABLE_WEBRTC_BACKEND=ON`
- `-DBUILD_TESTING=ON`

It then builds via `cmake --build build -j`.

The WebRTC backend requires a **media-enabled** libdatachannel build. In vcpkg terms that means
`libdatachannel[srtp]`; the plain default `libdatachannel[ws]` build disables media support and will
fail later when the server first tries to send RTP on a media track.

`test.sh` runs the base GoogleTest suite via `build/video_server_tests --gtest_color=yes`, which prints each default test case as it runs, automatically calls `./build.sh` if the build directory is missing, and explicitly reports whether ffmpeg-backed raw-to-H264 integration coverage is available in the current environment.

`test_pipeline.sh` runs the intentionally disabled future pipeline test via `--gtest_also_run_disabled_tests --gtest_filter=WebRtcPipelineTest.DISABLED_EndToEndCurrentPath` so it stays visible and runnable without affecting the default `./test.sh` result.

The raw-to-H264 integration tests require an `ffmpeg` executable. By default the suite probes `ffmpeg` on `PATH`; set `VIDEO_SERVER_TEST_FFMPEG=/path/to/ffmpeg` if you need to point the tests at a non-default binary. When ffmpeg is unavailable, the affected tests report an explicit `GTEST_SKIP` reason instead of failing silently.

If you intentionally want a no-backend build, configure manually with `-DENABLE_WEBRTC_BACKEND=OFF`.

## HTTP API surface

- `GET /api/video/streams`
- `GET /api/video/streams/{stream_id}`
- `GET /api/video/streams/{stream_id}/output`
- `GET /api/video/streams/{stream_id}/frame` (latest transformed frame as `image/x-portable-pixmap` PPM)
- `PUT /api/video/streams/{stream_id}/output`
- `POST /api/video/signaling/{stream_id}/offer`
- `POST /api/video/signaling/{stream_id}/answer`
- `POST /api/video/signaling/{stream_id}/candidate`
- `GET /api/video/signaling/{stream_id}/session`

More detail: `docs/video_server.md`.

## NiceGUI smoke harness

A manual NiceGUI smoke harness lives in `examples/nicegui_smoke/`. It keeps UI code isolated from the server core while exercising the current intended browser path: synthetic content -> H264 access units -> WebRTC signaling -> browser `<video>` playback.

Quick start:

```bash
./build.sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt
python examples/nicegui_smoke/app.py --start-server
```

Then open `http://127.0.0.1:8090/`.

## Raw-to-H264 bridge pipeline

The repo now includes a first-pass `IRawVideoPipeline` abstraction in `include/video_server/raw_video_pipeline.h`. It is intentionally separate from `IVideoServer` and the core stream state so the architecture stays split into:

- raw frame ingestion (`push_frame`)
- optional raw filter/transform policy (`RawVideoPipelineConfig`)
- raw-to-H264 encoding (`ffmpeg` subprocess backend)
- existing encoded access-unit ingestion (`push_access_unit`)
- existing WebRTC/browser delivery

Current first-pass pipeline capabilities:

- input raw frame acceptance for tightly packed `RGB24`, `BGR24`, `RGBA32`, `BGRA32`, `GRAY8`, `NV12`, and `I420` (non-tightly-packed raw strides are currently rejected)
- optional passthrough or resize scaling
- optional output FPS throttling through ffmpeg filters
- automatic pixel-format conversion to encoder-friendly `yuv420p`
- H264 Annex-B output parsing into access units and forwarding through the existing encoded path
- explicit validation that AUD NAL delimiters remain enabled for the first-pass backend; `emit_access_unit_delimiters=false` is currently rejected rather than guessed at
- explicit stream binding via `make_raw_to_h264_pipeline_for_server(stream_id, ..., server)`

The NiceGUI smoke server now uses this shared pipeline instead of a one-off ffmpeg parsing loop, so synthetic raw frames exercise the full path from raw production to browser-facing H264 delivery.

Current first-pass backend limits to keep in mind:

- tightly packed raw frames are required on input
- encoded access-unit timestamps currently reuse the most recently written raw-frame timestamp observed before ffmpeg emits the next split access unit
- sink rejection is treated as a hard pipeline failure and is surfaced on later `push_frame()` calls

What still remains for future hardening:

- richer encoder health/error reporting from stderr
- a correct non-AUD access-unit splitter if the backend later needs to support `emit_access_unit_delimiters=false`
- more robust timestamp/frame-to-access-unit correlation under heavy buffering
- support for non-tightly-packed raw frame strides and more advanced filters
- alternative encoder backends beyond the initial ffmpeg subprocess implementation
