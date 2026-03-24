from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ui_helpers import (  # noqa: E402
    build_stream_config_payload,
    default_demo_streams,
    parse_stream_spec,
    selected_stream_spec,
    widget_url,
)


class UiHelpersTest(unittest.TestCase):
    def test_parse_stream_spec_supports_optional_label(self) -> None:
        self.assertEqual(
            parse_stream_spec('alpha:640:360:30:Alpha Sweep'),
            {
                'streamId': 'alpha',
                'width': 640,
                'height': 360,
                'fps': 30.0,
                'label': 'Alpha Sweep',
            },
        )

    def test_parse_stream_spec_defaults_label_to_stream_id(self) -> None:
        self.assertEqual(
            parse_stream_spec('bravo:1280:720:60'),
            {
                'streamId': 'bravo',
                'width': 1280,
                'height': 720,
                'fps': 60.0,
                'label': 'bravo',
            },
        )

    def test_widget_url_encodes_widget_query(self) -> None:
        self.assertEqual(
            widget_url({'streamId': 'charlie', 'width': 320, 'height': 240, 'fps': 15.0, 'label': 'Charlie Checker'}),
            '/?widget=1&stream_id=charlie&fps=15.0&width=320&height=240&label=Charlie+Checker',
        )

    def test_selected_stream_spec_prefers_requested_stream(self) -> None:
        catalog = default_demo_streams()
        self.assertEqual(selected_stream_spec(catalog, 'bravo'), catalog[1])
        self.assertEqual(selected_stream_spec(catalog, 'missing'), catalog[0])
        self.assertIsNone(selected_stream_spec([], 'missing'))

    def test_build_stream_config_payload_normalizes_numeric_inputs(self) -> None:
        self.assertEqual(
            build_stream_config_payload('Ironbow', '32', 24, '12.5'),
            {
                'display_mode': 'Ironbow',
                'output_width': 32,
                'output_height': 24,
                'output_fps': 12.5,
            },
        )
        self.assertEqual(
            build_stream_config_payload('', None, None, None),
            {
                'display_mode': 'Passthrough',
                'output_width': 0,
                'output_height': 0,
                'output_fps': 0.0,
            },
        )


if __name__ == '__main__':
    unittest.main()
