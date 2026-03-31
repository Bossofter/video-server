# Architecture Overview

This project has a simple top-level shape:

```text
raw frames or H.264 access units
            |
            v
  Managed step / execution policy
            |
            +--> HTTP/WebRTC signaling pump
            +--> output FPS cadence decision
            +--> display transform
            +--> H.264 encode
            v
      VideoServerCore
            |
            +--> latest transformed RGB snapshot
            |         |
            |         +--> /api/video/streams/{id}/frame
            |
            +--> latest encoded H.264 snapshot
                      |
                      v
               WebRTC session sender
                      |
                      v
                 browser <video>
```

## Main Pieces

### Managed raw frame ingestion

The preferred path is a managed server with an execution policy. Producers push `VideoFrameView` frames ad hoc, and `step()` progresses the server. In `ManualStep`, no internal worker thread is required.

### Filter and output stage

During `step()`, the managed server:

- services pending HTTP/signaling work
- checks per-stream output timing
- picks the latest pending raw frame for ready streams
- applies the current `StreamOutputConfig`
- publishes the transformed RGB result as the canonical latest-frame snapshot
- encodes H.264 from that same transformed result

This keeps debug/latest-frame output aligned with encoded output.

### Encoded access units

Encoded H.264 data is stored as the latest immutable access-unit snapshot per stream. The WebRTC sender consumes those snapshots and packetizes them for browser delivery.

### WebRTC sender and session

Each stream currently has one active session slot. A new offer replaces the previous session for that stream and increments `session_generation`.

The session owns:

- a `rtc::PeerConnection`
- a negotiated H.264 video track
- sender state and counters
- the bridge to the latest encoded access-unit snapshot

### Browser playback

The browser uses the HTTP signaling API to exchange SDP and ICE, then plays the remote H.264 track in a normal `<video>` element. The NiceGUI harness is the main browser-side debug client in this repo.

## Design Intent

- Keep producer-facing APIs stable
- Keep raw ingestion separate from encoded delivery
- Make runtime config per-stream and observable
- Keep browser validation easy with the harness and soak runner

## Current High-Level Limits

- one active WebRTC session slot per stream
- H.264 is the encoded media path
- the first raw pipeline expects tightly packed raw buffers
