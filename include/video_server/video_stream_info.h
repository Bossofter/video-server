#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"
#include "video_server/video_types.h"

namespace video_server {

//Public stream status snapshot returned by the server APIs.
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
  uint64_t last_input_timestamp_ns{0};
  uint64_t last_output_timestamp_ns{0};
  uint64_t last_frame_id{0};
  bool has_latest_frame{false};
  bool has_latest_encoded_unit{false};
  VideoCodec last_encoded_codec{VideoCodec::H264};
  uint64_t last_encoded_timestamp_ns{0};
  uint64_t last_encoded_sequence_id{0};
  size_t last_encoded_size_bytes{0};
  bool last_encoded_keyframe{false};
  bool last_encoded_codec_config{false};
};

}  // namespace video_server
