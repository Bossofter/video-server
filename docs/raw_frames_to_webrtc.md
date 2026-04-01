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
```

Notes:

- `ManagedVideoServerConfig.execution_mode` controls the outer managed policy: `manual_step`, `inline_on_push`, or `worker_thread`
- `webrtc.execution_mode` must stay `manual_step` in the managed path because the managed server owns HTTP pumping explicitly
- `http_poll_timeout_ms` is mirrored into `webrtc.http_poll_timeout_ms`; if both are supplied they must match
- the managed TOML loader supports the full meaningful `WebRtcVideoServerConfig` HTTP/security/rate-limit surface
- `max_subscribers` defaults to `1`, which preserves the old single-viewer behavior unless you opt into fanout

## Lower-Level Split Path

The older split path still exists when you want direct control over raw-frame publication and encoder ownership:

- `server.push_frame(...)` updates core stream state, transformed latest-frame snapshots, and frame/debug observability
- `pipeline->push_frame(...)` performs raw-to-H.264 encoding and forwards encoded units into `push_access_unit(...)`

The managed server wraps that lower-level behavior into one stepped progression loop.

## Choosing The Input Format

The raw pipeline currently accepts tightly packed:

- `RGB24`
- `BGR24`
- `RGBA32`
- `BGRA32`
- `GRAY8`
- `NV12`
- `I420`

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
