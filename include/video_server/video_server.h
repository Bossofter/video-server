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
 * @brief 
 * 
 */
class IVideoServer {
 public:
  virtual ~IVideoServer() = default;

  /**
   * @brief 
   * 
   * @param config 
   * @return true 
   * @return false 
   */
  virtual bool register_stream(const StreamConfig& config) = 0;

  /**
   * @brief 
   * 
   * @param stream_id 
   * @return true 
   * @return false 
   */
  virtual bool remove_stream(const std::string& stream_id) = 0;

  /**
   * @brief 
   * 
   * @param stream_id 
   * @param frame 
   * @return true 
   * @return false 
   */
  virtual bool push_frame(const std::string& stream_id, const VideoFrameView& frame) = 0;

  /**
   * @brief 
   * 
   * @param stream_id 
   * @param access_unit 
   * @return true 
   * @return false 
   */
  virtual bool push_access_unit(const std::string& stream_id, const EncodedAccessUnitView& access_unit) = 0;

  /**
   * @brief 
   * 
   * @return std::vector<VideoStreamInfo> 
   */
  virtual std::vector<VideoStreamInfo> list_streams() const = 0;

  /**
   * @brief Get the stream info object
   * 
   * @param stream_id 
   * @return std::optional<VideoStreamInfo> 
   */
  virtual std::optional<VideoStreamInfo> get_stream_info(const std::string& stream_id) const = 0;

  /**
   * @brief Set the stream output config object
   * 
   * @param stream_id 
   * @param output_config 
   * @return true 
   * @return false 
   */
  virtual bool set_stream_output_config(const std::string& stream_id,
                                        const StreamOutputConfig& output_config) = 0;
  
  /**
   * @brief Get the stream output config object
   * 
   * @param stream_id 
   * @return std::optional<StreamOutputConfig> 
   */
  virtual std::optional<StreamOutputConfig> get_stream_output_config(
      const std::string& stream_id) const = 0;
};

}  // namespace video_server
