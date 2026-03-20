#pragma once

#include <cstdint>
#include <vector>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"

namespace video_server {

class SyntheticFrameGenerator {
 public:
  explicit SyntheticFrameGenerator(StreamConfig config);

  const StreamConfig& config() const { return config_; }
  VideoFrameView next_frame();

 private:
  StreamConfig config_;
  uint8_t channel_seed(size_t channel) const;

  std::vector<uint8_t> buffer_;
  uint64_t frame_counter_{0};
  uint32_t stream_seed_{0};
};

}  // namespace video_server
