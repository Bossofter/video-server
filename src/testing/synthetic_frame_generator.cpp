#include "synthetic_frame_generator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

SyntheticFrameGenerator::PatternVariant SyntheticFrameGenerator::pattern_variant() const {
  switch (fnv1a_hash(config_.stream_id) % 3u) {
    case 0:
      return PatternVariant::GradientOrbit;
    case 1:
      return PatternVariant::CheckerPulse;
    default:
      return PatternVariant::DiagonalSweep;
  }
}

VideoFrameView SyntheticFrameGenerator::next_frame() {
  ++frame_counter_;
  const uint32_t channels = (config_.input_pixel_format == VideoPixelFormat::GRAY8) ? 1 : 3;
  const uint32_t stride = config_.width * channels;

  const auto pattern = pattern_variant();
  const uint32_t band_height = std::max<uint32_t>(1, config_.height / 6);
  const uint32_t marker_width = std::max<uint32_t>(8, config_.width / 12);
  const uint32_t checker_size = std::max<uint32_t>(8, std::min(config_.width, config_.height) / 10);
  const float center_x = static_cast<float>(config_.width) * 0.5f;
  const float center_y = static_cast<float>(config_.height) * 0.5f;
  const float radius = static_cast<float>(std::min(config_.width, config_.height)) * 0.35f;
  const float orbit = static_cast<float>((frame_counter_ * (2 + (stream_seed_ % 5u))) % 360u) * 0.0174532925f;
  const float orbit_x = center_x + std::cos(orbit) * radius;
  const float orbit_y = center_y + std::sin(orbit) * radius;
  for (uint32_t y = 0; y < config_.height; ++y) {
    for (uint32_t x = 0; x < config_.width; ++x) {
      const uint8_t animated = static_cast<uint8_t>((x + frame_counter_ * 3 + channel_seed(0)) % 255);
      const uint8_t stripe = static_cast<uint8_t>(((y / band_height) * 37 + channel_seed(1)) % 255);
      const bool marker = ((x + frame_counter_ * (1 + (stream_seed_ % 3))) % (marker_width * 2)) < marker_width;
      const size_t idx = static_cast<size_t>(y) * stride + x * channels;
      uint8_t red = animated;
      uint8_t green = static_cast<uint8_t>((stripe + x / 2 + channel_seed(3)) % 255);
      uint8_t blue = static_cast<uint8_t>((animated / 2 + frame_counter_ + (marker ? 96 : 12)) % 255);
      if (pattern == PatternVariant::GradientOrbit) {
        const float dx = static_cast<float>(x) - orbit_x;
        const float dy = static_cast<float>(y) - orbit_y;
        const bool highlight = (dx * dx + dy * dy) < (radius * 0.22f) * (radius * 0.22f);
        red = marker ? static_cast<uint8_t>((channel_seed(2) + y) % 255) : animated;
        green = static_cast<uint8_t>((stripe + x / 3 + channel_seed(3)) % 255);
        blue = highlight ? static_cast<uint8_t>(220u - (frame_counter_ % 40u)) : static_cast<uint8_t>((animated / 2 + y + 24u) % 255);
      } else if (pattern == PatternVariant::CheckerPulse) {
        const bool checker = (((x / checker_size) + (y / checker_size) + (frame_counter_ / 6u)) % 2u) == 0u;
        const uint8_t pulse = static_cast<uint8_t>((frame_counter_ * 9u + channel_seed(2)) % 255u);
        red = checker ? pulse : static_cast<uint8_t>((channel_seed(0) + y) % 255u);
        green = checker ? static_cast<uint8_t>((channel_seed(1) + x) % 255u) : static_cast<uint8_t>((pulse / 2u + stripe) % 255u);
        blue = checker ? static_cast<uint8_t>(255u - pulse) : static_cast<uint8_t>((x + y + channel_seed(3)) % 255u);
      } else {
        const uint8_t diagonal = static_cast<uint8_t>(((x + y + frame_counter_ * 5u) + channel_seed(2)) % 255u);
        const bool sweep = (((x + config_.width - y) + frame_counter_ * 11u) % (marker_width * 3u)) < marker_width;
        red = sweep ? static_cast<uint8_t>(255u - diagonal) : diagonal;
        green = static_cast<uint8_t>((stripe + (x / 4u) + (frame_counter_ * 2u)) % 255u);
        blue = sweep ? static_cast<uint8_t>((channel_seed(3) + 180u) % 255u) : static_cast<uint8_t>((animated + y / 2u) % 255u);
      }
      if (channels == 1) {
        buffer_[idx] = static_cast<uint8_t>((red / 3u + green / 3u + blue / 3u + (marker ? 24u : 0u)) % 255u);
      } else {
        buffer_[idx + 0] = red;
        buffer_[idx + 1] = green;
        buffer_[idx + 2] = blue;
      }
    }
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

  return VideoFrameView{buffer_.data(), config_.width, config_.height, stride, config_.input_pixel_format,
                        static_cast<uint64_t>(ns), frame_counter_};
}

}  // namespace video_server
