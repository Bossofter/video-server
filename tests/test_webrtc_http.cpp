#include <array>
#include <stdexcept>

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define CHECK_TRUE(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while(false)

#if VIDEO_SERVER_TEST_WEBRTC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rtc/description.hpp>
#include <rtc/peerconnection.hpp>

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
  CHECK_TRUE(fd >= 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  CHECK_TRUE(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1);
  CHECK_TRUE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
    CHECK_TRUE(n > 0);
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
  CHECK_TRUE(header_end != std::string::npos);
  return response.substr(0, header_end);
}

std::string http_response_body(const std::string& response) {
  const auto header_end = response.find("\r\n\r\n");
  CHECK_TRUE(header_end != std::string::npos);
  return response.substr(header_end + 4);
}

void assert_ppm_payload(const std::string& body, uint32_t width, uint32_t height) {
  std::ostringstream prefix;
  prefix << "P6\n" << width << ' ' << height << "\n255\n";
  const std::string expected_header = prefix.str();
  CHECK_TRUE(body.size() == expected_header.size() + static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
  CHECK_TRUE(body.compare(0, expected_header.size(), expected_header) == 0);
}

void assert_json_response_headers(const std::string& response) {
  CHECK_TRUE(response.find("Content-Type: application/json") != std::string::npos);
  CHECK_TRUE(response.find("Content-Length:") != std::string::npos);
}

std::string json_string_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto start = json.find(needle);
  CHECK_TRUE(start != std::string::npos);
  const auto value_start = start + needle.size();
  const auto end = json.find('"', value_start);
  CHECK_TRUE(end != std::string::npos);
  return json.substr(value_start, end - value_start);
}

uint64_t json_uint_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto start = json.find(needle);
  CHECK_TRUE(start != std::string::npos);
  size_t pos = start + needle.size();
  size_t end = pos;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') {
    ++end;
  }
  return std::stoull(json.substr(pos, end - pos));
}

bool json_bool_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto start = json.find(needle);
  CHECK_TRUE(start != std::string::npos);
  const size_t pos = start + needle.size();
  if (json.compare(pos, 4, "true") == 0) {
    return true;
  }
  CHECK_TRUE(json.compare(pos, 5, "false") == 0);
  return false;
}

template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms = 5000, int sleep_ms = 25) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return predicate();
}

}  // namespace

TEST(WebRtcHttpTest, ExercisesHttpAndSignalingFlow) {
  const uint16_t port = static_cast<uint16_t>(20000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"stream-1", "label", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));
  CHECK_TRUE(server.start());

  const std::string list_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams"));
  CHECK_TRUE(list_resp.find("200 OK") != std::string::npos);
  CHECK_TRUE(list_resp.find("stream-1") != std::string::npos);
  assert_json_response_headers(list_resp);

  const std::string stream_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1"));
  CHECK_TRUE(stream_resp.find("200 OK") != std::string::npos);
  CHECK_TRUE(stream_resp.find("frames_received") != std::string::npos);
  assert_json_response_headers(stream_resp);

  std::array<uint8_t, 7> initial_access_unit_bytes{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00};
  video_server::EncodedAccessUnitView initial_access_unit{};
  initial_access_unit.data = initial_access_unit_bytes.data();
  initial_access_unit.size_bytes = initial_access_unit_bytes.size();
  initial_access_unit.codec = video_server::VideoCodec::H264;
  initial_access_unit.timestamp_ns = 7777;
  initial_access_unit.keyframe = false;
  initial_access_unit.codec_config = true;
  CHECK_TRUE(server.push_access_unit(cfg.stream_id, initial_access_unit));

  const std::string stream_resp_after_encoded =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1"));
  CHECK_TRUE(stream_resp_after_encoded.find("\"has_latest_encoded_unit\":true") != std::string::npos);
  CHECK_TRUE(stream_resp_after_encoded.find("\"latest_encoded_codec\":\"H264\"") != std::string::npos);
  CHECK_TRUE(stream_resp_after_encoded.find("\"latest_encoded_timestamp_ns\":7777") != std::string::npos);
  CHECK_TRUE(stream_resp_after_encoded.find("\"latest_encoded_codec_config\":true") != std::string::npos);

  const std::string output_resp = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/output"));
  CHECK_TRUE(output_resp.find("200 OK") != std::string::npos);
  CHECK_TRUE(output_resp.find("Passthrough") != std::string::npos);

  const std::string update_resp = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"WhiteHot\",\"mirrored\":true,\"rotation_degrees\":90,\"palette_min\":0.1,\"palette_max\":0.9}"));
  CHECK_TRUE(update_resp.find("200 OK") != std::string::npos);
  CHECK_TRUE(update_resp.find("WhiteHot") != std::string::npos);

  const std::string missing_stream = send_raw_request(host, port, http_request("GET", "/api/video/streams/does-not-exist"));
  CHECK_TRUE(missing_stream.find("404 Not Found") != std::string::npos);

  const std::string invalid_json = send_raw_request(
      host, port, http_request("PUT", "/api/video/streams/stream-1/output", "{\"display_mode\":\"WhiteHot\""));
  CHECK_TRUE(invalid_json.find("400 Bad Request") != std::string::npos);
  CHECK_TRUE(invalid_json.find("invalid request body") != std::string::npos);

  const std::string invalid_mode = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"invalid_value\",\"mirrored\":false,\"rotation_degrees\":0,\"palette_min\":0.0,\"palette_max\":1.0}"));
  CHECK_TRUE(invalid_mode.find("400 Bad Request") != std::string::npos);
  CHECK_TRUE(invalid_mode.find("invalid display_mode") != std::string::npos);

  const std::string invalid_rotation = send_raw_request(
      host, port,
      http_request("PUT", "/api/video/streams/stream-1/output",
                   "{\"display_mode\":\"WhiteHot\",\"mirrored\":false,\"rotation_degrees\":45,\"palette_min\":0.0,\"palette_max\":1.0}"));
  CHECK_TRUE(invalid_rotation.find("400 Bad Request") != std::string::npos);

  const std::string missing_frame_stream =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/nope/frame"));
  CHECK_TRUE(missing_frame_stream.find("404 Not Found") != std::string::npos);

  const std::string no_latest_frame =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  CHECK_TRUE(no_latest_frame.find("404 Not Found") != std::string::npos);

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
  CHECK_TRUE(server.push_frame(cfg.stream_id, frame_view));

  const std::string first_frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  CHECK_TRUE(first_frame_resp.find("200 OK") != std::string::npos);
  CHECK_TRUE(http_response_headers(first_frame_resp).find("Content-Type: image/x-portable-pixmap") != std::string::npos);
  const std::string first_frame_payload = http_response_body(first_frame_resp);
  assert_ppm_payload(first_frame_payload, cfg.width, cfg.height);

  const std::string update_output_for_change =
      send_raw_request(host, port,
                       http_request("PUT", "/api/video/streams/stream-1/output",
                                    "{\"display_mode\":\"BlackHot\",\"mirrored\":false,\"rotation_degrees\":0,\"palette_min\":0.0,\"palette_max\":1.0}"));
  CHECK_TRUE(update_output_for_change.find("200 OK") != std::string::npos);

  std::vector<uint8_t> second_frame(cfg.width * cfg.height);
  for (uint32_t y = 0; y < cfg.height; ++y) {
    for (uint32_t x = 0; x < cfg.width; ++x) {
      second_frame[y * cfg.width + x] = static_cast<uint8_t>(255 - ((x + y) % 256));
    }
  }
  frame_view.data = second_frame.data();
  frame_view.timestamp_ns = 2000;
  frame_view.frame_id = 2;
  CHECK_TRUE(server.push_frame(cfg.stream_id, frame_view));

  const std::string second_frame_resp =
      send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1/frame"));
  CHECK_TRUE(second_frame_resp.find("200 OK") != std::string::npos);
  const std::string second_frame_payload = http_response_body(second_frame_resp);
  assert_ppm_payload(second_frame_payload, cfg.width, cfg.height);
  CHECK_TRUE(first_frame_payload != second_frame_payload);

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
    CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    CHECK_TRUE(http_response_headers(resp).find("Content-Type: image/x-portable-pixmap") != std::string::npos);
    assert_ppm_payload(http_response_body(resp), cfg.width, cfg.height);
  }
  producer.join();
  CHECK_TRUE(producer_ok);

  const std::string stream_with_latest = send_raw_request(host, port, http_request("GET", "/api/video/streams/stream-1"));
  CHECK_TRUE(stream_with_latest.find("latest_frame_width") != std::string::npos);
  CHECK_TRUE(stream_with_latest.find("latest_frame_height") != std::string::npos);
  CHECK_TRUE(stream_with_latest.find("latest_frame_pixel_format") != std::string::npos);
  CHECK_TRUE(stream_with_latest.find("\"latest_frame_pixel_format\":\"RGB24\"") != std::string::npos);

  rtc::Configuration rtc_config;
  auto client = std::make_shared<rtc::PeerConnection>(rtc_config);
  auto bootstrap_channel = client->createDataChannel("bootstrap");
  (void)bootstrap_channel;

  std::mutex callback_mutex;
  std::string local_offer_sdp;
  std::vector<std::string> client_candidates;

  client->onLocalDescription([&](rtc::Description description) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    local_offer_sdp = std::string(description);
  });
  client->onLocalCandidate([&](rtc::Candidate candidate) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    client_candidates.push_back(std::string(candidate));
  });
  client->setLocalDescription(rtc::Description::Type::Offer);

  CHECK_TRUE(wait_until([&]() {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      if (!local_offer_sdp.empty()) {
        return true;
      }
    }
    const auto description = client->localDescription();
    if (!description.has_value()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(callback_mutex);
    local_offer_sdp = std::string(*description);
    return true;
  }));

  const auto offer_response =
      ([&]() {
        std::lock_guard<std::mutex> lock(callback_mutex);
        return server.handle_http_request_for_test("POST", "/api/video/signaling/stream-1/offer", local_offer_sdp);
      })();
  CHECK_TRUE(offer_response.status == 200);

  std::string session_json;
  CHECK_TRUE(wait_until([&]() {
    const auto response = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
    if (response.status != 200) {
      return false;
    }
    session_json = response.body;
    return !json_string_field(session_json, "answer_sdp").empty();
  }));

  CHECK_TRUE(session_json.find("peer_state") != std::string::npos);
  CHECK_TRUE(session_json.find("media_bridge_state") != std::string::npos);
  const uint64_t first_generation = json_uint_field(session_json, "session_generation");
  CHECK_TRUE(first_generation >= 1);

  auto replacement_client = std::make_shared<rtc::PeerConnection>(rtc_config);
  auto replacement_bootstrap_channel = replacement_client->createDataChannel("bootstrap-replacement");
  (void)replacement_bootstrap_channel;
  std::string replacement_offer_sdp;
  std::vector<std::string> replacement_candidates;
  replacement_client->onLocalDescription([&](rtc::Description description) {
    replacement_offer_sdp = std::string(description);
  });
  replacement_client->onLocalCandidate([&](rtc::Candidate candidate) {
    replacement_candidates.push_back(std::string(candidate));
  });
  replacement_client->setLocalDescription(rtc::Description::Type::Offer);
  CHECK_TRUE(wait_until([&]() {
    if (!replacement_offer_sdp.empty()) {
      return true;
    }
    const auto description = replacement_client->localDescription();
    if (!description.has_value()) {
      return false;
    }
    replacement_offer_sdp = std::string(*description);
    return true;
  }));

  const auto replacement_offer_response =
      server.handle_http_request_for_test("POST", "/api/video/signaling/stream-1/offer", replacement_offer_sdp);
  CHECK_TRUE(replacement_offer_response.status == 200);
  CHECK_TRUE(wait_until([&]() {
    const auto response = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
    if (response.status != 200) {
      return false;
    }
    session_json = response.body;
    return json_uint_field(session_json, "session_generation") > first_generation &&
           json_string_field(session_json, "offer_sdp") == replacement_offer_sdp;
  }));


  std::string client_candidate;
  wait_until([&]() {
    if (replacement_candidates.empty()) {
      return false;
    }
    client_candidate = replacement_candidates.front();
    return true;
  });

  bool remote_candidate_recorded = false;
  if (!client_candidate.empty()) {
    const auto candidate_response =
        server.handle_http_request_for_test("POST", "/api/video/signaling/stream-1/candidate", client_candidate);
    CHECK_TRUE(candidate_response.status == 200);
    const auto session_after_candidate = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
    CHECK_TRUE(session_after_candidate.status == 200);
    remote_candidate_recorded = !json_string_field(session_after_candidate.body, "last_remote_candidate").empty();
  }

  std::array<uint8_t, 5> access_unit_bytes{0x00, 0x00, 0x00, 0x01, 0x65};
  video_server::EncodedAccessUnitView access_unit{};
  access_unit.data = access_unit_bytes.data();
  access_unit.size_bytes = access_unit_bytes.size();
  access_unit.codec = video_server::VideoCodec::H264;
  access_unit.timestamp_ns = 123456;
  access_unit.keyframe = true;
  access_unit.codec_config = false;
  CHECK_TRUE(server.push_access_unit(cfg.stream_id, access_unit));

  std::vector<uint8_t> webrtc_frame(cfg.width * cfg.height, 200);
  frame_view.data = webrtc_frame.data();
  frame_view.timestamp_ns = 9999;
  frame_view.frame_id = 777;
  CHECK_TRUE(server.push_frame(cfg.stream_id, frame_view));

  for (int i = 0; i < 8; ++i) {
    frame_view.timestamp_ns += 1;
    frame_view.frame_id += 1;
    CHECK_TRUE(server.push_frame(cfg.stream_id, frame_view));
  }

  video_server::WebRtcHttpResponse session_after_updates{};
  CHECK_TRUE(wait_until([&]() {
    session_after_updates = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
    return session_after_updates.status == 200 && !json_string_field(session_after_updates.body, "answer_sdp").empty();
  }));
  CHECK_TRUE(json_string_field(session_after_updates.body, "media_bridge_state") == "awaiting-h264-video-track-bridge");
  CHECK_TRUE(json_string_field(session_after_updates.body, "preferred_media_path") == "encoded-access-unit");
  if (remote_candidate_recorded) {
    CHECK_TRUE(!json_string_field(session_after_updates.body, "last_remote_candidate").empty());
  }
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_snapshot_frame_id") == frame_view.frame_id);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_snapshot_timestamp_ns") == frame_view.timestamp_ns);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_snapshot_width") == cfg.width);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_snapshot_height") == cfg.height);
  CHECK_TRUE(json_string_field(session_after_updates.body, "latest_encoded_codec") == "H264");
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_encoded_timestamp_ns") == access_unit.timestamp_ns);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_encoded_sequence_id") == access_unit.timestamp_ns);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "latest_encoded_size_bytes") == access_unit.size_bytes);
  CHECK_TRUE(json_bool_field(session_after_updates.body, "latest_encoded_keyframe"));
  CHECK_TRUE(!json_bool_field(session_after_updates.body, "latest_encoded_codec_config"));

  CHECK_TRUE(server.remove_stream(cfg.stream_id));
  const auto removed_session = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
  CHECK_TRUE(removed_session.status == 404);

  server.stop();
}
#else
TEST(WebRtcHttpTest, ExercisesHttpAndSignalingFlow) { GTEST_SKIP() << "WebRTC backend disabled"; }
#endif
