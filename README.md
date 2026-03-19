# video-server

Portable C++17 video streaming subsystem focused on a stable producer-facing API (`register_stream`, `push_frame`, `push_access_unit`) with minimal-copy, pointer-based frame ingestion.

## Implemented

- Core reusable interfaces and types (`IVideoServer`, `VideoFrameView`, `EncodedAccessUnitView`, stream config/info types).
- Core in-memory stream state management, latest-frame snapshots, latest encoded H264 access-unit snapshots, and stats updates.
- Modular output/display transform stage with runtime display mode configuration and rotation/mirroring support.
- WebRTC backend shell (`WebRtcVideoServer`) with lightweight HTTP/signaling control surface, a real encoded-media bridge path for H264 access units, and a first session-side H264 sender architecture.
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
