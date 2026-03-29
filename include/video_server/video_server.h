#pragma once

#include <optional>
#include <string>
#include <vector>

#include "video_server/encoded_access_unit_view.h"
#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_stream_info.h"

namespace video_server {

/**
 * @brief Common video server interface used by the core and WebRTC-backed implementations.
 */
class IVideoServer {
 public:
  virtual ~IVideoServer() = default;

  /**
   * @brief Registers a new stream definition.
   *
   * @param config Stream registration parameters.
   * @return True when the stream was registered successfully, false otherwise.
   */
  virtual bool register_stream(const StreamConfig& config) = 0;

  /**
   * @brief Removes an existing stream and any associated backend state.
   *
   * @param stream_id Identifier of the stream to remove.
   * @return True when the stream was removed, false otherwise.
   */
  virtual bool remove_stream(const std::string& stream_id) = 0;

  /**
   * @brief Pushes one raw frame into the named stream.
   *
   * @param stream_id Identifier of the target stream.
   * @param frame Raw frame view to ingest.
   * @return True when the frame was accepted, false otherwise.
   */
  virtual bool push_frame(const std::string& stream_id, const VideoFrameView& frame) = 0;

  /**
   * @brief Pushes one encoded access unit into the named stream.
   *
   * @param stream_id Identifier of the target stream.
   * @param access_unit Encoded access unit to ingest.
   * @return True when the access unit was accepted, false otherwise.
   */
  virtual bool push_access_unit(const std::string& stream_id, const EncodedAccessUnitView& access_unit) = 0;

  /**
   * @brief Returns a snapshot of all registered streams.
   *
   * @return Current stream information for every registered stream.
   */
  virtual std::vector<VideoStreamInfo> list_streams() const = 0;

  /**
   * @brief Returns stream information for one stream when present.
   *
   * @param stream_id Identifier of the stream to query.
   * @return Stream information when the stream exists, otherwise `std::nullopt`.
   */
  virtual std::optional<VideoStreamInfo> get_stream_info(const std::string& stream_id) const = 0;

  /**
   * @brief Updates the runtime output config for a stream.
   *
   * @param stream_id Identifier of the stream to update.
   * @param output_config New runtime output configuration.
   * @return True when the configuration was applied, false otherwise.
   */
  virtual bool set_stream_output_config(const std::string& stream_id,
                                        const StreamOutputConfig& output_config) = 0;

  /**
   * @brief Returns the runtime output config for a stream when present.
   *
   * @param stream_id Identifier of the stream to query.
   * @return Runtime output configuration when the stream exists, otherwise `std::nullopt`.
   */
  virtual std::optional<StreamOutputConfig> get_stream_output_config(
      const std::string& stream_id) const = 0;
};

}  // namespace video_server
