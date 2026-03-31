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
python examples/nicegui_smoke/app.py --start-server --multi-stream-demo --auto-connect --debug --lan-only
```

Open:

```text
http://<smoke-host-lan-ip>:8090/
```

The smoke harness keeps the NiceGUI app on `0.0.0.0`, but the launched synthetic WebRTC server now defaults to `127.0.0.1` so local signaling works with the repo's safe security defaults. When `--lan-only` is set, the harness automatically widens the launched smoke-server bind to `0.0.0.0` unless you explicitly override `--server-host`.

If you enable shared-key protection on the server, launch the harness with `--shared-key YOUR_TOKEN`. When `--start-server` is used, the harness passes the same token to the smoke server and includes `Authorization: Bearer YOUR_TOKEN` on signaling, config, and debug requests. `--debug` also enables the smoke server's debug endpoint explicitly, which now defaults to disabled.

## Default multi-stream demo set

By default, the smoke harness launches the managed smoke server with `examples/nicegui_smoke/smoke_server.toml`. That file defines the default three-stream demo. `--multi-stream-demo` selects the same set explicitly:

- `alpha`: `640x360 @ 30 fps` with the sweep-style synthetic pattern generated as `GRAY8` so grayscale/palette config changes have a known-good demo stream
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
  --auto-connect --debug --lan-only
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
- It targets local/manual validation, including same-LAN smoke testing.
- `--lan-only` enables permissive CORS and unsafe public routes on the synthetic smoke server; do not treat that configuration as production-safe.
- The current backend still exposes one active WebRTC session slot per stream.
- Session and stats polling are lightweight but still periodic; increase the poll interval if you want less churn.
