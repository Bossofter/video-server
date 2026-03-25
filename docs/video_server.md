# Video Server Architecture

## Overview

This subsystem is split into portable core APIs and optional backend-specific transport layers.

- **Producer-facing API (stable):** register stream, push frame pointer, push encoded access unit.
- **Core state model:** stream metadata, runtime output config, stats, latest transformed frame storage, and latest encoded access-unit storage.
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

## Internal encoded H264 pipeline

`push_access_unit()` is now a real producer-to-backend path:

1. validate stream id
2. validate the encoded payload pointer + size
3. validate codec support (`H264` is accepted in this step)
4. copy the encoded bytes into an owning immutable per-stream snapshot
5. preserve timestamp, keyframe, codec-config, and sequence metadata
6. publish the new latest encoded-unit snapshot
7. update encoded stream counters and observability fields

Each stream now stores an internal latest encoded-unit object that contains:

- owning encoded bytes (`std::vector<uint8_t>`)
- codec
- timestamp
- keyframe flag
- codec-config flag
- sequence id
- validity flag

This snapshot follows the same publication model as latest transformed frames: construct fully, then publish via a `std::shared_ptr<const ...>` so backend readers can safely retain older snapshots while newer ones are published.


## Raw-to-H264 bridge pipeline

A new bridge layer now sits between raw frame production and the pre-existing encoded H264 ingestion path. This keeps the producer-facing server API stable while adding the missing upstream conversion path.

Current first-pass flow:

1. a raw producer generates `VideoFrameView` input
2. an `IRawVideoPipeline` instance is explicitly bound to a `stream_id`
3. the pipeline optionally applies in-process resize / fps filtering and any required pixel-format conversion
4. the pipeline delegates raw-frame encoding to an internal `IRawH264EncoderBackend` seam
5. the current `LibavH264EncoderBackend` implementation produces H264 packets and normalizes them to Annex-B access units
6. the pipeline forwards each encoded access unit into the existing `push_access_unit(...)` path (or a sink that ultimately lands there)
7. the already-existing encoded/WebRTC path delivers H264 onward to browser sessions

### Pipeline abstraction

The new public pipeline surface is intentionally separate from `IVideoServer`:

- `RawVideoPipelineConfig`: tightly-packed raw input contract, optional resize, optional output fps, explicit H264 encoder selection, and a small set of encoder knobs that are applied only when supported by the active encoder family
- `IRawVideoPipeline`: `start()`, `push_frame()`, `stop()`, and explicit `stream_id()` binding
- `make_raw_to_h264_pipeline(...)`: build a pipeline with an arbitrary encoded-unit sink
- `make_raw_to_h264_pipeline_for_server(...)`: bind encoded output directly into an `IVideoServer` stream through `push_access_unit(...)`

### First backend choice

The first functional backend now uses in-process libavcodec/libswscale primitives behind `IRawH264EncoderBackend`. That keeps the architecture intact while removing the shell-out bridge from the primary path:

- practical to implement quickly
- already aligned with the repo's smoke tooling
- keeps the work focused on achieving an end-to-end path without redesigning delivery

### Current filter/config support

`RawVideoPipelineConfig` currently supports:

- passthrough encode
- resize via libswscale
- output frame-rate control via pipeline-side admission throttling based on frame timestamps
- pixel-format conversion to encoder output `yuv420p`
- explicit encoder-family-specific option handling: libx264-family encoders receive preset/tune/profile/AUD knobs, while OpenH264 is opened with a main-profile + skip-frame-compatible configuration that it accepts without startup warnings

This is intentionally minimal and designed for extension rather than a large initial filter framework. `emit_access_unit_delimiters` remains a best-effort encoder knob for encoder families that support AUD emission, but Annex-B access-unit handoff no longer depends on AUD-delimited splitting.

### Lifecycle behavior

The pipeline owns the in-process encoder lifecycle:

- `start()` validates config, opens the selected encoder backend, and prepares its libavcodec/libswscale state plus Annex-B normalization
- `push_frame()` validates the raw frame contract, performs pipeline-side frame dropping to honor `output_fps`, and then submits the frame through the encoder backend seam
- each encoded packet is converted to Annex-B, treated as a complete H264 access unit, and forwarded to the encoded sink
- if the encoded sink rejects an access unit, the pipeline records a failure, tears down the encoder state for that stream, and surfaces the failure on later `push_frame()` calls
- `stop()` flushes pending packets when possible and tears down the in-process encoder/bitstream-filter state cleanly

### Demo / validation path

The NiceGUI smoke server now exercises the shared pipeline with synthetic raw frames. It still pushes raw frames through `push_frame()` for transformed-frame observability while also feeding the new raw-to-H264 pipeline, which then forwards encoded output back into the existing encoded-media/WebRTC path.

### What remains for future hardening

- broaden stride normalization and richer raw preprocess filter support
- improve encoder-specific option/capability reporting for additional future backends
- extend long-run timestamp and buffering validation under heavier load
- evaluate additional encoder backends (for example hardware encoders) behind the same abstraction

## Runtime output config behavior

Changing `StreamOutputConfig` via HTTP or direct API immediately affects the next `push_frame()` for that stream. Re-registration is not required.

## Stats and stream activity

Runtime stream info tracks practical counters and markers:

- `frames_received`, `frames_transformed`, `frames_dropped`
- `access_units_received`
- `last_input_timestamp_ns`, `last_output_timestamp_ns`
- `last_frame_id`
- `has_latest_frame`
- `has_latest_encoded_unit`
- `last_encoded_codec`, `last_encoded_timestamp_ns`
- `last_encoded_sequence_id`, `last_encoded_size_bytes`
- `last_encoded_keyframe`, `last_encoded_codec_config`

These fields are intended for lightweight operational visibility and backend readiness.

## Backend-ready latest-frame access

The core exposes internal getters (`get_latest_frame_for_stream`, `get_latest_encoded_unit_for_stream`) that return immutable `std::shared_ptr<const ...>` snapshots when available. This avoids copying full frame buffers on read while giving clear lifetime semantics for backend consumers (older snapshots remain valid after newer frames are published). The HTTP frame endpoint uses the frame getter today, and the WebRTC backend now uses both getters to seed per-session media bridge state.

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

WebRTC **DataChannels are not used for video transport**. Each active signaling session now owns an explicit media-source bridge abstraction plus a real browser-facing video track path for H264 delivery.

For each active signaling session, the backend now owns:

- a real `rtc::PeerConnection`
- a real session-owned `rtc::Track` configured as an H264 video sender
- a media-source bridge object tied to the stream id
- raw-snapshot observation for current transformed-frame state
- encoded-unit observation for the preferred H.264 sender path
- a dedicated session-side H.264 sender/packetizer object that consumes immutable encoded snapshots through the bridge boundary
- bridge status (`media_bridge_state`, `preferred_media_path`, snapshot metadata, encoded access-unit metadata, sender metadata)

Current bridge behavior:

1. producer pushes raw frames through the unchanged producer API
2. core transforms and publishes a new immutable latest-frame snapshot
3. `push_frame()` forwards the latest immutable transformed frame snapshot into the bridge through `on_latest_frame(...)`
4. `push_access_unit()` publishes a new immutable `LatestEncodedUnit` and forwards that snapshot into the bridge through `on_latest_encoded_unit(...)`
5. `WebRtcStreamSession` forwards the immutable encoded snapshot to its `H264EncodedVideoSender` consumer path without taking direct access to core stream internals
6. the sender inspects H.264 Annex-B NAL units, preserves `codec_config`, `keyframe`, `timestamp_ns`, and `sequence_id`, skips duplicate sequence ids, and caches codec-config data for later keyframe delivery when needed
7. once codec config + keyframe state is available, the sender packetizes H264 NAL units into RTP packets and forwards them through the libdatachannel video track
8. new signaling sessions are seeded from both current latest-frame and latest-encoded getters, so an already-running H264 producer path is visible immediately
9. signaling/session inspection can now report whether the session owns a video track, whether H264 delivery is active, whether codec config and keyframes have been seen, and what the latest packetization status is

The current bridge state is intentionally explicit:

- `awaiting-video-track-bridge` when only latest-frame snapshot state is available
- `awaiting-h264-video-track-bridge` once encoded H.264 access units have been observed

That makes the system’s status explicit: the backend now has real peer/session plumbing, a real encoded-media source boundary, a real session-owned H264 video track, and real RTP packet emission work on the encoded path.

### Threading and lifetime

- Session inspection queries the media-source bridge and sender for immutable metadata without mutating shared frame or encoded-unit state.
- Encoded delivery work is done from immutable `std::shared_ptr<const LatestEncodedUnit>` snapshots, so older H264 units remain valid while newer latest snapshots are published.
- The session only accesses encoded units through the media bridge / getter boundary and does not reach into `VideoServerCore` stream internals.
- Replacing a stream session on a new offer cleanly stops the old peer connection and bridge state object.
- Removing a stream or stopping the server also closes all session peer connections and releases bridge state objects.

### What remains for later steps

Still incomplete / for later hardening:

- richer RTCP handling, retransmission, bitrate adaptation, and browser feedback processing
- stronger packet pacing / buffering policy for not-yet-open tracks
- multi-session addressing beyond the current stream-keyed session model
- richer signaling payloads / structured JSON bodies
- browser-side playback validation across Chromium / Firefox / Safari with production SDP tuning

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

## Security hardening

Current first-pass hardening behavior is intentionally lightweight and LAN-oriented:

- Default bind stays `127.0.0.1`.
- `GET /api/video/debug/stats` is disabled by default and must be enabled explicitly.
- Sensitive routes (`/api/video/signaling/*`, `/api/video/debug/*`, `/api/video/streams/{stream_id}/frame`, `/api/video/streams/{stream_id}/output`, and `/api/video/streams/{stream_id}/config`) are allowed by default on loopback binds.
- On non-loopback binds, those same sensitive routes are blocked unless you set `allow_unsafe_public_routes = true` or enable a shared key and/or IP allowlist.
- Shared-key auth is optional and accepts `Authorization: Bearer <key>` or `X-Video-Server-Key: <key>`.
- IP allowlists accept exact IPs or CIDR ranges for IPv4/IPv6 and apply to the HTTP API surface.
- Loopback CORS is allowed automatically for loopback binds. Non-loopback deployments should set `cors_allowed_origins` explicitly instead of relying on wildcard CORS.
- Runtime config JSON is now validated more strictly, stream IDs are bounded to `[A-Za-z0-9._-]` up to 64 characters, signaling payload sizes are capped, pending ICE candidates per stream are bounded, and light fixed-window rate limits are applied to signaling/config/debug routes.

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
- `latest_encoded_sequence_id`
- `latest_encoded_size_bytes`
- `latest_encoded_keyframe`
- `latest_encoded_codec_config`
- `encoded_sender_state`
- `encoded_sender_codec`
- `encoded_sender_has_pending_encoded_unit`
- `encoded_sender_codec_config_seen`
- `encoded_sender_ready_for_video_track`
- `encoded_sender_video_track_exists`
- `encoded_sender_video_track_open`
- `encoded_sender_h264_delivery_active`
- `encoded_sender_keyframe_seen`
- `encoded_sender_cached_codec_config_available`
- `encoded_sender_delivered_units`
- `encoded_sender_duplicate_units_skipped`
- `encoded_sender_failed_units`
- `encoded_sender_packets_attempted`
- `encoded_sender_last_delivered_sequence_id`
- `encoded_sender_last_delivered_timestamp_ns`
- `encoded_sender_last_delivered_size_bytes`
- `encoded_sender_last_delivered_keyframe`
- `encoded_sender_last_delivered_codec_config`
- `encoded_sender_last_contains_sps`
- `encoded_sender_last_contains_pps`
- `encoded_sender_last_contains_idr`
- `encoded_sender_last_contains_non_idr`
- `encoded_sender_last_packetization_status`
- `encoded_sender_video_mid`

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

Stream metadata now also exposes encoded observability fields:

- `has_latest_encoded_unit`
- `latest_encoded_codec`
- `latest_encoded_timestamp_ns`
- `latest_encoded_sequence_id`
- `latest_encoded_size_bytes`
- `latest_encoded_keyframe`
- `latest_encoded_codec_config`

PPM was chosen for this milestone because the core already stores transformed output as packed `RGB24`, which maps directly to PPM P6 with only a small textual header and no extra third-party image codec dependency.

## LAN-only assumptions

This subsystem currently assumes peers on the same local network and keeps signaling/control intentionally simple. The current WebRTC work is optimized for that environment: a single stream-scoped session, lightweight candidate forwarding, and an explicit media-source bridge that is ready to be connected to a real video sender implementation. DataChannels are not the intended video path.

## Preferred encoded H.264 direction

The producer-facing API remains unchanged: producers can still call `push_frame()` or `push_access_unit()` with the same view types. Internally, however, the architecture is now explicitly split into two source paths:

- Raw path: `push_frame()` → transforms → `LatestFrame` → media bridge
- Encoded path: `push_access_unit(H264)` → `LatestEncodedUnit` → media bridge → `WebRtcStreamSession` → `H264EncodedVideoSender` → session video track → browser

Raw frames remain supported for transforms, snapshots, and inspection. Encoded H264 is now the preferred browser-facing media architecture because it is the path that now drives session RTP/video-track delivery.

### H.264 sender behavior in this step

The sender path is now transport-facing:

- it consumes immutable latest encoded units from the bridge/session boundary
- it performs minimal H.264 Annex-B parsing to identify SPS, PPS, IDR, and non-IDR slice NAL units
- it preserves and exposes `codec_config`, `keyframe`, `timestamp_ns`, and `sequence_id`
- it tracks duplicate suppression via `sequence_id`
- it caches codec-config access units so SPS/PPS can be injected ahead of later keyframes when needed
- it packetizes NAL units into RTP payloads, including FU-A fragmentation for large NAL units
- it forwards the generated RTP packets through the session-owned libdatachannel video track
- it reports whether the session has a track, whether the track is open, whether packetization happened, and what the most recent packetization status was

`H264EncodedVideoSender` should therefore be read as the session-side encoded transport component for the current backend. Session lifecycle and signaling stay separate from H264 parsing / packetization logic.

The current `sequence_id` is also still a temporary identity source derived from the encoded timestamp in core state. A dedicated per-stream monotonically increasing encoded-unit counter may be preferable in a later transport-focused step.

This step therefore moves the backend beyond “encoded state exists” and into “the session is actively consuming H264 units and pushing real RTP/video-track media toward the browser path.”

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
