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

The core exposes an internal getter (`get_latest_frame_for_stream`) that returns an immutable `std::shared_ptr<const LatestFrame>` snapshot when available. This avoids copying full frame buffers on read while giving clear lifetime semantics for backend consumers (older snapshots remain valid after newer frames are published). The same snapshot boundary is now consumed by both the HTTP frame endpoint and the first real WebRTC delivery loop.

## WebRTC backend notes

`WebRtcVideoServer` implements `IVideoServer` and composes:

- core stream/state manager
- transform stage
- signaling/session manager
- HTTP API host
- per-session WebRTC transport workers

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
  - exposes the current backend session snapshot, including answer SDP, latest candidates, peer state, channel state, and delivery counters
- `POST /api/video/signaling/{stream_id}/answer`
  - is retained for API stability and supports the reverse direction if the backend later originates offers

A session is currently keyed by `stream_id`, which keeps the first LAN-focused backend step simple while still making the stream/session relationship explicit.

### Current media-delivery path

This milestone makes the backend media path real by using the immutable latest-frame snapshots as the WebRTC source.

For each active signaling session, the backend now owns:

- a real `rtc::PeerConnection`
- a `video-snapshots` WebRTC data channel
- a delivery thread that periodically checks `get_latest_frame_for_stream(stream_id)`
- per-session delivery counters (`frames_sent`, `last_frame_id`)

Delivery flow:

1. producer pushes raw frames through the unchanged producer API
2. core transforms and publishes a new immutable latest-frame snapshot
3. the per-session WebRTC delivery loop reads the snapshot without mutating shared state
4. when a new frame id appears and the data channel is open, the backend encodes the snapshot as PPM
5. the backend sends:
   - a JSON metadata message (`type=latest-frame`, stream/frame/timestamp/dimensions/content-type)
   - followed by the binary PPM payload over the same WebRTC connection

This is intentionally a **snapshot-driven WebRTC bridge**, not yet the final RTP video track path. It is nevertheless a real backend delivery path: signaling now drives actual peer connections, and latest transformed frames now leave the backend over WebRTC instead of only through HTTP.

### Threading and lifetime

- Each stream session owns its own delivery thread.
- The delivery thread only grabs lightweight session state under lock, then fetches and encodes the immutable latest-frame snapshot outside long critical sections.
- Replacing a stream session on a new offer cleanly stops the old peer connection and worker thread.
- Removing a stream or stopping the server also stops and joins all session workers.

### What remains for later steps

Not yet implemented:

- RTP video-track delivery from encoded frames
- H.264 packetization / RTCP feedback handling for a real video sender track
- multi-session addressing beyond the current stream-keyed session model
- richer signaling payloads / structured JSON bodies
- transport selection between raw-snapshot fallback and final encoded real-time media

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

- `offer_sdp`
- `answer_sdp`
- `last_remote_candidate`
- `last_local_candidate`
- `peer_state`
- `data_channel_open`
- `frames_sent`
- `last_frame_id`

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

This subsystem currently assumes peers on the same local network and keeps signaling/control intentionally simple. The current WebRTC delivery path is optimized for that environment: a single stream-scoped session, lightweight candidate forwarding, and snapshot delivery over a WebRTC data channel.

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
