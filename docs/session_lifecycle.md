# Session Lifecycle

This project keeps session behavior explicit and easy to reason about.

## Connect

`POST /api/video/signaling/{stream_id}/offer` creates or replaces the current WebRTC session for that stream.

On success:

- a new `WebRtcStreamSession` is created
- the remote offer is applied
- the backend generates an answer
- the stream's `session_generation` increments

## Disconnect And Replace

There is currently one active session slot per stream.

That means:

- one stream can have multiple streams overall, but only one live peer session per stream
- a new offer for the same stream replaces the previous session
- when a session is replaced or the peer disconnects, the old sender is deactivated

## Reconnect

Reconnect means creating a new session for the same stream after disconnect or replacement.

What to look for:

- `session_generation` increases
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
curl http://127.0.0.1:8080/api/video/signaling/alpha/session
```

If a session is healthy, the usual end state is a move into `sending-h264-rtp`.
