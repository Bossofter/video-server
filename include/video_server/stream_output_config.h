#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

/// Runtime output transform and throttling settings for a stream.
struct StreamOutputConfig {
  /// Display or palette transform mode.
  VideoDisplayMode display_mode{VideoDisplayMode::Passthrough};
  /// Mirrors output horizontally when true.
  bool mirrored{false};
  /// Output rotation in degrees.
  int rotation_degrees{0};
  /// Low end of the palette mapping range.
  float palette_min{0.0F};
  /// High end of the palette mapping range.
  float palette_max{1.0F};
  /// Requested output width, or zero for source size.
  uint32_t output_width{0};
  /// Requested output height, or zero for source size.
  uint32_t output_height{0};
  /// Requested output FPS, or zero for source cadence.
  double output_fps{0.0};
  /// Monotonic generation number assigned by the server on update.
  uint64_t config_generation{1};
};

}  // namespace video_server
