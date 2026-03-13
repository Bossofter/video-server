#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

struct VideoFrameView {
  const void* data{nullptr};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t stride_bytes{0};
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
  uint64_t timestamp_ns{0};
  uint64_t frame_id{0};
};

}  // namespace video_server
