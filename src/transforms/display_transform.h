#pragma once

#include <cstdint>
#include <vector>

#include "video_server/stream_output_config.h"
#include "video_server/video_frame_view.h"

namespace video_server {

/// Packed RGB image produced by the display transform stage.
struct RgbImage {
  uint32_t width{0};
  uint32_t height{0};
  std::vector<uint8_t> rgb;  // packed RGB24
};

/// Applies the active output transform to a raw frame and writes packed RGB24 output.
bool apply_display_transform(const VideoFrameView& frame, const StreamOutputConfig& config, RgbImage& out);

}  // namespace video_server
