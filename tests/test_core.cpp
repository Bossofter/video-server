#include <cassert>
#include <cstdint>
#include <vector>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "../src/core/video_server_core.h"

int test_core() {
  video_server::VideoServerCore server;

  video_server::StreamConfig cfg{"stream-a", "primary", 64, 32, 30.0, video_server::VideoPixelFormat::RGB24};
  assert(server.register_stream(cfg));
  assert(!server.register_stream(cfg));

  std::vector<uint8_t> frame_data(static_cast<size_t>(cfg.width) * cfg.height * 3, 8);
  video_server::VideoFrameView frame{frame_data.data(), cfg.width, cfg.height, cfg.width * 3,
                                     video_server::VideoPixelFormat::RGB24, 100, 1};
  assert(server.push_frame(cfg.stream_id, frame));

  auto info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->frames_received == 1);

  video_server::StreamOutputConfig out_cfg;
  out_cfg.display_mode = video_server::VideoDisplayMode::Rainbow;
  out_cfg.rotation_degrees = 90;
  assert(server.set_stream_output_config(cfg.stream_id, out_cfg));
  auto roundtrip = server.get_stream_output_config(cfg.stream_id);
  assert(roundtrip.has_value());
  assert(roundtrip->rotation_degrees == 90);

  assert(server.remove_stream(cfg.stream_id));
  assert(!server.remove_stream(cfg.stream_id));

  return 0;
}
