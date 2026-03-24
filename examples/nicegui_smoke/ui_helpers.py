from __future__ import annotations

from typing import Any
from urllib.parse import urlencode


def parse_stream_spec(value: str) -> dict[str, Any]:
    parts = value.split(':')
    if len(parts) not in (4, 5):
        raise ValueError(f'Invalid stream spec {value!r}; expected id:width:height:fps[:label]')
    stream_id, width, height, fps = parts[:4]
    label = parts[4] if len(parts) == 5 else stream_id
    return {
        'streamId': stream_id,
        'width': int(width),
        'height': int(height),
        'fps': float(fps),
        'label': label,
    }


def default_demo_streams() -> list[dict[str, Any]]:
    return [
        {'streamId': 'alpha', 'width': 640, 'height': 360, 'fps': 30.0, 'label': 'Alpha Sweep 640x360'},
        {'streamId': 'bravo', 'width': 1280, 'height': 720, 'fps': 30.0, 'label': 'Bravo Orbit 1280x720'},
        {'streamId': 'charlie', 'width': 320, 'height': 240, 'fps': 30.0, 'label': 'Charlie Checker 320x240'},
    ]


def widget_url(spec: dict[str, Any]) -> str:
    query = urlencode({
        'widget': 1,
        'stream_id': spec['streamId'],
        'fps': spec['fps'],
        'width': spec['width'],
        'height': spec['height'],
        'label': spec.get('label', spec['streamId']),
    })
    return f'/?{query}'


def selected_stream_spec(stream_catalog: list[dict[str, Any]], stream_id: str) -> dict[str, Any] | None:
    return next((spec for spec in stream_catalog if spec.get('streamId') == stream_id), stream_catalog[0] if stream_catalog else None)


def build_stream_config_payload(display_mode: str, output_width: Any, output_height: Any, output_fps: Any) -> dict[str, Any]:
    return {
        'display_mode': display_mode or 'Passthrough',
        'output_width': int(output_width or 0),
        'output_height': int(output_height or 0),
        'output_fps': float(output_fps or 0),
    }
