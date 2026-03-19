# NiceGUI smoke harness

This example is a **manual smoke-test harness** for the current browser-facing H264 WebRTC path.

It is intentionally separate from the reusable server core:

- the C++ smoke server executable starts `WebRtcVideoServer`
- registers a synthetic stream
- pushes raw synthetic frames for server-side observability
- launches `ffmpeg` (via `imageio-ffmpeg`) to generate a moving H264 `testsrc` stream
- feeds those H264 access units into the existing `push_access_unit()` path
- the NiceGUI page consumes the stream using the existing HTTP signaling API and a native browser `<video>` element

## Quick start

From the repo root:

```bash
./build.sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt
python examples/nicegui_smoke/app.py --start-server
```

Open:

```text
http://127.0.0.1:8090/
```

When working, you should see:

- a moving browser video generated from the WebRTC H264 path
- log lines showing offer/answer progress and connection state changes
- the stream sourced from the synthetic smoke server stream id `synthetic-h264`

## Run against an already-running server

If you already launched the smoke server yourself:

```bash
./build/video_server_nicegui_smoke_server --ffmpeg "$(python -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')"
python examples/nicegui_smoke/app.py --video-server-url http://127.0.0.1:8080 --stream-id synthetic-h264
```

## Caveats

- This is a smoke harness, not a production UI.
- It targets local/manual validation.
- It keeps signaling intentionally simple and primarily relies on SDP exchange on localhost.
- The current backend still exposes one active WebRTC session slot per stream.
