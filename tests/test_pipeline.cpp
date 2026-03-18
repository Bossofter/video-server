#include <cassert>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#if VIDEO_SERVER_TEST_WEBRTC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "video_server/webrtc_video_server.h"

namespace {

std::string http_request(const std::string& method, const std::string& path, const std::string& body = "") {
  std::ostringstream out;
  out << method << " " << path << " HTTP/1.1\r\n";
  out << "Host: 127.0.0.1\r\n";
  out << "Content-Type: application/json\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << body;
  return out.str();
}

std::string send_raw_request(const std::string& host, uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  assert(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1);
  assert(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
    assert(n > 0);
    sent += static_cast<size_t>(n);
  }

  std::string response;
  char buf[1024];
  while (true) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    response.append(buf, static_cast<size_t>(n));
  }

  ::close(fd);
  return response;
}

std::string http_response_headers(const std::string& response) {
  const auto header_end = response.find("\r\n\r\n");
  assert(header_end != std::string::npos);
  return response.substr(0, header_end);
}

std::string http_response_body(const std::string& response) {
  const auto header_end = response.find("\r\n\r\n");
  assert(header_end != std::string::npos);
  return response.substr(header_end + 4);
}

void push_test_frame(video_server::WebRtcVideoServer& server, const video_server::StreamConfig& cfg,
                     uint64_t timestamp_ns, uint64_t frame_id) {
  std::vector<uint8_t> frame(cfg.width * cfg.height, 0);
  for (uint32_t y = 0; y < cfg.height; ++y) {
    for (uint32_t x = 0; x < cfg.width; ++x) {
      frame[y * cfg.width + x] = static_cast<uint8_t>((x + y) % 256);
    }
  }

  video_server::VideoFrameView view{};
  view.data = frame.data();
  view.width = cfg.width;
  view.height = cfg.height;
  view.stride_bytes = cfg.width;
  view.pixel_format = video_server::VideoPixelFormat::GRAY8;
  view.timestamp_ns = timestamp_ns;
  view.frame_id = frame_id;
  assert(server.push_frame(cfg.stream_id, view));
}

}  // namespace

int test_stream_metadata_update() {
  const std::string host = "127.0.0.1";
  const uint16_t port = static_cast<uint16_t>(26000 + (::getpid() % 2000));
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"metadata-stream", "metadata", 8, 6, 30.0, video_server::VideoPixelFormat::GRAY8};

  assert(server.register_stream(cfg));
  assert(server.start());

  push_test_frame(server, cfg, 424242, 7);

  auto info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->has_latest_frame);
  assert(info->frames_received == 1);
  assert(info->frames_transformed == 1);
  assert(info->frames_dropped == 0);
  assert(info->last_input_timestamp_ns == 424242);
  assert(info->last_output_timestamp_ns == 424242);

  const std::string stream_info_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/metadata-stream"));
  assert(stream_info_resp.find("200 OK") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_width\":8") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_height\":6") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_pixel_format\":\"RGB24\"") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_timestamp_ns\":424242") != std::string::npos);

  server.stop();
  return 0;
}

int test_http_latest_frame_endpoint() {
  const std::string host = "127.0.0.1";
  const uint16_t port = static_cast<uint16_t>(28000 + (::getpid() % 2000));
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"frame-stream", "frame", 5, 4, 30.0, video_server::VideoPixelFormat::GRAY8};

  assert(server.register_stream(cfg));
  assert(server.start());
  push_test_frame(server, cfg, 1234, 1);

  const std::string frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/frame-stream/frame"));
  assert(frame_resp.find("200 OK") != std::string::npos);
  const std::string headers = http_response_headers(frame_resp);
  assert(headers.find("Content-Type: image/x-portable-pixmap") != std::string::npos);

  const std::string body = http_response_body(frame_resp);
  assert(!body.empty());

  std::ostringstream prefix;
  prefix << "P6\n" << cfg.width << ' ' << cfg.height << "\n255\n";
  const std::string expected_header = prefix.str();
  assert(body.size() == expected_header.size() + static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height) * 3u);
  assert(body.compare(0, expected_header.size(), expected_header) == 0);

  server.stop();
  return 0;
}

int test_http_stream_info_metadata() {
  const std::string host = "127.0.0.1";
  const uint16_t port = static_cast<uint16_t>(30000 + (::getpid() % 2000));
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"info-stream", "info", 7, 3, 30.0, video_server::VideoPixelFormat::GRAY8};

  assert(server.register_stream(cfg));
  assert(server.start());
  push_test_frame(server, cfg, 777000, 11);

  const std::string stream_info_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/info-stream"));
  assert(stream_info_resp.find("200 OK") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_width\":7") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_height\":3") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_pixel_format\":\"RGB24\"") != std::string::npos);
  assert(stream_info_resp.find("\"latest_frame_timestamp_ns\":777000") != std::string::npos);

  server.stop();
  return 0;
}

int test_pipeline_end_to_end_current_path() {
  // Future-facing scaffold: this validates the currently implemented end-to-end path and is intended
  // to grow in future PRs toward stronger transform assertions, future transport/media behavior,
  // encoded H264 bridge validation, and future WebRTC media output validation.
  const std::string host = "127.0.0.1";
  const uint16_t port = static_cast<uint16_t>(32000 + (::getpid() % 2000));
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"pipeline-stream", "pipeline", 6, 6, 30.0, video_server::VideoPixelFormat::GRAY8};

  assert(server.register_stream(cfg));
  assert(server.start());

  video_server::StreamOutputConfig out_cfg;
  out_cfg.display_mode = video_server::VideoDisplayMode::BlackHot;
  out_cfg.mirrored = false;
  out_cfg.rotation_degrees = 0;
  out_cfg.palette_min = 0.0;
  out_cfg.palette_max = 1.0;
  assert(server.set_stream_output_config(cfg.stream_id, out_cfg));

  push_test_frame(server, cfg, 8888, 99);

  std::array<uint8_t, 6> encoded_bytes{0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
  video_server::EncodedAccessUnitView access_unit{};
  access_unit.data = encoded_bytes.data();
  access_unit.size_bytes = encoded_bytes.size();
  access_unit.codec = video_server::VideoCodec::H264;
  access_unit.timestamp_ns = 9999;
  access_unit.keyframe = true;
  access_unit.codec_config = false;
  assert(server.push_access_unit(cfg.stream_id, access_unit));

  const std::string frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/pipeline-stream/frame"));
  assert(frame_resp.find("200 OK") != std::string::npos);
  assert(http_response_headers(frame_resp).find("Content-Type: image/x-portable-pixmap") != std::string::npos);
  const std::string body = http_response_body(frame_resp);
  assert(!body.empty());

  std::ostringstream prefix;
  prefix << "P6\n" << cfg.width << ' ' << cfg.height << "\n255\n";
  assert(body.compare(0, prefix.str().size(), prefix.str()) == 0);

  const std::string stream_info_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/pipeline-stream"));
  assert(stream_info_resp.find("200 OK") != std::string::npos);
  assert(stream_info_resp.find("\"has_latest_frame\":true") != std::string::npos);
  assert(stream_info_resp.find("\"has_latest_encoded_unit\":true") != std::string::npos);
  assert(stream_info_resp.find("\"latest_encoded_codec\":\"H264\"") != std::string::npos);
  assert(stream_info_resp.find("\"latest_encoded_timestamp_ns\":9999") != std::string::npos);
  assert(stream_info_resp.find("\"latest_encoded_keyframe\":true") != std::string::npos);

  server.stop();
  return 0;
}

#else
int test_stream_metadata_update() { return 0; }
int test_http_latest_frame_endpoint() { return 0; }
int test_http_stream_info_metadata() { return 0; }
int test_pipeline_end_to_end_current_path() { return 0; }
#endif
