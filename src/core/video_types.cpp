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
