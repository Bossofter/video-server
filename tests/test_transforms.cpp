#include <cassert>
#include <cstdint>
#include <vector>

#include "../src/transforms/display_transform.h"

int test_transforms() {
  std::vector<uint8_t> gray = {0, 64, 128, 255};
  video_server::VideoFrameView frame{gray.data(), 2, 2, 2, video_server::VideoPixelFormat::GRAY8, 1, 1};

  video_server::RgbImage out;
  video_server::StreamOutputConfig cfg;

  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb.size() == 12);

  cfg.display_mode = video_server::VideoDisplayMode::WhiteHot;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb[0] <= out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::BlackHot;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb[0] >= out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::Rainbow;
  assert(video_server::apply_display_transform(frame, cfg, out));

  cfg.rotation_degrees = 90;
  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.width == 2);
  assert(out.height == 2);

  cfg.rotation_degrees = 45;
  assert(!video_server::apply_display_transform(frame, cfg, out));

  return 0;
}
