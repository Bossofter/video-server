#pragma once

#include <cstdint>
#include <vector>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"

namespace video_server {

/// Synthetic frame generator used by the smoke server and soak tests.
class SyntheticFrameGenerator {
 public:
  /// Creates a generator bound to one stream definition.
  explicit SyntheticFrameGenerator(StreamConfig config);

  /// Returns the stream config used by the generator.
  const StreamConfig& config() const { return config_; }
  /// Produces the next synthetic frame view.
  VideoFrameView next_frame();

 private:
  /// Internal pattern variants used to differentiate streams visually.
  enum class PatternVariant {
    GradientOrbit,
    CheckerPulse,
    DiagonalSweep,
  };

  /// Selects the pattern variant for the current stream.
  PatternVariant pattern_variant() const;
  StreamConfig config_;
  /// Derives a deterministic channel seed from the stream identity.
  uint8_t channel_seed(size_t channel) const;

  std::vector<uint8_t> buffer_;
  uint64_t frame_counter_{0};
  uint32_t stream_seed_{0};
};

}  // namespace video_server
