# Architecture Overview

This project has a simple top-level shape:

```text
raw frames or H.264 access units
            |
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

### Raw frame ingestion

Producers can push `VideoFrameView` frames into the server. The core validates the stream, validates the frame shape, applies the active `StreamOutputConfig`, and stores the latest transformed RGB snapshot.

### Filter and output stage

The transform stage applies the current per-stream output settings:

- display mode
- mirroring
- rotation
- palette range
- output resize
- output FPS throttling

These settings affect the next admitted frame for that stream.

### Raw -> H.264 pipeline

For browser playback, the repo includes a raw-to-H.264 pipeline:

```text
raw input -> transform result -> H.264 encoder -> Annex-B access units -> push_access_unit(...)
```

The synthetic smoke server uses this path today.

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
