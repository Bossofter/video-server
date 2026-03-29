# Configuration And Filters

Per-stream runtime output config controls how raw input is transformed before it is exposed as the latest frame and before the smoke server rebuilds its encoded pipeline.

## Fields

`StreamOutputConfig` supports:

- `display_mode`
- `mirrored`
- `rotation_degrees`
- `palette_min`
- `palette_max`
- `output_width`
- `output_height`
- `output_fps`
- `config_generation`

## Display Modes

Supported display modes:

- `Passthrough`
- `Grayscale`
- `WhiteHot`
- `BlackHot`
- `Ironbow`
- `Rainbow`
- `Arctic`

Practical meaning:

- `Passthrough` keeps normal color presentation
- `Grayscale` converts to grayscale output
- palette modes map intensity into a false-color palette

`palette_min` and `palette_max` matter for intensity-mapped output such as `WhiteHot`, `BlackHot`, `Ironbow`, `Rainbow`, and `Arctic`. The server requires `palette_max > palette_min`.

## Output Size

`output_width` and `output_height` control the transformed output size.

- `0` and `0` mean no explicit resize
- non-zero values must be in the valid server range
- the current core validation accepts either both zero or both set to valid dimensions

In the smoke server path, changing the effective transformed size causes the per-stream raw-to-H.264 pipeline to be rebuilt on the next frame for that stream.

## Output FPS

`output_fps` controls output throttling.

- `0` means use the source cadence
- non-zero values must be valid and finite
- the core drops transformed output frames as needed to honor the target FPS

Expected behavior:

- `frames_received` keeps increasing at source rate
- `frames_transformed` increases at the admitted output rate
- `frames_dropped` increases when throttling is active

This is normal and expected during FPS reduction.

## Reconfigure Semantics

Runtime config changes are immediate for future frames.

When `PUT /api/video/streams/{stream_id}/output` succeeds:

- `config_generation` increments
- the next `push_frame()` uses the new config
- the latest transformed frame snapshot is cleared
- per-stream output throttling state is reset
- in the synthetic smoke server, the raw-to-H.264 pipeline is restarted on the next frame so encoded output matches the new config

Re-registration is not required.

## HTTP API Example

Read the current config:

```bash
curl http://127.0.0.1:8080/api/video/streams/alpha/output
```

Example response:

```json
{
  "display_mode": "Passthrough",
  "mirrored": false,
  "rotation_degrees": 0,
  "palette_min": 0,
  "palette_max": 1,
  "output_width": 0,
  "output_height": 0,
  "output_fps": 0,
  "config_generation": 1
}
```

Apply a runtime change:

```bash
curl -X PUT http://127.0.0.1:8080/api/video/streams/alpha/output \
  -H 'Content-Type: application/json' \
  -d '{
    "display_mode": "Ironbow",
    "output_width": 640,
    "output_height": 360,
    "output_fps": 12
  }'
```

Compatibility note: the same route also accepts `/config`.

## NiceGUI Interaction Example

The NiceGUI harness exposes stream config editing from the UI:

1. Open the widget or smoke view.
2. Select the stream.
3. Open the stream config dialog.
4. Click `Reload from backend`.
5. Change `Filter mode`, `Output width`, `Output height`, or `Output fps`.
6. Click `Apply to stream`.

The dialog shows:

- current backend config
- observed active config from debug data
- `config_generation`
- raw JSON response

## Example Programmatic Usage

```cpp
video_server::StreamOutputConfig cfg;
cfg.display_mode = video_server::VideoDisplayMode::Ironbow;
cfg.output_width = 640;
cfg.output_height = 360;
cfg.output_fps = 12.0;

server.set_stream_output_config("alpha", cfg);
```

## Limits And Validation

- `rotation_degrees` must be `0`, `90`, `180`, or `270`
- `palette_max` must be greater than `palette_min`
- invalid values are rejected with HTTP `400`
- unknown JSON fields are rejected
