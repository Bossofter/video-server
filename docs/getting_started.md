# Getting Started

This guide gets a new developer from a fresh clone to visible browser video.

## Prerequisites

- C++17 toolchain
- CMake
- Python 3
- `vcpkg`
- Media-enabled `libdatachannel` via vcpkg: `libdatachannel[srtp]`

The supported build flow uses the vcpkg CMake toolchain.

## 1. Build The Repo

```bash
git clone https://github.com/Bossofter/video-server
cd video-server

export VCPKG_ROOT=/path/to/vcpkg
./build.sh
```

If `/opt/vcpkg` exists, `./build.sh` uses it automatically when `VCPKG_ROOT` is unset.

## 2. Fastest End-To-End Flow

This is the recommended first run because it starts both the C++ smoke server and the NiceGUI browser harness.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r examples/nicegui_smoke/requirements.txt

python examples/nicegui_smoke/app.py --start-server --multi-stream-demo --auto-connect
```

Open:

```text
http://127.0.0.1:8090/
```

What happens:

- the NiceGUI app starts on port `8090`
- the synthetic WebRTC smoke server starts on port `8080`
- three demo streams are registered: `alpha`, `bravo`, `charlie`
- the browser harness connects automatically

## 3. Verify That Video Works

Use this checklist:

- the page loads without a server error
- the status row shows a connected session
- the remote video element leaves the placeholder state
- the widget or smoke tab starts rendering video
- the selected stream reports an increasing session/debug state instead of staying idle

If you want a simple API check before opening the page:

```bash
curl http://127.0.0.1:8080/api/video/streams
```

You should get a JSON list of the registered demo streams.

## 4. Run The Smoke Server Manually

Use this when you want the server and browser harness in separate terminals.

Terminal 1:

```bash
./build/video_server_nicegui_smoke_server \
  --config examples/nicegui_smoke/smoke_server.toml \
  --host 127.0.0.1 \
  --port 8080
```

Terminal 2:

```bash
source .venv/bin/activate
python examples/nicegui_smoke/app.py \
  --video-server-url http://127.0.0.1:8080 \
  --multi-stream-demo \
  --auto-connect
```

Open `http://127.0.0.1:8090/`.

## 5. Connect To A Specific Stream

The default demo streams are:

- `alpha`: `640x360 @ 30 fps`
- `bravo`: `1280x720 @ 30 fps`
- `charlie`: `320x240 @ 30 fps`

In the NiceGUI page:

- pick the stream from the selector
- click `Connect` if auto-connect is disabled
- use `Reconnect` after switching streams

## 6. Run LAN Testing

Launch the harness and the synthetic server in LAN smoke mode:

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --multi-stream-demo \
  --auto-connect \
  --lan-only
```

Then open the UI from another machine:

```text
http://<host-lan-ip>:8090/
```

Notes:

- the NiceGUI app binds to `0.0.0.0` by default
- the smoke server binds to `127.0.0.1` by default when started from the harness
- `--lan-only` automatically widens the launched smoke server bind to `0.0.0.0` unless you explicitly pass `--server-host`
- use an explicit non-loopback `--server-host` and matching non-loopback `--video-server-url` only when you intentionally want LAN access
- the smoke server loads `examples/nicegui_smoke/smoke_server.toml` by default and then applies CLI overrides
- `--lan-only` enables remote signaling, runtime config, and debug access for LAN smoke testing
- do not use this mode as a production security model

## 7. Use A Shared Key

Protect sensitive routes with a shared key:

```bash
python examples/nicegui_smoke/app.py \
  --start-server \
  --multi-stream-demo \
  --auto-connect \
  --shared-key dev-secret
```

The harness sends the key on signaling, config, and debug requests automatically.

Manual API example:

```bash
curl \
  -H 'Authorization: Bearer dev-secret' \
  http://127.0.0.1:8080/api/video/streams/alpha/output
```

## 8. Troubleshooting

Build fails early:

- confirm `VCPKG_ROOT` points to a valid vcpkg checkout
- confirm the toolchain file exists at `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
- confirm `libdatachannel` includes SRTP/media support

The harness starts but there is no video:

- check that `http://127.0.0.1:8080/api/video/streams` returns JSON
- check that the selected stream exists
- use the smoke tab and refresh debug data
- confirm the session moves past `waiting-for-video-track-open`

Another machine cannot connect in LAN mode:

- use the host machine's LAN IP, not `127.0.0.1`
- start with `--lan-only`
- confirm the two machines are on the same network and ports `8080` and `8090` are reachable

Next documents:

- [Configuration And Filters](configuration_and_filters.md)
- [Session Lifecycle](session_lifecycle.md)
- [Security Model](security_model.md)
- [Soak Testing](soak_testing.md)
