#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "video_server/raw_video_pipeline.h"

namespace video_server {

/**
 * @brief Backend-private encoder option key/value pair.
 */
struct RawH264EncoderOption {
  std::string key;
  std::string value;
};

/**
 * @brief Encoder family used for backend-specific option handling.
 */
enum class RawH264EncoderFamily {
  X264,
  OpenH264,
  Generic
};

/**
 * @brief Backend options derived from the public raw pipeline configuration.
 */
struct RawH264EncoderBackendOptions {
  RawH264EncoderFamily family{RawH264EncoderFamily::Generic};
  std::optional<std::string> profile_name;
  bool set_constrained_baseline_profile{false};
  std::vector<RawH264EncoderOption> private_options;
};

/**
 * @brief Internal H.264 encoder backend interface used by the raw pipeline.
 */
class IRawH264EncoderBackend {
 public:
  virtual ~IRawH264EncoderBackend() = default;

  /**
   * @brief Opens encoder resources.
   *
   * @param error_message Optional destination for a human-readable failure reason.
   * @return True when the backend was opened successfully, false otherwise.
   */
  virtual bool open(std::string* error_message = nullptr) = 0;

  /**
   * @brief Encodes one raw frame.
   *
   * @param frame Raw frame view to encode.
   * @param error_message Optional destination for a human-readable failure reason.
   * @return True when the frame was accepted by the encoder, false otherwise.
   */
  virtual bool encode_frame(const VideoFrameView& frame, std::string* error_message = nullptr) = 0;
  
  /**
   * @brief Flushes delayed packets from the encoder.
   *
   * @param error_message Optional destination for a human-readable failure reason.
   * @return True when flush succeeded, false otherwise.
   */
  virtual bool flush(std::string* error_message = nullptr) = 0;

  /**
   * @brief Closes encoder resources.
   * 
   */
  virtual void close() = 0;

  /**
   * @brief Returns true when the backend is currently open.
   *
   * @return True when the backend has active encoder resources, false otherwise.
   */
  virtual bool is_open() const = 0;

  /**
   * @brief Returns the backend implementation name.
   *
   * @return Null-terminated string naming the backend implementation.
   */
  virtual const char* backend_name() const = 0;

  /**
   * @brief Returns the active codec implementation name.
   *
   * @return Null-terminated string naming the encoder implementation.
   */
  virtual const char* encoder_name() const = 0;
  /**
   * @brief Returns the active encoder family.
   *
   * @return Encoder family associated with the active backend.
   */
  virtual RawH264EncoderFamily encoder_family() const = 0;
};

/**
 * @brief Builds libav-specific backend options from the public pipeline config.
 *
 * @param config Public raw pipeline configuration.
 * @param codec_name Codec implementation name requested from libav.
 * @return Backend options derived from the supplied configuration.
 */
RawH264EncoderBackendOptions build_libav_h264_encoder_backend_options(const RawVideoPipelineConfig& config,
                                                                      const std::string& codec_name);

/**
 * @brief Creates the internal H.264 encoder backend for a pipeline instance.
 *
 * @param stream_id Identifier of the bound stream.
 * @param config Raw pipeline configuration.
 * @param sink Encoded-unit sink invoked for emitted access units.
 * @return Newly created encoder backend instance.
 */
std::unique_ptr<IRawH264EncoderBackend> make_raw_h264_encoder_backend(std::string stream_id,
                                                                      RawVideoPipelineConfig config,
                                                                      EncodedAccessUnitSink sink);


/**
 * @brief Returns a readable name for an encoder selection value.
 *
 * @param encoder Encoder selection to convert.
 * @return Null-terminated string describing the encoder selection.
 */
const char* to_string(RawH264Encoder encoder);

/**
 * @brief Returns a readable name for an encoder family value.
 *
 * @param family Encoder family to convert.
 * @return Null-terminated string describing the encoder family.
 */
const char* to_string(RawH264EncoderFamily family);

}  // namespace video_server
