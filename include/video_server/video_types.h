#pragma once

#include <cstdint>
#include <optional>

namespace video_server {

enum class VideoPixelFormat {
  RGB24,
  BGR24,
  RGBA32,
  BGRA32,
  NV12,
  I420,
  GRAY8
};

enum class VideoCodec {
  H264
};

enum class VideoDisplayMode {
  Passthrough,
  Grayscale,
  WhiteHot,
  BlackHot,
  Rainbow
};

const char* to_string(VideoDisplayMode mode);
std::optional<VideoDisplayMode> video_display_mode_from_string(const char* value);

}  // namespace video_server
