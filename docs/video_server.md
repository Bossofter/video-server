# Video Server Architecture

## Overview

This subsystem is split into portable core APIs and optional backend-specific transport layers.

- **Producer-facing API (stable):** register stream, push frame pointer, push encoded access unit.
- **Core state model:** stream metadata, runtime output config, stats, and latest transformed frame storage.
- **Transform stage:** display/output mapping independent of source pixel format.
- **WebRTC backend:** transport/signaling integration point for browser-native LAN delivery.
- **HTTP control surface:** stream inspection, output config, and signaling endpoints.
- **Synthetic generator:** built-in server-side fake frame source for development and validation.

## Core types

- `VideoPixelFormat`: source input format (`RGB24`, `BGR24`, `RGBA32`, `BGRA32`, `NV12`, `I420`, `GRAY8`).
- `VideoCodec`: encoded input type (`H264`).
- `VideoDisplayMode`: output transform (`Passthrough`, `Grayscale`, `WhiteHot`, `BlackHot`, `Rainbow`).
- `VideoFrameView`: non-owning raw frame view (`const void*` + dimensions/stride/timestamps/id).
- `EncodedAccessUnitView`: non-owning encoded AU view with codec + keyframe/config flags.
- `StreamConfig`: producer-facing stream registration settings.
- `StreamOutputConfig`: runtime output/display configuration.
- `VideoStreamInfo`: status + counters + current configs.

## Why non-owning frame views

The producer API uses non-owning pointer views so existing camera or pipeline buffers can be passed with minimal copies. This keeps the producer side stable even as server internals evolve (software transforms, hardware encode, direct encoded input).

## Pixel format vs display mode

`VideoPixelFormat` is an input/source representation detail.
`VideoDisplayMode` is a presentation/output transform policy.
They are intentionally separate, so runtime output choices like `WhiteHot` do not alter producer input contracts.

## Internal real frame pipeline

`push_frame()` now performs real ingestion and transform work in the core:

1. validate stream id and basic frame shape (`data`, dimensions, format, stride)
2. validate frame dimensions + source pixel format against per-stream `StreamConfig`
3. apply `StreamOutputConfig` transforms (`display_mode`, mirroring, rotation, palette range)
4. write transformed output to a per-stream owning latest-frame buffer
5. update stream counters and timestamps

Each stream stores an internal latest frame object that contains:

- transformed frame bytes (owning `std::vector<uint8_t>`)
- transformed width/height
- transformed pixel format
- timestamp and frame id
- validity flag

The stored internal output format is **packed `RGB24`**.

## Runtime output config behavior

Changing `StreamOutputConfig` via HTTP or direct API immediately affects the next `push_frame()` for that stream. Re-registration is not required.

## Stats and stream activity

Runtime stream info tracks practical counters and markers:

- `frames_received`, `frames_transformed`, `frames_dropped`
- `access_units_received`
- `last_input_timestamp_ns`, `last_output_timestamp_ns`
- `last_frame_id`
- `has_latest_frame`

These fields are intended for lightweight operational visibility and backend readiness.

## Backend-ready latest-frame access

The core exposes an internal getter (`get_latest_frame_for_stream`) that returns an immutable `std::shared_ptr<const LatestFrame>` snapshot when available. This avoids copying full frame buffers on read while giving clear lifetime semantics for backend consumers (older snapshots remain valid after newer frames are published). This keeps transport/backend concerns separate while preparing the next step where WebRTC delivery consumes the latest transformed frame.

## WebRTC backend notes

`WebRtcVideoServer` implements `IVideoServer` and composes:

- core stream/state manager
- transform stage
- signaling state holder
- HTTP API host

The current implementation is intentionally lightweight and LAN-oriented, with explicit extension points for full libdatachannel peer connection wiring and encoded frame transport.

## HTTP API

Implemented endpoints:

- `GET /api/video/streams`
- `GET /api/video/streams/{stream_id}`
- `GET /api/video/streams/{stream_id}/output`
- `PUT /api/video/streams/{stream_id}/output`
- `POST /api/video/signaling/{stream_id}/offer`
- `POST /api/video/signaling/{stream_id}/answer`
- `POST /api/video/signaling/{stream_id}/candidate`
- `GET /api/video/signaling/{stream_id}/session`

Output-config JSON fields:
`display_mode`, `mirrored`, `rotation_degrees`, `palette_min`, `palette_max`.

## LAN-only assumptions

This subsystem currently assumes peers on the same local network and keeps signaling/control intentionally simple.

## Future encoded H.264 path

`push_access_unit` and `EncodedAccessUnitView` are already part of the core interface, so producers can later provide direct H.264 access units without breaking API shape.

The internal latest-frame storage introduced for raw frame ingestion does not block encoded ingestion. Encoded path work can be added in parallel and later integrated with transport selection policy.

## Synthetic generation path

`SyntheticFrameGenerator` emits dynamic test patterns at caller-controlled cadence. It allows validating registration, ingestion, transforms, and stats without external sensors or middleware.

## Dependency rationale

- `libdatachannel`: browser-focused WebRTC integration.
- `libjuice`: ICE for NAT traversal / local peer connectivity.
- `openssl`: TLS/DTLS foundation.
- `libsrtp`: SRTP media security.
- `spdlog`: small, portable logging.

Dependencies are kept minimal and explicit to simplify long-term portability work (including non-Linux targets).

## Build

Use vcpkg manifest mode and CMake options:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DENABLE_VIDEO_SERVER=ON \
  -DENABLE_WEBRTC_BACKEND=ON \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```
