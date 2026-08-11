# Soak Testing

`video_server_soak_runner` is the long-running validation tool for this repo. It starts an embedded synthetic multi-stream server, creates real libdatachannel client sessions, drives reconnect and runtime config churn through the HTTP API, polls session/debug state, and records metrics over time.

## Build

```bash
./build.sh
```

## Quick Run

Short validation:

```bash
./build/video_server_soak_runner \
  --duration 1m \
  --summary-interval 5s \
  --poll-interval 1s \
  --json-report build/soak-report.json \
  --csv-report build/soak-report.csv
```

Longer run:

```bash
./build/video_server_soak_runner \
  --duration 2h \
  --summary-interval 10s \
  --poll-interval 2s \
  --reconnect-interval 30s \
  --config-interval 15s \
  --json-report build/overnight-soak.json \
  --csv-report build/overnight-soak.csv
```

Custom stream mix:

```bash
./build/video_server_soak_runner \
  --clear-default-streams \
  --stream alpha:640:360:30:Alpha \
  --stream bravo:1280:720:24:Bravo \
  --stream charlie:320:240:15:Charlie \
  --duration 30m
```

Managed split-step run with one caller-owned API thread and two caller-owned stream threads:

```bash
./build/video_server_soak_runner \
  --api-mode manual \
  --streams 10 \
  --stream-groups 0,1,2,3,4:5,6,7,8,9 \
  --rate-hz 120 \
  --duration-sec 3600
```

Managed API-thread run where the library owns API progression and the runner owns only stream-group threads:

```bash
./build/video_server_soak_runner \
  --api-mode managed \
  --api-cpu 2 \
  --streams 10 \
  --stream-groups 0,1,2,3,4:5,6,7,8,9 \
  --rate-hz 120 \
  --duration-sec 3600
```

## What It Exercises

- multiple concurrent streams
- caller-defined managed stream groups
- manual API stepping or managed API-thread progression
- real WebRTC offer/answer and candidate exchange
- periodic reconnect churn
- periodic per-stream runtime config churn
- continuous polling of `GET /api/video/signaling/{stream}/session`
- continuous polling of `GET /api/video/debug/stats`

Config churn is applied through the runtime output config route. The runner currently uses the compatibility `/config` path; the server also accepts `/output`.

## Metrics Collected

Per stream, the runner records samples including:

- `session_generation`
- `disconnect_count`
- `packets_sent`
- `packets_attempted`
- `send_failures`
- `packetization_failures`
- `delivered_units`
- `failed_units`
- `total_frames_dropped`
- `config_generation`
- `session_active`
- `sending_active`
- `sender_state`
- `last_packetization_status`

The runner also reports step latency summaries:

- API loop latency in manual API mode
- per stream-group `step_streams()` latency
- requested, successful, and failed step counts
- deadline misses where one step takes longer than the configured cadence
- achieved step frequency
- p50, p95, p99, and max step duration

In managed API-thread mode, API loop latency is not sampled because the loop is owned internally by the library. Stream-group latency is still sampled and is the primary signal for production, transform, upload, and encoder cost.

Reports can be written as:

- JSON summary plus full sample set
- CSV sample stream for plotting or spreadsheet review

Latency summaries are included in the JSON report under `latency`.

## How To Interpret Results

Healthy run characteristics:

- each stream reaches active sending
- `sender_state` settles into `sending-h264-rtp`
- `session_generation` only increases when reconnect churn is expected
- `config_generation` increases when config churn is expected
- `packets_sent` and `delivered_units` keep increasing while active
- `send_failures` stays at `0`
- `packetization_failures` stays at `0`

Normal during churn:

- temporary waiting states during setup or reconnect
- `disconnect_count` increases during explicit reconnect cycles
- `session_generation` increases during explicit reconnect cycles

## What "Pass" Looks Like

A passing run ends with:

- process exit code `0`
- final summary line with `success=true`
- no `[soak][failure]` lines
- reconnect churn observed for all intended streams when enabled
- config churn observed for all intended streams when enabled

Typical final lines look like:

```text
[soak] completed duration=60.0s success=true failures=0
[soak] scenario_coverage reconnect=all-streams config=all-streams
[soak] latency name=stream-group-0 requested=7200 ok=7200 failed=0 deadline_misses=0 achieved_hz=120.0 p50_ms=4.1 p95_ms=5.8 p99_ms=7.2 max_ms=12.4 streams=stream0,stream1
```

Short runs can still pass with partial churn coverage if they are not long enough to cycle through every stream.

Observed 35-second run:

```text
[soak] completed duration=35.1291s success=true failures=0
[soak] scenario_coverage reconnect=partial config=partial missing_reconnect=bravo,charlie missing_config=charlie
```

That result is still healthy for a short run because:

- all active streams stayed in `sending-h264-rtp`
- `send_failures` stayed at `0`
- `packetization_failures` stayed at `0`
- the run reported no failures

Use a longer duration when you want full reconnect/config coverage across all streams.

## Failure Detection

The runner flags problems such as:

- active sending session stalls without packet progress
- session resets without an explicit reconnect
- disconnect count increases without an explicit reconnect
- config churn does not apply before timeout
- session endpoint state diverges from debug state
- sender enters an unknown or invalid state
- rapid reconnect loops exceed the configured threshold

## Dropped Frames Under FPS Throttling

Dropped frames are expected when output FPS is lower than input FPS.

Example:

- input stream is `30 fps`
- runtime config changes `output_fps` to `12`
- the server continues receiving frames at `30 fps`
- the output path drops excess frames to respect `12 fps`

In that case:

- `total_frames_dropped` should increase
- this is not a failure by itself
- packet send failures should still remain at zero in a healthy run

Observed during the short soak run:

- `alpha` stayed healthy in `sending-h264-rtp`
- `alpha` accumulated dropped frames after config churn lowered effective output cadence
- send and packetization failures still stayed at `0`

Interpret `total_frames_dropped` as a throttling/selection metric, not automatically as a bug.

## Practical Review Checklist

- check the runner exit code
- check the final `success=` line
- check for any `[soak][failure]` lines
- inspect per-stream `sender_state`, `send_failures`, and `packetization_failures`
- inspect `dropped_frames` in context of any configured FPS reduction
