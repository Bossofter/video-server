#pragma once

#include <cstdint>
#include <optional>

namespace video_server {

//Supported raw input pixel formats.
enum class VideoPixelFormat {
  RGB24,
  BGR24,
  RGBA32,
  BGRA32,
  NV12,
  I420,
  GRAY8
};

//Supported encoded codecs.
enum class VideoCodec {
  H264
};

//Supported output display and palette modes.
enum class VideoDisplayMode {
  Passthrough,
  Grayscale,
  WhiteHot,
  BlackHot,
  Ironbow,
  Rainbow,
  Arctic
};

/**
 * @brief Returns a readable name for a display mode value.
 * 
 * @param mode 
 * @return const char* 
 */
const char* to_string(VideoDisplayMode mode);


/**
 * @brief Returns a readable name for a pixel format value.
 * 
 * @param pixel_format 
 * @return const char* 
 */
const char* to_string(VideoPixelFormat pixel_format);


/**
 * @brief Returns a readable name for a codec value.
 * 
 * @param codec 
 * @return const char* 
 */
const char* to_string(VideoCodec codec);

/**
 * @brief Parses a display mode from a case-insensitive API string.
 * 
 * @param value 
 * @return std::optional<VideoDisplayMode> 
 */
std::optional<VideoDisplayMode> video_display_mode_from_string(const char* value);

}  // namespace video_server
