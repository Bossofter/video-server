#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "video_server/raw_video_pipeline.h"

namespace video_server {

// Backend-private encoder option key/value pair.
struct RawH264EncoderOption {
  std::string key;
  std::string value;
};

// Encoder family used for backend-specific option handling.
enum class RawH264EncoderFamily {
  X264,
  OpenH264,
  Generic
};

// Backend options derived from the public raw pipeline configuration.
struct RawH264EncoderBackendOptions {
  RawH264EncoderFamily family{RawH264EncoderFamily::Generic};
  std::optional<std::string> profile_name;
  bool set_constrained_baseline_profile{false};
  std::vector<RawH264EncoderOption> private_options;
};

// Internal H.264 encoder backend interface used by the raw pipeline.
class IRawH264EncoderBackend {
 public:
  virtual ~IRawH264EncoderBackend() = default;

  /**
   * @brief Opens encoder resources.
   * 
   * @param error_message 
   * @return true 
   * @return false 
   */
  virtual bool open(std::string* error_message = nullptr) = 0;

  /**
   * @brief Encodes one raw frame.
   * 
   * @param frame 
   * @param error_message 
   * @return true 
   * @return false 
   */
  virtual bool encode_frame(const VideoFrameView& frame, std::string* error_message = nullptr) = 0;
  
  /**
   * @brief Flushes delayed packets from the encoder.
   * 
   * @param error_message 
   * @return true 
   * @return false 
   */
  virtual bool flush(std::string* error_message = nullptr) = 0;

  /**
   * @brief Closes encoder resources.
   * 
   */
  virtual void close() = 0;

  // 

  /**
   * @brief Returns true when the backend is currently open.
   * 
   * @return true 
   * @return false 
   */
  virtual bool is_open() const = 0;

  /**
   * @brief Returns the backend implementation name.
   * 
   * @return const char* 
   */
  virtual const char* backend_name() const = 0;

  /**
   * @brief Returns the active codec implementation name.
   * 
   * @return const char* 
   */
  virtual const char* encoder_name() const = 0;
  /**
   * @brief Returns the active encoder family.
   *
   * @return RawH264EncoderFamily 
   */
  virtual RawH264EncoderFamily encoder_family() const = 0;
};

/**
 * @brief Builds libav-specific backend options from the public pipeline config.
 * 
 * @param config 
 * @param codec_name 
 * @return RawH264EncoderBackendOptions 
 */
RawH264EncoderBackendOptions build_libav_h264_encoder_backend_options(const RawVideoPipelineConfig& config,
                                                                      const std::string& codec_name);

/**
 * @brief Creates the internal H.264 encoder backend for a pipeline instance.
 * 
 * @param stream_id 
 * @param config 
 * @param sink 
 * @return std::unique_ptr<IRawH264EncoderBackend> 
 */
std::unique_ptr<IRawH264EncoderBackend> make_raw_h264_encoder_backend(std::string stream_id,
                                                                      RawVideoPipelineConfig config,
                                                                      EncodedAccessUnitSink sink);


/**
 * @brief Returns a readable name for an encoder selection value.
 * 
 * @param encoder 
 * @return const char* 
 */
const char* to_string(RawH264Encoder encoder);

/**
 * @brief Returns a readable name for an encoder family value.
 * 
 * @param family 
 * @return const char* 
 */
const char* to_string(RawH264EncoderFamily family);

}  // namespace video_server
