# video-server

`video-server` is a C++17 server for turning raw frames or H.264 access units into browser-playable WebRTC streams.

The repo provides:

- a producer-facing API for registering streams and pushing frames
- a raw -> H.264 -> WebRTC delivery path
- per-stream runtime output configuration
- a lightweight HTTP and signaling surface
- example harnesses for browser, LAN, and soak validation

## Start Here

- Build and first run: [docs/getting_started.md](docs/getting_started.md)
- Integrate your own raw-frame producer: [docs/raw_frames_to_webrtc.md](docs/raw_frames_to_webrtc.md)
- Runtime config and filters: [docs/configuration_and_filters.md](docs/configuration_and_filters.md)
- Session behavior: [docs/session_lifecycle.md](docs/session_lifecycle.md)
- Security defaults and LAN exposure: [docs/security_model.md](docs/security_model.md)
- Soak testing: [docs/soak_testing.md](docs/soak_testing.md)
- Architecture overview: [docs/architecture_overview.md](docs/architecture_overview.md)
- NiceGUI harness details: [examples/nicegui_smoke/README.md](examples/nicegui_smoke/README.md)

## Quick Build

```bash
export VCPKG_ROOT=/path/to/vcpkg
./build.sh
```

The supported build and first-run workflow is in [docs/getting_started.md](docs/getting_started.md).

## License

[MIT](LICENSE)
