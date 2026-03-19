#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "video_server/encoded_access_unit_view.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_server.h"

namespace video_server {

enum class RawPipelineScaleMode {
  Passthrough,
  Resize
};

struct RawVideoPipelineConfig {
  // Current first-pass ffmpeg backend limitation: raw input must be tightly packed.
  std::string ffmpeg_path{"ffmpeg"};
  uint32_t input_width{0};
  uint32_t input_height{0};
  VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24};
  double input_fps{30.0};
  RawPipelineScaleMode scale_mode{RawPipelineScaleMode::Passthrough};
  std::optional<uint32_t> output_width;
  std::optional<uint32_t> output_height;
  std::optional<double> output_fps;
  std::string encoder_preset{"ultrafast"};
  std::string encoder_tune{"zerolatency"};
  std::string encoder_profile{"baseline"};
  bool repeat_headers{true};
  // Current first-pass backend requires AUD NAL units for access-unit splitting; setting this to
  // false is rejected until a non-AUD fallback splitter is implemented.
  bool emit_access_unit_delimiters{true};
};

class IRawVideoPipeline {
 public:
  virtual ~IRawVideoPipeline() = default;

  virtual const std::string& stream_id() const = 0;
  virtual bool start(std::string* error_message = nullptr) = 0;
  virtual bool push_frame(const VideoFrameView& frame, std::string* error_message = nullptr) = 0;
  virtual void stop() = 0;
};

// Returning false from the sink is treated as a hard pipeline failure; the pipeline records
// the error, stops the ffmpeg subprocess bridge, and later push_frame() calls fail with that error.
using EncodedAccessUnitSink = std::function<bool(const EncodedAccessUnitView& access_unit)>;

std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline(std::string stream_id,
                                                             RawVideoPipelineConfig config,
                                                             EncodedAccessUnitSink sink);

std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline_for_server(std::string stream_id,
                                                                        RawVideoPipelineConfig config,
                                                                        IVideoServer& server);

const char* to_string(RawPipelineScaleMode scale_mode);

}  // namespace video_server
