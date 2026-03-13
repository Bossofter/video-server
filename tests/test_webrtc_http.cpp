#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
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

void assert_ppm_payload(const std::string& body, uint32_t width, uint32_t height) {
  std::ostringstream prefix;
  prefix << "P6\n" << width << ' ' << height << "\n255\n";
  const std::string expected_header = prefix.str();
  assert(body.size() == expected_header.size() + static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
  assert(body.compare(0, expected_header.size(), expected_header) == 0);
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

  const std::string missing_frame_stream =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/nope/frame"));
  assert(missing_frame_stream.find("404 Not Found") != std::string::npos);

  const std::string no_latest_frame =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  assert(no_latest_frame.find("404 Not Found") != std::string::npos);

  std::vector<uint8_t> first_frame(cfg.width * cfg.height);
  for (uint32_t y = 0; y < cfg.height; ++y) {
    for (uint32_t x = 0; x < cfg.width; ++x) {
      first_frame[y * cfg.width + x] = static_cast<uint8_t>((x + y) % 256);
    }
  }

  video_server::VideoFrameView frame_view{};
  frame_view.data = first_frame.data();
  frame_view.width = cfg.width;
  frame_view.height = cfg.height;
  frame_view.stride_bytes = cfg.width;
  frame_view.pixel_format = video_server::VideoPixelFormat::GRAY8;
  frame_view.timestamp_ns = 1000;
  frame_view.frame_id = 1;
  assert(server.push_frame(cfg.stream_id, frame_view));

  const std::string first_frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  assert(first_frame_resp.find("200 OK") != std::string::npos);
  assert(http_response_headers(first_frame_resp).find("Content-Type: image/x-portable-pixmap") != std::string::npos);
  const std::string first_frame_payload = http_response_body(first_frame_resp);
  assert_ppm_payload(first_frame_payload, cfg.width, cfg.height);

  const std::string update_output_for_change =
      send_raw_request(host, port,
                       http_request("PUT", "/api/video/streams/stream-1/output",
                                    "{\"display_mode\":\"BlackHot\",\"mirrored\":false,\"rotation_degrees\":0,\"palette_min\":0.0,\"palette_max\":1.0}"));
  assert(update_output_for_change.find("200 OK") != std::string::npos);

  std::vector<uint8_t> second_frame(cfg.width * cfg.height);
  for (uint32_t y = 0; y < cfg.height; ++y) {
    for (uint32_t x = 0; x < cfg.width; ++x) {
      second_frame[y * cfg.width + x] = static_cast<uint8_t>(255 - ((x + y) % 256));
    }
  }
  frame_view.data = second_frame.data();
  frame_view.timestamp_ns = 2000;
  frame_view.frame_id = 2;
  assert(server.push_frame(cfg.stream_id, frame_view));

  const std::string second_frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  assert(second_frame_resp.find("200 OK") != std::string::npos);
  const std::string second_frame_payload = http_response_body(second_frame_resp);
  assert_ppm_payload(second_frame_payload, cfg.width, cfg.height);
  assert(first_frame_payload != second_frame_payload);

  bool producer_ok = true;
  std::thread producer([&]() {
    std::vector<uint8_t> frame(cfg.width * cfg.height, 0);
    for (uint64_t i = 0; i < 64; ++i) {
      for (size_t j = 0; j < frame.size(); ++j) {
        frame[j] = static_cast<uint8_t>((j + i) % 256);
      }
      video_server::VideoFrameView f{};
      f.data = frame.data();
      f.width = cfg.width;
      f.height = cfg.height;
      f.stride_bytes = cfg.width;
      f.pixel_format = video_server::VideoPixelFormat::GRAY8;
      f.timestamp_ns = 3000 + i;
      f.frame_id = 100 + i;
      if (!server.push_frame(cfg.stream_id, f)) {
        producer_ok = false;
      }
    }
  });

  for (int i = 0; i < 12; ++i) {
    const std::string resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
    assert(resp.find("200 OK") != std::string::npos);
    assert(http_response_headers(resp).find("Content-Type: image/x-portable-pixmap") != std::string::npos);
    assert_ppm_payload(http_response_body(resp), cfg.width, cfg.height);
  }
  producer.join();
  assert(producer_ok);

  const std::string stream_with_latest = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1"));
  assert(stream_with_latest.find("latest_frame_width") != std::string::npos);
  assert(stream_with_latest.find("latest_frame_height") != std::string::npos);
  assert(stream_with_latest.find("latest_frame_pixel_format") != std::string::npos);

  const std::string offer_resp =
      send_raw_request(host, port, http_request("POST", "/api/video/signaling/stream-1/offer", "offer-sdp"));
  assert(offer_resp.find("200 OK") != std::string::npos);

  server.stop();
  return 0;
}
#else
int test_webrtc_http() { return 0; }
#endif
