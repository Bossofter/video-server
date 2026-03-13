#include "video_server_core.h"

namespace video_server {

bool VideoServerCore::register_stream(const StreamConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config.stream_id.empty() || config.width == 0 || config.height == 0 || config.nominal_fps <= 0.0) {
    return false;
  }

  auto [it, inserted] = streams_.try_emplace(config.stream_id);
  if (!inserted) {
    return false;
  }

  VideoStreamInfo& info = it->second;
  info.stream_id = config.stream_id;
  info.label = config.label;
  info.config = config;
  info.output_config = StreamOutputConfig{};
  info.active = true;
  return true;
}

bool VideoServerCore::remove_stream(const std::string& stream_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return streams_.erase(stream_id) > 0;
}

bool VideoServerCore::push_frame(const std::string& stream_id, const VideoFrameView& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || frame.data == nullptr) {
    return false;
  }

  VideoStreamInfo& info = it->second;
  ++info.frames_received;
  ++info.frames_transformed;
  info.last_frame_timestamp_ns = frame.timestamp_ns;
  return true;
}

bool VideoServerCore::push_access_unit(const std::string& stream_id,
                                       const EncodedAccessUnitView& access_unit) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || access_unit.data == nullptr || access_unit.size_bytes == 0) {
    return false;
  }

  VideoStreamInfo& info = it->second;
  ++info.access_units_received;
  info.last_frame_timestamp_ns = access_unit.timestamp_ns;
  return true;
}

std::vector<VideoStreamInfo> VideoServerCore::list_streams() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VideoStreamInfo> output;
  output.reserve(streams_.size());
  for (const auto& [_, info] : streams_) {
    output.push_back(info);
  }
  return output;
}

std::optional<VideoStreamInfo> VideoServerCore::get_stream_info(const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool VideoServerCore::set_stream_output_config(const std::string& stream_id,
                                               const StreamOutputConfig& output_config) {
  if (!is_valid_rotation(output_config.rotation_degrees)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return false;
  }

  it->second.output_config = output_config;
  return true;
}

std::optional<StreamOutputConfig> VideoServerCore::get_stream_output_config(
    const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return std::nullopt;
  }
  return it->second.output_config;
}

bool VideoServerCore::is_valid_rotation(int degrees) {
  return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

}  // namespace video_server
