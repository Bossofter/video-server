#include "video_server/video_types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace video_server {

const char* to_string(VideoDisplayMode mode) {
  switch (mode) {
    case VideoDisplayMode::Passthrough:
      return "Passthrough";
    case VideoDisplayMode::Grayscale:
      return "Grayscale";
    case VideoDisplayMode::WhiteHot:
      return "WhiteHot";
    case VideoDisplayMode::BlackHot:
      return "BlackHot";
    case VideoDisplayMode::Rainbow:
      return "Rainbow";
  }
  return "Passthrough";
}

const char* to_string(VideoPixelFormat pixel_format) {
  switch (pixel_format) {
    case VideoPixelFormat::RGB24:
      return "RGB24";
    case VideoPixelFormat::BGR24:
      return "BGR24";
    case VideoPixelFormat::RGBA32:
      return "RGBA32";
    case VideoPixelFormat::BGRA32:
      return "BGRA32";
    case VideoPixelFormat::NV12:
      return "NV12";
    case VideoPixelFormat::I420:
      return "I420";
    case VideoPixelFormat::GRAY8:
      return "GRAY8";
  }
  return "RGB24";
}

std::optional<VideoDisplayMode> video_display_mode_from_string(const char* value) {
  if (value == nullptr) {
    return std::nullopt;
  }

  std::string normalized(value);
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                  [](unsigned char c) { return std::isspace(c) != 0; }),
                   normalized.end());

  if (normalized == "Passthrough") {
    return VideoDisplayMode::Passthrough;
  }
  if (normalized == "Grayscale") {
    return VideoDisplayMode::Grayscale;
  }
  if (normalized == "WhiteHot") {
    return VideoDisplayMode::WhiteHot;
  }
  if (normalized == "BlackHot") {
    return VideoDisplayMode::BlackHot;
  }
  if (normalized == "Rainbow") {
    return VideoDisplayMode::Rainbow;
  }

  return std::nullopt;
}

}  // namespace video_server
