#pragma once

#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

//Non-owning raw video frame view passed into the server.
struct VideoFrameView {
  //Pointer to the first byte of the frame.
  const void* data{nullptr};
  //Frame width in pixels.
  uint32_t width{0};
  //Frame height in pixels.
  uint32_t height{0};
  //Bytes between the start of consecutive rows.
  uint32_t stride_bytes{0};
  //Raw pixel format.
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
  //Presentation timestamp in nanoseconds.
  uint64_t timestamp_ns{0};
  //Monotonic frame identifier supplied by the producer.
  uint64_t frame_id{0};
};

}  // namespace video_server
