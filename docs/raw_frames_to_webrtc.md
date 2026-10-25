# Raw Frames To WebRTC

This is the main integration path for `video-server`: your application produces raw frames, and the library turns them into browser-playable WebRTC video.

Current version: `1.3.0`

## Flow

```text
your producer
    -> push_frame(...)
    -> transform/output config
    -> raw-to-H.264 pipeline
    -> bounded encoded access-unit handoff
    -> step_api()
    -> WebRTC sender
    -> browser <video>
```

The rest of the server does not need to know whether H.264 came from OpenH264, libx264, or VA-API.

## Main Headers

- `include/video_server/managed_video_server.h`
- `include/video_server/video_frame_view.h`
- `include/video_server/stream_config.h`
- `include/video_server/raw_video_pipeline.h`
- `include/video_server/webrtc_video_server.h`

## Managed Server Model

Most applications should use `IManagedVideoServer`.

The application owns:

- frame production
- stream-thread creation
- stream grouping
- stream cadence
- stream-thread affinity and scheduling

The library owns:

- stream registration
- transform/output config
- H.264 encoding
- bounded encoded handoff
- HTTP/WebRTC signaling
- WebRTC delivery

## Stepping

API work and stream work are intentionally separate.

`step_api()` progresses:

- HTTP request handling
- signaling
- session/subscriber lifecycle
- delivery of already encoded H.264 access units

`IManagedStreamGroup::step_streams()` progresses only the streams registered to that handle:

- pending raw-frame consumption
- transform/filter work
- H.264 encoding
- handoff to the API/WebRTC side

Concurrency rules:

- a stream can belong to only one registered group
- the same stream-group handle rejects concurrent `step_streams()` calls
- different stream groups may run concurrently
- `step_api()` may run concurrently with stream groups
- stream work always runs on the calling thread
- the library does not create stream worker pools

The older `step()` method remains as a compatibility wrapper for simple single-loop callers.

## Manual API Thread Example

```cpp
auto server = video_server::CreateManagedVideoServer("video_server.toml");
server->start();

auto group_a = server->register_streams({"camera-1"});
auto group_b = server->register_streams({"camera-2"});

std::atomic_bool running{true};

std::thread api_thread([&] {
    while (running) {
        server->step_api();
    }
});

std::thread stream_thread_a([&] {
    while (running) {
        group_a->step_streams();
    }
});

std::thread stream_thread_b([&] {
    while (running) {
        group_b->step_streams();
    }
});
```

Multiple groups can also share one application thread:

```cpp
while (running) {
    group_a->step_streams();
    group_b->step_streams();
}
```

## Managed API Thread Example

If you want the library to own API progression, configure one managed API thread:

```cpp
video_server::ManagedVideoServerConfig config;
config.api_execution.mode = video_server::ManagedApiExecutionMode::ManagedThread;
config.api_execution.thread.cpu_affinity = {2};
config.api_execution.thread.scheduling_priority = 10;

auto server = video_server::CreateManagedVideoServer(config);
server->start();

auto group_a = server->register_streams({"camera-1"});
auto group_b = server->register_streams({"camera-2"});
```

In this mode:

- manual `step_api()` calls are rejected
- the library owns exactly one API progression thread
- requested API-thread affinity or priority must be applied successfully or startup fails
- stream work is still caller-driven through `step_streams()`

TOML equivalent:

```toml
api_execution = "managed_thread"
api_cpu_affinity = [2]
api_scheduling_priority = 10
```

## Minimal Config

```toml
execution_mode = "manual_step"
api_execution = "manual_step"
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

For AV1 hardware encoding on Radeon/VA-API:

```toml
codec = "av1"
encoder = "vaapi"
hardware_device = "/dev/dri/renderD128"
```

For AV1 hardware encoding on NVIDIA:

```toml
codec = "av1"
encoder = "nvenc"
encoder_preset = "p1"
```

For hardware-independent AV1 testing with libaom:

```toml
codec = "av1"
encoder = "libaom-av1"
encoder_crf = 32
```

`automatic` AV1 selection probes `av1_vaapi` first, then `av1_nvenc`. It does not silently fall back to the substantially more CPU-intensive software encoder; select `libaom-av1` explicitly when desired. The linked FFmpeg build must expose the selected encoder; the default vcpkg manifests include libaom, while the overlay port's `vaapi` and/or `nvenc` features enable hardware paths. AV1 access units are emitted as low-overhead temporal-unit OBUs and packetized for WebRTC AV1 RTP.

## Pushing Frames

```cpp
video_server::VideoFrameView frame{
    pixels,
    1280,
    720,
    1280u * 3u,
    video_server::VideoPixelFormat::RGB24,
    timestamp_ns,
    frame_id,
};

server->push_frame("camera-1", frame);
```

Use monotonically increasing timestamps and frame ids. `output_fps` throttling, if configured, is based on stream timing rather than a hidden fixed 30 FPS loop.

## Encoder Selection

Raw pipelines support:

- `automatic`: VA-API hardware H.264 when compiled in and usable, otherwise OpenH264
- `vaapi`: explicit Linux VA-API hardware H.264 through libav `h264_vaapi`
- `libopenh264`: explicit software OpenH264
- `libx264`: explicit software libx264 when available

Example VA-API config:

```toml
[default_raw_pipelines.default]
encoder = "vaapi"
hardware_device = "/dev/dri/renderD128"
```

VA-API requires a usable Linux GPU/media engine, render-node access, drivers, and a libav/FFmpeg build exposing `h264_vaapi`. AMD and Intel devices are supported through the generic VA-API stack. NVIDIA/NVENC is not part of this backend.

Useful diagnostics:

```bash
ls -l /dev/dri
vainfo
ffmpeg -hide_banner -encoders | grep vaapi
```

`ffmpeg` is only a diagnostic command here. `video-server` does not shell out to it at runtime.

## Browser/API Side

Browser clients use the HTTP signaling routes under `/api/video/signaling/...`.

Runtime stream discovery, output config, latest-frame diagnostics, and debug stats are documented in [API Routes](api_routes.md).
