#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import os
import shlex
import signal
import subprocess
import sys
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


PAGE_JS = """
<script>
window.videoSmokeHarness = async function(config) {
  const video = document.getElementById(config.videoId);
  const log = document.getElementById(config.logId);
  const status = document.getElementById(config.statusId);

  const append = (message) => {
    const line = `[${new Date().toLocaleTimeString()}] ${message}`;
    console.log('[video-smoke]', message);
    log.textContent = `${line}\n${log.textContent}`.trim();
  };

  status.textContent = 'creating RTCPeerConnection';
  const pc = new RTCPeerConnection({iceServers: []});
  pc.addTransceiver('video', {direction: 'recvonly'});

  pc.ontrack = (event) => {
    append(`remote track received (${event.track.kind})`);
    video.srcObject = event.streams[0];
    video.play().catch((error) => append(`video.play() warning: ${error}`));
    status.textContent = 'remote track attached';
  };

  pc.onconnectionstatechange = () => {
    append(`connectionState=${pc.connectionState}`);
    status.textContent = `connectionState=${pc.connectionState}`;
  };

  pc.oniceconnectionstatechange = () => append(`iceConnectionState=${pc.iceConnectionState}`);
  pc.onicegatheringstatechange = () => append(`iceGatheringState=${pc.iceGatheringState}`);

  const bufferedLocalCandidates = [];
  let offerPosted = false;
  let remoteDescriptionApplied = false;

  const postCandidate = async (candidate) => {
    append(`posting browser ICE candidate: ${candidate}`);
    const response = await fetch(`${config.serverBase}/api/video/signaling/${config.streamId}/candidate`, {
      method: 'POST',
      headers: {'Content-Type': 'text/plain'},
      body: candidate,
    });
    if (!response.ok) {
      const detail = await response.text();
      throw new Error(`HTTP ${response.status} ${detail}`);
    }
  };

  const flushBufferedCandidates = async () => {
    if (!offerPosted || !remoteDescriptionApplied || bufferedLocalCandidates.length === 0) {
      return;
    }
    append(`flushing ${bufferedLocalCandidates.length} buffered ICE candidate(s)`);
    while (bufferedLocalCandidates.length > 0) {
      const candidate = bufferedLocalCandidates.shift();
      append(`candidate flushed: ${candidate}`);
      await postCandidate(candidate);
    }
  };

  pc.onicecandidate = async (event) => {
    if (!event.candidate) {
      append('local ICE gathering complete');
      return;
    }
    const candidate = event.candidate.candidate;
    if (!offerPosted || !remoteDescriptionApplied) {
      bufferedLocalCandidates.push(candidate);
      append(`candidate buffered (${bufferedLocalCandidates.length} queued): ${candidate}`);
      return;
    }
    try {
      await postCandidate(candidate);
      append(`candidate posted immediately: ${candidate}`);
    } catch (error) {
      append(`candidate post failed: ${error}`);
    }
  };

  append('creating offer');
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);

  append('posting SDP offer to server');
  const offerResponse = await fetch(`${config.serverBase}/api/video/signaling/${config.streamId}/offer`, {
    method: 'POST',
    headers: {'Content-Type': 'text/plain'},
    body: pc.localDescription.sdp,
  });

  if (!offerResponse.ok) {
    const detail = await offerResponse.text();
    append(`offer failed: HTTP ${offerResponse.status} ${detail}`);
    status.textContent = `offer failed: HTTP ${offerResponse.status}`;
    return;
  }
  offerPosted = true;
  append('offer posted successfully');

  append('waiting for backend answer');
  let lastRemoteCandidate = '';
  for (let attempt = 0; attempt < 80; attempt += 1) {
    const sessionResponse = await fetch(`${config.serverBase}/api/video/signaling/${config.streamId}/session`);
    if (!sessionResponse.ok) {
      append(`session poll failed: HTTP ${sessionResponse.status}`);
      await new Promise((resolve) => setTimeout(resolve, 250));
      continue;
    }

    const sessionText = await sessionResponse.text();
    let session;
    try {
      session = JSON.parse(sessionText);
      append('answer/session JSON parse success');
    } catch (error) {
      append(`answer/session JSON parse failed: ${error}; body=${sessionText}`);
      status.textContent = 'session JSON parse failed';
      return;
    }
    status.textContent =
      `peer=${session.peer_state} media=${session.media_bridge_state} sender=${session.encoded_sender_state} ` +
      `codec_config=${session.encoded_sender_cached_codec_config_available} ` +
      `idr=${session.encoded_sender_cached_idr_available} ` +
      `startup_sent=${session.encoded_sender_startup_sequence_sent} ` +
      `first_decodable=${session.encoded_sender_first_decodable_frame_sent} ` +
      `pkts_after_open=${session.encoded_sender_packets_sent_after_track_open}`;

    if (!remoteDescriptionApplied && session.answer_sdp) {
      append('answer received from backend session');
      append('applying backend SDP answer');
      await pc.setRemoteDescription({type: 'answer', sdp: session.answer_sdp});
      append('remote description applied');
      remoteDescriptionApplied = true;
      await flushBufferedCandidates();
    }

    if (session.last_local_candidate && session.last_local_candidate !== lastRemoteCandidate) {
      lastRemoteCandidate = session.last_local_candidate;
      append(`backend ICE candidate observed: ${lastRemoteCandidate}`);
    }

    append(
      `sender snapshot state=${session.encoded_sender_state}` +
      ` codec_config_seen=${session.encoded_sender_codec_config_seen}` +
      ` cached_codec_config=${session.encoded_sender_cached_codec_config_available}` +
      ` cached_idr=${session.encoded_sender_cached_idr_available}` +
      ` ready=${session.encoded_sender_ready_for_video_track}` +
      ` first_decodable=${session.encoded_sender_first_decodable_frame_sent}` +
      ` startup_sent=${session.encoded_sender_startup_sequence_sent}` +
      ` packets=${session.encoded_sender_packets_attempted}` +
      ` packets_after_open=${session.encoded_sender_packets_sent_after_track_open}` +
      ` payload=${session.encoded_sender_negotiated_h264_payload_type}` +
      ` fmtp=${session.encoded_sender_negotiated_h264_fmtp}`
    );

    if (remoteDescriptionApplied && (pc.connectionState === 'connected' || pc.connectionState === 'completed')) {
      append('peer connection is connected');
      return;
    }

    await new Promise((resolve) => setTimeout(resolve, 250));
  }

  append('timed out waiting for connected playback; inspect session JSON and browser console');
  status.textContent = 'timeout waiting for playback';
};
</script>
"""


@ui.page('/')
def index() -> None:
    ui.add_head_html(PAGE_JS)
    ui.page_title('video-server NiceGUI smoke harness')

    with ui.column().classes('w-full items-center gap-4 p-6'):
        ui.label('video-server NiceGUI smoke harness').classes('text-2xl font-bold')
        ui.markdown(
            f"""
This page is a manual smoke test for the current H264 WebRTC consumer path.

- **Video server:** `{ARGS.video_server_url}`
- **Stream id:** `{ARGS.stream_id}`
- **Mode:** `{'launch smoke server + consume WebRTC stream' if ARGS.start_server else 'consume existing video server'}`
            """
        ).classes('max-w-3xl')
        ui.html(
            '''
            <div class="w-full max-w-5xl">
              <video id="smoke-video" autoplay playsinline muted controls style="width: 100%; background: #111; border: 1px solid #444; border-radius: 12px;"></video>
              <div id="smoke-status" style="margin-top: 0.75rem; font-family: monospace; color: #ddd;">starting...</div>
              <pre id="smoke-log" style="margin-top: 0.75rem; max-height: 18rem; overflow: auto; background: #111; color: #8ef; padding: 1rem; border-radius: 12px; white-space: pre-wrap;"></pre>
            </div>
            '''
        )
        ui.button('Reconnect stream', on_click=lambda: ui.run_javascript(
            f"window.videoSmokeHarness({{serverBase: {ARGS.video_server_url!r}, streamId: {ARGS.stream_id!r}, videoId: 'smoke-video', logId: 'smoke-log', statusId: 'smoke-status'}})"
        ))

    ui.timer(
        0.2,
        lambda: ui.run_javascript(
            f"window.videoSmokeHarness({{serverBase: {ARGS.video_server_url!r}, streamId: {ARGS.stream_id!r}, videoId: 'smoke-video', logId: 'smoke-log', statusId: 'smoke-status'}})"
        ),
        once=True,
    )


if __name__ in {'__main__', '__mp_main__'}:
    try:
        ui.run(host=ARGS.ui_host, port=ARGS.ui_port, reload=False, show=False, title='video-server NiceGUI smoke harness')
    finally:
        if SMOKE_PROCESS is not None and SMOKE_PROCESS.poll() is None:
            try:
                if SMOKE_PROCESS.stdin:
                    SMOKE_PROCESS.stdin.write('\n')
                    SMOKE_PROCESS.stdin.flush()
            except BrokenPipeError:
                os.kill(SMOKE_PROCESS.pid, signal.SIGTERM)
