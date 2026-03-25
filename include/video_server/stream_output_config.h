#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

struct StreamOutputConfig {
  VideoDisplayMode display_mode{VideoDisplayMode::Passthrough};
  bool mirrored{false};
  int rotation_degrees{0};
  float palette_min{0.0F};
  float palette_max{1.0F};
  uint32_t output_width{0};
  uint32_t output_height{0};
  double output_fps{0.0};
  uint64_t config_generation{1};
};

}  // namespace video_server
