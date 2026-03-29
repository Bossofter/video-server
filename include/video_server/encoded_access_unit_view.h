#pragma once

#include <cstddef>
#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

/**
 * @brief Non-owning view of a single encoded access unit submitted to the server.
 * 
 */
struct EncodedAccessUnitView {
  //Pointer to the encoded payload bytes.
  const void* data{nullptr};
  //Payload size in bytes.
  size_t size_bytes{0};
  //Encoded codec type.
  VideoCodec codec{VideoCodec::H264};
  //Presentation timestamp in nanoseconds.
  uint64_t timestamp_ns{0};
  //True when this access unit contains a keyframe.
  bool keyframe{false};
  //True when this access unit carries codec configuration data.
  bool codec_config{false};
};

}  // namespace video_server
