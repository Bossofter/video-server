# Configuration Reference

This page documents every option accepted by the managed-server TOML loader and every field accepted by the per-stream runtime output API.

The managed server loads TOML with:

```cpp
auto server = video_server::CreateManagedVideoServer("server.toml");
```

The same schema is used by `examples/nicegui_smoke/smoke_server.toml` and the files under `examples/encoder_profiles/`.

## TOML Structure and Precedence

A managed configuration has four parts:

```toml
# Top-level managed-server settings
execution_mode = "manual_step"
api_execution = "manual_step"

[webrtc]
# HTTP, WebRTC, ICE, security, and API settings

[[streams]]
# Repeat once for every input stream

[default_raw_pipelines.default]
# Encoder settings used when a stream has no stream-specific table

[default_raw_pipelines.camera_1]
# Optional settings for the stream whose stream_id is "camera_1"
```

Pipeline selection follows these rules:

1. `[default_raw_pipelines.<stream_id>]` wins when a table exactly matches the stream ID.
2. Otherwise, `[default_raw_pipelines.default]` is used when present.
3. Otherwise, the built-in `RawVideoPipelineConfig` defaults are used.

A stream-specific pipeline table **replaces** the `default` table; the two tables are not merged. Missing fields in the stream-specific table receive built-in defaults, not values from `[default_raw_pipelines.default]`.

Unknown TOML keys are currently ignored. Misspelled keys can therefore look valid while having no effect. Use the names in this reference exactly.

## Complete Managed Configuration Example

This example shows every effective managed-server option. Settings that normally need elevated privileges or deployment-specific values are commented out.

```toml
# How stream work is progressed: manual_step, inline_on_push, or worker_thread.
execution_mode = "manual_step"

# How HTTP/signaling/WebRTC API work is progressed: manual_step or managed_thread.
api_execution = "manual_step"
# api_cpu_affinity = [2, 3]
# api_scheduling_priority = 10

# Poll timeout for managed API progression. Must match webrtc.http_poll_timeout_ms
# when both are present. Zero makes each poll non-blocking.
http_poll_timeout_ms = 5

# Zero processes every ready ungrouped stream per step.
max_streams_per_step = 0

[webrtc]
# A managed server requires this inner mode to remain manual_step.
execution_mode = "manual_step"
http_host = "127.0.0.1"
http_port = 8080
enable_http_api = true
enable_debug_api = false
enable_runtime_config_api = true

# Remote-access protection.
allow_unsafe_public_routes = false
enable_shared_key_auth = false
shared_key = ""
ip_allowlist = []
cors_allowed_origins = []

# HTTP and signaling request limits.
max_http_request_bytes = 262144
max_json_body_bytes = 16384
max_signaling_sdp_bytes = 131072
max_signaling_candidate_bytes = 8192
max_pending_candidates_per_stream = 32

# Per-client, per-route rate limits. A zero window or request count disables
# that rate-limit bucket.
signaling_rate_limit_window_seconds = 10
signaling_rate_limit_max_requests = 60
config_rate_limit_window_seconds = 10
config_rate_limit_max_requests = 20
debug_rate_limit_window_seconds = 10
debug_rate_limit_max_requests = 60

# ICE/STUN/TURN configuration.
ice_transport_policy = "all"
ice_bind_address = ""
ice_servers = []
# ice_port_range_begin = 49152
# ice_port_range_end = 49200

# Managed HTTP polling. Must match the top-level timeout when both are set.
http_poll_timeout_ms = 5
max_http_connections_per_step = 1

# Replace ice_servers = [] with one or more array-of-table entries when needed:
# [[webrtc.ice_servers]]
# uri = "stun:stun.example.com:3478"
#
# [[webrtc.ice_servers]]
# uri = "turns:turn.example.com:443?transport=tcp"
# username = "user"
# credential = "secret"

[[streams]]
stream_id = "camera_1"
label = "Front camera"
width = 1280
height = 720
nominal_fps = 30.0
input_pixel_format = "RGB24"
max_subscribers = 4

[default_raw_pipelines.default]
codec = "h264"
encoder = "automatic"
encoder_preset = "ultrafast"
encoder_tune = "zerolatency"
encoder_profile = "baseline"
# encoder_crf = 23
hardware_device = ""
repeat_headers = true
emit_access_unit_delimiters = true
```

## Top-Level Managed-Server Options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `execution_mode` | string | `"manual_step"` | Outer stream progression mode. Valid values are `manual_step`, `inline_on_push`, and `worker_thread`. See [Execution modes](#execution-modes). |
| `api_execution` | string | `"manual_step"` | API progression policy. `manual_step` requires caller-driven API progression; `managed_thread` creates one library-owned API thread. |
| `api_cpu_affinity` | integer array | `[]` | Linux CPU IDs assigned to the managed API thread. Only meaningful when an API thread is active. Invalid IDs or inability to apply affinity make startup fail. |
| `api_scheduling_priority` | integer | unset | Requests Linux `SCHED_RR` with this priority for the managed API thread. This commonly requires elevated scheduling privileges. Unsupported platforms or failure to apply it make startup fail. |
| `http_poll_timeout_ms` | unsigned integer | `5` | Timeout used by each managed HTTP/API poll. `0` is non-blocking. If `webrtc.http_poll_timeout_ms` is also supplied, the values must match. |
| `max_streams_per_step` | unsigned integer | `0` | Maximum number of ready, ungrouped streams progressed by one `server->step()`. `0` means no cap. Idle streams do not consume the budget, and the starting stream rotates for fairness. Stream groups have their own explicit membership and are stepped independently. |

### Execution Modes

`execution_mode` controls stream processing:

- `manual_step`: `push_frame()` latches the latest frame. Call `server->step()` or `IManagedStreamGroup::step_streams()` to transform and encode it.
- `inline_on_push`: the target stream is transformed and encoded before `push_frame()` returns. This gives simple ownership but places transform and encoder latency on the producer thread.
- `worker_thread`: creates an internal API progression thread. Stream work is still progressed by `step()` or registered stream groups; it is not silently moved to one encoder thread.

`api_execution = "managed_thread"` also creates the internal API thread without changing stream progression. While either managed API-thread mechanism is active, direct `step_api()` calls are rejected.

For explicit placement, use `api_execution = "managed_thread"` with `api_cpu_affinity` and/or `api_scheduling_priority`.

## `[webrtc]` Options

### HTTP and API Surface

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `http_host` | string | `"127.0.0.1"` | Address on which the built-in HTTP server listens. Use `0.0.0.0` or `::` only with an appropriate remote-access policy. |
| `http_port` | integer | `8080` | TCP listen port, from `0` through `65535`. Port `0` asks the OS for an ephemeral port and is mainly useful in tests. |
| `enable_http_api` | boolean | `true` | Opens the built-in HTTP/API listener. When false, no built-in discovery, signaling, debug, or runtime-config HTTP routes are available. |
| `enable_debug_api` | boolean | `false` | Enables `/api/video/debug/stats`. Remote route guards still apply. |
| `enable_runtime_config_api` | boolean | `true` | Enables the `GET` and `PUT` runtime-config routes at `/api/video/streams/{stream_id}/output` and the `/config` alias. Other read-only stream routes are unaffected. |
| `execution_mode` | string | `"manual_step"` | Inner WebRTC/HTTP execution mode. A `ManagedVideoServer` requires `manual_step`; `inline_on_push` and `worker_thread` are valid only when constructing `WebRtcVideoServer` directly in C++. |
| `http_poll_timeout_ms` | unsigned integer | `5` | Timeout for one HTTP poll. It must match the top-level `http_poll_timeout_ms` when both appear. |
| `max_http_connections_per_step` | unsigned integer | `1` | Maximum accepted HTTP connections serviced by one API step. Must be at least `1`. |

### Security and CORS

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `allow_unsafe_public_routes` | boolean | `false` | Allows sensitive routes on a non-loopback bind without shared-key authentication or an IP allowlist. This is intended for controlled testing, not an untrusted network. |
| `enable_shared_key_auth` | boolean | `false` | Enables shared-key checks on protected routes. It is effective only when `shared_key` is non-empty. Clients may send `Authorization: Bearer <key>` or `X-Video-Server-Key: <key>`. |
| `shared_key` | string | `""` | Shared secret used when authentication is enabled. Keep secrets out of committed example files. An empty key does not activate a guard. |
| `ip_allowlist` | string array | `[]` | Permitted remote IPv4/IPv6 addresses or CIDRs, for example `["127.0.0.1", "192.168.8.0/24", "2001:db8::/32"]`. Invalid entries make startup fail. An empty list disables this guard. |
| `cors_allowed_origins` | string array | `[]` | Exact browser origins allowed on API responses, such as `"https://viewer.example.com"`. `"*"` allows any origin. With an empty list and a loopback bind, loopback origins are allowed automatically; otherwise no cross-origin response header is added. |

On a non-loopback bind, sensitive routes require at least one of the following unless `allow_unsafe_public_routes` is true:

- a non-empty shared key with authentication enabled;
- a non-empty valid IP allowlist.

See [Security Model](security_model.md) for route-level behavior and deployment guidance.

### Request and Queue Limits

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `max_http_request_bytes` | unsigned integer | `262144` | Maximum complete HTTP request size accepted by the built-in server. |
| `max_json_body_bytes` | unsigned integer | `16384` | Maximum body size for a runtime output-config request. |
| `max_signaling_sdp_bytes` | unsigned integer | `131072` | Maximum SDP offer body size. |
| `max_signaling_candidate_bytes` | unsigned integer | `8192` | Maximum ICE candidate body size. |
| `max_pending_candidates_per_stream` | unsigned integer | `32` | Maximum candidates queued before a signaling session exists. This bounds pre-session memory use. |

### Rate Limits

| Option | Type | Default | Applies to |
| --- | --- | --- | --- |
| `signaling_rate_limit_window_seconds` | unsigned integer | `10` | Signaling rate-limit window. |
| `signaling_rate_limit_max_requests` | unsigned integer | `60` | Maximum signaling requests per client, route, method, and window. |
| `config_rate_limit_window_seconds` | unsigned integer | `10` | Runtime-config mutation rate-limit window. |
| `config_rate_limit_max_requests` | unsigned integer | `20` | Maximum runtime-config mutations per client, route, method, and window. |
| `debug_rate_limit_window_seconds` | unsigned integer | `10` | Debug API rate-limit window. |
| `debug_rate_limit_max_requests` | unsigned integer | `60` | Maximum debug requests per client, route, method, and window. |

Setting either the window or maximum request count to `0` disables that bucket's rate limiting.

### ICE Options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `ice_transport_policy` | string | `"all"` | `all` allows host, server-reflexive, and relay candidates. `relay` requires at least one TURN server and restricts transport to relay candidates. |
| `ice_bind_address` | string | unset | Optional local address used for ICE sockets. `""` clears the setting and lets the WebRTC stack choose. |
| `ice_port_range_begin` | integer | unset | First local ICE UDP port. Must be supplied together with `ice_port_range_end`. |
| `ice_port_range_end` | integer | unset | Last local ICE UDP port. Must be greater than or equal to the beginning. |
| `ice_servers` | array of tables | `[]` | STUN/TURN servers sent to native peer connections and browser clients. Use `[[webrtc.ice_servers]]` entries as described below. |

Each `[[webrtc.ice_servers]]` table accepts:

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `uri` | string | required | URI beginning with `stun:`, `stuns:`, `turn:`, or `turns:`. Transport query parameters such as `?transport=tcp` are allowed. |
| `username` | string | `""` | TURN username. Username and credential must be provided together and are rejected for STUN entries. |
| `credential` | string | `""` | TURN password/credential. Username and credential must be provided together. |

An inline array is equivalent to the array-of-tables form and is convenient for short configurations:

```toml
[webrtc]
ice_servers = [
  { uri = "stun:stun.example.com:3478" },
  { uri = "turns:turn.example.com:443?transport=tcp", username = "user", credential = "secret" }
]
```

Examples:

```toml
[webrtc]
ice_transport_policy = "all"

[[webrtc.ice_servers]]
uri = "stun:stun.example.com:3478"

[[webrtc.ice_servers]]
uri = "turn:turn.example.com:3478?transport=udp"
username = "video-user"
credential = "replace-me"
```

For relay-only operation:

```toml
[webrtc]
ice_transport_policy = "relay"

[[webrtc.ice_servers]]
uri = "turns:turn.example.com:443?transport=tcp"
username = "video-user"
credential = "replace-me"
```

## `[[streams]]` Options

Add one `[[streams]]` table per producer-owned input stream.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `stream_id` | string | required | Stable identifier used in API routes and signaling. It must be non-empty. For HTTP use, keep it to 1–64 ASCII letters, digits, `.`, `_`, or `-`. IDs must be unique. |
| `label` | string | `stream_id` | Human-readable name returned by discovery and observability APIs. |
| `width` | unsigned integer | required | Exact width expected from every pushed raw frame. Must be greater than zero. |
| `height` | unsigned integer | required | Exact height expected from every pushed raw frame. Must be greater than zero. |
| `nominal_fps` | number | required | Declared source cadence and default encoder cadence. Must be greater than zero. It does not schedule the producer for you. |
| `input_pixel_format` | string | `"RGB24"` | Exact raw format expected by `push_frame()`. See the accepted formats below. |
| `max_subscribers` | unsigned integer | `1` | Maximum simultaneous WebRTC recipients. Must be at least `1`. When set to `1`, a new offer replaces the existing single-subscriber session; larger limits use multi-recipient fanout and reject offers above the cap. |

Accepted managed-stream pixel formats are:

| Value | Storage expected by the producer |
| --- | --- |
| `RGB24` | Packed red, green, blue; 3 bytes per pixel. |
| `BGR24` | Packed blue, green, red; 3 bytes per pixel. |
| `RGBA32` | Packed red, green, blue, alpha; 4 bytes per pixel. Alpha is ignored by display transforms. |
| `BGRA32` | Packed blue, green, red, alpha; 4 bytes per pixel. Alpha is ignored. |
| `GRAY8` | One 8-bit luminance sample per pixel. |
| `GRAY10LE` | One little-endian 16-bit word per pixel with 10 significant bits. `GRAY10` is an alias. |
| `GRAY12LE` | One little-endian 16-bit word per pixel with 12 significant bits. `GRAY12` is an alias. |
| `GRAY16LE` | One little-endian 16-bit luminance sample per pixel. `GRAY16` is an alias. |

Pixel-format parsing is case-insensitive and ignores spaces, `_`, and `-`. Although the direct raw encoder API can accept `NV12` and `I420`, the managed stream path rejects them because its display-transform stage does not currently support planar input.

Each frame passed to `push_frame()` must match the configured width, height, and pixel format. Packed formats require a stride at least as large as one packed row.

## `[default_raw_pipelines.*]` Options

These tables configure the raw-frame encoder created for managed streams.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `codec` | string | `"h264"` | Encoded output: `h264`, `H264`, `av1`, or `AV1`. |
| `input_width` | unsigned integer | `0` | Direct-pipeline input width. In managed-server TOML this is accepted but overwritten from the transformed frame width. |
| `input_height` | unsigned integer | `0` | Direct-pipeline input height. In managed-server TOML this is accepted but overwritten from the transformed frame height. |
| `input_pixel_format` | string | `"RGB24"` | Direct-pipeline input format. In managed-server TOML this is accepted but overwritten with `RGB24` after display transformation. |
| `input_fps` | number | `30.0` | Direct-pipeline source cadence. In managed-server TOML this is accepted but overwritten with runtime `output_fps`, or the stream's `nominal_fps` when no runtime limit is set. |
| `scale_mode` | string | `"passthrough"` | `passthrough` or `resize`. In managed-server TOML this is accepted but overwritten with `passthrough`; use the runtime output config to resize managed streams. |
| `output_width` | unsigned integer | unset | Direct-pipeline resize width. Accepted but cleared by the managed server. Direct use with `scale_mode = "resize"` requires both output dimensions to be non-zero. |
| `output_height` | unsigned integer | unset | Direct-pipeline resize height. Accepted but cleared by the managed server. |
| `output_fps` | number | unset | Direct-pipeline encoder throttle. Accepted but cleared by the managed server; use the runtime output config for managed streams. |
| `encoder` | string | `"automatic"` | Encoder selection: `automatic`, `libx264`, `libopenh264`, `libaom-av1`, `vaapi`, or `nvenc`. Compatibility is listed below. |
| `encoder_preset` | string | `"ultrafast"` | Passed to libx264. Passed to NVENC as its preset; the default `ultrafast` is translated to `p1`. Ignored by the other current backends. |
| `encoder_tune` | string | `"zerolatency"` | Passed to libx264. NVENC currently uses its fixed ultra-low-latency tune; other backends ignore this field. |
| `encoder_profile` | string | `"baseline"` | Passed to libx264. Other current backends choose their own profile or ignore this field. |
| `encoder_crf` | unsigned integer | unset | Quality target from `0` through `51`. Used by libx264 when supplied. libaom uses the supplied value or `32` when omitted. Current VA-API, NVENC, and OpenH264 paths do not consume this field. Lower values generally mean higher quality and more data. |
| `hardware_device` | string | `""` | VA-API render node. Empty selects `/dev/dri/renderD128`. It is not used by NVENC. |
| `repeat_headers` | boolean | `true` | Requests repeated H.264 headers from libx264. Other current backends ignore it. |
| `emit_access_unit_delimiters` | boolean | `true` | Requests H.264 AUD NAL units from libx264 and H.264 VA-API. It does not apply to AV1. |

The six direct-pipeline geometry/cadence fields are documented because the TOML parser accepts them, but the managed server intentionally derives its encoder input from the stream's transformed RGB frame. Setting those fields in a managed config does not override the stream contract or runtime output settings.

### Codec and Encoder Compatibility

| `codec` | Valid `encoder` values | `automatic` behavior |
| --- | --- | --- |
| `h264` | `automatic`, `libx264`, `libopenh264`, `vaapi`, `nvenc` | Probes VA-API first, then falls back to software OpenH264. It does not automatically select NVENC. |
| `av1` | `automatic`, `libaom-av1`, `vaapi`, `nvenc` | Probes VA-API, then NVENC. It deliberately does not fall back to CPU-intensive libaom; request `libaom-av1` explicitly. |

Backend requirements:

- `libopenh264` requires OpenH264 in the linked FFmpeg/libav build.
- `libx264` requires an FFmpeg/libav build with libx264. This may affect the licensing obligations of the final binary; it is not enabled by the project's MIT-compatible default dependency set.
- `libaom-av1` is the software AV1 path and is included by the default vcpkg manifests.
- `vaapi` requires a build with the `vaapi` feature, a usable Linux render node, permissions, and a driver supporting the requested codec/profile.
- `nvenc` requires a build with the `nvenc` feature and a compatible NVIDIA driver/GPU. `hardware_device` is not used for this backend.

Examples:

```toml
# AMD Radeon or Intel AV1 through VA-API
[default_raw_pipelines.default]
codec = "av1"
encoder = "vaapi"
hardware_device = "/dev/dri/renderD128"
```

```toml
# NVIDIA AV1 through NVENC
[default_raw_pipelines.default]
codec = "av1"
encoder = "nvenc"
encoder_preset = "p1"
```

```toml
# Hardware-independent software AV1
[default_raw_pipelines.default]
codec = "av1"
encoder = "libaom-av1"
encoder_crf = 32
```

```toml
# One stream uses software AV1 while all others use automatic H.264.
[default_raw_pipelines.default]
codec = "h264"
encoder = "automatic"

[default_raw_pipelines.archive_camera]
codec = "av1"
encoder = "libaom-av1"
encoder_crf = 36
```

## Runtime Per-Stream Output Configuration

The TOML file defines startup, stream contracts, and encoder selection. Display transforms, rotation, resizing, and output throttling are changed at runtime through:

```http
GET /api/video/streams/{stream_id}/output
PUT /api/video/streams/{stream_id}/output
```

`/config` is an alias for `/output`.

### Runtime Fields

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `display_mode` | string | `"Passthrough"` | `Passthrough`, `Grayscale`, `WhiteHot`, `BlackHot`, `Ironbow`, `Rainbow`, or `Arctic`. Common lowercase forms are accepted; `white_hot` and `black_hot` are also accepted. |
| `mirrored` | boolean | `false` | Mirrors the transformed image horizontally. |
| `rotation_degrees` | integer | `0` | Clockwise output rotation. Valid values are `0`, `90`, `180`, and `270`. |
| `palette_min` | number | `0.0` | Lower normalized intensity bound for palette mapping. Normally between `0.0` and `1.0`. Must be less than `palette_max`. |
| `palette_max` | number | `1.0` | Upper normalized intensity bound for palette mapping. Normally between `0.0` and `1.0`. Must be greater than `palette_min`. |
| `output_width` | unsigned integer | `0` | Requested transformed width. Width and height must both be `0`, or both be between `16` and `3840`. |
| `output_height` | unsigned integer | `0` | Requested transformed height. The resize is applied after rotation. |
| `output_fps` | number | `0.0` | `0` uses source cadence. Otherwise use a value from `1.0` through `120.0`. Frames not admitted by the throttle count as dropped output frames. |
| `config_generation` | unsigned integer | `1` | Read-only server-assigned generation. It increments after each successful update and must not be included in a `PUT` body. |

`PUT` uses patch semantics: omitted fields keep their current values. Unknown JSON fields are rejected.

### HTTP Example

Read the current settings:

```bash
curl http://127.0.0.1:8080/api/video/streams/camera_1/output
```

Apply a partial update:

```bash
curl -X PUT http://127.0.0.1:8080/api/video/streams/camera_1/output \
  -H 'Content-Type: application/json' \
  -d '{
    "display_mode": "Ironbow",
    "mirrored": true,
    "rotation_degrees": 90,
    "palette_min": 0.1,
    "palette_max": 0.9,
    "output_width": 640,
    "output_height": 360,
    "output_fps": 12
  }'
```

When an update succeeds:

- `config_generation` increments;
- the next `push_frame()` uses the new transform;
- the cached latest transformed frame is cleared;
- output-FPS throttle state is reset;
- the managed raw encoder is rebuilt on the next admitted frame so its dimensions and cadence match the new output.

During intentional FPS reduction, `frames_received` continues at source rate while `frames_transformed` follows the admitted rate and `frames_dropped` increases. That is expected throttle behavior.

### C++ Example

```cpp
video_server::StreamOutputConfig output;
output.display_mode = video_server::VideoDisplayMode::Ironbow;
output.mirrored = true;
output.rotation_degrees = 90;
output.palette_min = 0.1F;
output.palette_max = 0.9F;
output.output_width = 640;
output.output_height = 360;
output.output_fps = 12.0;

auto result = server->set_stream_output_config("camera_1", output);
if (!result) {
    std::cerr << result.message() << '\n';
}
```

## Startup and Validation Notes

- TOML syntax and recognized value-type errors throw from `load_managed_video_server_config()`.
- Some cross-field and platform checks occur later during `CreateManagedVideoServer()` or `start()`, including inner execution mode, HTTP step budget, allowlist parsing, ICE consistency, thread placement, stream registration, and encoder availability.
- A configuration loading successfully does not prove that the selected hardware encoder or ICE network path is usable. Check the startup result and encoder observability fields.
- Configuration is read at creation time; the managed TOML file is not watched for changes. Restart to apply startup-setting changes.
- Runtime output changes are per stream and do not modify the TOML file.

For route response formats and authentication behavior, continue with [API Routes](api_routes.md) and [Security Model](security_model.md).
