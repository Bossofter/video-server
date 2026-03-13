#pragma once

#include <cstdint>
#include <string>

#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"

namespace video_server {

struct VideoStreamInfo {
  std::string stream_id;
  std::string label;
  StreamConfig config;
  StreamOutputConfig output_config;
  bool active{false};
  uint64_t frames_received{0};
  uint64_t frames_transformed{0};
  uint64_t frames_dropped{0};
  uint64_t access_units_received{0};
  uint64_t last_frame_timestamp_ns{0};
};

}  // namespace video_server
