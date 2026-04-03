# Session Lifecycle

This project keeps session behavior explicit and easy to reason about.

## Connect

`POST /api/video/signaling/{stream_id}/offer` creates one WebRTC recipient session for that stream.

On success:

- a new `WebRtcStreamSession` is created
- the response includes a per-recipient `session_id`
- the remote offer is applied
- the backend generates an answer
- that recipient's `session_generation` increments

For concurrent viewers, send `X-Video-Session-Id: {session_id}` on:

- `GET /api/video/signaling/{stream_id}/session`
- `POST /api/video/signaling/{stream_id}/candidate`
- `POST /api/video/signaling/{stream_id}/answer`

If the header is omitted, the API falls back to the newest recipient session for that stream so older single-viewer clients still work.

## Fanout

One stream can now have multiple active WebRTC recipient sessions at the same time.

Important behavior:

- encoding still happens once per stream
- the latest encoded access unit is fanned out to every active recipient
- packetization and transport stay per-recipient because each viewer has its own peer connection and track
- `StreamConfig.max_subscribers` limits concurrent recipients per stream

When the limit is reached:

- new offers are rejected with HTTP `409`
- the signaling error is `max subscribers reached`

## Disconnect And Reuse

When a recipient disconnects:

- that session becomes inactive
- its sender is deactivated
- the slot becomes available for a later subscriber on the same stream

## Reconnect

Reconnect means creating a new recipient session for the same stream after disconnect.

What to look for:

- the new recipient gets a fresh `session_id`
- that recipient's `session_generation` increases
- `disconnect_count` on the old session path increases when it transitions inactive
- the browser gets a fresh answer and candidate flow

The soak runner uses this behavior deliberately to detect unexpected resets versus expected reconnect churn.

## Sender States

Common sender states:

- `waiting-for-encoded-input`
- `video-track-missing`
- `waiting-for-video-track-open`
- `waiting-for-h264-codec-config`
- `waiting-for-h264-keyframe`
- `waiting-for-decoded-startup-idr`
- `sending-h264-rtp`
- `session-inactive`

Practical interpretation:

- waiting states are normal during setup
- `sending-h264-rtp` means the sender is actively packetizing H.264 to the negotiated track
- `session-inactive` means the session has been torn down and should not resume sending

## No-Send-After-Disconnect Guarantee

When a session becomes inactive:

- `active` becomes false
- `sending_active` becomes false
- the encoded sender is deactivated
- the track sink is unbound

The important practical guarantee is that the old session stops sending after disconnect, failure, close, or explicit replacement.

## Useful Debug Fields

Session inspection surfaces:

- `session_generation`
- `active`
- `sending_active`
- `peer_state`
- `sender_state`
- `disconnect_count`
- `last_transition_reason`

Useful endpoint:

```bash
curl -H 'X-Video-Session-Id: session-2' http://127.0.0.1:8080/api/video/signaling/alpha/session
```

If a session is healthy, the usual end state is a move into `sending-h264-rtp`.
