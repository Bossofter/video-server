# video-server

Portable C++17 video streaming subsystem focused on a stable producer-facing API (`register_stream`, `push_frame`) with minimal-copy, pointer-based frame ingestion.

## Implemented

- Core reusable interfaces and types (`IVideoServer`, `VideoFrameView`, `EncodedAccessUnitView`, stream config/info types).
- Core in-memory stream state management and stats updates.
- Modular output/display transform stage with runtime display mode configuration and rotation/mirroring support.
- WebRTC backend shell (`WebRtcVideoServer`) with lightweight HTTP/signaling control surface.
- Internal synthetic frame generator for server-side validation without external producers.
- CMake + vcpkg manifest + tests.

## Dependencies

Managed via vcpkg manifest (`vcpkg.json`):

- libdatachannel
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
```

`build.sh` configures with:

- `-DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"`
- `-DENABLE_VIDEO_SERVER=ON`
- `-DENABLE_WEBRTC_BACKEND=ON`
- `-DBUILD_TESTING=ON`

It then builds via `cmake --build build -j`.

`test.sh` runs `ctest --test-dir build --output-on-failure`, and automatically calls `./build.sh` if the build directory is missing.

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
