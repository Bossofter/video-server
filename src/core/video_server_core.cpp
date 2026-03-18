#include "video_server_core.h"

#include <algorithm>
#include <memory>

#include "../transforms/display_transform.h"

namespace video_server {

bool VideoServerCore::register_stream(const StreamConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config.stream_id.empty() || config.width == 0 || config.height == 0 || config.nominal_fps <= 0.0 ||
      !is_supported_input_pixel_format(config.input_pixel_format)) {
    return false;
  }

  auto [it, inserted] = streams_.try_emplace(config.stream_id);
  if (!inserted) {
    return false;
  }

  StreamState& stream = it->second;
  VideoStreamInfo& info = stream.info;
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
  StreamConfig config_snapshot;
  StreamOutputConfig output_config_snapshot;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
      return false;
    }
    config_snapshot = it->second.info.config;
    output_config_snapshot = it->second.info.output_config;
  }

  auto mark_drop = [&]() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
      ++it->second.info.frames_dropped;
    }
  };

  if (frame.data == nullptr || frame.width == 0 || frame.height == 0) {
    mark_drop();
    return false;
  }

  if (frame.width != config_snapshot.width || frame.height != config_snapshot.height ||
      frame.pixel_format != config_snapshot.input_pixel_format) {
    mark_drop();
    return false;
  }

  if (!is_supported_input_pixel_format(frame.pixel_format)) {
    mark_drop();
    return false;
  }

  const uint32_t expected_stride = frame.width * bytes_per_pixel(frame.pixel_format);
  if (frame.stride_bytes < expected_stride) {
    mark_drop();
    return false;
  }

  RgbImage transformed;
  if (!apply_display_transform(frame, output_config_snapshot, transformed)) {
    mark_drop();
    return false;
  }

  auto published_frame = std::make_shared<LatestFrame>();
  published_frame->bytes = std::move(transformed.rgb);
  published_frame->width = transformed.width;
  published_frame->height = transformed.height;
  published_frame->pixel_format = VideoPixelFormat::RGB24;
  published_frame->timestamp_ns = frame.timestamp_ns;
  published_frame->frame_id = frame.frame_id;
  published_frame->valid = true;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
      return false;
    }

    StreamState& stream = it->second;
    VideoStreamInfo& info = stream.info;
    stream.latest_frame = published_frame;

    ++info.frames_received;
    ++info.frames_transformed;
    info.last_input_timestamp_ns = frame.timestamp_ns;
    info.last_output_timestamp_ns = frame.timestamp_ns;
    info.last_frame_timestamp_ns = frame.timestamp_ns;
    info.last_frame_id = frame.frame_id;
    info.has_latest_frame = true;
  }

  return true;
}

bool VideoServerCore::push_access_unit(const std::string& stream_id,
                                       const EncodedAccessUnitView& access_unit) {
  if (access_unit.data == nullptr || access_unit.size_bytes == 0) {
    return false;
  }

  if (access_unit.codec != VideoCodec::H264) {
    return false;
  }

  auto published_unit = std::make_shared<LatestEncodedUnit>();
  published_unit->bytes.assign(static_cast<const uint8_t*>(access_unit.data),
                               static_cast<const uint8_t*>(access_unit.data) + access_unit.size_bytes);
  published_unit->codec = access_unit.codec;
  published_unit->timestamp_ns = access_unit.timestamp_ns;
  published_unit->sequence_id = access_unit.timestamp_ns;
  published_unit->keyframe = access_unit.keyframe;
  published_unit->codec_config = access_unit.codec_config;
  published_unit->valid = true;

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return false;
  }

  StreamState& stream = it->second;
  VideoStreamInfo& info = stream.info;
  stream.latest_encoded_unit = published_unit;

  ++info.access_units_received;
  info.last_frame_timestamp_ns = access_unit.timestamp_ns;
  info.has_latest_encoded_unit = true;
  info.last_encoded_codec = access_unit.codec;
  info.last_encoded_timestamp_ns = access_unit.timestamp_ns;
  info.last_encoded_sequence_id = published_unit->sequence_id;
  info.last_encoded_size_bytes = access_unit.size_bytes;
  info.last_encoded_keyframe = access_unit.keyframe;
  info.last_encoded_codec_config = access_unit.codec_config;
  return true;
}

std::vector<VideoStreamInfo> VideoServerCore::list_streams() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VideoStreamInfo> output;
  output.reserve(streams_.size());
  for (const auto& [_, stream] : streams_) {
    output.push_back(stream.info);
  }
  return output;
}

std::optional<VideoStreamInfo> VideoServerCore::get_stream_info(const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return std::nullopt;
  }
  return it->second.info;
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

  it->second.info.output_config = output_config;
  return true;
}

std::optional<StreamOutputConfig> VideoServerCore::get_stream_output_config(
    const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return std::nullopt;
  }
  return it->second.info.output_config;
}

std::shared_ptr<const LatestFrame> VideoServerCore::get_latest_frame_for_stream(
    const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return nullptr;
  }
  return it->second.latest_frame;
}


std::shared_ptr<const LatestEncodedUnit> VideoServerCore::get_latest_encoded_unit_for_stream(
    const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return nullptr;
  }
  return it->second.latest_encoded_unit;
}

bool VideoServerCore::is_valid_rotation(int degrees) {
  return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

bool VideoServerCore::is_supported_input_pixel_format(VideoPixelFormat pixel_format) {
  return pixel_format == VideoPixelFormat::RGB24 || pixel_format == VideoPixelFormat::BGR24 ||
         pixel_format == VideoPixelFormat::GRAY8;
}

uint32_t VideoServerCore::bytes_per_pixel(VideoPixelFormat pixel_format) {
  switch (pixel_format) {
    case VideoPixelFormat::RGB24:
    case VideoPixelFormat::BGR24:
      return 3;
    case VideoPixelFormat::GRAY8:
      return 1;
    default:
      return 0;
  }
}

}  // namespace video_server
