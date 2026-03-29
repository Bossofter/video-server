#pragma once

#include <cstdint>
#include <string>

#include "video_server/video_types.h"

namespace video_server {

//Registration-time properties for a producer-owned video stream.
struct StreamConfig {
  //Stable stream identifier used by the API surface.
  std::string stream_id;
  //Human-readable stream label.
  std::string label;
  //Expected input width in pixels.
  uint32_t width{0};
  //Expected input height in pixels.
  uint32_t height{0};
  //Nominal source frame rate.
  double nominal_fps{0.0};
  //Expected raw input pixel format.
  VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24};
};

}  // namespace video_server
