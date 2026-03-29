#pragma once

#include <cstddef>
#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

/**
 * @brief Non-owning view of a single encoded access unit submitted to the server.
 */
struct EncodedAccessUnitView {
  const void* data{nullptr};        /**< Pointer to the encoded payload bytes. */
  size_t size_bytes{0};             /**< Payload size in bytes. */
  VideoCodec codec{VideoCodec::H264}; /**< Encoded codec type. */
  uint64_t timestamp_ns{0};         /**< Presentation timestamp in nanoseconds. */
  bool keyframe{false};             /**< True when this access unit contains a keyframe. */
  bool codec_config{false};         /**< True when this access unit carries codec configuration data. */
};

}  // namespace video_server
