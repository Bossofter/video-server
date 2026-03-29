#pragma once

#include <cstdint>
#include <vector>

#include "video_server/stream_output_config.h"
#include "video_server/video_frame_view.h"

namespace video_server {

/**
 * @brief Packed RGB image produced by the display transform stage.
 */
struct RgbImage {
  uint32_t width{0};         /**< Output width in pixels. */
  uint32_t height{0};        /**< Output height in pixels. */
  std::vector<uint8_t> rgb;  /**< Packed RGB24 output bytes. */
};

/**
 * @brief Applies the active output transform to a raw frame and writes packed RGB24 output.
 *
 * @param frame Raw input frame to transform.
 * @param config Runtime output configuration to apply.
 * @param out Destination image populated on success.
 * @return True when the transform succeeded, false otherwise.
 */
bool apply_display_transform(const VideoFrameView& frame, const StreamOutputConfig& config, RgbImage& out);

}  // namespace video_server
