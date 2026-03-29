#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/observability.h"
#include "video_server/video_server.h"

namespace video_server {

/// Immutable snapshot of the latest encoded unit for a stream.
struct LatestEncodedUnit {
  std::vector<uint8_t> bytes;
  VideoCodec codec{VideoCodec::H264};
  uint64_t timestamp_ns{0};
  uint64_t sequence_id{0};
  bool keyframe{false};
  bool codec_config{false};
  bool valid{false};
};

/// Immutable snapshot of the latest transformed frame for a stream.
struct LatestFrame {
  std::vector<uint8_t> bytes;
  uint32_t width{0};
  uint32_t height{0};
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
  uint64_t timestamp_ns{0};
  uint64_t frame_id{0};
  bool valid{false};
};

/// Core in-memory implementation of stream state, transforms, and latest snapshots.
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

  /// Returns the latest transformed frame snapshot for a stream.
  std::shared_ptr<const LatestFrame> get_latest_frame_for_stream(const std::string& stream_id) const;
  /// Returns the latest encoded access-unit snapshot for a stream.
  std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit_for_stream(const std::string& stream_id) const;
  /// Returns a debug snapshot for one stream when present.
  std::optional<StreamDebugSnapshot> get_stream_debug_snapshot(const std::string& stream_id) const;
  /// Returns debug snapshots for all streams.
  std::vector<StreamDebugSnapshot> list_stream_debug_snapshots() const;

 public:
  /// Mutable state tracked for one registered stream.
  struct StreamState {
    VideoStreamInfo info;
    std::shared_ptr<const LatestFrame> latest_frame;
    std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit;
    uint64_t next_allowed_output_timestamp_ns{0};
  };

 private:
  /// Validates supported rotation values.
  static bool is_valid_rotation(int degrees);
  /// Validates requested output dimensions.
  static bool is_valid_output_dimensions(uint32_t width, uint32_t height);
  /// Validates requested output FPS.
  static bool is_valid_output_fps(double fps);
  /// Checks whether the input pixel format is supported by the core.
  static bool is_supported_input_pixel_format(VideoPixelFormat pixel_format);
  /// Returns bytes per pixel for packed formats handled directly by the core.
  static uint32_t bytes_per_pixel(VideoPixelFormat pixel_format);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, StreamState> streams_;
};

}  // namespace video_server
