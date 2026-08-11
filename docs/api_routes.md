# API Routes

This is the HTTP surface exposed by the WebRTC server. The API is intentionally small: stream discovery, latest-frame inspection, runtime output config, WebRTC signaling, and debug stats.

Examples below assume:

```text
http://127.0.0.1:8080
```

## Security

Public stream discovery is available through:

```http
GET /api/video/streams
GET /api/video/streams/{stream_id}
```

Sensitive routes include signaling, runtime config, latest-frame image access, and debug stats. When the server is bound beyond loopback, sensitive routes require one of the configured guards:

- shared-key auth
- IP allowlist
- `allow_unsafe_public_routes = true` for trusted smoke-test networks only

Shared-key requests use:

```http
Authorization: Bearer <shared-key>
```

CORS preflight is supported for API paths with:

```http
OPTIONS /api/...
```

## Streams

### List Streams

```http
GET /api/video/streams
```

Returns registered stream ids, labels, and active state.

```json
{
  "streams": [
    {
      "stream_id": "alpha",
      "label": "Alpha",
      "active": true
    }
  ]
}
```

### Get Stream Info

```http
GET /api/video/streams/{stream_id}
```

Returns counters and latest-frame/latest-access-unit metadata for one stream.

Important fields include:

- `frames_received`
- `frames_transformed`
- `frames_dropped`
- `access_units_received`
- `has_latest_frame`
- `has_latest_encoded_unit`
- `latest_encoded_codec`
- `latest_encoded_sequence_id`
- `latest_encoded_keyframe`
- `output_config`

### Get Latest Frame Image

```http
GET /api/video/streams/{stream_id}/frame
```

Returns the latest raw frame as `image/x-portable-pixmap` for diagnostics. This is not the WebRTC video path.

Common responses:

- `200 OK`: latest frame encoded as PPM
- `404 Not Found`: stream or latest frame is missing

## Runtime Output Config

The `/output` and `/config` suffixes are aliases for the same runtime output configuration.

### Read Output Config

```http
GET /api/video/streams/{stream_id}/output
GET /api/video/streams/{stream_id}/config
```

Returns the current per-stream output configuration.

### Update Output Config

```http
PUT /api/video/streams/{stream_id}/output
PUT /api/video/streams/{stream_id}/config
Content-Type: application/json
```

Supported JSON fields:

- `display_mode`: `passthrough`, `white_hot`, `black_hot`, `rainbow`, or project aliases
- `mirrored`: boolean
- `rotation_degrees`: integer rotation accepted by the transform path
- `palette_min`: number
- `palette_max`: number
- `output_width`: integer
- `output_height`: integer
- `output_fps`: number

Example:

```bash
curl -X PUT \
  -H 'Content-Type: application/json' \
  --data '{"display_mode":"white_hot","output_fps":30}' \
  http://127.0.0.1:8080/api/video/streams/alpha/output
```

Invalid JSON, unknown fields, unsupported display modes, invalid dimensions, and invalid FPS values return `400 Bad Request`.

## WebRTC Signaling

Signaling routes are per stream and support multiple viewer sessions. Responses that create or address a session include or accept:

```http
X-Video-Session-Id: <session-id>
```

Most clients use:

1. `GET /ice-config`
2. `POST /offer`
3. `GET /session`
4. `POST /candidate` as ICE candidates are gathered
5. repeat `GET /session` until the answer/candidates are available

### ICE Config

```http
GET /api/video/signaling/{stream_id}/ice-config
```

Returns configured ICE servers and transport policy.

### Submit Offer

```http
POST /api/video/signaling/{stream_id}/offer
Content-Type: application/sdp
```

Body is the browser offer SDP. On success:

```json
{
  "ok": true,
  "session_id": "session-1"
}
```

Common errors:

- `404 Not Found`: stream does not exist
- `409 Conflict`: max subscribers reached
- `413 Payload Too Large`: offer exceeds configured limit

### Submit Answer

```http
POST /api/video/signaling/{stream_id}/answer
POST /api/video/signaling/{stream_id}/answer/{session_id}
```

Body is SDP. The session may be selected by the path or by `X-Video-Session-Id`.

### Submit ICE Candidate

```http
POST /api/video/signaling/{stream_id}/candidate
POST /api/video/signaling/{stream_id}/candidate/{session_id}
Content-Type: application/json
```

Body is the candidate payload. The session may be selected by the path or by `X-Video-Session-Id`.

### Read Session

```http
GET /api/video/signaling/{stream_id}/session
GET /api/video/signaling/{stream_id}/session/{session_id}
```

Returns the signaling session state, answer SDP when available, queued candidates, and sender/session counters.

## Debug Stats

```http
GET /api/video/debug/stats
```

Requires `enable_debug_api = true` and the normal sensitive-route access rules.

Returns:

- total stream count
- active session count
- security counters
- per-stream debug snapshots
- encoder observability, including requested encoder, active encoder, hardware acceleration state, hardware device, and VA-API fallback reason when applicable

## Status Codes

Common status codes:

- `200 OK`: request succeeded
- `400 Bad Request`: malformed body, invalid config, invalid stream id, or invalid signaling payload
- `401 Unauthorized`: missing or wrong shared key
- `403 Forbidden`: remote address blocked or sensitive route unavailable
- `404 Not Found`: route, stream, frame, or session not found
- `409 Conflict`: subscriber limit reached
- `413 Payload Too Large`: body exceeds configured size limit
- `429 Too Many Requests`: rate limit exceeded
- `500 Internal Server Error`: internal handler or latest-frame encoding failure
