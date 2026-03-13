#pragma once

#include "video_server/video_types.h"

namespace video_server {

struct StreamOutputConfig {
  VideoDisplayMode display_mode{VideoDisplayMode::Passthrough};
  bool mirrored{false};
  int rotation_degrees{0};
  float palette_min{0.0F};
  float palette_max{1.0F};
};

}  // namespace video_server
