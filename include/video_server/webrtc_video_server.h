#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "video_server/observability.h"
#include "video_server/video_server.h"

namespace video_server {

struct WebRtcVideoServerConfig {
  std::string http_host{"127.0.0.1"};
  uint16_t http_port{8080};
  bool enable_http_api{true};
};

struct WebRtcHttpResponse {
  int status{200};
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

class WebRtcVideoServer : public IVideoServer {
 public:
  explicit WebRtcVideoServer(WebRtcVideoServerConfig config = {});
  ~WebRtcVideoServer() override;

  WebRtcVideoServer(const WebRtcVideoServer&) = delete;
  WebRtcVideoServer& operator=(const WebRtcVideoServer&) = delete;

  bool start();
  void stop();

  bool register_stream(const StreamConfig& config) override;
  bool remove_stream(const std::string& stream_id) override;
  bool push_frame(const std::string& stream_id, const VideoFrameView& frame) override;
  bool push_access_unit(const std::string& stream_id, const EncodedAccessUnitView& access_unit) override;
  std::vector<VideoStreamInfo> list_streams() const override;
  std::optional<VideoStreamInfo> get_stream_info(const std::string& stream_id) const override;
  bool set_stream_output_config(const std::string& stream_id,
                                const StreamOutputConfig& output_config) override;
  std::optional<StreamOutputConfig> get_stream_output_config(const std::string& stream_id) const override;

  WebRtcHttpResponse handle_http_request_for_test(const std::string& method, const std::string& path,
                                                  const std::string& body = "",
                                                  std::unordered_map<std::string, std::string> headers = {});
  ServerDebugSnapshot get_debug_snapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace video_server
