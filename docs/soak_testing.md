# Soak Testing

`video_server_soak_runner` is the reusable long-duration validation tool for this repo. It starts an embedded synthetic multi-stream WebRTC server, creates real libdatachannel client sessions, drives reconnect and config churn over the existing HTTP APIs, polls the session/debug HTTP surfaces, and records per-stream metrics over time.

## Build

Use the normal repo build:

```bash
./build.sh
```

## Run

Short local validation:

```bash
./build/video_server_soak_runner --duration 1m --summary-interval 5s --poll-interval 1s \
  --json-report build/soak-report.json --csv-report build/soak-report.csv
```

Longer overnight-style run:

```bash
./build/video_server_soak_runner --duration 2h --summary-interval 10s --poll-interval 2s \
  --reconnect-interval 30s --config-interval 15s \
  --json-report build/overnight-soak.json --csv-report build/overnight-soak.csv
```

Custom stream mix:

```bash
./build/video_server_soak_runner --clear-default-streams \
  --stream alpha:640:360:30:Alpha \
  --stream bravo:1280:720:24:Bravo \
  --stream charlie:320:240:15:Charlie \
  --duration 30m
```

## Implemented Scenarios

- Multi-stream concurrent streaming with mixed resolutions and FPS.
- Periodic per-stream reconnect churn using the normal signaling endpoints.
- Periodic per-stream config churn using `PUT /api/video/streams/{stream}/config`.
- Continuous polling of `GET /api/video/signaling/{stream}/session`.
- Continuous polling of `GET /api/video/debug/stats`.

## Metrics Collected

- `packets_sent_after_track_open`
- `packets_attempted`
- `send_failures`
- `packetization_failures`
- `delivered_units`
- `failed_units`
- `total_frames_dropped`
- `session_generation`
- `disconnect_count`
- `sender_state`
- `sending_active`
- `config_generation`

Metrics stay in memory for the full run and can also be written to JSON and CSV.

## Automatic Failure Detection

- Active sending session stops increasing packet counters beyond the configured stall threshold.
- `sending_active=true` persists beyond startup grace without emitted packets.
- `session_generation` changes without an explicit reconnect.
- Disconnects occur without an explicit reconnect.
- Reconnects exceed the configured rapid reconnect threshold inside the configured window.
- Sender enters an unknown or invalid state.
- Config churn does not apply before timeout.
- Session endpoint state diverges from the debug snapshot.
- Any runner exception or crash aborts the run.

## Output

- Periodic concise summary lines for each stream.
- Final pass/fail summary with per-stream end-state, including reconnect count, config update count, reconnect/config coverage booleans, final session generation, and final config generation.
- Run-level scenario coverage line showing whether all streams exercised reconnect churn and config churn, plus any missing streams.
- Optional JSON report for long-run inspection.
- Optional CSV samples for spreadsheets/plotting.
