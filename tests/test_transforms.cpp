#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

#include "../src/transforms/display_transform.h"

namespace {

std::array<uint8_t, 3> at_rgb(const video_server::RgbImage& img, uint32_t x, uint32_t y) {
  const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
  return {img.rgb[i], img.rgb[i + 1], img.rgb[i + 2]};
}

}  // namespace

int test_transforms() {
  std::vector<uint8_t> gray = {0, 64, 128, 255};
  video_server::VideoFrameView frame{gray.data(), 2, 2, 2, video_server::VideoPixelFormat::GRAY8, 1, 1};

  video_server::RgbImage out;
  video_server::StreamOutputConfig cfg;

  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb.size() == 12);
  assert((at_rgb(out, 0, 0) == std::array<uint8_t, 3>{0, 0, 0}));
  assert((at_rgb(out, 1, 1) == std::array<uint8_t, 3>{255, 255, 255}));

  cfg.display_mode = video_server::VideoDisplayMode::Grayscale;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert((at_rgb(out, 1, 0) == std::array<uint8_t, 3>{64, 64, 64}));

  cfg.display_mode = video_server::VideoDisplayMode::WhiteHot;
  cfg.palette_min = 0.0F;
  cfg.palette_max = 1.0F;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb[0] <= out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::BlackHot;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.rgb[0] >= out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::Rainbow;
  assert(video_server::apply_display_transform(frame, cfg, out));
  const auto rainbow_mid = at_rgb(out, 1, 0);
  assert(!(rainbow_mid[0] == rainbow_mid[1] && rainbow_mid[1] == rainbow_mid[2]));

  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  cfg.rotation_degrees = 90;
  cfg.mirrored = false;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert(out.width == 2);
  assert(out.height == 2);
  assert((at_rgb(out, 0, 0) == std::array<uint8_t, 3>{128, 128, 128}));

  cfg.rotation_degrees = 180;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert((at_rgb(out, 0, 0) == std::array<uint8_t, 3>{255, 255, 255}));

  cfg.rotation_degrees = 270;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert((at_rgb(out, 0, 0) == std::array<uint8_t, 3>{64, 64, 64}));

  cfg.rotation_degrees = 0;
  cfg.mirrored = true;
  assert(video_server::apply_display_transform(frame, cfg, out));
  assert((at_rgb(out, 0, 0) == std::array<uint8_t, 3>{64, 64, 64}));

  cfg.rotation_degrees = 45;
  assert(!video_server::apply_display_transform(frame, cfg, out));

  return 0;
}
