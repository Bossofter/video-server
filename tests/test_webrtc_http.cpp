#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>

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

void assert_json_response_headers(const std::string& response) {
  assert(response.find("Content-Type: application/json") != std::string::npos);
  assert(response.find("Content-Length:") != std::string::npos);
}

}  // namespace

int test_webrtc_http() {
  const uint16_t port = static_cast<uint16_t>(20000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"stream-1", "label", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  assert(server.register_stream(cfg));
  assert(server.start());

  const std::string list_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams"));
  assert(list_resp.find("200 OK") != std::string::npos);
  assert(list_resp.find("stream-1") != std::string::npos);
  assert_json_response_headers(list_resp);

  const std::string stream_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1"));
  assert(stream_resp.find("200 OK") != std::string::npos);
  assert(stream_resp.find("frames_received") != std::string::npos);
  assert_json_response_headers(stream_resp);

  const std::string output_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/output"));
  assert(output_resp.find("200 OK") != std::string::npos);
  assert(output_resp.find("Passthrough") != std::string::npos);

  const std::string update_resp = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"WhiteHot\",\"mirrored\":true,\"rotation_degrees\":90,\"palette_min\":0.1,\"palette_max\":0.9}"));
  assert(update_resp.find("200 OK") != std::string::npos);
  assert(update_resp.find("WhiteHot") != std::string::npos);

  const std::string missing_stream = send_raw_request(host, port, http_request("GET", "/api/video/streams/does-not-exist"));
  assert(missing_stream.find("404 Not Found") != std::string::npos);

  const std::string invalid_json = send_raw_request(
      host, port, http_request("PUT", "/api/video/streams/stream-1/output", "{\"display_mode\":\"WhiteHot\""));
  assert(invalid_json.find("400 Bad Request") != std::string::npos);
  assert(invalid_json.find("invalid request body") != std::string::npos);

  const std::string invalid_mode = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"invalid_value\",\"mirrored\":false,\"rotation_degrees\":0,\"palette_min\":0.0,\"palette_max\":1.0}"));
  assert(invalid_mode.find("400 Bad Request") != std::string::npos);
  assert(invalid_mode.find("invalid display_mode") != std::string::npos);

  const std::string invalid_rotation = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"WhiteHot\",\"mirrored\":false,\"rotation_degrees\":45,\"palette_min\":0.0,\"palette_max\":1.0}"));
  assert(invalid_rotation.find("400 Bad Request") != std::string::npos);

  const std::string offer_resp =
      send_raw_request(host, port, http_request("POST", "/api/video/signaling/stream-1/offer", "offer-sdp"));
  assert(offer_resp.find("200 OK") != std::string::npos);

  server.stop();
  return 0;
}
#else
int test_webrtc_http() { return 0; }
#endif
