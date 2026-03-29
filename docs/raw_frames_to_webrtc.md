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

- `StreamConfig`
- `VideoFrameView`
- `EncodedAccessUnitView`
- `IVideoServer`
- `IRawVideoPipeline`

Relevant headers:

- `include/video_server/video_server.h`
- `include/video_server/stream_config.h`
- `include/video_server/video_frame_view.h`
- `include/video_server/raw_video_pipeline.h`
- `include/video_server/webrtc_video_server.h`

## Integration Path For Raw Frames

The normal raw-frame path is:

1. Create a `WebRtcVideoServer`.
2. Start it.
3. Register a stream with width, height, fps, and pixel format.
4. Create a raw-to-H.264 pipeline bound to that stream.
5. Push raw frames into the server for transform/observability.
6. Push the same raw frames, or the transformed result you intend to encode, into the pipeline.
7. Let the browser connect through the signaling API or the NiceGUI harness.

## Minimal Example

```cpp
#include "video_server/raw_video_pipeline.h"
#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/webrtc_video_server.h"

video_server::WebRtcVideoServerConfig server_cfg;
server_cfg.http_host = "127.0.0.1";
server_cfg.http_port = 8080;

video_server::WebRtcVideoServer server(server_cfg);
server.start();

video_server::StreamConfig stream_cfg;
stream_cfg.stream_id = "camera-1";
stream_cfg.label = "Camera 1";
stream_cfg.width = 1280;
stream_cfg.height = 720;
stream_cfg.nominal_fps = 30.0;
stream_cfg.input_pixel_format = video_server::VideoPixelFormat::RGB24;

server.register_stream(stream_cfg);

video_server::RawVideoPipelineConfig pipeline_cfg;
pipeline_cfg.input_width = 1280;
pipeline_cfg.input_height = 720;
pipeline_cfg.input_pixel_format = video_server::VideoPixelFormat::RGB24;
pipeline_cfg.input_fps = 30.0;

auto pipeline = video_server::make_raw_to_h264_pipeline_for_server(
    "camera-1", pipeline_cfg, server);

std::string error;
pipeline->start(&error);

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

server.push_frame("camera-1", frame);
pipeline->push_frame(frame, &error);
```

## Why Push To Both

In the current architecture:

- `server.push_frame(...)` updates core stream state, transformed latest-frame snapshots, and frame/debug observability
- `pipeline->push_frame(...)` performs raw-to-H.264 encoding and forwards encoded units into `push_access_unit(...)`

That split is deliberate. The raw frame path and encoded delivery path stay separate.

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
curl http://127.0.0.1:8080/api/video/signaling/camera-1/session
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

- one active WebRTC session slot per stream
- a new offer replaces the previous session for that stream
- old sessions are deactivated and should not continue sending

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

- synthetic server that registers streams and runs the shared raw-to-H.264 pipeline:
  - `examples/nicegui_smoke/synthetic_webrtc_smoke_server.cpp`
- browser harness that connects and renders video:
  - `examples/nicegui_smoke/app.py`

Those examples are for validation and debugging, but they also show the intended end-to-end integration shape.
