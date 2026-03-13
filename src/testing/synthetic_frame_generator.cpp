#include "synthetic_frame_generator.h"

#include <chrono>

namespace video_server {

SyntheticFrameGenerator::SyntheticFrameGenerator(StreamConfig config) : config_(std::move(config)) {
  const uint32_t channels = (config_.input_pixel_format == VideoPixelFormat::GRAY8) ? 1 : 3;
  buffer_.resize(static_cast<size_t>(config_.width) * config_.height * channels);
}

VideoFrameView SyntheticFrameGenerator::next_frame() {
  ++frame_counter_;
  const uint32_t channels = (config_.input_pixel_format == VideoPixelFormat::GRAY8) ? 1 : 3;
  const uint32_t stride = config_.width * channels;

  for (uint32_t y = 0; y < config_.height; ++y) {
    for (uint32_t x = 0; x < config_.width; ++x) {
      const uint8_t base = static_cast<uint8_t>((x + frame_counter_) % 255);
      const size_t idx = static_cast<size_t>(y) * stride + x * channels;
      if (channels == 1) {
        buffer_[idx] = static_cast<uint8_t>((base + y) % 255);
      } else {
        buffer_[idx + 0] = base;
        buffer_[idx + 1] = static_cast<uint8_t>((base + y) % 255);
        buffer_[idx + 2] = static_cast<uint8_t>((base + frame_counter_ / 2) % 255);
      }
    }
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

  return VideoFrameView{buffer_.data(), config_.width, config_.height, stride, config_.input_pixel_format,
                        static_cast<uint64_t>(ns), frame_counter_};
}

}  // namespace video_server
