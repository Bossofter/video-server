#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

/**
 * @brief Runtime output transform and throttling settings for a stream.
 */
struct StreamOutputConfig {
  VideoDisplayMode display_mode{VideoDisplayMode::Passthrough}; /**< Display or palette transform mode. */
  bool mirrored{false};                                         /**< Mirrors output horizontally when true. */
  int rotation_degrees{0};                                      /**< Output rotation in degrees. */
  float palette_min{0.0F};                                      /**< Low end of the palette mapping range. */
  float palette_max{1.0F};                                      /**< High end of the palette mapping range. */
  uint32_t output_width{0};                                     /**< Requested output width, or zero for source size. */
  uint32_t output_height{0};                                    /**< Requested output height, or zero for source size. */
  double output_fps{0.0};                                       /**< Requested output FPS, or zero for source cadence. */
  uint64_t config_generation{1};                                /**< Monotonic generation number assigned by the server on update. */
};

}  // namespace video_server
