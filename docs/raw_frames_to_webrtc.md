# Raw Frames To WebRTC

This guide is about the main integration goal of the project: taking your raw frames and making them visible in a browser over WebRTC.

It does not assume you want to use the synthetic smoke server in production. The smoke and NiceGUI examples are referenced where they help explain the flow.

## The Basic Model

Your application owns frame production.

`video-server` owns:

- stream registration
- per-stream output config
- optional raw-to-H.264 pipeline
- WebRTC signaling and browser delivery

At a high level:

```text
your raw frames
    -> register stream
    -> push_frame(...)
    -> raw-to-H.264 pipeline
    -> push_access_unit(...)
    -> WebRTC sender
    -> browser <video>
```

## Main APIs

The producer-facing interfaces are:

- `ManagedVideoServerConfig`
- `IManagedVideoServer`
- `StreamConfig`
- `VideoFrameView`
- `EncodedAccessUnitView`
- `IVideoServer`
- `IRawVideoPipeline`

Relevant headers:

- `include/video_server/managed_video_server.h`
- `include/video_server/video_server.h`
- `include/video_server/stream_config.h`
- `include/video_server/video_frame_view.h`
- `include/video_server/raw_video_pipeline.h`
- `include/video_server/webrtc_video_server.h`

## Preferred Integration Path

The preferred producer-facing path is now:

1. Load a `ManagedVideoServerConfig` from TOML.
2. Create an `IManagedVideoServer`.
3. Start it.
4. Push raw frames ad hoc with `push_frame(...)`.
5. Call `step()` from your own loop.

`step()` owns:

- stepped HTTP/WebRTC signaling progress
- per-stream output FPS cadence decisions
- transform/filter application
- raw-to-H.264 encode
- forwarding one encoded output stream into the WebRTC fanout path for all active recipients

When `max_streams_per_step` is set, it limits streams that were actually progressed during that call to `step()`. Idle or not-due streams do not consume the budget.

## Minimal Example

```cpp
#include "video_server/managed_video_server.h"
#include "video_server/video_frame_view.h"

auto server = video_server::CreateManagedVideoServer("video_server.toml");
server->start();

// In your frame loop:
video_server::VideoFrameView frame{
    rgb_bytes,
    1280,
    720,
    1280u * 3u,
    video_server::VideoPixelFormat::RGB24,
    timestamp_ns,
    frame_id,
};

server->push_frame("camera-1", frame);
server->step();
```

## Config File Sketch

```toml
execution_mode = "manual_step"
http_poll_timeout_ms = 5

[webrtc]
execution_mode = "manual_step"
http_host = "127.0.0.1"
http_port = 8080

[[streams]]
stream_id = "camera-1"
label = "Camera 1"
width = 1280
height = 720
nominal_fps = 30.0
input_pixel_format = "RGB24"
max_subscribers = 4

[default_raw_pipelines.default]
encoder = "automatic"
encoder_crf = 23
```

Notes:

- `ManagedVideoServerConfig.execution_mode` controls the outer managed policy: `manual_step`, `inline_on_push`, or `worker_thread`
- `webrtc.execution_mode` must stay `manual_step` in the managed path because the managed server owns HTTP pumping explicitly
- `http_poll_timeout_ms` is mirrored into `webrtc.http_poll_timeout_ms`; if both are supplied they must match
- the managed TOML loader supports the full meaningful `WebRtcVideoServerConfig` HTTP/security/rate-limit surface
- `max_subscribers` defaults to `1`, which preserves the old single-viewer behavior unless you opt into fanout
- `encoder_crf` is forwarded to libx264 when available; lower values improve image quality at higher bitrate/CPU cost, and valid values are `0..51`

## Encoder Profile Examples

Example profile configs live in `examples/encoder_profiles/`:

- `min_latency.toml`: `ultrafast`, `zerolatency`, baseline profile, higher CRF
- `balanced.toml`: `veryfast`, `zerolatency`, main profile, default-quality CRF
- `max_quality.toml`: slower preset, high profile, lower CRF; x264-specific knobs apply when the active FFmpeg build has libx264, otherwise unsupported knobs are ignored by the OpenH264 fallback

You can compare these profiles with the in-process profiler:

```bash
cmake --build build --target video_server_config_profiler
./build/video_server_config_profiler --duration-seconds 5
```

Pass explicit TOML paths to compare custom profiles:

```bash
./build/video_server_config_profiler --duration-seconds 10 path/to/profile-a.toml path/to/profile-b.toml
```

The profiler drives synthetic frames through the managed `push_frame()` + `step()` path and reports pushed frame rate, encoded access-unit rate, push/step timing, and sampled encoded access-unit sizes.

Profiler fields:

- `encoder requested`: the encoder mode requested by the TOML, such as `Automatic`, `LibX264`, or `LibOpenH264`
- `encoder selected`: the actual libav encoder found on this machine; if `automatic` resolves to `libopenh264`, x264-only settings do not affect the run
- `x264_options_active`: `yes` only when the selected encoder name contains `x264`; `encoder_preset`, `encoder_tune`, `encoder_profile`, and `encoder_crf` are meaningful for that path
- `elapsed_s`: wall-clock duration of the profile run
- `frames`: raw synthetic frames pushed into the managed server across all streams
- `frame_rate`: pushed frames per second across all streams
- `access_units`: encoded H.264 access units accepted by the server across all streams
- `access_unit_rate`: encoded access units per second across all streams
- `avg_sampled_encoded_bytes`: average size of the latest encoded access unit samples observed after each `step()`, not a full bitrate calculation
- `push_us_avg` and `push_us_p95`: mean and 95th percentile time spent in `push_frame()`; this mostly covers frame validation, transform/latest-frame update, and latching
- `step_us_avg` and `step_us_p95`: mean and 95th percentile time spent in `step()`; with HTTP disabled in the profiler, this is dominated by managed stream progression and H.264 encode work
- `step_calls`: total managed `step()` calls completed during the run

Interpretation notes:

- Similar `step_us_p95` values across profiles usually mean the profiles resolved to the same underlying encoder path or the tested settings are not supported by the selected encoder.
- On builds without `libx264`, `automatic` commonly resolves to OpenH264. In that case the example profiles still run, but x264 quality knobs are effectively a no-op and the profiler should report `x264_options_active=no`.
- The profiler is CPU-side and in-process. It does not measure browser decode, RTP packet delivery, WebRTC jitter, or visual quality.
- Use longer runs when comparing small deltas, because one-second and short runs can be dominated by startup, scheduling, and synthetic-frame generation noise.

## Lower-Level Split Path

The older split path still exists when you want direct control over raw-frame publication and encoder ownership:

- `server.push_frame(...)` updates core stream state, transformed latest-frame snapshots, and frame/debug observability
- `pipeline->push_frame(...)` performs raw-to-H.264 encoding and forwards encoded units into `push_access_unit(...)`

The managed server wraps that lower-level behavior into one stepped progression loop.

## Choosing The Input Format

The producer-facing `VideoPixelFormat` enum remains the public API. Backend/libav pixel types stay internal to the raw-to-H.264 layer.

Managed/raw-frame ingest and latest-frame transforms currently accept:

- `RGB24`
- `BGR24`
- `RGBA32`
- `BGRA32`
- `GRAY8`
- `GRAY10LE`
- `GRAY12LE`
- `GRAY16LE`

Config parsing also accepts grayscale aliases `GRAY10`, `GRAY12`, and `GRAY16`, which normalize to the explicit little-endian enum values.

The direct raw-to-H.264 pipeline additionally accepts:

- `NV12`
- `I420`

Practical grayscale notes:

- high-bit grayscale inputs are normalized into 8-bit RGB snapshots for latest-frame HTTP/debug output
- the raw-to-H.264 pipeline converts supported grayscale inputs into the encoder working format internally before H.264 encode
- managed/raw-frame config validation rejects formats that do not have a supported display-transform path today, so unsupported combinations fail before runtime encode

For the simplest first integration, use tightly packed `RGB24` if that matches your producer.

## Browser Side

You have two practical options while integrating:

- use your own browser client against the signaling API
- use the NiceGUI harness as the browser client and debug UI

The harness is useful because it already knows how to:

- create the peer connection
- exchange offer, answer, and candidates
- show stream/session state
- switch streams
- edit runtime output config

Harness docs:

- [examples/nicegui_smoke/README.md](../examples/nicegui_smoke/README.md)

## Verifying Your Integration

Minimum verification steps:

1. `GET /api/video/streams` shows your stream id.
2. `GET /api/video/streams/{stream_id}` shows the stream as active.
3. The browser client can complete signaling.
4. Session state reaches `sending-h264-rtp`.
5. Video renders in the browser.

Useful checks:

```bash
curl http://127.0.0.1:8080/api/video/streams
curl http://127.0.0.1:8080/api/video/streams/camera-1
curl -H 'X-Video-Session-Id: session-1' http://127.0.0.1:8080/api/video/signaling/camera-1/session
```

If debug is enabled:

```bash
curl http://127.0.0.1:8080/api/video/debug/stats
```

## Applying Runtime Output Config

Runtime output config is per stream and applies to future frames. Typical uses:

- switch display/palette mode
- resize output
- reduce output FPS

See:

- [configuration_and_filters.md](configuration_and_filters.md)

## Session And Reconnect Behavior

Important current behavior:

- one stream can fan out to multiple concurrent WebRTC recipients
- each recipient gets its own peer connection and track
- encoded H.264 is produced once per stream and then fanned out to all active recipients
- `max_subscribers` enforces the concurrent recipient cap per stream
- the signaling `offer` response returns a `session_id`; multi-viewer clients should send that value back in `X-Video-Session-Id`

See:

- [session_lifecycle.md](session_lifecycle.md)

## Security And LAN Use

For local development, default loopback binding is the safe starting point.

When you need a second machine to view the stream:

- use a non-loopback bind intentionally
- use shared-key auth where appropriate
- use LAN-only example settings only on a trusted network

See:

- [security_model.md](security_model.md)

## Example References

Useful example code:

- synthetic server that loads the managed config and runs a single-threaded `push_frame(...)` + `step()` loop:
  - `examples/nicegui_smoke/synthetic_webrtc_smoke_server.cpp`
- browser harness that connects and renders video:
  - `examples/nicegui_smoke/app.py`

Those examples are for validation and debugging, but they also show the intended end-to-end integration shape.
