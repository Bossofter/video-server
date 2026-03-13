#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/video_server.h"

namespace video_server {

class VideoServerCore : public IVideoServer {
 public:
  bool register_stream(const StreamConfig& config) override;
  bool remove_stream(const std::string& stream_id) override;
  bool push_frame(const std::string& stream_id, const VideoFrameView& frame) override;
  bool push_access_unit(const std::string& stream_id, const EncodedAccessUnitView& access_unit) override;
  std::vector<VideoStreamInfo> list_streams() const override;
  std::optional<VideoStreamInfo> get_stream_info(const std::string& stream_id) const override;
  bool set_stream_output_config(const std::string& stream_id,
                                const StreamOutputConfig& output_config) override;
  std::optional<StreamOutputConfig> get_stream_output_config(const std::string& stream_id) const override;

 private:
  static bool is_valid_rotation(int degrees);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, VideoStreamInfo> streams_;
};

}  // namespace video_server
