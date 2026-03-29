#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

/**
 * @brief Non-owning raw video frame view passed into the server.
 */
struct VideoFrameView {
  const void* data{nullptr};                  /**< Pointer to the first byte of the frame. */
  uint32_t width{0};                          /**< Frame width in pixels. */
  uint32_t height{0};                         /**< Frame height in pixels. */
  uint32_t stride_bytes{0};                   /**< Bytes between the start of consecutive rows. */
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24}; /**< Raw pixel format. */
  uint64_t timestamp_ns{0};                   /**< Presentation timestamp in nanoseconds. */
  uint64_t frame_id{0};                       /**< Monotonic frame identifier supplied by the producer. */
};

}  // namespace video_server
