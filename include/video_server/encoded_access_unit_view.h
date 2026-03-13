#pragma once

#include <cstddef>
#include <cstdint>

#include "video_server/video_types.h"

namespace video_server {

struct EncodedAccessUnitView {
  const void* data{nullptr};
  size_t size_bytes{0};
  VideoCodec codec{VideoCodec::H264};
  uint64_t timestamp_ns{0};
  bool keyframe{false};
  bool codec_config{false};
};

}  // namespace video_server
