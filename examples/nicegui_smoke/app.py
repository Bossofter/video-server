#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Optional

from nicegui import app, ui
from ui_helpers import default_demo_streams, parse_stream_spec

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SMOKE_BINARY = ROOT / 'build' / 'video_server_nicegui_smoke_server'


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='NiceGUI smoke harness for the video-server WebRTC H264 path.')
    parser.add_argument('--video-server-url', default='http://127.0.0.1:8080', help='Base URL for the running video server.')
    parser.add_argument('--stream-id', default='synthetic-h264', help='Synthetic stream id to consume.')
    parser.add_argument('--stream', action='append', default=[], help='Repeatable multi-stream demo spec: id:width:height:fps[:label].')
    parser.add_argument('--multi-stream-demo', action='store_true', help='Launch the default alpha/bravo/charlie multi-stream demo set.')
    parser.add_argument('--ui-host', default='127.0.0.1', help='NiceGUI host.')
    parser.add_argument('--ui-port', type=int, default=8090, help='NiceGUI port.')
    parser.add_argument('--start-server', action='store_true', help='Launch the smoke C++ server executable automatically.')
    parser.add_argument('--smoke-binary', default=str(DEFAULT_SMOKE_BINARY), help='Path to the smoke server executable.')
    parser.add_argument('--server-host', default='127.0.0.1', help='Host to pass to the smoke server when --start-server is used.')
    parser.add_argument('--server-port', type=int, default=8080, help='Port to pass to the smoke server when --start-server is used.')
    parser.add_argument('--shared-key', default='', help='Optional shared key for protected server endpoints.')
    parser.add_argument('--width', type=int, default=640, help='Synthetic stream width for the launched smoke server.')
    parser.add_argument('--height', type=int, default=360, help='Synthetic stream height for the launched smoke server.')
    parser.add_argument('--fps', type=float, default=30.0, help='Synthetic stream FPS for the launched smoke server.')
    parser.add_argument('--auto-connect', action='store_true', help='Auto-connect the browser harness on page load.')
    parser.add_argument('--debug', action='store_true', help='Start with debug telemetry visible.')
    parser.add_argument('--auto-reconnect', action='store_true', help='Retry automatically after connection failures/disconnects.')
    parser.add_argument('--session-poll-ms', type=int, default=500, help='Session poll interval in milliseconds.')
    parser.add_argument('--stress-duration-seconds', type=float, default=0.0, help='Optional smoke-server soak duration.')
    parser.add_argument('--stress-stats-interval-seconds', type=float, default=5.0, help='Observability summary print cadence for soak mode.')
    parser.add_argument('--stress-print-summary', action='store_true', help='Ask the smoke server to print periodic observability summaries.')
    return parser.parse_args()


ARGS = parse_args()
SMOKE_PROCESS: Optional[subprocess.Popen[str]] = None

def requested_streams() -> list[dict[str, Any]]:
    explicit_single_stream = has_cli_flag('--stream-id', '--width', '--height', '--fps')
    if ARGS.multi_stream_demo:
        return default_demo_streams()
    if ARGS.stream:
        return [parse_stream_spec(value) for value in ARGS.stream]
    if explicit_single_stream:
        return [{'streamId': ARGS.stream_id, 'width': ARGS.width, 'height': ARGS.height, 'fps': ARGS.fps, 'label': ARGS.stream_id}]
    return default_demo_streams()
def has_cli_flag(*names: str) -> bool:
    return any(name in sys.argv[1:] for name in names)


STREAM_SPECS = requested_streams()
DEFAULT_STREAM_ID = STREAM_SPECS[0]['streamId']

def start_smoke_server() -> subprocess.Popen[str]:
    smoke_binary = Path(ARGS.smoke_binary)
    if not smoke_binary.exists():
        raise FileNotFoundError(
            f'Smoke server binary not found: {smoke_binary}. Build the repo first with ./build.sh.'
        )

    cmd = [
        str(smoke_binary),
        '--host',
        ARGS.server_host,
        '--port',
        str(ARGS.server_port),
    ]
    explicit_single_stream = has_cli_flag('--stream-id', '--width', '--height', '--fps')
    if ARGS.multi_stream_demo or (not ARGS.stream and not explicit_single_stream):
        cmd.append('--multi-stream-demo')
    elif ARGS.stream:
        for spec in ARGS.stream:
            cmd.extend(['--stream', spec])
    else:
        cmd.extend([
            '--stream-id',
            ARGS.stream_id,
            '--width',
            str(ARGS.width),
            '--height',
            str(ARGS.height),
            '--fps',
            str(ARGS.fps),
        ])
    if ARGS.stress_duration_seconds > 0:
        cmd.extend(['--duration-seconds', str(ARGS.stress_duration_seconds)])
        cmd.extend(['--stats-interval-seconds', str(ARGS.stress_stats_interval_seconds)])
        if ARGS.stress_print_summary:
            cmd.append('--print-observability-summary')
    if ARGS.debug:
        cmd.append('--enable-debug-api')
    if ARGS.shared_key:
        cmd.extend(['--shared-key', ARGS.shared_key])

    probe_host = ARGS.server_host
    if probe_host in ('0.0.0.0', '::', ''):
        probe_host = '127.0.0.1'
    try:
        with socket.create_connection((probe_host, ARGS.server_port), timeout=0.25):
            raise RuntimeError(
                f'Cannot launch smoke server because {probe_host}:{ARGS.server_port} is already in use. '
                'Stop the existing process or choose a different --server-port.'
            )
    except ConnectionRefusedError:
        pass
    except OSError:
        pass

    print('[nicegui-smoke] launching:', ' '.join(shlex.quote(part) for part in cmd), flush=True)
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE, text=True)

    readiness_url = f'http://{probe_host}:{ARGS.server_port}/api/video/streams'
    deadline = time.monotonic() + 5.0
    last_error: Optional[Exception] = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f'Smoke server exited before becoming ready (exit code {process.returncode}). '
                f'Check whether {ARGS.server_host}:{ARGS.server_port} is already in use.'
            )
        try:
            with urllib.request.urlopen(readiness_url, timeout=0.5) as response:
                if process.poll() is not None:
                    raise RuntimeError(
                        f'Smoke server exited while probing {readiness_url} '
                        f'(exit code {process.returncode}).'
                    )
                if response.status == 200:
                    return process
        except urllib.error.HTTPError as exc:
            last_error = exc
        except OSError as exc:
            last_error = exc
        time.sleep(0.1)

    process.terminate()
    raise RuntimeError(
        f'Smoke server did not become ready at {readiness_url} within 5.0s'
        + (f': {last_error}' if last_error else '.')
    )


if ARGS.start_server:
    try:
        SMOKE_PROCESS = start_smoke_server()
    except Exception as exc:
        print(f'[nicegui-smoke] failed to launch smoke server: {exc}', file=sys.stderr, flush=True)
        raise SystemExit(1) from exc
    ARGS.video_server_url = f'http://{ARGS.server_host}:{ARGS.server_port}'


async def stop_smoke_server() -> None:
    global SMOKE_PROCESS
    if SMOKE_PROCESS is None:
        return
    process = SMOKE_PROCESS
    SMOKE_PROCESS = None
    if process.poll() is None:
        try:
            if process.stdin:
                process.stdin.write('\n')
                process.stdin.flush()
        except Exception:
            process.terminate()
        await asyncio.sleep(0.5)
    if process.poll() is None:
        process.terminate()
        await asyncio.sleep(0.5)
    if process.poll() is None:
        process.kill()


@app.on_shutdown
async def _shutdown() -> None:
    await stop_smoke_server()


INITIAL_CONFIG = {
    'serverBase': ARGS.video_server_url,
    'sharedKey': ARGS.shared_key,
    'streamId': DEFAULT_STREAM_ID,
    'streamCatalog': STREAM_SPECS,
    'widgetFps': STREAM_SPECS[0]['fps'],
    'widgetWidth': STREAM_SPECS[0]['width'],
    'widgetHeight': STREAM_SPECS[0]['height'],
    'debugMode': ARGS.debug,
    'autoReconnect': ARGS.auto_reconnect,
    'autoConnect': True if ARGS.auto_connect or ARGS.start_server else False,
    'sessionPollMs': max(200, ARGS.session_poll_ms),
    'modeLabel': 'launch smoke server + consume WebRTC stream(s)' if ARGS.start_server else 'consume existing video server',
    'smokeServerManaged': ARGS.start_server,
}

PAGE_JS = f"""
<script>
window.videoSmokeDefaults = {json.dumps(INITIAL_CONFIG)};
window.videoSmokeHarness = (() => {{
  const search = new URLSearchParams(window.location.search);
  const widgetMode = search.get('widget') === '1';
  const STORAGE_KEY = 'video-smoke-harness-config-v3' + (search.get('stream_id') ? ':' + search.get('stream_id') : '');
  const state = {{
    config: null,
    pc: null,
    reconnectTimer: null,
    generation: 0,
    connectToken: 0,
    sessionPollAbort: false,
    statsInterval: null,
    sessionSummary: null,
    statsSummary: null,
    observabilitySummary: null,
    offerStatus: 'idle',
    candidateStatus: 'idle',
    connectedStreamId: '',
    remoteDescriptionApplied: false,
    remoteTrackReceived: false,
    playbackActive: false,
    lastBackendCandidate: '',
    appliedBackendCandidates: new Set(),
    lastStatsAt: '',
    disconnectReason: 'idle',
    selectedCodec: '',
    recentLogs: [],
  }};

  const refs = {{}};

  const categories = [
    ['ui', 'UI'],
    ['signaling', 'Signaling'],
    ['ice', 'ICE'],
    ['media', 'Media'],
    ['stats', 'Stats'],
    ['session', 'Session'],
    ['error', 'Error'],
  ];

  const byId = (id) => document.getElementById(id);
  const setText = (id, value) => {{
    const el = byId(id);
    if (el) el.textContent = value;
  }};
  const setHtml = (id, value) => {{
    const el = byId(id);
    if (el) el.innerHTML = value;
  }};
  const setValue = (id, value) => {{
    const el = byId(id);
    if (el) el.value = value;
  }};
  const setChecked = (id, value) => {{
    const el = byId(id);
    if (el) el.checked = !!value;
  }};
  const setDisabled = (id, value) => {{
    const el = byId(id);
    if (el) el.disabled = !!value;
  }};
  const setDisplay = (id, value) => {{
    const el = byId(id);
    if (el) el.style.display = value;
  }};
  const setVisible = (id, value) => setDisplay(id, value ? 'block' : 'none');
  const escapeHtml = (value) => String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
  const shortJson = (value) => escapeHtml(JSON.stringify(value ?? {{}}, null, 2));
  const formatNumber = (value, fallback='n/a') => {{
    if (value === null || value === undefined || value === '') return fallback;
    if (typeof value === 'number') return Number.isInteger(value) ? String(value) : value.toFixed(2).replace(/\\.00$/, '');
    return String(value);
  }};
  const formatGeometry = (width, height, fallback='native') => {{
    if (!width && !height) return fallback;
    return `${{formatNumber(width, '?')}}x${{formatNumber(height, '?')}}`;
  }};
  const configUrl = (serverBase, streamId) => `${{serverBase}}/api/video/streams/${{encodeURIComponent(streamId)}}/config`;
  const authHeaders = (contentType=null) => {{
    const headers = {{}};
    if (contentType) headers['Content-Type'] = contentType;
    if (state.config?.sharedKey) headers['Authorization'] = `Bearer ${{state.config.sharedKey}}`;
    return headers;
  }};
  const selectedStreamSpec = (streamId) => {{
    const catalog = Array.isArray(state.config?.streamCatalog) ? state.config.streamCatalog : [];
    return catalog.find((spec) => spec.streamId === streamId) || catalog[0] || null;
  }};
  const selectedObservabilityStream = (streamId) => {{
    const streams = Array.isArray(state.observabilitySummary?.streams) ? state.observabilitySummary.streams : [];
    return streams.find((stream) => stream.stream_id === streamId) || null;
  }};

  function loadConfig() {{
    let saved = {{}};
    try {{
      saved = JSON.parse(localStorage.getItem(STORAGE_KEY) || '{{}}');
    }} catch (error) {{
      console.warn('[video-smoke] failed to parse saved config', error);
    }}
    const defaults = Object.assign({{}}, window.videoSmokeDefaults || {{}});
    state.config = Object.assign({{}}, defaults, saved);
    state.config.streamCatalog = Array.isArray(defaults.streamCatalog) ? defaults.streamCatalog : (state.config.streamCatalog || []);
    state.config.modeLabel = defaults.modeLabel || state.config.modeLabel;
    state.config.smokeServerManaged = !!defaults.smokeServerManaged;
    if (search.get('stream_id')) state.config.streamId = search.get('stream_id');
    if (search.get('fps')) state.config.widgetFps = Number(search.get('fps')) || state.config.widgetFps;
    if (search.get('width')) state.config.widgetWidth = Number(search.get('width')) || state.config.widgetWidth;
    if (search.get('height')) state.config.widgetHeight = Number(search.get('height')) || state.config.widgetHeight;
    if (search.get('label')) state.config.widgetLabel = search.get('label');
    state.config = syncSelectedStreamConfig(state.config);
    state.config.sessionPollMs = Math.max(200, Number(state.config.sessionPollMs) || 500);
    state.config.logFilter = state.config.logFilter || 'all';
    state.config.placeholderDisplay = state.config.placeholderDisplay || 'default';
    state.config.logVerbosity = state.config.logVerbosity || 'normal';
    state.config.widgetShowDebug = !!state.config.widgetShowDebug;
    state.config.widgetDebugLevel = state.config.widgetDebugLevel || 'basic';
    state.config.widgetShowConnection = state.config.widgetShowConnection !== false;
    state.config.widgetShowPlayback = state.config.widgetShowPlayback !== false;
    state.config.widgetShowVideo = !!state.config.widgetShowVideo;
    state.config.widgetShowSession = !!state.config.widgetShowSession;
    state.config.activeTab = widgetMode ? 'widget' : (state.config.activeTab || ((state.config.streamCatalog || []).length > 1 ? 'widget' : 'smoke'));
    return state.config;
  }}

  function syncSelectedStreamConfig(cfg) {{
    const catalog = Array.isArray(cfg.streamCatalog) ? cfg.streamCatalog : [];
    const selected = catalog.find((spec) => spec.streamId === cfg.streamId) || catalog[0] || null;
    if (!selected) return cfg;
    if (!cfg.streamId || !catalog.some((spec) => spec.streamId === cfg.streamId)) {{
      cfg.streamId = selected.streamId;
    }}
    if (!search.get('fps')) cfg.widgetFps = selected.fps;
    if (!search.get('width')) cfg.widgetWidth = selected.width;
    if (!search.get('height')) cfg.widgetHeight = selected.height;
    if (!search.get('label')) cfg.widgetLabel = selected.label || selected.streamId;
    return cfg;
  }}

  function saveConfig() {{
    if (!state.config) return;
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state.config));
  }}

  function updateConfigStreamIdentity(streamId) {{
    const spec = selectedStreamSpec(streamId);
    const label = spec?.label || streamId || 'unknown stream';
    const geometry = spec ? `${{spec.width}}x${{spec.height}} @ ${{spec.fps}} fps` : 'custom stream';
    setText('widget-stream-id', streamId || 'no stream');
    setText('widget-stream-label', label);
    setText('widget-stream-meta', geometry);
    setText('config-stream-name', streamId ? `${{streamId}} · ${{label}}` : 'No stream selected');
    setText('config-stream-meta', geometry);
  }}

  function setConfigStatus(message, kind='info') {{
    const box = byId('config-feedback');
    if (!box) return;
    box.textContent = message || 'Waiting for config action.';
    box.className = `config-feedback ${{kind}}`;
  }}

  function applyConfigDataToUi(streamId, data, sourceLabel='loaded') {{
    setValue('config-filter-mode', data.display_mode || 'Passthrough');
    setValue('config-output-width', String(data.output_width || 0));
    setValue('config-output-height', String(data.output_height || 0));
    setValue('config-output-fps', String(data.output_fps || 0));
    setText('config-generation', String(data.config_generation || 0));
    setText('config-current-summary', `${{data.display_mode || 'Passthrough'}} • ${{formatGeometry(data.output_width, data.output_height)}} • ${{formatNumber(data.output_fps, 'source')}} fps`);
    setHtml('config-response-json', '<pre>' + shortJson(data) + '</pre>');
    setText('widget-active-config', `${{data.display_mode || 'Passthrough'}} • ${{formatGeometry(data.output_width, data.output_height)}} • ${{formatNumber(data.output_fps, 'source')}} fps`);
    appendLog('ui', `${{sourceLabel}} stream config for ${{streamId}}`);
  }}

  function refreshActiveConfigSummary() {{
    const cfg = state.config || loadConfig();
    const streamId = cfg.streamId || '';
    updateConfigStreamIdentity(streamId);
    const obsStream = selectedObservabilityStream(streamId);
    if (obsStream) {{
      setText('config-observed-summary', `${{obsStream.active_filter_mode || 'unknown'}} • ${{formatGeometry(obsStream.active_output_width, obsStream.active_output_height)}} • ${{formatNumber(obsStream.active_output_fps, 'source')}} fps`);
    }} else {{
      setText('config-observed-summary', 'Waiting for observability data');
    }}
  }}

  function showConfigDialog(open) {{
    const dialog = byId('config-dialog');
    if (!dialog) return;
    dialog.style.display = open ? 'flex' : 'none';
    if (open) refreshActiveConfigSummary();
  }}

  function nowStamp() {{
    return new Date().toLocaleTimeString();
  }}

  function appendLog(category, message) {{
    const entry = {{time: nowStamp(), category, message: String(message)}};
    state.recentLogs.unshift(entry);
    state.recentLogs = state.recentLogs.slice(0, 400);
    const logFilter = state.config?.logFilter || 'all';
    const visible = state.recentLogs.filter((item) => logFilter === 'all' || item.category === logFilter);
    // NOTE: PAGE_JS is embedded in a Python string literal, so JS backslashes must stay double-escaped.
    const text = visible.map((item) => `[${{item.time}}] [${{item.category}}] ${{item.message}}`).join('\\n');
    const logEl = byId('smoke-log');
    if (logEl) logEl.textContent = text;
    console.log(`[video-smoke:${{category}}]`, message);
  }}

  function updateBadge(id, text, className='') {{
    const el = byId(id);
    if (!el) return;
    el.textContent = text;
    el.className = `status-badge ${{className}}`;
  }}

  function computeOverallStatus() {{
    const video = byId('smoke-video');
    const visiblyRendering = !!video && !video.paused && video.readyState >= 2 && video.videoWidth > 0;
    if (state.pc && ['connected', 'completed'].includes(state.pc.connectionState) && (state.playbackActive || visiblyRendering)) {{
      return {{text: 'connected + rendering', className: 'good'}};
    }}
    if (state.pc && ['connected', 'completed'].includes(state.pc.connectionState) && state.remoteTrackReceived) {{
      return {{text: 'connected + track received', className: 'warn'}};
    }}
    if (state.offerStatus.startsWith('failed') || state.disconnectReason.startsWith('error')) {{
      return {{text: 'error', className: 'bad'}};
    }}
    if (state.pc) {{
      return {{text: `connecting (${{state.pc.connectionState}})`, className: 'warn'}};
    }}
    return {{text: state.disconnectReason === 'manual disconnect' ? 'disconnected' : 'idle', className: ''}};
  }}

  function applyStreamToVideos(stream) {{
    ['smoke-video', 'widget-video'].forEach((id) => {{
      const video = byId(id);
      if (!video) return;
      video.srcObject = stream;
      video.play().catch((error) => appendLog('media', `${{id}}.play() warning: ${{error}}`));
    }});
  }}

  function clearVideos() {{
    ['smoke-video', 'widget-video'].forEach((id) => {{
      const video = byId(id);
      if (!video) return;
      try {{ video.pause(); }} catch (error) {{}}
      video.srcObject = null;
      video.load();
    }});
  }}

  function updateSummary() {{
    const cfg = state.config || loadConfig();
    const video = byId('smoke-video');
    const status = computeOverallStatus();
    updateBadge('summary-connection', status.text, status.className);
    updateBadge('summary-track', state.remoteTrackReceived ? 'track received' : 'no remote track', state.remoteTrackReceived ? 'good' : '');
    updateBadge('summary-playback', state.playbackActive ? 'rendering' : 'not rendering', state.playbackActive ? 'good' : '');
    updateBadge('summary-offer', state.offerStatus, state.offerStatus.includes('failed') ? 'bad' : '');
    updateBadge('summary-candidates', state.candidateStatus, state.candidateStatus.includes('failed') ? 'bad' : '');
    setText('summary-server', cfg.serverBase || '');
    setText('summary-stream', state.connectedStreamId || cfg.streamId || '');
    setText('summary-generation', String(state.generation));
    setText('summary-last-stats', state.lastStatsAt || 'never');
    setText('playback-headline', status.text);
    setText('playback-detail', state.disconnectReason || 'waiting');
    setText('video-ready-state', video ? String(video.readyState) : 'n/a');
    setText('video-paused', video ? String(video.paused) : 'n/a');
    setText('video-current-time', video ? video.currentTime.toFixed(2) : 'n/a');
    setText('video-size', video ? `${{video.videoWidth}}x${{video.videoHeight}}` : 'n/a');
    setText('video-network-state', video ? String(video.networkState) : 'n/a');
    setText('video-muted-state', video ? String(video.muted) : 'n/a');

    const pc = state.pc;
    setText('debug-connection-state', pc ? pc.connectionState : 'closed');
    setText('debug-ice-connection-state', pc ? pc.iceConnectionState : 'closed');
    setText('debug-ice-gathering-state', pc ? pc.iceGatheringState : 'closed');
    setText('debug-signaling-state', pc ? pc.signalingState : 'closed');
    setText('debug-generation', String(state.generation));
    setText('debug-server-stream', `${{cfg.serverBase}} / ${{state.connectedStreamId || cfg.streamId || ''}}`);
    setText('debug-offer-status', state.offerStatus);
    setText('debug-candidate-status', state.candidateStatus);
    setText('debug-remote-description', state.remoteDescriptionApplied ? 'yes' : 'no');
    setText('debug-remote-track', state.remoteTrackReceived ? 'yes' : 'no');
    setText('debug-playback', state.playbackActive ? 'active' : 'inactive');
    setText('debug-last-candidate', state.lastBackendCandidate || 'none');
    setText('debug-codec', state.selectedCodec || 'unknown');
    setText('debug-last-stats', state.lastStatsAt || 'never');

    const session = state.sessionSummary || {{}};
    const sessionActive = session.active === false ? 'inactive' : (session.active === true ? 'active' : 'n/a');
    setText('session-peer-state', `${{session.peer_state || 'n/a'}} / ${{sessionActive}}`);
    setText('session-media-bridge', session.media_bridge_state || 'n/a');
    setText('session-sender-state', session.encoded_sender_state || 'n/a');
    setText('session-preferred-path', `${{session.preferred_media_path || 'n/a'}} / ${{session.last_transition_reason || session.teardown_reason || 'steady-state'}}`);
    setText('session-startup', session.encoded_sender_startup_sequence_sent ? 'yes' : 'no');
    setText('session-first-dec', session.encoded_sender_first_decodable_frame_sent ? 'yes' : 'no');
    setText('session-packets-open', String(session.encoded_sender_packets_sent_after_track_open ?? 'n/a'));
    setHtml('session-json', '<pre>' + shortJson(session) + '</pre>');

    const stats = state.statsSummary || {{}};
    setText('stats-packets', String(stats.packetsReceived ?? 'n/a'));
    setText('stats-bytes', String(stats.bytesReceived ?? 'n/a'));
    setText('stats-frames-received', String(stats.framesReceived ?? 'n/a'));
    setText('stats-frames-decoded', String(stats.framesDecoded ?? 'n/a'));
    setText('stats-frames-dropped', String(stats.framesDropped ?? 'n/a'));
    setText('stats-resolution', stats.frameWidth && stats.frameHeight ? `${{stats.frameWidth}}x${{stats.frameHeight}}` : 'n/a');
    setText('stats-fps', String(stats.framesPerSecond ?? 'n/a'));

    const obs = state.observabilitySummary || {{}};
    setText('obs-stream-count', String(obs.stream_count ?? 'n/a'));
    setText('obs-session-count', String(obs.active_session_count ?? 'n/a'));
    const obsStreams = Array.isArray(obs.streams) ? obs.streams : [];
    setHtml('observability-grid', obsStreams.map((stream) => {{
      const session = stream.current_session || {{}};
      const counters = session.counters || {{}};
      return `
        <div class="widget-debug-card">
          <span>${{escapeHtml(stream.stream_id || 'unknown stream')}}</span>
          <strong>${{escapeHtml(`${{stream.configured_width || '?'}}x${{stream.configured_height || '?'}} @ ${{stream.configured_fps || '?'}} fps`)}}</strong>
          <span>session: <strong>${{escapeHtml(session.active === false ? 'inactive' : (session.active === true ? 'active' : 'no-session'))}}</strong></span>
          <span>generation/teardown: <strong>${{escapeHtml(`${{session.session_generation ?? 0}} / ${{session.teardown_reason || 'none'}}`)}}</strong></span>
          <span>sender: <strong>${{escapeHtml(session.sender_state || 'no-session')}}</strong></span>
          <span>status: <strong>${{escapeHtml(session.last_packetization_status || 'n/a')}}</strong></span>
          <span>AU recv/send: <strong>${{stream.total_access_units_received ?? 0}} / ${{counters.delivered_units ?? 0}}</strong></span>
          <span>Packets/open: <strong>${{counters.packets_sent_after_track_open ?? 0}}</strong></span>
          <span>Startup/first-dec: <strong>${{counters.startup_sequence_injections ?? 0}} / ${{counters.first_decodable_transitions ?? 0}}</strong></span>
          <span>Track closed/send fail: <strong>${{counters.track_closed_events ?? 0}} / ${{counters.send_failures ?? 0}}</strong></span>
        </div>`;
    }}).join(''));

    const healthClass = status.className || '';
    const healthText = status.text;
    const healthDot = byId('widget-health-dot');
    if (healthDot) healthDot.className = `health-dot ${{healthClass || 'idle'}}`;
    setText('widget-health-summary', `${{healthText}} • ${{cfg.streamId || 'no stream'}}`);

    const widgetShowDebug = !!cfg.widgetShowDebug;
    const widgetLevelOrder = ['basic', 'detailed', 'full'];
    const widgetLevel = cfg.widgetDebugLevel || 'basic';
    const levelIndex = widgetLevelOrder.indexOf(widgetLevel);
    const detailEnabled = widgetShowDebug && levelIndex >= 0;
    const showConnection = detailEnabled && cfg.widgetShowConnection;
    const showPlayback = detailEnabled && (levelIndex >= 0) && cfg.widgetShowPlayback;
    const showVideo = detailEnabled && (levelIndex >= 1) && cfg.widgetShowVideo;
    const showSession = detailEnabled && (levelIndex >= 2) && cfg.widgetShowSession;
    setVisible('widget-debug-sections', widgetShowDebug);
    setVisible('widget-connection-section', showConnection);
    setVisible('widget-playback-section', showPlayback);
    setVisible('widget-video-section', showVideo);
    setVisible('widget-session-section', showSession);
    setText('widget-connection-state', pc ? pc.connectionState : 'closed');
    setText('widget-signaling-state', pc ? pc.signalingState : 'closed');
    setText('widget-playback-state', state.playbackActive ? 'rendering' : 'waiting');
    setText('widget-track-state', state.remoteTrackReceived ? 'yes' : 'no');
    setText('widget-video-size', video ? `${{video.videoWidth}}x${{video.videoHeight}}` : 'n/a');
    setText('widget-video-time', video ? video.currentTime.toFixed(2) : 'n/a');
    setText('widget-session-peer', `${{session.peer_state || 'n/a'}} / ${{sessionActive}}`);
    setText('widget-session-sender', `${{session.encoded_sender_state || 'n/a'}} / ${{session.teardown_reason || 'live'}}`);
    setText('widget-expected-fps', (cfg.widgetFps || 'n/a').toString());
    setText('widget-target-geometry', `${{cfg.widgetWidth || '?'}}x${{cfg.widgetHeight || '?'}}`);
    refreshActiveConfigSummary();
  }}

  function renderMultiStreamDashboard() {{
    const root = byId('multi-stream-dashboard');
    if (!root) return;
    const catalog = (state.config?.streamCatalog || []).slice();
    const showMultiGrid = !widgetMode && catalog.length > 1 && (state.config?.activeTab || 'widget') === 'widget';
    setDisplay('widget-dashboard-wrapper', showMultiGrid ? 'block' : 'none');
    setDisplay('widget-shell', showMultiGrid ? 'none' : 'block');
    setDisplay('widget-context-menu', showMultiGrid ? 'none' : 'none');
    if (!showMultiGrid) {{
      root.innerHTML = '';
      return;
    }}
    root.innerHTML = catalog.map((spec) => `
      <div class="multi-card">
        <div class="multi-card-header">
          <div><strong>${{escapeHtml(spec.streamId)}}</strong> · ${{escapeHtml(spec.label || spec.streamId)}}</div>
          <div>${{spec.width}}x${{spec.height}} @ ${{spec.fps}} fps</div>
        </div>
        <iframe class="multi-widget-frame" src="${{escapeHtml(`/?widget=1&stream_id=${{encodeURIComponent(spec.streamId)}}&fps=${{encodeURIComponent(spec.fps)}}&width=${{encodeURIComponent(spec.width)}}&height=${{encodeURIComponent(spec.height)}}&label=${{encodeURIComponent(spec.label || spec.streamId)}}`)}}"></iframe>
      </div>`).join('');
  }}

  function bindDebugHooks() {{
    if (!state.config?.debugMode) {{
      delete window.__videoSmokePc;
      delete window.__videoSmokeVideo;
      delete window.__videoSmokeState;
      delete window.__videoSmokeStats;
      return;
    }}
    window.__videoSmokePc = state.pc;
    window.__videoSmokeVideo = byId('smoke-video');
    window.__videoSmokeState = state;
    window.__videoSmokeStats = async () => state.pc ? summarizeStats(await state.pc.getStats()) : null;
  }}

  function switchTab(tabName) {{
    const cfg = state.config || loadConfig();
    const previousTab = cfg.activeTab || 'smoke';
    cfg.activeTab = tabName;
    state.config = cfg;
    saveConfig();
    setDisplay('smoke-tab-panel', tabName === 'smoke' ? 'block' : 'none');
    setDisplay('widget-tab-panel', tabName === 'widget' ? 'block' : 'none');
    byId('tab-smoke')?.classList.toggle('active', tabName === 'smoke');
    byId('tab-widget')?.classList.toggle('active', tabName === 'widget');
    renderMultiStreamDashboard();
    const multiStreamMode = !widgetMode && (cfg.streamCatalog || []).length > 1;
    if (multiStreamMode && tabName === 'widget' && state.pc) {{
      disconnect('switch to widget tab').catch((error) => appendLog('error', `widget tab disconnect failed: ${{error}}`));
    }} else if (multiStreamMode && previousTab === 'widget' && tabName === 'smoke' && cfg.autoConnect && !state.pc) {{
      connect('smoke tab activated').catch((error) => appendLog('error', `smoke tab connect failed: ${{error}}`));
    }}
    if (widgetMode) {{
      setDisplay('toolbar', 'none');
      setDisplay('smoke-tab-button-row', 'none');
      setDisplay('smoke-tab-panel', 'none');
      setDisplay('info-note-block', 'none');
    }}
  }}

  function showContextMenu(x, y) {{
    const menu = byId('widget-context-menu');
    if (!menu) return;
    menu.style.display = 'block';
    const rect = menu.getBoundingClientRect();
    const left = Math.max(16, Math.min(x, window.innerWidth - rect.width - 16));
    const top = Math.max(16, Math.min(y, window.innerHeight - rect.height - 16));
    menu.style.left = `${{left}}px`;
    menu.style.top = `${{top}}px`;
  }}

  function hideContextMenu() {{
    setDisplay('widget-context-menu', 'none');
  }}

  function syncFormFromConfig() {{
    const cfg = state.config || loadConfig();
    syncSmokeStreamSelector();
    setValue('config-server-url', cfg.serverBase || '');
    setValue('config-stream-id', cfg.streamId || '');
    setValue('config-widget-fps', String(cfg.widgetFps || ''));
    setChecked('config-debug-mode', cfg.debugMode);
    setChecked('config-auto-reconnect', cfg.autoReconnect);
    setChecked('config-auto-connect', cfg.autoConnect);
    setChecked('config-widget-show-debug', cfg.widgetShowDebug);
    setValue('config-session-poll-ms', String(cfg.sessionPollMs || 500));
    setValue('config-log-filter', cfg.logFilter || 'all');
    setValue('config-log-verbosity', cfg.logVerbosity || 'normal');
    setValue('config-placeholder-display', cfg.placeholderDisplay || 'default');
    setValue('config-widget-debug-level', cfg.widgetDebugLevel || 'basic');
    setChecked('config-widget-show-connection', cfg.widgetShowConnection);
    setChecked('config-widget-show-playback', cfg.widgetShowPlayback);
    setChecked('config-widget-show-video', cfg.widgetShowVideo);
    setChecked('config-widget-show-session', cfg.widgetShowSession);
    renderMultiStreamDashboard();
    setText('active-mode-label', cfg.modeLabel || 'consume existing video server');
    setText('managed-server-note', cfg.smokeServerManaged ? 'NiceGUI launched the local smoke server for this session.' : 'Harness is attaching to an existing server process.');
    setText('page-reload-note', cfg.autoConnect ? 'Page reload will reconnect automatically using the saved settings.' : 'Page reload keeps the settings but waits for a manual connect.');
    setDisplay('debug-panel', cfg.debugMode ? 'block' : 'none');
    switchTab(cfg.activeTab || 'smoke');
  }}

  function syncSmokeStreamSelector() {{
    const select = byId('smoke-stream-select');
    const hint = byId('smoke-stream-hint');
    if (!select) return;
    const cfg = state.config || loadConfig();
    const catalog = Array.isArray(cfg.streamCatalog) ? cfg.streamCatalog : [];
    const currentValue = cfg.streamId || '';
    const options = catalog.map((spec) => {{
      const selected = spec.streamId === currentValue ? ' selected' : '';
      const label = spec.label || spec.streamId;
      return `<option value="${{escapeHtml(spec.streamId)}}"${{selected}}>${{escapeHtml(`${{spec.streamId}} — ${{label}}`)}}</option>`;
    }});
    if (currentValue && !catalog.some((spec) => spec.streamId === currentValue)) {{
      options.unshift(`<option value="${{escapeHtml(currentValue)}}" selected>${{escapeHtml(`${{currentValue}} — custom`)}}</option>`);
    }}
    if (!options.length) {{
      options.push('<option value="">No streams configured</option>');
    }}
    select.innerHTML = options.join('');
    select.value = currentValue;
    if (hint) {{
      const selected = catalog.find((spec) => spec.streamId === currentValue);
      hint.textContent = selected
        ? `${{selected.label || selected.streamId}} • ${{selected.width}}x${{selected.height}} @ ${{selected.fps}} fps`
        : (currentValue ? `Custom stream ${{currentValue}}` : 'Pick a stream before connecting');
    }}
  }}

  function readConfigForm() {{
    const cfg = state.config || loadConfig();
    cfg.serverBase = (byId('config-server-url')?.value || '').trim();
    cfg.streamId = (byId('config-stream-id')?.value || '').trim();
    cfg.widgetFps = Math.max(0, Number(byId('config-widget-fps')?.value) || 0);
    syncSelectedStreamConfig(cfg);
    cfg.debugMode = !!byId('config-debug-mode')?.checked;
    cfg.autoReconnect = !!byId('config-auto-reconnect')?.checked;
    cfg.autoConnect = !!byId('config-auto-connect')?.checked;
    cfg.widgetShowDebug = !!byId('config-widget-show-debug')?.checked;
    cfg.sessionPollMs = Math.max(200, Number(byId('config-session-poll-ms')?.value) || 500);
    cfg.logFilter = byId('config-log-filter')?.value || 'all';
    cfg.logVerbosity = byId('config-log-verbosity')?.value || 'normal';
    cfg.placeholderDisplay = byId('config-placeholder-display')?.value || 'default';
    cfg.widgetDebugLevel = byId('config-widget-debug-level')?.value || 'basic';
    cfg.widgetShowConnection = !!byId('config-widget-show-connection')?.checked;
    cfg.widgetShowPlayback = !!byId('config-widget-show-playback')?.checked;
    cfg.widgetShowVideo = !!byId('config-widget-show-video')?.checked;
    cfg.widgetShowSession = !!byId('config-widget-show-session')?.checked;
    state.config = cfg;
    saveConfig();
    syncFormFromConfig();
    bindDebugHooks();
    updateSummary();
    appendLog('ui', `saved settings for server=${{cfg.serverBase}} stream=${{cfg.streamId}} debug=${{cfg.debugMode}}`);
  }}

  function showSettings(open) {{
    const dialog = byId('settings-dialog');
    if (!dialog) return;
    dialog.style.display = open ? 'flex' : 'none';
  }}

  function resetRuntimeState(reason='idle') {{
    state.sessionPollAbort = true;
    state.offerStatus = 'idle';
    state.candidateStatus = 'idle';
    state.connectedStreamId = '';
    state.remoteDescriptionApplied = false;
    state.remoteTrackReceived = false;
    state.playbackActive = false;
    state.lastBackendCandidate = '';
    state.appliedBackendCandidates = new Set();
    state.sessionSummary = null;
    state.statsSummary = null;
    state.observabilitySummary = null;
    state.selectedCodec = '';
    state.lastStatsAt = '';
    state.disconnectReason = reason;
    if (state.statsInterval) {{
      clearInterval(state.statsInterval);
      state.statsInterval = null;
    }}
  }}

  async function disconnect(reason='manual disconnect') {{
    if (state.reconnectTimer) {{
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = null;
    }}
    state.connectToken += 1;
    state.sessionPollAbort = true;
    if (state.statsInterval) {{
      clearInterval(state.statsInterval);
      state.statsInterval = null;
    }}
    if (state.pc) {{
      state.pc.ontrack = null;
      state.pc.onconnectionstatechange = null;
      state.pc.onicecandidate = null;
      state.pc.oniceconnectionstatechange = null;
      state.pc.onicegatheringstatechange = null;
      state.pc.onsignalingstatechange = null;
      state.pc.close();
      state.pc = null;
    }}
    clearVideos();
    resetRuntimeState(reason);
    bindDebugHooks();
    updateSummary();
    appendLog('ui', `disconnect complete (${{reason}})`);
  }}

  function scheduleReconnect(origin) {{
    if (!state.config?.autoReconnect) return;
    if (state.reconnectTimer) return;
    const delayMs = 1500;
    appendLog('ui', `auto-reconnect scheduled in ${{delayMs}}ms after ${{origin}}`);
    state.reconnectTimer = setTimeout(() => {{
      state.reconnectTimer = null;
      connect('auto-reconnect').catch((error) => appendLog('error', `auto-reconnect failed: ${{error}}`));
    }}, delayMs);
  }}

  async function postCandidate(serverBase, streamId, candidate) {{
    const response = await fetch(`${{serverBase}}/api/video/signaling/${{streamId}}/candidate`, {{
      method: 'POST',
      headers: authHeaders('text/plain'),
      body: candidate,
    }});
    if (!response.ok) {{
      throw new Error(`HTTP ${{response.status}} ${{await response.text()}}`);
    }}
  }}

  function summarizeSession(session) {{
    if (!session || typeof session !== 'object') return null;
    return {{
      peer_state: session.peer_state,
      media_bridge_state: session.media_bridge_state,
      encoded_sender_state: session.encoded_sender_state,
      preferred_media_path: session.preferred_media_path,
      encoded_sender_cached_codec_config_available: session.encoded_sender_cached_codec_config_available,
      encoded_sender_cached_idr_available: session.encoded_sender_cached_idr_available,
      encoded_sender_ready_for_video_track: session.encoded_sender_ready_for_video_track,
      encoded_sender_startup_sequence_sent: session.encoded_sender_startup_sequence_sent,
      encoded_sender_first_decodable_frame_sent: session.encoded_sender_first_decodable_frame_sent,
      encoded_sender_packets_attempted: session.encoded_sender_packets_attempted,
      encoded_sender_packets_sent_after_track_open: session.encoded_sender_packets_sent_after_track_open,
      encoded_sender_startup_sequence_injections: session.encoded_sender_startup_sequence_injections,
      encoded_sender_first_decodable_transitions: session.encoded_sender_first_decodable_transitions,
      encoded_sender_track_closed_events: session.encoded_sender_track_closed_events,
      encoded_sender_send_failures: session.encoded_sender_send_failures,
      encoded_sender_negotiated_h264_payload_type: session.encoded_sender_negotiated_h264_payload_type,
      encoded_sender_negotiated_h264_fmtp: session.encoded_sender_negotiated_h264_fmtp,
      encoded_sender_video_mid: session.encoded_sender_video_mid,
      last_local_candidate: session.last_local_candidate,
      answer_present: !!session.answer_sdp,
    }};
  }}

  function summarizeStats(report) {{
    const summary = {{}};
    report.forEach((value) => {{
      if (value.type === 'inbound-rtp' && value.kind === 'video') {{
        summary.packetsReceived = value.packetsReceived;
        summary.bytesReceived = value.bytesReceived;
        summary.framesReceived = value.framesReceived;
        summary.framesDecoded = value.framesDecoded;
        summary.framesDropped = value.framesDropped;
        summary.frameWidth = value.frameWidth;
        summary.frameHeight = value.frameHeight;
        summary.framesPerSecond = value.framesPerSecond;
        summary.codecId = value.codecId;
      }}
      if (summary.codecId && value.id === summary.codecId && value.type === 'codec') {{
        summary.codec = value.mimeType || value.sdpFmtpLine || value.payloadType;
      }}
    }});
    return summary;
  }}

  async function refreshStats() {{
    if (!state.pc || !state.config?.debugMode) return;
    try {{
      state.statsSummary = summarizeStats(await state.pc.getStats());
      state.selectedCodec = state.statsSummary.codec || state.selectedCodec || '';
      state.lastStatsAt = nowStamp();
      updateSummary();
    }} catch (error) {{
      appendLog('stats', `stats read failed: ${{error}}`);
    }}
  }}

  async function refreshSession(streamIdOverride=null) {{
    const token = state.connectToken;
    const cfg = state.config;
    if (!cfg) return;
    const streamId = streamIdOverride || state.connectedStreamId || cfg.streamId;
    if (!streamId) return;
    try {{
      const sessionResponse = await fetch(`${{cfg.serverBase}}/api/video/signaling/${{streamId}}/session`, {{
        headers: authHeaders(),
      }});
      if (!sessionResponse.ok) {{
        appendLog('session', `session poll failed: HTTP ${{sessionResponse.status}}`);
        return;
      }}
      const session = await sessionResponse.json();
      if (token !== state.connectToken) return;
      state.sessionSummary = summarizeSession(session);
      await refreshObservability();
      if (session.last_local_candidate && session.last_local_candidate !== state.lastBackendCandidate) {{
        state.lastBackendCandidate = session.last_local_candidate;
        appendLog('ice', `backend ICE candidate observed: ${{session.last_local_candidate}}`);
      }}
      if (state.remoteDescriptionApplied && session.last_local_candidate && !state.appliedBackendCandidates.has(session.last_local_candidate)) {{
        await state.pc.addIceCandidate({{
          candidate: session.last_local_candidate,
          sdpMid: session.encoded_sender_video_mid || '0',
          sdpMLineIndex: 0,
        }});
        state.appliedBackendCandidates.add(session.last_local_candidate);
        appendLog('ice', `backend ICE candidate applied: ${{session.last_local_candidate}}`);
      }}
      if (!state.remoteDescriptionApplied && session.answer_sdp) {{
        appendLog('signaling', 'answer received from backend session');
        await state.pc.setRemoteDescription({{type: 'answer', sdp: session.answer_sdp}});
        state.remoteDescriptionApplied = true;
        state.offerStatus = 'answer applied';
        appendLog('signaling', 'remote SDP answer applied');
      }}
      updateSummary();
    }} catch (error) {{
      appendLog('session', `session refresh failed: ${{error}}`);
    }}
  }}

  async function refreshObservability() {{
    const cfg = state.config;
    if (!cfg?.serverBase) return;
    try {{
      const response = await fetch(`${{cfg.serverBase}}/api/video/debug/stats`, {{
        headers: authHeaders(),
      }});
      if (!response.ok) {{
        appendLog('session', `observability poll failed: HTTP ${{response.status}}`);
        return;
      }}
      state.observabilitySummary = await response.json();
      updateSummary();
    }} catch (error) {{
      appendLog('session', `observability poll failed: ${{error}}`);
    }}
  }}

  async function connect(origin='manual connect') {{
    loadConfig();
    readConfigForm();
    const cfg = state.config;
    if (!cfg.serverBase || !cfg.streamId) {{
      appendLog('error', 'server URL and stream ID are required');
      return;
    }}
    await disconnect(`reset before connect (${{origin}})`);
    state.generation += 1;
    state.connectToken += 1;
    const token = state.connectToken;
    const streamId = cfg.streamId;
    state.sessionPollAbort = false;
    state.connectedStreamId = streamId;
    state.disconnectReason = `connecting from ${{origin}}`;
    state.offerStatus = 'creating offer';
    state.candidateStatus = 'waiting';
    updateSummary();
    appendLog('ui', `connect requested (${{origin}})`);

    const video = byId('smoke-video');
    const pc = new RTCPeerConnection({{iceServers: []}});
    state.pc = pc;
    bindDebugHooks();
    pc.addTransceiver('video', {{direction: 'recvonly'}});

    const bufferedLocalCandidates = [];
    let offerPosted = false;

    const flushBufferedCandidates = async () => {{
      if (!offerPosted || !state.remoteDescriptionApplied || bufferedLocalCandidates.length === 0) return;
      state.candidateStatus = `flushing ${{bufferedLocalCandidates.length}} candidate(s)`;
      updateSummary();
      while (bufferedLocalCandidates.length > 0) {{
        const candidate = bufferedLocalCandidates.shift();
        try {{
          await postCandidate(cfg.serverBase, streamId, candidate);
          state.candidateStatus = 'posted';
          appendLog('ice', `candidate posted: ${{candidate}}`);
        }} catch (error) {{
          state.candidateStatus = `failed: ${{error}}`;
          appendLog('error', `candidate post failed: ${{error}}`);
        }}
      }}
      updateSummary();
    }};

    pc.ontrack = (event) => {{
      state.remoteTrackReceived = true;
      appendLog('media', `remote track received (${{event.track.kind}})`);
      applyStreamToVideos(event.streams[0]);
      updateSummary();
    }};
    pc.onconnectionstatechange = () => {{
      appendLog('ice', `connectionState=${{pc.connectionState}}`);
      if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) {{
        state.disconnectReason = `peer connection ${{pc.connectionState}}`;
        scheduleReconnect(pc.connectionState);
      }}
      updateSummary();
    }};
    pc.oniceconnectionstatechange = () => {{
      appendLog('ice', `iceConnectionState=${{pc.iceConnectionState}}`);
      updateSummary();
    }};
    pc.onicegatheringstatechange = () => {{
      appendLog('ice', `iceGatheringState=${{pc.iceGatheringState}}`);
      updateSummary();
    }};
    pc.onsignalingstatechange = () => {{
      appendLog('signaling', `signalingState=${{pc.signalingState}}`);
      updateSummary();
    }};
    pc.onicecandidate = async (event) => {{
      if (!event.candidate) {{
        state.candidateStatus = 'gathering complete';
        appendLog('ice', 'local ICE gathering complete');
        updateSummary();
        return;
      }}
      const candidate = event.candidate.candidate;
      if (!offerPosted || !state.remoteDescriptionApplied) {{
        bufferedLocalCandidates.push(candidate);
        state.candidateStatus = `buffered (${{bufferedLocalCandidates.length}})`;
        appendLog('ice', `candidate buffered: ${{candidate}}`);
        updateSummary();
        return;
      }}
      try {{
        await postCandidate(cfg.serverBase, streamId, candidate);
        state.candidateStatus = 'posted';
        appendLog('ice', `candidate posted immediately: ${{candidate}}`);
      }} catch (error) {{
        state.candidateStatus = `failed: ${{error}}`;
        appendLog('error', `candidate post failed: ${{error}}`);
      }}
      updateSummary();
    }};

    const handleVideoEvent = (eventName) => {{
      state.playbackActive = !video.paused && video.readyState >= 2 && video.currentTime > 0;
      appendLog('media', `video event=${{eventName}} readyState=${{video.readyState}} paused=${{video.paused}} t=${{video.currentTime.toFixed(2)}}`);
      updateSummary();
    }};
    video.onplay = () => handleVideoEvent('play');
    video.onplaying = () => handleVideoEvent('playing');
    video.onpause = () => handleVideoEvent('pause');
    video.onwaiting = () => handleVideoEvent('waiting');
    video.onstalled = () => handleVideoEvent('stalled');
    video.onended = () => handleVideoEvent('ended');
    video.onloadedmetadata = () => handleVideoEvent('loadedmetadata');

    try {{
      appendLog('signaling', 'creating SDP offer');
      const offer = await pc.createOffer();
      if (token !== state.connectToken) return;
      await pc.setLocalDescription(offer);
      state.offerStatus = 'local description set';
      updateSummary();

      appendLog('signaling', 'posting SDP offer to server');
      const offerResponse = await fetch(`${{cfg.serverBase}}/api/video/signaling/${{streamId}}/offer`, {{
        method: 'POST',
        headers: authHeaders('text/plain'),
        body: pc.localDescription.sdp,
      }});
      if (!offerResponse.ok) {{
        const detail = await offerResponse.text();
        state.offerStatus = `failed: HTTP ${{offerResponse.status}}`;
        state.disconnectReason = `error posting offer: ${{detail}}`;
        updateSummary();
        appendLog('error', `offer failed: HTTP ${{offerResponse.status}} ${{detail}}`);
        scheduleReconnect('offer failure');
        return;
      }}
      offerPosted = true;
      state.offerStatus = 'offer posted';
      appendLog('signaling', 'offer posted successfully');
      updateSummary();

      await refreshSession(streamId);
      await flushBufferedCandidates();

      const pollLoop = async () => {{
        while (!state.sessionPollAbort && token === state.connectToken) {{
          await refreshSession(streamId);
          await refreshStats();
          await new Promise((resolve) => setTimeout(resolve, cfg.sessionPollMs));
        }}
      }};
      pollLoop();
      state.statsInterval = setInterval(() => refreshStats(), Math.max(1000, cfg.sessionPollMs));
    }} catch (error) {{
      state.offerStatus = `failed: ${{error}}`;
      state.disconnectReason = `error: ${{error}}`;
      appendLog('error', `connect failed: ${{error}}`);
      updateSummary();
      scheduleReconnect('connect exception');
      return;
    }}
  }}



  async function loadStreamConfig(openDialog=false) {{
    const cfg = state.config || loadConfig();
    if (!cfg.serverBase || !cfg.streamId) return;
    updateConfigStreamIdentity(cfg.streamId);
    setConfigStatus(`Loading config for ${{cfg.streamId}}...`);
    const response = await fetch(configUrl(cfg.serverBase, cfg.streamId), {{
      headers: authHeaders(),
    }});
    if (!response.ok) throw new Error(await response.text() || `config load failed: ${{response.status}}`);
    const data = await response.json();
    applyConfigDataToUi(cfg.streamId, data);
    setConfigStatus(`Loaded current config for ${{cfg.streamId}}.`, 'info');
    if (openDialog) showConfigDialog(true);
  }}

  async function applyStreamConfig() {{
    const cfg = state.config || loadConfig();
    if (!cfg.serverBase || !cfg.streamId) return;
    const payload = {{
      display_mode: byId('config-filter-mode')?.value || 'Passthrough',
      output_width: Number(byId('config-output-width')?.value || 0),
      output_height: Number(byId('config-output-height')?.value || 0),
      output_fps: Number(byId('config-output-fps')?.value || 0),
    }};
    setConfigStatus(`Applying config to ${{cfg.streamId}}...`);
    const response = await fetch(configUrl(cfg.serverBase, cfg.streamId), {{
      method: 'PUT',
      headers: authHeaders('application/json'),
      body: JSON.stringify(payload),
    }});
    const bodyText = await response.text();
    if (!response.ok) {{
      setConfigStatus(bodyText || `config apply failed: ${{response.status}}`, 'error');
      setHtml('config-response-json', '<pre>' + escapeHtml(bodyText || '') + '</pre>');
      throw new Error(bodyText || `config apply failed: ${{response.status}}`);
    }}
    const data = bodyText ? JSON.parse(bodyText) : null;
    if (data) {{
      applyConfigDataToUi(cfg.streamId, data, 'applied');
    }}
    setConfigStatus(`Applied config to ${{cfg.streamId}}.`, 'success');
    await refreshStats();
    await refreshSession(cfg.streamId);
  }}

  async function copyLogs() {{
    const logText = byId('smoke-log')?.textContent || '';
    await navigator.clipboard.writeText(logText);
    appendLog('ui', 'logs copied to clipboard');
  }}

  function wireUi() {{
    if (refs.initialized) return;
    refs.initialized = true;
    byId('open-settings')?.addEventListener('click', () => showSettings(true));
    byId('close-settings')?.addEventListener('click', () => showSettings(false));
    byId('tab-smoke')?.addEventListener('click', () => switchTab('smoke'));
    byId('tab-widget')?.addEventListener('click', () => switchTab('widget'));
    byId('save-settings')?.addEventListener('click', () => {{
      readConfigForm();
      showSettings(false);
    }});
    byId('connect-button')?.addEventListener('click', () => connect('connect button'));
    byId('reconnect-button')?.addEventListener('click', () => connect('reconnect button'));
    byId('disconnect-button')?.addEventListener('click', () => disconnect('manual disconnect'));
    byId('refresh-button')?.addEventListener('click', async () => {{
      appendLog('ui', 'manual refresh requested');
      await refreshSession();
      await refreshStats();
    }});
    byId('smoke-stream-select')?.addEventListener('change', (event) => {{
      const nextStreamId = event.target.value || '';
      const cfg = state.config || loadConfig();
      cfg.streamId = nextStreamId;
      state.config = syncSelectedStreamConfig(cfg);
      saveConfig();
      syncFormFromConfig();
      updateSummary();
      appendLog('ui', `selected smoke stream ${{nextStreamId || '(none)'}}`);
      if (state.pc) {{
        connect('stream selector changed').catch((error) => appendLog('error', `stream switch failed: ${{error}}`));
      }}
    }});

    byId('load-config-button')?.addEventListener('click', () => loadStreamConfig().catch((error) => appendLog('error', `load config failed: ${{error}}`)));
    byId('apply-config-button')?.addEventListener('click', () => applyStreamConfig().catch((error) => appendLog('error', `apply config failed: ${{error}}`)));
    byId('close-config-dialog')?.addEventListener('click', () => showConfigDialog(false));

    byId('copy-logs')?.addEventListener('click', () => copyLogs().catch((error) => appendLog('error', `copy failed: ${{error}}`)));
    byId('config-log-filter')?.addEventListener('change', () => {{ readConfigForm(); appendLog('ui', 'log filter updated'); }});
    byId('context-connect')?.addEventListener('click', () => {{ hideContextMenu(); connect('context connect'); }});
    byId('context-reconnect')?.addEventListener('click', () => {{ hideContextMenu(); connect('context reconnect'); }});
    byId('context-disconnect')?.addEventListener('click', () => {{ hideContextMenu(); disconnect('context disconnect'); }});
    byId('context-refresh')?.addEventListener('click', async () => {{ hideContextMenu(); await refreshSession(); await refreshStats(); }});
    byId('context-edit-config')?.addEventListener('click', () => {{
      hideContextMenu();
      loadStreamConfig(true).catch((error) => {{
        setConfigStatus(String(error), 'error');
        showConfigDialog(true);
        appendLog('error', `load config failed: ${{error}}`);
      }});
    }});
    byId('context-open-settings')?.addEventListener('click', () => {{ hideContextMenu(); showSettings(true); }});
    byId('context-toggle-widget-debug')?.addEventListener('click', () => {{
      const input = byId('config-widget-show-debug');
      if (input) input.checked = !input.checked;
      readConfigForm();
      hideContextMenu();
    }});
    document.addEventListener('keydown', (event) => {{
      if (event.key === 'Escape') showSettings(false);
      if (event.key === 'Escape') showConfigDialog(false);
    }});
    document.addEventListener('click', () => hideContextMenu());
    const widgetWrap = byId('widget-shell');
    widgetWrap?.addEventListener('contextmenu', (event) => {{
      event.preventDefault();
      showContextMenu(event.clientX, event.clientY);
      appendLog('ui', 'opened widget context menu');
    }});
  }}

  function init() {{
    loadConfig();
    wireUi();
    syncFormFromConfig();
    appendLog('ui', 'harness initialized');
    updateSummary();
    const multiStreamWidgetTab = !widgetMode && (state.config.streamCatalog || []).length > 1 &&
      (state.config.activeTab || 'widget') === 'widget';
    if (state.config.autoConnect && !multiStreamWidgetTab) {{
      connect('page load').catch((error) => appendLog('error', `auto-connect failed: ${{error}}`));
    }}
  }}

  return {{
    init,
    connect,
    disconnect,
    refreshSession,
    refreshStats,
    showSettings,
    readConfigForm,
  }};
}})();

if (!window.__videoSmokeHarnessBootstrapped) {{
  window.__videoSmokeHarnessBootstrapped = true;
  const bootstrapHarness = () => window.videoSmokeHarness?.init();
  if (document.readyState === 'loading') {{
    document.addEventListener('DOMContentLoaded', bootstrapHarness, {{once: true}});
  }} else {{
    setTimeout(bootstrapHarness, 0);
  }}
}}
</script>
"""

PAGE_HTML = """
<div class="harness-shell">
  <div class="toolbar-row">
    <div>
      <div class="title">video-server NiceGUI browser harness</div>
      <div class="subtitle">A manual WebRTC debug client for the H264 browser path.</div>
    </div>
    <div class="toolbar-actions">
      <button id="connect-button" class="toolbar-button primary">Connect</button>
      <button id="reconnect-button" class="toolbar-button">Reconnect</button>
      <button id="disconnect-button" class="toolbar-button danger">Disconnect</button>
      <button id="refresh-button" class="toolbar-button">Refresh debug</button>
      <button id="open-settings" class="toolbar-button">Settings</button>
    </div>
  </div>

  <div id="smoke-tab-button-row" class="tab-row">
    <button id="tab-smoke" class="tab-button active">Smoke debug</button>
    <button id="tab-widget" class="tab-button">Widget preview</button>
  </div>

  <div id="info-note-block" class="info-note">
    <div><strong>Mode:</strong> <span id="active-mode-label"></span></div>
    <div id="managed-server-note"></div>
    <div id="page-reload-note"></div>
  </div>

  <div id="smoke-tab-panel">
    <div class="summary-grid">
      <div class="summary-card"><div class="summary-label">Connection</div><div id="summary-connection" class="status-badge">idle</div></div>
      <div class="summary-card"><div class="summary-label">Remote track</div><div id="summary-track" class="status-badge">no remote track</div></div>
      <div class="summary-card"><div class="summary-label">Playback</div><div id="summary-playback" class="status-badge">not rendering</div></div>
      <div class="summary-card"><div class="summary-label">Offer/answer</div><div id="summary-offer" class="status-badge">idle</div></div>
      <div class="summary-card"><div class="summary-label">Candidates</div><div id="summary-candidates" class="status-badge">idle</div></div>
      <div class="summary-card"><div class="summary-label">Generation</div><div id="summary-generation" class="summary-value">0</div></div>
      <div class="summary-card"><div class="summary-label">Server</div><div id="summary-server" class="summary-value"></div></div>
      <div class="summary-card"><div class="summary-label">Stream</div><div id="summary-stream" class="summary-value"></div></div>
      <div class="summary-card"><div class="summary-label">Last stats</div><div id="summary-last-stats" class="summary-value">never</div></div>
    </div>

    <div class="main-grid">
      <div class="video-panel">
        <div class="panel-header">
          <div>
            <div class="panel-title">Player</div>
            <div class="panel-subtitle">Full smoke/debug view with all telemetry. Choose which configured stream this tab should watch.</div>
          </div>
          <div class="playback-headline">
            <div id="playback-headline">idle</div>
            <div id="playback-detail" class="panel-subtitle">waiting</div>
          </div>
        </div>
        <div class="smoke-stream-picker">
          <label for="smoke-stream-select">Smoke tab stream</label>
          <select id="smoke-stream-select"></select>
          <div id="smoke-stream-hint" class="panel-subtitle">Pick a stream before connecting</div>
        </div>
        <div id="video-shell" class="video-shell">
          <video id="smoke-video" autoplay playsinline muted controls></video>
        </div>
        <div class="video-stats-grid">
          <div><span>readyState</span><strong id="video-ready-state">n/a</strong></div>
          <div><span>paused</span><strong id="video-paused">n/a</strong></div>
          <div><span>currentTime</span><strong id="video-current-time">n/a</strong></div>
          <div><span>video size</span><strong id="video-size">n/a</strong></div>
          <div><span>networkState</span><strong id="video-network-state">n/a</strong></div>
          <div><span>muted</span><strong id="video-muted-state">n/a</strong></div>
        </div>
      </div>

      <div class="side-panel">
        <div class="panel-card">
          <div class="panel-header compact">
            <div>
              <div class="panel-title">Logs</div>
              <div class="panel-subtitle">Timestamped by category.</div>
            </div>
            <button id="copy-logs" class="toolbar-button small">Copy</button>
          </div>
          <pre id="smoke-log" class="log-area"></pre>
        </div>
        <details id="debug-panel" class="panel-card" open>
          <summary class="panel-title">Debug telemetry</summary>
          <div class="debug-grid">
            <div><span>connectionState</span><strong id="debug-connection-state">closed</strong></div>
            <div><span>iceConnectionState</span><strong id="debug-ice-connection-state">closed</strong></div>
            <div><span>iceGatheringState</span><strong id="debug-ice-gathering-state">closed</strong></div>
            <div><span>signalingState</span><strong id="debug-signaling-state">closed</strong></div>
            <div><span>generation</span><strong id="debug-generation">0</strong></div>
            <div><span>server / stream</span><strong id="debug-server-stream"></strong></div>
            <div><span>offer status</span><strong id="debug-offer-status">idle</strong></div>
            <div><span>candidate status</span><strong id="debug-candidate-status">idle</strong></div>
            <div><span>remote description</span><strong id="debug-remote-description">no</strong></div>
            <div><span>remote track</span><strong id="debug-remote-track">no</strong></div>
            <div><span>playback</span><strong id="debug-playback">inactive</strong></div>
            <div><span>backend candidate</span><strong id="debug-last-candidate">none</strong></div>
            <div><span>codec</span><strong id="debug-codec">unknown</strong></div>
            <div><span>last stats</span><strong id="debug-last-stats">never</strong></div>
          </div>

          <div class="subpanel-title">Session summary</div>
          <div class="debug-grid">
            <div><span>peer_state</span><strong id="session-peer-state">n/a</strong></div>
            <div><span>media_bridge_state</span><strong id="session-media-bridge">n/a</strong></div>
            <div><span>sender_state</span><strong id="session-sender-state">n/a</strong></div>
            <div><span>preferred path</span><strong id="session-preferred-path">n/a</strong></div>
            <div><span>startup sent</span><strong id="session-startup">n/a</strong></div>
            <div><span>first decodable</span><strong id="session-first-dec">n/a</strong></div>
            <div><span>packets after open</span><strong id="session-packets-open">n/a</strong></div>
          </div>
          <div id="session-json" class="json-box"><pre>{}</pre></div>

          <div class="subpanel-title">Browser stats</div>
          <div class="debug-grid">
            <div><span>packets</span><strong id="stats-packets">n/a</strong></div>
            <div><span>bytes</span><strong id="stats-bytes">n/a</strong></div>
            <div><span>frames received</span><strong id="stats-frames-received">n/a</strong></div>
            <div><span>frames decoded</span><strong id="stats-frames-decoded">n/a</strong></div>
            <div><span>frames dropped</span><strong id="stats-frames-dropped">n/a</strong></div>
            <div><span>resolution</span><strong id="stats-resolution">n/a</strong></div>
            <div><span>FPS</span><strong id="stats-fps">n/a</strong></div>
          </div>
        </details>
      </div>
    </div>
  </div>

  <div id="widget-tab-panel" style="display:none;">
    <div id="widget-dashboard-wrapper" class="widget-dashboard-wrapper">
      <div class="summary-card">
        <div class="summary-label">Manual multi-stream compare</div>
        <div class="info-note">Each card below loads an isolated widget instance bound to one stream.</div>
        <div id="multi-stream-dashboard" class="multi-stream-dashboard"></div>
      </div>
    </div>
    <div class="widget-layout">
      <div id="widget-shell" class="widget-box">
        <div class="widget-header">
          <div>
            <div class="panel-title">Widget preview</div>
            <div class="panel-subtitle">Right-click the widget to connect, refresh, or edit backend stream output config.</div>
            <div class="widget-stream-chip">
              <strong id="widget-stream-id">no stream</strong>
              <span id="widget-stream-label">No stream selected</span>
              <span id="widget-stream-meta">custom stream</span>
            </div>
          </div>
        </div>
        <div class="widget-video-wrap">
          <video id="widget-video" autoplay playsinline muted controls></video>
        </div>
        <div class="widget-status-bar compact">
          <div class="widget-status-chip combined">
            <div class="widget-health">
              <span id="widget-health-dot" class="health-dot idle"></span>
              <strong id="widget-health-summary">idle • no stream</strong>
            </div>
          </div>
          <div class="widget-status-chip combined">Expected <strong id="widget-expected-fps">n/a</strong> fps</div>
          <div class="widget-status-chip combined">Geometry <strong id="widget-target-geometry">n/a</strong></div>
          <div class="widget-status-chip combined">Active config <strong id="widget-active-config">Waiting for config</strong></div>
        </div>
        <div id="widget-debug-sections" class="widget-debug-sections" style="display:none;">
          <div id="widget-connection-section" class="widget-debug-card">
            <span>Connection</span>
            <strong id="widget-connection-state">closed</strong>
            <span>Signaling</span>
            <strong id="widget-signaling-state">closed</strong>
          </div>
          <div id="widget-playback-section" class="widget-debug-card">
            <span>Playback</span>
            <strong id="widget-playback-state">waiting</strong>
            <span>Track</span>
            <strong id="widget-track-state">no</strong>
          </div>
          <div id="widget-video-section" class="widget-debug-card">
            <span>Video size</span>
            <strong id="widget-video-size">n/a</strong>
            <span>Current time</span>
            <strong id="widget-video-time">n/a</strong>
          </div>
          <div id="widget-session-section" class="widget-debug-card">
            <span>peer_state</span>
            <strong id="widget-session-peer">n/a</strong>
            <span>sender_state</span>
            <strong id="widget-session-sender">n/a</strong>
          </div>
          <div id="widget-observability-section" class="widget-debug-card">
            <span>observability streams/sessions</span>
            <strong><span id="obs-stream-count">n/a</span> / <span id="obs-session-count">n/a</span></strong>
            <div id="observability-grid" class="observability-grid"></div>
          </div>
        </div>
      </div>

      <div id="widget-context-menu" class="context-menu" style="display:none;">
        <div class="context-group">
          <div class="context-title">Actions</div>
          <button id="context-connect" class="context-button">Connect</button>
          <button id="context-reconnect" class="context-button">Reload / reconnect</button>
          <button id="context-disconnect" class="context-button">Disconnect</button>
          <button id="context-refresh" class="context-button">Refresh status</button>
        </div>
        <div class="context-group">
          <div class="context-title">Stream config</div>
          <button id="context-edit-config" class="context-button">Edit stream config</button>
          <div class="context-note">Loads current backend config for the selected stream and applies changes through the existing API.</div>
        </div>
        <div class="context-group">
          <div class="context-title">Local widget settings</div>
          <button id="context-open-settings" class="context-button">Open settings panel</button>
          <button id="context-toggle-widget-debug" class="context-button">Toggle in-box debug</button>
        </div>
      </div>
    </div>
  </div>

  <div id="config-dialog" class="settings-backdrop" style="display:none;">
    <div class="settings-card config-dialog-card">
      <div class="panel-header compact">
        <div>
          <div class="panel-title">Widget stream config</div>
          <div class="panel-subtitle">Right-click entry point for per-stream output config edits.</div>
        </div>
        <button id="close-config-dialog" class="toolbar-button small">Close</button>
      </div>
      <div class="config-stream-identity">
        <strong id="config-stream-name">No stream selected</strong>
        <span id="config-stream-meta">custom stream</span>
      </div>
      <div class="config-summary-grid">
        <div class="summary-card compact-card">
          <div class="summary-label">Current backend config</div>
          <div id="config-current-summary" class="summary-value">Load a stream config to inspect it.</div>
        </div>
        <div class="summary-card compact-card">
          <div class="summary-label">Observed active config</div>
          <div id="config-observed-summary" class="summary-value">Waiting for observability data</div>
        </div>
        <div class="summary-card compact-card">
          <div class="summary-label">Config generation</div>
          <div id="config-generation" class="summary-value">0</div>
        </div>
      </div>
      <div id="config-feedback" class="config-feedback info">Waiting for config action.</div>
      <label>Palette / filter mode
        <select id="config-filter-mode">
          <option>Passthrough</option><option>Grayscale</option><option>WhiteHot</option><option>BlackHot</option><option>Ironbow</option><option>Arctic</option><option>Rainbow</option>
        </select>
      </label>
      <div class="config-edit-grid">
        <label>Output width <input id="config-output-width" type="number" min="0" step="1" value="0"></label>
        <label>Output height <input id="config-output-height" type="number" min="0" step="1" value="0"></label>
        <label>Output fps <input id="config-output-fps" type="number" min="0" step="0.1" value="0"></label>
      </div>
      <div class="settings-actions split-actions">
        <button id="load-config-button" class="toolbar-button">Reload from backend</button>
        <button id="apply-config-button" class="toolbar-button primary">Apply to stream</button>
      </div>
      <div id="config-response-json" class="json-box"><pre>{}</pre></div>
    </div>
  </div>

  <div id="settings-dialog" class="settings-backdrop" style="display:none;">
    <div class="settings-card">
      <div class="panel-header compact">
        <div>
          <div class="panel-title">Stream settings</div>
          <div class="panel-subtitle">Visible button + video context menu access.</div>
        </div>
        <button id="close-settings" class="toolbar-button small">Close</button>
      </div>
      <label>Server URL<input id="config-server-url" type="text" placeholder="http://127.0.0.1:8080"></label>
      <label>Stream ID<input id="config-stream-id" type="text" placeholder="synthetic-h264"></label>
      <label>Session poll interval (ms)<input id="config-session-poll-ms" type="number" min="200" step="100"></label>
      <label>Log filter
        <select id="config-log-filter">
          <option value="all">All categories</option>
          <option value="ui">UI</option>
          <option value="signaling">Signaling</option>
          <option value="ice">ICE</option>
          <option value="media">Media</option>
          <option value="stats">Stats</option>
          <option value="session">Session</option>
          <option value="error">Error</option>
        </select>
      </label>
      <label>Log verbosity
        <select id="config-log-verbosity">
          <option value="normal">Normal</option>
          <option value="verbose">Verbose placeholder</option>
        </select>
      </label>
      <label>Display mode placeholder
        <select id="config-placeholder-display">
          <option value="default">Default</option>
          <option value="fit-width">Fit width placeholder</option>
          <option value="pixel-inspect">Pixel inspect placeholder</option>
        </select>
      </label>
      <label class="checkbox-row"><input id="config-debug-mode" type="checkbox"> Enable debug telemetry + browser console hooks</label>
      <label class="checkbox-row"><input id="config-auto-reconnect" type="checkbox"> Reconnect automatically after failures/disconnects</label>
      <label class="checkbox-row"><input id="config-auto-connect" type="checkbox"> Connect automatically after reload/open</label>
      <div class="settings-section-title">Widget preview box</div>
      <label class="checkbox-row"><input id="config-widget-show-debug" type="checkbox"> Show debug info inside the widget box</label>
      <label>Widget debug level
        <select id="config-widget-debug-level">
          <option value="basic">Basic</option>
          <option value="detailed">Detailed</option>
          <option value="full">Full</option>
        </select>
      </label>
      <label class="checkbox-row"><input id="config-widget-show-connection" type="checkbox"> Include connection/signaling fields</label>
      <label class="checkbox-row"><input id="config-widget-show-playback" type="checkbox"> Include playback/track fields</label>
      <label class="checkbox-row"><input id="config-widget-show-video" type="checkbox"> Include video element fields</label>
      <label class="checkbox-row"><input id="config-widget-show-session" type="checkbox"> Include session summary fields</label>
      <label>Expected fps<input id="config-widget-fps" type="number" min="0" step="0.1"></label>
      <div class="settings-actions">
        <button id="save-settings" class="toolbar-button primary">Save settings</button>
      </div>
    </div>
  </div>
</div>
"""

PAGE_CSS = """
<style>
body { background: #111827; }
.harness-shell { color: #e5eefc; width: min(1800px, 100%); margin: 0 auto; }
.toolbar-row, .panel-header { display: flex; justify-content: space-between; gap: 1rem; align-items: center; }
.toolbar-row { flex-wrap: wrap; margin-bottom: 1rem; }
.tab-row { display: flex; gap: 0.75rem; margin-bottom: 1rem; }
.tab-button { background: #0f172a; color: #cbd5e1; border: 1px solid #334155; border-radius: 999px; padding: 0.5rem 1rem; cursor: pointer; }
.tab-button.active { background: #2563eb; color: #fff; }
.title { font-size: 1.8rem; font-weight: 700; }
.subtitle, .panel-subtitle, .info-note { color: #9fb0ca; }
.toolbar-actions { display: flex; flex-wrap: wrap; gap: 0.5rem; }
.toolbar-button { background: #1f2937; color: #e5eefc; border: 1px solid #334155; border-radius: 0.75rem; padding: 0.6rem 1rem; cursor: pointer; }
.toolbar-button.primary { background: #2563eb; }
.toolbar-button.danger { background: #7f1d1d; }
.toolbar-button.small { padding: 0.35rem 0.7rem; }
.smoke-stream-picker { display: grid; gap: 0.35rem; margin-bottom: 0.85rem; }
.smoke-stream-picker label { font-size: 0.9rem; color: #cbd5e1; font-weight: 600; }
.smoke-stream-picker select { width: 100%; max-width: 420px; background: #020617; color: #e5eefc; border: 1px solid #334155; border-radius: 0.7rem; padding: 0.65rem 0.8rem; }
.info-note { background: #0f172a; border: 1px solid #1e293b; border-radius: 1rem; padding: 0.85rem 1rem; margin-bottom: 1rem; }
.summary-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 0.75rem; margin-bottom: 1rem; }
.summary-card, .panel-card, .video-panel { background: #0f172a; border: 1px solid #1e293b; border-radius: 1rem; padding: 1rem; }
.summary-label { color: #94a3b8; font-size: 0.85rem; margin-bottom: 0.4rem; }
.summary-value { word-break: break-word; }
.status-badge { display: inline-block; padding: 0.35rem 0.65rem; border-radius: 999px; background: #1e293b; }
.status-badge.good { background: #14532d; }
.status-badge.warn { background: #78350f; }
.status-badge.bad { background: #7f1d1d; }
.main-grid { display: grid; grid-template-columns: minmax(0, 2fr) minmax(340px, 1fr); gap: 1rem; align-items: start; }
.video-shell { margin-top: 1rem; background: #020617; border: 1px solid #334155; border-radius: 1rem; padding: 0.75rem; }
#smoke-video { width: 100%; min-height: 320px; background: #000; border-radius: 0.75rem; }
.video-stats-grid, .debug-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 0.75rem; margin-top: 1rem; }
.video-stats-grid div, .debug-grid div { background: #111827; padding: 0.7rem; border-radius: 0.75rem; border: 1px solid #1f2937; }
.video-stats-grid span, .debug-grid span { display: block; color: #94a3b8; font-size: 0.8rem; margin-bottom: 0.25rem; }
.side-panel { display: grid; gap: 1rem; }
.log-area { min-height: 360px; max-height: 460px; overflow: auto; background: #020617; color: #8ef; padding: 1rem; border-radius: 0.75rem; white-space: pre-wrap; }
.subpanel-title { margin-top: 1rem; font-weight: 700; }
.json-box pre { margin: 0.75rem 0 0; max-height: 220px; overflow: auto; background: #020617; color: #cbd5e1; padding: 0.9rem; border-radius: 0.75rem; }
.widget-layout { position: relative; }
.multi-stream-dashboard { display:grid; grid-template-columns:repeat(auto-fit,minmax(560px,1fr)); gap:20px; margin-top:12px; }
.multi-card { border:1px solid #1e293b; border-radius:16px; background:#0f172a; padding:16px; }
.multi-card-header { display:flex; justify-content:space-between; gap:12px; flex-wrap:wrap; margin-bottom:10px; font-size:14px; color:#9fb0ca; }
.multi-widget-frame { width:100%; min-height:680px; border:0; border-radius:14px; background:#020617; }
.widget-dashboard-wrapper { margin-bottom:16px; }
.widget-box { background: #0f172a; border: 1px solid #1e293b; border-radius: 1rem; padding: 1rem; width: min(100%, 980px); min-height: 680px; }
.widget-header { display: flex; justify-content: space-between; gap: 1rem; align-items: start; margin-bottom: 0.75rem; }
.widget-stream-chip { display: grid; gap: 0.2rem; margin-top: 0.65rem; padding: 0.75rem 0.9rem; background: #111827; border: 1px solid #1f2937; border-radius: 0.85rem; max-width: 420px; }
.widget-stream-chip span { color: #94a3b8; font-size: 0.88rem; }
.widget-video-wrap { background: #020617; border: 1px solid #334155; border-radius: 1rem; padding: 0.75rem; min-height: 420px; }
#widget-video { width: 100%; min-height: 420px; background: #000; border-radius: 0.75rem; }
.widget-status-bar { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 0.75rem; margin-top: 0.9rem; }
.widget-status-bar.compact { grid-template-columns: minmax(0, 1fr); }
.widget-status-chip, .widget-debug-card { background: #111827; border: 1px solid #1f2937; border-radius: 0.75rem; padding: 0.75rem; }
.widget-status-chip.combined { padding: 0.9rem 1rem; }
.widget-health { display: flex; align-items: center; gap: 0.5rem; }
.health-dot { width: 0.85rem; height: 0.85rem; border-radius: 999px; display: inline-block; background: #475569; }
.health-dot.good { background: #22c55e; }
.health-dot.warn { background: #f59e0b; }
.health-dot.bad { background: #ef4444; }
.health-dot.idle { background: #64748b; }
.widget-debug-sections { display: grid; gap: 0.75rem; margin-top: 0.9rem; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); }
.widget-debug-card span { display: block; color: #94a3b8; font-size: 0.8rem; margin-bottom: 0.25rem; }
.widget-debug-card strong { display: block; margin-bottom: 0.45rem; }
.observability-grid { display: grid; gap: 0.5rem; }
.context-menu { position: fixed; z-index: 60; width: min(320px, calc(100vw - 2rem)); background: #020617; border: 1px solid #334155; border-radius: 1rem; box-shadow: 0 18px 40px rgba(0,0,0,0.35); padding: 0.75rem; }
.context-group + .context-group { margin-top: 0.75rem; padding-top: 0.75rem; border-top: 1px solid #1e293b; }
.context-title, .settings-section-title { font-weight: 700; margin-bottom: 0.35rem; }
.context-button { width: 100%; text-align: left; background: #111827; color: #e5eefc; border: 1px solid #1f2937; border-radius: 0.65rem; padding: 0.55rem 0.7rem; margin-top: 0.35rem; cursor: pointer; }
.context-note { color: #94a3b8; font-size: 0.9rem; }
.settings-backdrop { position: fixed; inset: 0; background: rgba(2, 6, 23, 0.75); align-items: center; justify-content: center; z-index: 50; }
.settings-card { width: min(560px, calc(100vw - 2rem)); background: #0f172a; border: 1px solid #334155; border-radius: 1rem; padding: 1rem; display: grid; gap: 0.8rem; }
.config-dialog-card { width: min(720px, calc(100vw - 2rem)); }
.settings-card label { display: grid; gap: 0.35rem; color: #cbd5e1; }
.settings-card input, .settings-card select { width: 100%; background: #020617; color: #e5eefc; border: 1px solid #334155; border-radius: 0.7rem; padding: 0.65rem 0.8rem; }
.config-stream-identity { display: grid; gap: 0.2rem; padding: 0.8rem 0.9rem; background: #111827; border: 1px solid #1f2937; border-radius: 0.85rem; }
.config-stream-identity span { color: #94a3b8; }
.config-summary-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 0.75rem; }
.compact-card { padding: 0.85rem; }
.config-edit-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 0.75rem; }
.config-feedback { border-radius: 0.85rem; padding: 0.8rem 0.9rem; border: 1px solid #334155; background: #111827; color: #dbeafe; }
.config-feedback.success { border-color: #14532d; background: #052e16; color: #dcfce7; }
.config-feedback.error { border-color: #7f1d1d; background: #450a0a; color: #fee2e2; }
.split-actions { justify-content: space-between; gap: 0.75rem; }
.checkbox-row { display: flex !important; align-items: center; gap: 0.6rem; }
.checkbox-row input { width: auto; }
.settings-actions { display: flex; justify-content: end; }
@media (max-width: 1100px) { .main-grid { grid-template-columns: 1fr; } .multi-stream-dashboard { grid-template-columns: 1fr; } .multi-widget-frame, .widget-box, #widget-video, .widget-video-wrap { min-height: 520px; } }
</style>
"""


@ui.page('/')
def index() -> None:
    ui.add_head_html(PAGE_JS)
    ui.add_head_html(PAGE_CSS)
    ui.page_title('video-server NiceGUI browser harness')

    with ui.column().classes('w-full items-center gap-4 p-6'):
        ui.markdown(
            f"""
This page is a manual browser harness for the current H264 WebRTC consumer path.

- **Video server:** `{ARGS.video_server_url}`
- **Default primary stream id:** `{DEFAULT_STREAM_ID}`
- **Configured streams:** `{', '.join(spec['streamId'] for spec in STREAM_SPECS)}`
- **Mode:** `{INITIAL_CONFIG['modeLabel']}`
- **Settings access:** use the **Settings** button or right-click the video area.
            """
        ).classes('max-w-5xl w-full')
        ui.html(PAGE_HTML).classes('w-full')

if __name__ in {'__main__', '__mp_main__'}:
    try:
        ui.run(host=ARGS.ui_host, port=ARGS.ui_port, reload=False, show=False, title='video-server NiceGUI browser harness')
    finally:
        if SMOKE_PROCESS is not None and SMOKE_PROCESS.poll() is None:
            try:
                if SMOKE_PROCESS.stdin:
                    SMOKE_PROCESS.stdin.write('\n')
                    SMOKE_PROCESS.stdin.flush()
            except BrokenPipeError:
                os.kill(SMOKE_PROCESS.pid, signal.SIGTERM)
