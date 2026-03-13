#pragma once

#include <cstdint>
#include <string>

#include "video_server/video_types.h"

namespace video_server {

struct StreamConfig {
  std::string stream_id;
  std::string label;
  uint32_t width{0};
  uint32_t height{0};
  double nominal_fps{0.0};
  VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24};
};

}  // namespace video_server
