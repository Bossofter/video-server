# NiceGUI browser harness

This example is a **manual development/debug harness** for the browser-facing H264 WebRTC path.

It remains intentionally separate from the reusable server core:

- the C++ smoke server executable starts `WebRtcVideoServer`
- registers a synthetic stream
- pushes raw synthetic frames for server-side observability
- launches `ffmpeg` (via `imageio-ffmpeg`) to generate a moving H264 `testsrc` stream
- feeds those H264 access units into the existing `push_access_unit()` path
- the NiceGUI page consumes the stream using the existing HTTP signaling API and a native browser `<video>` element
- all browser debug UX, reconnect logic, telemetry, and console hooks live in `examples/nicegui_smoke/app.py`

## Quick start

From the repo root:

```bash
./build.sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt
python examples/nicegui_smoke/app.py --start-server --auto-connect --debug
```

Open:

```text
http://127.0.0.1:8090/
```

## What changed in the harness

The page is now a more useful day-to-day debug client instead of a basic smoke test:

- **Two views:** a full **Smoke debug** tab and a compact **Widget preview** tab for the future embeddable box layout.
- **Settings UI:** use the visible **Settings** button or the widget preview right-click menu.
- **Config persistence:** server URL, stream ID, debug mode, reconnect mode, and poll interval are saved in browser storage.
- **Connection controls:** explicit **Connect**, **Reconnect**, **Disconnect**, and **Refresh debug** buttons.
- **Summary row:** quick status badges for connection, remote track, playback, offer/answer, candidate handling, and generation in the smoke tab.
- **Widget preview box:** keeps the video plus its action buttons/debug badges inside a single div for future dynamic placement work.
- **Debug telemetry panel:** shows peer connection state, signaling state, ICE state, selected stream/server, remote description/track status, playback state, session summary, and browser stats.
- **Video state widget:** shows `readyState`, `paused`, `currentTime`, rendered size, `networkState`, and mute state.
- **Logs:** timestamped category logs with filtering and copy-to-clipboard support.
- **Console hooks in debug mode:** `window.__videoSmokePc`, `window.__videoSmokeVideo`, `window.__videoSmokeState`, and `window.__videoSmokeStats()`.

## Run against an already-running server

If you already launched the smoke server yourself:

```bash
./build/video_server_nicegui_smoke_server --ffmpeg "$(python -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')"
python examples/nicegui_smoke/app.py --video-server-url http://127.0.0.1:8080 --stream-id synthetic-h264 --auto-connect
```

## Settings panel

Open the settings panel either by:

- clicking **Settings** in the toolbar, or
- right-clicking the **Widget preview** video box to open the grouped context menu and then jumping into local settings.

Current controls include:

- server URL
- stream ID
- debug mode toggle
- auto reconnect toggle
- auto connect on reload toggle
- session poll interval
- log filter
- placeholder controls for future verbosity / display options

The settings are saved in browser local storage so a page reload keeps the previous session target and behavior.


## Widget preview tab

The **Widget preview** tab is a compact single-box layout intended to mirror the future embeddable widget form:

- the video and its action buttons stay inside one container
- the base display only keeps a simple health indicator and the stream ID
- additional in-box debug details are controlled from settings
- right-clicking the widget opens grouped menu sections for:
  - connection actions (`Connect`, `Reload / reconnect`, `Disconnect`, `Refresh`)
  - local widget settings (`Open settings panel`, `Toggle in-box debug`)
  - placeholder server-request grouping for future stream-setting calls

Widget debug controls now support:

- enable/disable debug info inside the widget box
- debug level selection: `Basic`, `Detailed`, `Full`
- field toggles for connection/signaling, playback/track, video element details, and session summary data

The original full **Smoke debug** tab remains available for the all-info smoke-test workflow.

## Debug telemetry

When **debug mode** is enabled, the collapsible debug panel shows:

- `connectionState`
- `iceConnectionState`
- `iceGatheringState`
- `signalingState`
- current connection/session generation
- selected server URL + stream ID
- offer/answer status
- candidate exchange status
- whether the remote description has been applied
- whether a remote track was received
- whether playback appears active
- selected backend candidate observation
- selected fields from the server session JSON
- browser `getStats()` summary for inbound video packets/bytes/frames/codec/resolution/FPS

## Reconnect / disconnect workflow

- **Connect** starts a new browser peer connection using the saved settings.
- **Reconnect** clears stale browser state first, then builds a fresh peer connection.
- **Disconnect** tears down the active browser peer connection, clears the `<video>` element, and resets the runtime telemetry.
- **Refresh debug** manually polls session info and browser stats without forcing a reconnect.
- If **auto reconnect** is enabled, the harness will retry after connection failures/disconnects.

## Browser console helpers

When debug mode is enabled, the page exposes:

- `window.__videoSmokePc` → active `RTCPeerConnection`
- `window.__videoSmokeVideo` → active `<video>` element
- `window.__videoSmokeState` → internal harness state object
- `window.__videoSmokeStats()` → async helper returning the summarized `getStats()` snapshot

These are intentionally debug-only helpers for manual browser investigation.

## Caveats

- This is a debug harness, not a production UI.
- It targets local/manual validation.
- It keeps signaling intentionally simple and primarily relies on SDP exchange on localhost.
- The current backend still exposes one active WebRTC session slot per stream.
- Session and stats polling are lightweight but still periodic; increase the poll interval if you want less churn.
