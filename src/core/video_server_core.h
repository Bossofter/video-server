#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/video_server.h"

namespace video_server {

struct LatestEncodedUnit {
  std::vector<uint8_t> bytes;
  VideoCodec codec{VideoCodec::H264};
  uint64_t timestamp_ns{0};
  uint64_t sequence_id{0};
  bool keyframe{false};
  bool codec_config{false};
  bool valid{false};
};

struct LatestFrame {
  std::vector<uint8_t> bytes;
  uint32_t width{0};
  uint32_t height{0};
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
  uint64_t timestamp_ns{0};
  uint64_t frame_id{0};
  bool valid{false};
};

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

  std::shared_ptr<const LatestFrame> get_latest_frame_for_stream(const std::string& stream_id) const;
  std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit_for_stream(const std::string& stream_id) const;

 private:
  struct StreamState {
    VideoStreamInfo info;
    std::shared_ptr<const LatestFrame> latest_frame;
    std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit;
  };

  static bool is_valid_rotation(int degrees);
  static bool is_supported_input_pixel_format(VideoPixelFormat pixel_format);
  static uint32_t bytes_per_pixel(VideoPixelFormat pixel_format);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, StreamState> streams_;
};

}  // namespace video_server
