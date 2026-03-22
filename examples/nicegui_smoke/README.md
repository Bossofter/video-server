# NiceGUI browser harness

This example is a **manual development/debug harness** for the browser-facing H264 WebRTC path.

It remains intentionally separate from the reusable server core:

- the C++ smoke server executable starts `WebRtcVideoServer`
- registers one or more synthetic streams
- pushes raw synthetic frames for server-side observability
- runs the shared raw→H264 in-process libav pipeline per stream
- feeds those H264 access units into the existing `push_access_unit()` path
- the NiceGUI page consumes the stream(s) using the existing HTTP signaling API and native browser `<video>` elements
- all browser debug UX, reconnect logic, telemetry, and console hooks live in `examples/nicegui_smoke/app.py`

## Quick start

From the repo root:

```bash
./build.sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt
python examples/nicegui_smoke/app.py --start-server --multi-stream-demo --auto-connect --debug
```

Open:

```text
http://127.0.0.1:8090/
```

## Default multi-stream demo set

By default, the smoke harness now launches a three-stream demo unless you explicitly pass single-stream sizing/id flags. `--multi-stream-demo` selects the same default set explicitly:

- `alpha`: `640x360 @ 30 fps` with the sweep-style synthetic pattern
- `bravo`: `1280x720 @ 30 fps` with the orbit-style synthetic pattern
- `charlie`: `320x240 @ 30 fps` with the checker-style synthetic pattern

Each stream gets its own raw-frame generator, libav-backed raw pipeline, server registration, and WebRTC signaling/session slot.

## Custom multi-stream launch

You can provide repeatable `--stream` arguments instead of the default demo set:

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --stream alpha:640:360:30:Alpha-Sweep \
  --stream bravo:1280:720:30:Bravo-Orbit \
  --stream charlie:320:240:30:Charlie-Checker \
  --auto-connect --debug
```

The smoke tab now also includes a dropdown selector so you can point the full debug player at any configured stream before connecting or reconnecting.

Stream format:

```text
--stream STREAM_ID:WIDTH:HEIGHT:FPS[:LABEL]
```

## Harness capabilities

The page is now a more useful day-to-day debug client instead of a basic smoke test:

- **Two views:** a full **Smoke debug** tab and a **Widget preview** tab.
- **Manual multi-stream compare:** when multiple streams are configured, the widget tab renders multiple isolated widget instances side-by-side in iframes so each stream keeps its own browser peer connection and local state.
- **Per-widget stream labeling:** each comparison card shows the stream id, label, geometry, and expected FPS.
- **Widget debug settings:** widget config now includes expected FPS alongside connection/playback/session debug toggles.
- **Config persistence:** server URL, stream ID, debug mode, reconnect mode, poll interval, and widget settings are saved in browser storage.
- **Connection controls:** explicit **Connect**, **Reconnect**, **Disconnect**, and **Refresh debug** buttons.
- **Summary row:** quick status badges for connection, remote track, playback, offer/answer, candidate handling, and generation in the smoke tab.
- **Debug telemetry panel:** shows peer connection state, signaling state, ICE state, selected stream/server, remote description/track status, playback state, session summary, and browser stats.
- **Logs:** timestamped category logs with filtering and copy-to-clipboard support.
- **Console hooks in debug mode:** `window.__videoSmokePc`, `window.__videoSmokeVideo`, `window.__videoSmokeState`, and `window.__videoSmokeStats()`.

## Run against an already-running server

If you already launched the smoke server yourself:

```bash
./build/video_server_nicegui_smoke_server --multi-stream-demo
python examples/nicegui_smoke/app.py --video-server-url http://127.0.0.1:8080 --stream alpha:640:360:30:Alpha-Sweep --stream bravo:1280:720:30:Bravo-Orbit --stream charlie:320:240:30:Charlie-Checker --auto-connect
```

## Caveats

- This is a debug harness, not a production UI.
- It targets local/manual validation.
- It keeps signaling intentionally simple and primarily relies on SDP exchange on localhost.
- The current backend still exposes one active WebRTC session slot per stream.
- Session and stats polling are lightweight but still periodic; increase the poll interval if you want less churn.
