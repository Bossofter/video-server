# video-server

Portable C++17 video streaming subsystem focused on a stable producer-facing API (`register_stream`, `push_frame`, `push_access_unit`) with minimal-copy, pointer-based frame ingestion. A new bridge pipeline now converts raw producer frames into H264 while reusing the existing encoded-media/WebRTC delivery path.

## Implemented

- Core reusable interfaces and types (`IVideoServer`, `VideoFrameView`, `EncodedAccessUnitView`, stream config/info types).
- Core in-memory stream state management, latest-frame snapshots, latest encoded H264 access-unit snapshots, and stats updates.
- Modular output/display transform stage with runtime display mode configuration and rotation/mirroring support.
- WebRTC backend shell (`WebRtcVideoServer`) with lightweight HTTP/signaling control surface, a real encoded-media bridge path for H264 access units, and a first session-side H264 sender architecture.
- New raw-to-H264 bridge pipeline layer (`IRawVideoPipeline` / `RawVideoPipelineConfig`) that feeds encoded Annex-B access units into the existing `push_access_unit(...)` server path through an explicit in-process H264 encoder backend seam with the current libav implementation behind it.
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
- ffmpeg/libav (libavcodec + libavutil + libswscale via the vcpkg `ffmpeg` package)

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

`test.sh` runs the base GoogleTest suite via `build/video_server_tests --gtest_color=yes`, which prints each default test case as it runs and automatically calls `./build.sh` if the build directory is missing.

`test_pipeline.sh` runs the intentionally disabled future pipeline test via `--gtest_also_run_disabled_tests --gtest_filter=WebRtcPipelineTest.DISABLED_EndToEndCurrentPath` so it stays visible and runnable without affecting the default `./test.sh` result.


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

## Security hardening defaults

- HTTP still binds to `127.0.0.1` by default.
- `GET /api/video/debug/stats` is now disabled by default; enable it explicitly with `WebRtcVideoServerConfig.enable_debug_api = true`.
- Runtime config and signaling routes remain available on loopback by default, but sensitive routes are blocked on non-loopback binds unless you explicitly opt into `allow_unsafe_public_routes` or enable a shared key and/or IP allowlist.
- Shared-key protection accepts either `Authorization: Bearer <key>` or `X-Video-Server-Key: <key>`.
- CORS is no longer wildcard by default. Loopback origins are allowed automatically for loopback binds, and non-loopback deployments should set `cors_allowed_origins` explicitly.

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

To exercise a protected server from the harness, pass `--shared-key your-token`. The harness forwards that token on signaling, config, and debug requests, and when `--start-server` is used it also launches the smoke server with the same shared key.

## Raw-to-H264 bridge pipeline

The repo now includes a first-pass `IRawVideoPipeline` abstraction in `include/video_server/raw_video_pipeline.h`. It is intentionally separate from `IVideoServer` and the core stream state so the architecture stays split into:

- raw frame ingestion (`push_frame`)
- optional raw filter/transform policy (`RawVideoPipelineConfig`)
- raw-to-H264 encoding (current libav H264 backend behind an internal encoder seam)
- existing encoded access-unit ingestion (`push_access_unit`)
- existing WebRTC/browser delivery

Current first-pass pipeline capabilities:

- input raw frame acceptance for tightly packed `RGB24`, `BGR24`, `RGBA32`, `BGRA32`, `GRAY8`, `NV12`, and `I420` (non-tightly-packed raw strides are currently rejected)
- optional passthrough or resize scaling
- optional output FPS throttling inside the libav-backed pipeline
- automatic pixel-format conversion and scaling to encoder-friendly `yuv420p` using libswscale
- H264 Annex-B access-unit emission per encoded packet and forwarding through the existing encoded path
- explicit H264 encoder selection (`Automatic`, `LibX264`, or `LibOpenH264`)
- optional AUD/repeat-header encoder knobs for libx264-family encoders, with Annex-B access units normalized through a libav bitstream filter before handoff
- explicit stream binding via `make_raw_to_h264_pipeline_for_server(stream_id, ..., server)`

The NiceGUI smoke server now uses this shared in-process libav pipeline, so synthetic raw frames exercise the full path from raw production to browser-facing H264 delivery.

Current first-pass backend limits to keep in mind:

- tightly packed raw frames are required on input
- encoded access-unit timestamps now come from encoder packet PTS values derived from the raw-frame timestamps that were admitted to the pipeline
- sink rejection is treated as a hard pipeline failure and is surfaced on later `push_frame()` calls

What still remains for future hardening:

- broader stride support for non-tightly-packed raw frame inputs and more advanced filters
- richer encoder capability negotiation and observability for additional future encoder backends
- stronger long-run timestamp/latency characterization under heavy buffering
- alternative encoder backends beyond the current libav implementation
