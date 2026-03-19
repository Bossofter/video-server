#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import os
import shlex
import signal
import subprocess
from pathlib import Path
from typing import Optional

import imageio_ffmpeg
from nicegui import app, ui

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SMOKE_BINARY = ROOT / 'build' / 'video_server_nicegui_smoke_server'


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='NiceGUI smoke harness for the video-server WebRTC H264 path.')
    parser.add_argument('--video-server-url', default='http://127.0.0.1:8080', help='Base URL for the running video server.')
    parser.add_argument('--stream-id', default='synthetic-h264', help='Synthetic stream id to consume.')
    parser.add_argument('--ui-host', default='127.0.0.1', help='NiceGUI host.')
    parser.add_argument('--ui-port', type=int, default=8090, help='NiceGUI port.')
    parser.add_argument('--start-server', action='store_true', help='Launch the smoke C++ server executable automatically.')
    parser.add_argument('--smoke-binary', default=str(DEFAULT_SMOKE_BINARY), help='Path to the smoke server executable.')
    parser.add_argument('--server-host', default='127.0.0.1', help='Host to pass to the smoke server when --start-server is used.')
    parser.add_argument('--server-port', type=int, default=8080, help='Port to pass to the smoke server when --start-server is used.')
    parser.add_argument('--width', type=int, default=640, help='Synthetic stream width for the launched smoke server.')
    parser.add_argument('--height', type=int, default=360, help='Synthetic stream height for the launched smoke server.')
    parser.add_argument('--fps', type=float, default=30.0, help='Synthetic stream FPS for the launched smoke server.')
    parser.add_argument('--auto-connect', action='store_true', help='Auto-connect the browser harness on page load.')
    parser.add_argument('--debug', action='store_true', help='Start with debug telemetry visible.')
    parser.add_argument('--auto-reconnect', action='store_true', help='Retry automatically after connection failures/disconnects.')
    parser.add_argument('--session-poll-ms', type=int, default=500, help='Session poll interval in milliseconds.')
    return parser.parse_args()


ARGS = parse_args()
SMOKE_PROCESS: Optional[subprocess.Popen[str]] = None


def start_smoke_server() -> subprocess.Popen[str]:
    ffmpeg_exe = imageio_ffmpeg.get_ffmpeg_exe()
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
        '--stream-id',
        ARGS.stream_id,
        '--width',
        str(ARGS.width),
        '--height',
        str(ARGS.height),
        '--fps',
        str(ARGS.fps),
        '--ffmpeg',
        ffmpeg_exe,
    ]
    print('[nicegui-smoke] launching:', ' '.join(shlex.quote(part) for part in cmd), flush=True)
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, text=True)


if ARGS.start_server:
    SMOKE_PROCESS = start_smoke_server()
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
    'streamId': ARGS.stream_id,
    'debugMode': ARGS.debug,
    'autoReconnect': ARGS.auto_reconnect,
    'autoConnect': True if ARGS.auto_connect or ARGS.start_server else False,
    'sessionPollMs': max(200, ARGS.session_poll_ms),
    'modeLabel': 'launch smoke server + consume WebRTC stream' if ARGS.start_server else 'consume existing video server',
    'smokeServerManaged': ARGS.start_server,
}

PAGE_JS = f"""
<script>
window.videoSmokeDefaults = {json.dumps(INITIAL_CONFIG)};
window.videoSmokeHarness = (() => {{
  const STORAGE_KEY = 'video-smoke-harness-config-v2';
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
    offerStatus: 'idle',
    candidateStatus: 'idle',
    remoteDescriptionApplied: false,
    remoteTrackReceived: false,
    playbackActive: false,
    lastBackendCandidate: '',
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

  function loadConfig() {{
    let saved = {{}};
    try {{
      saved = JSON.parse(localStorage.getItem(STORAGE_KEY) || '{{}}');
    }} catch (error) {{
      console.warn('[video-smoke] failed to parse saved config', error);
    }}
    state.config = Object.assign({{}}, window.videoSmokeDefaults || {{}}, saved);
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
    state.config.activeTab = state.config.activeTab || 'smoke';
    return state.config;
  }}

  function saveConfig() {{
    if (!state.config) return;
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state.config));
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
    setText('summary-stream', cfg.streamId || '');
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
    setText('debug-server-stream', `${{cfg.serverBase}} / ${{cfg.streamId}}`);
    setText('debug-offer-status', state.offerStatus);
    setText('debug-candidate-status', state.candidateStatus);
    setText('debug-remote-description', state.remoteDescriptionApplied ? 'yes' : 'no');
    setText('debug-remote-track', state.remoteTrackReceived ? 'yes' : 'no');
    setText('debug-playback', state.playbackActive ? 'active' : 'inactive');
    setText('debug-last-candidate', state.lastBackendCandidate || 'none');
    setText('debug-codec', state.selectedCodec || 'unknown');
    setText('debug-last-stats', state.lastStatsAt || 'never');

    const session = state.sessionSummary || {{}};
    setText('session-peer-state', session.peer_state || 'n/a');
    setText('session-media-bridge', session.media_bridge_state || 'n/a');
    setText('session-sender-state', session.encoded_sender_state || 'n/a');
    setText('session-preferred-path', session.preferred_media_path || 'n/a');
    setText('session-startup', session.encoded_sender_startup_sequence_sent ? 'yes' : 'no');
    setText('session-first-dec', session.encoded_sender_first_decodable_frame_sent ? 'yes' : 'no');
    setText('session-packets-open', String(session.encoded_sender_packets_sent_after_track_open ?? 'n/a'));
    setHtml('session-json', `<pre>${{shortJson(session)}}</pre>`);

    const stats = state.statsSummary || {{}};
    setText('stats-packets', String(stats.packetsReceived ?? 'n/a'));
    setText('stats-bytes', String(stats.bytesReceived ?? 'n/a'));
    setText('stats-frames-received', String(stats.framesReceived ?? 'n/a'));
    setText('stats-frames-decoded', String(stats.framesDecoded ?? 'n/a'));
    setText('stats-frames-dropped', String(stats.framesDropped ?? 'n/a'));
    setText('stats-resolution', stats.frameWidth && stats.frameHeight ? `${{stats.frameWidth}}x${{stats.frameHeight}}` : 'n/a');
    setText('stats-fps', String(stats.framesPerSecond ?? 'n/a'));

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
    setText('widget-session-peer', session.peer_state || 'n/a');
    setText('widget-session-sender', session.encoded_sender_state || 'n/a');
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
    cfg.activeTab = tabName;
    state.config = cfg;
    saveConfig();
    setDisplay('smoke-tab-panel', tabName === 'smoke' ? 'block' : 'none');
    setDisplay('widget-tab-panel', tabName === 'widget' ? 'block' : 'none');
    byId('tab-smoke')?.classList.toggle('active', tabName === 'smoke');
    byId('tab-widget')?.classList.toggle('active', tabName === 'widget');
  }}

  function showContextMenu(x, y) {{
    const menu = byId('widget-context-menu');
    if (!menu) return;
    menu.style.display = 'block';
    menu.style.left = `${{x}}px`;
    menu.style.top = `${{y}}px`;
  }}

  function hideContextMenu() {{
    setDisplay('widget-context-menu', 'none');
  }}

  function syncFormFromConfig() {{
    const cfg = state.config || loadConfig();
    setValue('config-server-url', cfg.serverBase || '');
    setValue('config-stream-id', cfg.streamId || '');
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
    setText('active-mode-label', cfg.modeLabel || 'consume existing video server');
    setText('managed-server-note', cfg.smokeServerManaged ? 'NiceGUI launched the local smoke server for this session.' : 'Harness is attaching to an existing server process.');
    setText('page-reload-note', cfg.autoConnect ? 'Page reload will reconnect automatically using the saved settings.' : 'Page reload keeps the settings but waits for a manual connect.');
    setDisplay('debug-panel', cfg.debugMode ? 'block' : 'none');
    switchTab(cfg.activeTab || 'smoke');
  }}

  function readConfigForm() {{
    const cfg = state.config || loadConfig();
    cfg.serverBase = (byId('config-server-url')?.value || '').trim();
    cfg.streamId = (byId('config-stream-id')?.value || '').trim();
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
    state.remoteDescriptionApplied = false;
    state.remoteTrackReceived = false;
    state.playbackActive = false;
    state.lastBackendCandidate = '';
    state.sessionSummary = null;
    state.statsSummary = null;
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
      headers: {{'Content-Type': 'text/plain'}},
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
      encoded_sender_negotiated_h264_payload_type: session.encoded_sender_negotiated_h264_payload_type,
      encoded_sender_negotiated_h264_fmtp: session.encoded_sender_negotiated_h264_fmtp,
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

  async function refreshSession() {{
    const token = state.connectToken;
    const cfg = state.config;
    if (!cfg) return;
    try {{
      const sessionResponse = await fetch(`${{cfg.serverBase}}/api/video/signaling/${{cfg.streamId}}/session`);
      if (!sessionResponse.ok) {{
        appendLog('session', `session poll failed: HTTP ${{sessionResponse.status}}`);
        return;
      }}
      const session = await sessionResponse.json();
      if (token !== state.connectToken) return;
      state.sessionSummary = summarizeSession(session);
      if (session.last_local_candidate && session.last_local_candidate !== state.lastBackendCandidate) {{
        state.lastBackendCandidate = session.last_local_candidate;
        appendLog('ice', `backend ICE candidate observed: ${{session.last_local_candidate}}`);
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
    state.sessionPollAbort = false;
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
          await postCandidate(cfg.serverBase, cfg.streamId, candidate);
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
        await postCandidate(cfg.serverBase, cfg.streamId, candidate);
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
      const offerResponse = await fetch(`${{cfg.serverBase}}/api/video/signaling/${{cfg.streamId}}/offer`, {{
        method: 'POST',
        headers: {{'Content-Type': 'text/plain'}},
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

      await refreshSession();
      await flushBufferedCandidates();

      const pollLoop = async () => {{
        while (!state.sessionPollAbort && token === state.connectToken) {{
          await refreshSession();
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
    byId('copy-logs')?.addEventListener('click', () => copyLogs().catch((error) => appendLog('error', `copy failed: ${{error}}`)));
    byId('config-log-filter')?.addEventListener('change', () => {{ readConfigForm(); appendLog('ui', 'log filter updated'); }});
    byId('context-connect')?.addEventListener('click', () => {{ hideContextMenu(); connect('context connect'); }});
    byId('context-reconnect')?.addEventListener('click', () => {{ hideContextMenu(); connect('context reconnect'); }});
    byId('context-disconnect')?.addEventListener('click', () => {{ hideContextMenu(); disconnect('context disconnect'); }});
    byId('context-refresh')?.addEventListener('click', async () => {{ hideContextMenu(); await refreshSession(); await refreshStats(); }});
    byId('context-open-settings')?.addEventListener('click', () => {{ hideContextMenu(); showSettings(true); }});
    byId('context-toggle-widget-debug')?.addEventListener('click', () => {{
      const input = byId('config-widget-show-debug');
      if (input) input.checked = !input.checked;
      readConfigForm();
      hideContextMenu();
    }});
    document.addEventListener('keydown', (event) => {{
      if (event.key === 'Escape') showSettings(false);
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
    if (state.config.autoConnect) {{
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

  <div class="tab-row">
    <button id="tab-smoke" class="tab-button active">Smoke debug</button>
    <button id="tab-widget" class="tab-button">Widget preview</button>
  </div>

  <div class="info-note">
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
            <div class="panel-subtitle">Full smoke/debug view with all telemetry.</div>
          </div>
          <div class="playback-headline">
            <div id="playback-headline">idle</div>
            <div id="playback-detail" class="panel-subtitle">waiting</div>
          </div>
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
    <div class="widget-layout">
      <div id="widget-shell" class="widget-box">
        <div class="widget-header">
          <div>
            <div class="panel-title">Widget preview</div>
            <div class="panel-subtitle">Single box preview for future dynamic placement. Right-click for grouped actions/settings.</div>
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
          <div class="context-title">Local widget settings</div>
          <button id="context-open-settings" class="context-button">Open settings panel</button>
          <button id="context-toggle-widget-debug" class="context-button">Toggle in-box debug</button>
        </div>
        <div class="context-group">
          <div class="context-title">Server request group</div>
          <div class="context-note">Placeholder for future server-side stream/display requests.</div>
        </div>
      </div>
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
.harness-shell { color: #e5eefc; width: min(1400px, 100%); margin: 0 auto; }
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
.widget-box { background: #0f172a; border: 1px solid #1e293b; border-radius: 1rem; padding: 1rem; max-width: 760px; }
.widget-header { display: flex; justify-content: space-between; gap: 1rem; align-items: start; margin-bottom: 0.75rem; }
.widget-video-wrap { background: #020617; border: 1px solid #334155; border-radius: 1rem; padding: 0.75rem; }
#widget-video { width: 100%; min-height: 260px; background: #000; border-radius: 0.75rem; }
.widget-status-bar { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 0.75rem; margin-top: 0.9rem; }
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
.context-menu { position: fixed; z-index: 60; width: min(320px, calc(100vw - 2rem)); background: #020617; border: 1px solid #334155; border-radius: 1rem; box-shadow: 0 18px 40px rgba(0,0,0,0.35); padding: 0.75rem; }
.context-group + .context-group { margin-top: 0.75rem; padding-top: 0.75rem; border-top: 1px solid #1e293b; }
.context-title, .settings-section-title { font-weight: 700; margin-bottom: 0.35rem; }
.context-button { width: 100%; text-align: left; background: #111827; color: #e5eefc; border: 1px solid #1f2937; border-radius: 0.65rem; padding: 0.55rem 0.7rem; margin-top: 0.35rem; cursor: pointer; }
.context-note { color: #94a3b8; font-size: 0.9rem; }
.settings-backdrop { position: fixed; inset: 0; background: rgba(2, 6, 23, 0.75); align-items: center; justify-content: center; z-index: 50; }
.settings-card { width: min(560px, calc(100vw - 2rem)); background: #0f172a; border: 1px solid #334155; border-radius: 1rem; padding: 1rem; display: grid; gap: 0.8rem; }
.settings-card label { display: grid; gap: 0.35rem; color: #cbd5e1; }
.settings-card input, .settings-card select { width: 100%; background: #020617; color: #e5eefc; border: 1px solid #334155; border-radius: 0.7rem; padding: 0.65rem 0.8rem; }
.checkbox-row { display: flex !important; align-items: center; gap: 0.6rem; }
.checkbox-row input { width: auto; }
.settings-actions { display: flex; justify-content: end; }
@media (max-width: 1100px) { .main-grid { grid-template-columns: 1fr; } }
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
- **Default stream id:** `{ARGS.stream_id}`
- **Mode:** `{INITIAL_CONFIG['modeLabel']}`
- **Settings access:** use the **Settings** button or right-click the video area.
            """
        ).classes('max-w-5xl w-full')
        ui.html(PAGE_HTML).classes('w-full')

    ui.timer(0.2, lambda: ui.run_javascript('window.videoSmokeHarness.init()'), once=True)


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
