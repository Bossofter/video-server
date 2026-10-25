# video-server

Current version: `1.3.0`

`video-server` is a C++17 library and smoke-test harness for getting producer-owned raw frames into browser-playable WebRTC video.

The main path is:

```text
raw frames
    -> transform/output config
    -> H.264 encode
    -> bounded access-unit handoff
    -> WebRTC signaling/fanout
    -> browser <video>
```

## Documentation

- Build and first run: [docs/getting_started.md](docs/getting_started.md)
- Integrate your own raw-frame producer: [docs/raw_frames_to_webrtc.md](docs/raw_frames_to_webrtc.md)
- HTTP API and signaling routes: [docs/api_routes.md](docs/api_routes.md)
- Runtime config fields: [docs/configuration_and_filters.md](docs/configuration_and_filters.md)
- Session behavior: [docs/session_lifecycle.md](docs/session_lifecycle.md)
- Security defaults: [docs/security_model.md](docs/security_model.md)
- Soak testing: [docs/soak_testing.md](docs/soak_testing.md)
- NiceGUI harness details: [examples/nicegui_smoke/README.md](examples/nicegui_smoke/README.md)

## Quick Build

```bash
export VCPKG_ROOT=/path/to/vcpkg
./build.sh
```

The supported first-run workflow is in [docs/getting_started.md](docs/getting_started.md).

## CMake Package Consumption

After installing the project, downstream consumers can use the exported config package:

```cmake
find_package(video-server CONFIG REQUIRED)
target_link_libraries(test_app PRIVATE video_server::video_server)
```

The repository also includes a local vcpkg overlay port in `vcpkg/` for packaging the current checkout:

```bash
vcpkg install video-server --overlay-ports="${PWD}/vcpkg"
vcpkg install 'video-server[vaapi]' --overlay-ports="${PWD}/vcpkg"
```

`video-server[vaapi]` enables Linux VA-API H.264 and AV1 hardware encoding for supported AMD Radeon and Intel devices. `video-server[nvenc]` enables NVIDIA H.264 and AV1 NVENC; both features can be selected together. Software H.264 installs use OpenH264 or explicit libx264 where available, and software AV1 uses libaom.

AV1 is selected per raw pipeline with `codec = "av1"` and `encoder = "libaom-av1"`, `"vaapi"`, `"nvenc"`, or `"automatic"`. Use `libaom-av1` for an explicit hardware-independent software path. Automatic AV1 selection still tries VA-API first and then NVENC, and fails with probe diagnostics rather than silently consuming substantial CPU. AV1 WebRTC output uses the standardized AV1 RTP payload and is negotiated only when the browser offers AV1.

The project remains MIT licensed. The AV1 paths use FFmpeg/libav with the BSD-licensed libaom library or hardware wrappers and do not add GPL codec libraries; NVIDIA's driver is a system/runtime prerequisite and is not distributed by this project.

When video-server is linked against vcpkg's libva but uses a distribution-provided VA driver, libva may need its system driver directory explicitly:

```bash
export LIBVA_DRIVERS_PATH=/usr/lib/$(gcc -print-multiarch)/dri
```

The NiceGUI smoke launcher discovers common system driver directories automatically. `LIBVA_DRIVER_NAME=radeonsi` is normally unnecessary; use it only to override libva's driver selection deliberately.

## License

[MIT](LICENSE)
