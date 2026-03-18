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

The core exposes an internal getter (`get_latest_frame_for_stream`) that returns an immutable `std::shared_ptr<const LatestFrame>` snapshot when available. This avoids copying full frame buffers on read while giving clear lifetime semantics for backend consumers (older snapshots remain valid after newer frames are published). The HTTP frame endpoint uses this directly today, and the WebRTC backend now exposes an explicit media-source bridge abstraction that observes the same snapshot boundary for the upcoming real video sender path.

## WebRTC backend notes

`WebRtcVideoServer` implements `IVideoServer` and composes:

- core stream/state manager
- transform stage
- signaling/session manager
- HTTP API host
- per-session WebRTC media-source bridge state

### Current signaling/session architecture

The signaling endpoints now create and manage **real libdatachannel `rtc::PeerConnection` objects** per stream-scoped session.

Current mapping:

- `POST /api/video/signaling/{stream_id}/offer`
  - validates the stream exists
  - creates a new backend WebRTC session for that stream
  - applies the remote SDP offer to the backend peer connection
  - triggers generation of the backend local description (answer)
- `POST /api/video/signaling/{stream_id}/candidate`
  - forwards the remote ICE candidate into the matching peer connection
- `GET /api/video/signaling/{stream_id}/session`
  - exposes the current backend session snapshot, including answer SDP, latest candidates, peer state, and media-bridge snapshot metadata
- `POST /api/video/signaling/{stream_id}/answer`
  - is retained for API stability and supports the reverse direction if the backend later originates offers

A session is currently keyed by `stream_id`, and the signaling layer currently exposes a **single session slot per stream**. A new offer for the same stream replaces the previous slot and increments the reported `session_generation`. This keeps the current LAN-focused step explicit while making the present single-viewer limitation visible instead of implicit.

### Current media-source bridge state

This milestone keeps the backend media path honest: WebRTC **DataChannels are not used for video transport**. Instead, each active signaling session now owns an explicit media-source bridge abstraction that can observe temporary raw-frame snapshots today and future encoded H264 access units later.

For each active signaling session, the backend now owns:

- a real `rtc::PeerConnection`
- a media-source bridge object tied to the stream id
- raw-snapshot observation for current transformed-frame state
- encoded-access-unit observation for the future H.264 sender path
- bridge status (`media_bridge_state`, `preferred_media_path`, snapshot metadata, encoded access-unit metadata)

Current bridge behavior:

1. producer pushes raw frames through the unchanged producer API
2. core transforms and publishes a new immutable latest-frame snapshot
3. `push_frame()` forwards the latest immutable transformed frame snapshot into the bridge through `on_latest_frame(...)`
4. `push_access_unit()` forwards encoded access-unit metadata into the bridge through `on_encoded_access_unit(...)`
5. signaling/session inspection can report what raw or encoded source state is currently available to a future real sender path

The current bridge state is intentionally explicit:

- `awaiting-video-track-bridge` when only latest-frame snapshot state is available
- `awaiting-h264-video-track-bridge` once encoded H.264 access units have been observed

That makes the system’s status explicit: the backend now has real peer/session plumbing plus a real backend-side media source boundary, but it does **not** yet claim to deliver final video media over WebRTC.

This leaves the next step well-defined: attach a real encoded/RTP video sender implementation to the bridge rather than replacing another temporary transport.

### Threading and lifetime

- Session inspection queries the media-source bridge for latest snapshot metadata without mutating shared frame state.
- Replacing a stream session on a new offer cleanly stops the old peer connection and bridge state object.
- Removing a stream or stopping the server also closes all session peer connections and releases bridge state objects.

### What remains for later steps

Not yet implemented:

- RTP video-track delivery from encoded frames
- H.264 packetization / RTCP feedback handling for a real video sender track
- wiring the current bridge to an actual encoded/video media sender
- multi-session addressing beyond the current stream-keyed session model
- richer signaling payloads / structured JSON bodies

## HTTP API

Implemented endpoints:

- `GET /api/video/streams`
- `GET /api/video/streams/{stream_id}`
- `GET /api/video/streams/{stream_id}/output`
- `GET /api/video/streams/{stream_id}/frame`
- `PUT /api/video/streams/{stream_id}/output`
- `POST /api/video/signaling/{stream_id}/offer`
- `POST /api/video/signaling/{stream_id}/answer`
- `POST /api/video/signaling/{stream_id}/candidate`
- `GET /api/video/signaling/{stream_id}/session`

Current session payload fields include:

- `session_generation`
- `offer_sdp`
- `answer_sdp`
- `last_remote_candidate`
- `last_local_candidate`
- `peer_state`
- `media_bridge_state`
- `preferred_media_path`
- `latest_snapshot_available`
- `latest_snapshot_frame_id`
- `latest_snapshot_timestamp_ns`
- `latest_snapshot_width`
- `latest_snapshot_height`
- `latest_encoded_access_unit_available`
- `latest_encoded_codec`
- `latest_encoded_timestamp_ns`
- `latest_encoded_size_bytes`
- `latest_encoded_keyframe`
- `latest_encoded_codec_config`

Output-config JSON fields:
`display_mode`, `mirrored`, `rotation_degrees`, `palette_min`, `palette_max`.

Frame retrieval endpoint:

- `GET /api/video/streams/{stream_id}/frame`
- Returns `404` when stream does not exist.
- Returns `404` when stream exists but has no latest transformed frame snapshot yet.
- Returns `200` with the current latest transformed frame encoded as binary **PPM P6** (`Content-Type: image/x-portable-pixmap`) when available.

The frame response uses the immutable latest-frame snapshot (`std::shared_ptr<const LatestFrame>`) returned by the core. The HTTP layer only takes a snapshot pointer under lock and performs encoding afterwards, so no stream mutex is held during image encoding. This preserves thread safety under concurrent frame pushes and keeps snapshot lifetime semantics explicit for future transport consumers.

Current stream metadata now also includes lightweight latest-frame fields for observability:

- `latest_frame_width`
- `latest_frame_height`
- `latest_frame_pixel_format`
- `latest_frame_timestamp_ns`

PPM was chosen for this milestone because the core already stores transformed output as packed `RGB24`, which maps directly to PPM P6 with only a small textual header and no extra third-party image codec dependency.

## LAN-only assumptions

This subsystem currently assumes peers on the same local network and keeps signaling/control intentionally simple. The current WebRTC work is optimized for that environment: a single stream-scoped session, lightweight candidate forwarding, and an explicit media-source bridge that is ready to be connected to a real video sender implementation. DataChannels are not the intended video path.

## Future encoded H.264 path

`push_access_unit` and `EncodedAccessUnitView` are already part of the core interface, so producers can later provide direct H.264 access units without breaking API shape.

The media bridge now observes both latest transformed frames and, when present, encoded access-unit metadata. That keeps the raw-frame path and the future encoded H.264 path structurally separate while making it obvious how a real browser-native sender can prefer encoded input once that transport step lands.

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

Supported flow (repo root):

```bash
export VCPKG_ROOT=/path/to/vcpkg
./build.sh
./test.sh
```

`build.sh` explicitly configures with:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DENABLE_VIDEO_SERVER=ON \
  -DENABLE_WEBRTC_BACKEND=ON \
  -DBUILD_TESTING=ON
```

Then runs:

```bash
cmake --build build -j
```

`test.sh` runs:

```bash
ctest --test-dir build --output-on-failure
```

If `build/` is missing, `test.sh` calls `build.sh` first.
