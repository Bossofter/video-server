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

//Scaling policy applied before H.264 encoding.
enum class RawPipelineScaleMode {
  Passthrough,
  Resize
};

//Encoder selection hint for the raw-to-H.264 pipeline.
enum class RawH264Encoder {
  Automatic,
  LibX264,
  LibOpenH264
};

//Configuration for a raw frame to H.264 pipeline instance.
struct RawVideoPipelineConfig {
  uint32_t input_width{0};
  uint32_t input_height{0};
  VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24};
  double input_fps{30.0};
  RawPipelineScaleMode scale_mode{RawPipelineScaleMode::Passthrough};
  std::optional<uint32_t> output_width;
  std::optional<uint32_t> output_height;
  std::optional<double> output_fps;
  RawH264Encoder encoder{RawH264Encoder::Automatic};
  std::string encoder_preset{"ultrafast"};
  std::string encoder_tune{"zerolatency"};
  std::string encoder_profile{"baseline"};
  bool repeat_headers{true};
  // Requests AUD NAL emission from encoders that support it. The libav backend normalizes
  // packets to Annex-B access units either way, so this remains a best-effort tuning knob.
  bool emit_access_unit_delimiters{true};
};

//Raw frame pipeline interface that emits encoded H.264 access units.
class IRawVideoPipeline {
 public:
  virtual ~IRawVideoPipeline() = default;

  //Returns the bound stream id for this pipeline instance.
  virtual const std::string& stream_id() const = 0;
  //Opens pipeline resources and validates the active configuration.
  virtual bool start(std::string* error_message = nullptr) = 0;
  //Admits one raw frame into the pipeline.
  virtual bool push_frame(const VideoFrameView& frame, std::string* error_message = nullptr) = 0;
  //Flushes and releases pipeline resources.
  virtual void stop() = 0;
};

// Returning false from the sink is treated as a hard pipeline failure; the pipeline records
// the error, stops the in-process encoder backend, and later push_frame() calls fail with that error.
using EncodedAccessUnitSink = std::function<bool(const EncodedAccessUnitView& access_unit)>;

/**
 * @brief Builds a raw-to-H.264 pipeline that delivers access units to a caller-provided sink.
 * 
 * @param stream_id 
 * @param config 
 * @param sink 
 * @return std::unique_ptr<IRawVideoPipeline> 
 */
std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline(std::string stream_id,
                                                             RawVideoPipelineConfig config,
                                                             EncodedAccessUnitSink sink);

/**
 * @brief Builds a raw-to-H.264 pipeline bound directly to an IVideoServer stream.
 * 
 * @param stream_id 
 * @param config 
 * @param server 
 * @return std::unique_ptr<IRawVideoPipeline> 
 */
std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline_for_server(std::string stream_id,
                                                                        RawVideoPipelineConfig config,
                                                                        IVideoServer& server);

/**
 * @brief Returns a readable name for a scale mode value.
 * 
 * @param scale_mode 
 * @return const char* 
 */
const char* to_string(RawPipelineScaleMode scale_mode);

/**
 * @brief Returns a readable name for an encoder selection value.
 * 
 * @param encoder 
 * @return const char* 
 */
const char* to_string(RawH264Encoder encoder);

}  // namespace video_server
