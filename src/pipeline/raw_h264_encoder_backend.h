#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "video_server/raw_video_pipeline.h"

namespace video_server {

struct RawH264EncoderOption {
  std::string key;
  std::string value;
};

enum class RawH264EncoderFamily {
  X264,
  OpenH264,
  Generic
};

struct RawH264EncoderBackendOptions {
  RawH264EncoderFamily family{RawH264EncoderFamily::Generic};
  std::optional<std::string> profile_name;
  bool set_constrained_baseline_profile{false};
  std::vector<RawH264EncoderOption> private_options;
};

class IRawH264EncoderBackend {
 public:
  virtual ~IRawH264EncoderBackend() = default;

  virtual bool open(std::string* error_message = nullptr) = 0;
  virtual bool encode_frame(const VideoFrameView& frame, std::string* error_message = nullptr) = 0;
  virtual bool flush(std::string* error_message = nullptr) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;
  virtual const char* backend_name() const = 0;
  virtual const char* encoder_name() const = 0;
  virtual RawH264EncoderFamily encoder_family() const = 0;
};

RawH264EncoderBackendOptions build_libav_h264_encoder_backend_options(const RawVideoPipelineConfig& config,
                                                                      const std::string& codec_name);

std::unique_ptr<IRawH264EncoderBackend> make_raw_h264_encoder_backend(std::string stream_id,
                                                                      RawVideoPipelineConfig config,
                                                                      EncodedAccessUnitSink sink);

const char* to_string(RawH264Encoder encoder);
const char* to_string(RawH264EncoderFamily family);

}  // namespace video_server
