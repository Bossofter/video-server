# video-server

`video-server` is a C++17 video streaming server for browser playback. It accepts raw frames or pre-encoded H.264 access units, applies per-stream runtime output settings, and serves the result over WebRTC with a lightweight HTTP control API.

The repo is aimed at practical development workflows:

- start a synthetic multi-stream server quickly
- view streams in a browser with the NiceGUI harness
- test on the same machine or across a LAN
- exercise reconnect and config churn with the soak runner

## Features

- Raw `VideoFrameView` ingestion with minimal-copy producer-facing APIs
- Raw -> transformed RGB -> H.264 -> WebRTC browser delivery path
- Direct encoded H.264 access-unit ingestion through `push_access_unit(...)`
- Multiple concurrent streams
- Per-stream runtime output config:
  - display/filter mode
  - output width and height
  - output FPS throttling
- Correct session lifecycle handling with explicit session generations
- Practical security defaults:
  - loopback bind by default
  - debug API disabled by default
  - optional shared-key auth
  - optional IP allowlist
- NiceGUI debug harness for browser-side validation
- LAN smoke testing flow
- Soak runner with reconnect churn, config churn, metrics, JSON, and CSV reports

## Quick Start

Fastest path from clone to browser video:

```bash
git clone <your-fork-or-copy> video-server
cd video-server

export VCPKG_ROOT=/path/to/vcpkg
./build.sh

python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt

python examples/nicegui_smoke/app.py --start-server --multi-stream-demo --auto-connect
```

Open `http://127.0.0.1:8090/`.

Expected result:

- the NiceGUI page loads
- the harness starts the synthetic WebRTC smoke server on `127.0.0.1:8080`
- the browser auto-connects to the default stream
- video appears in the page

If `VCPKG_ROOT` is unset and `/opt/vcpkg` exists, `./build.sh` uses `/opt/vcpkg` automatically.

## Minimal Commands

Build only:

```bash
./build.sh
```

Run the synthetic smoke server directly:

```bash
./build/video_server_nicegui_smoke_server --host 127.0.0.1 --port 8080 --multi-stream-demo
```

Run the NiceGUI harness against an already-running server:

```bash
python examples/nicegui_smoke/app.py \
  --video-server-url http://127.0.0.1:8080 \
  --multi-stream-demo \
  --auto-connect
```

Run the soak runner:

```bash
./build/video_server_soak_runner \
  --duration 1m \
  --summary-interval 5s \
  --poll-interval 1s \
  --json-report build/soak-report.json \
  --csv-report build/soak-report.csv
```

## Build And Test

Supported build flow:

```bash
export VCPKG_ROOT=/path/to/vcpkg
./build.sh
./test.sh
./test_pipeline.sh
```

The project requires a media-enabled `libdatachannel` build. With vcpkg that means `libdatachannel[srtp]`.

## Common Workflows

### Run local streaming

```bash
python examples/nicegui_smoke/app.py --start-server --multi-stream-demo --auto-connect
```

Open `http://127.0.0.1:8090/`.

### Run LAN streaming

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --multi-stream-demo \
  --auto-connect \
  --lan-only
```

Open `http://<host-lan-ip>:8090/` from another machine on the same network.

`--lan-only` is for development smoke testing. It enables permissive CORS and remote access to signaling, runtime config, and debug routes on the launched smoke server.

### Protect the server with a shared key

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --multi-stream-demo \
  --auto-connect \
  --shared-key dev-secret
```

The harness forwards `Authorization: Bearer dev-secret` automatically.

## HTTP API At A Glance

- `GET /api/video/streams`
- `GET /api/video/streams/{stream_id}`
- `GET /api/video/streams/{stream_id}/output`
- `PUT /api/video/streams/{stream_id}/output`
- `GET /api/video/streams/{stream_id}/frame`
- `POST /api/video/signaling/{stream_id}/offer`
- `POST /api/video/signaling/{stream_id}/answer`
- `POST /api/video/signaling/{stream_id}/candidate`
- `GET /api/video/signaling/{stream_id}/session`
- `GET /api/video/debug/stats` when enabled

The runtime config route also accepts `/config` as a compatibility alias. The docs use `/output`.

## Documentation

- [Getting Started](docs/getting_started.md)
- [Architecture Overview](docs/architecture_overview.md)
- [Configuration And Filters](docs/configuration_and_filters.md)
- [Session Lifecycle](docs/session_lifecycle.md)
- [Security Model](docs/security_model.md)
- [Soak Testing](docs/soak_testing.md)
- [NiceGUI Harness Notes](examples/nicegui_smoke/README.md)

## Current Limits

- One active WebRTC session slot per stream
- H.264 is the encoded browser delivery path today
- Raw pipeline input currently expects tightly packed input buffers
- `--lan-only` is for trusted LAN development, not Internet exposure
- Shared-key auth is a simple route guard, not a full user/authz system
