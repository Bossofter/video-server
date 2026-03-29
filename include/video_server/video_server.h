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

/// Common video server interface used by the core and WebRTC-backed implementations.
class IVideoServer {
 public:
  virtual ~IVideoServer() = default;

  /// Registers a new stream definition.
  virtual bool register_stream(const StreamConfig& config) = 0;
  /// Removes an existing stream and any associated backend state.
  virtual bool remove_stream(const std::string& stream_id) = 0;

  /// Pushes one raw frame into the named stream.
  virtual bool push_frame(const std::string& stream_id, const VideoFrameView& frame) = 0;
  /// Pushes one encoded access unit into the named stream.
  virtual bool push_access_unit(const std::string& stream_id, const EncodedAccessUnitView& access_unit) = 0;

  /// Returns a snapshot of all registered streams.
  virtual std::vector<VideoStreamInfo> list_streams() const = 0;
  /// Returns stream information for one stream when present.
  virtual std::optional<VideoStreamInfo> get_stream_info(const std::string& stream_id) const = 0;

  /// Updates the runtime output config for a stream.
  virtual bool set_stream_output_config(const std::string& stream_id,
                                        const StreamOutputConfig& output_config) = 0;
  /// Returns the runtime output config for a stream when present.
  virtual std::optional<StreamOutputConfig> get_stream_output_config(
      const std::string& stream_id) const = 0;
};

}  // namespace video_server
