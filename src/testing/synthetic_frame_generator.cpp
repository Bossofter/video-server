#include "synthetic_frame_generator.h"

#include <algorithm>
#include <chrono>
#include <string_view>

namespace video_server {

namespace {
uint32_t fnv1a_hash(std::string_view value) {
  uint32_t hash = 2166136261u;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}
}  // namespace

SyntheticFrameGenerator::SyntheticFrameGenerator(StreamConfig config) : config_(std::move(config)) {
  const uint32_t channels = (config_.input_pixel_format == VideoPixelFormat::GRAY8) ? 1 : 3;
  buffer_.resize(static_cast<size_t>(config_.width) * config_.height * channels);
  stream_seed_ = fnv1a_hash(config_.stream_id + ":" + config_.label);
}

uint8_t SyntheticFrameGenerator::channel_seed(size_t channel) const {
  return static_cast<uint8_t>((stream_seed_ >> ((channel % 4) * 8)) & 0xffu);
}

VideoFrameView SyntheticFrameGenerator::next_frame() {
  ++frame_counter_;
  const uint32_t channels = (config_.input_pixel_format == VideoPixelFormat::GRAY8) ? 1 : 3;
  const uint32_t stride = config_.width * channels;

  const uint32_t band_height = std::max<uint32_t>(1, config_.height / 6);
  const uint32_t marker_width = std::max<uint32_t>(8, config_.width / 12);
  for (uint32_t y = 0; y < config_.height; ++y) {
    for (uint32_t x = 0; x < config_.width; ++x) {
      const uint8_t animated = static_cast<uint8_t>((x + frame_counter_ * 3 + channel_seed(0)) % 255);
      const uint8_t stripe = static_cast<uint8_t>(((y / band_height) * 37 + channel_seed(1)) % 255);
      const bool marker = ((x + frame_counter_ * (1 + (stream_seed_ % 3))) % (marker_width * 2)) < marker_width;
      const size_t idx = static_cast<size_t>(y) * stride + x * channels;
      if (channels == 1) {
        buffer_[idx] = static_cast<uint8_t>((animated / 2 + stripe / 2 + (marker ? 48 : 0)) % 255);
      } else {
        buffer_[idx + 0] = marker ? static_cast<uint8_t>((channel_seed(2) + y) % 255) : animated;
        buffer_[idx + 1] = static_cast<uint8_t>((stripe + x / 2 + channel_seed(3)) % 255);
        buffer_[idx + 2] = static_cast<uint8_t>((animated / 2 + frame_counter_ + (marker ? 96 : 12)) % 255);
      }
    }
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

  return VideoFrameView{buffer_.data(), config_.width, config_.height, stride, config_.input_pixel_format,
                        static_cast<uint64_t>(ns), frame_counter_};
}

}  // namespace video_server
