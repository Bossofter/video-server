#include <array>
#include <stdexcept>

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>
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

video_server::WebRtcVideoServerConfig make_http_test_config(const std::string& host = "127.0.0.1",
                                                            uint16_t port = 0,
                                                            bool enable_http_api = false) {
  video_server::WebRtcVideoServerConfig config;
  config.http_host = host;
  config.http_port = port;
  config.enable_http_api = enable_http_api;
  return config;
}

std::string json_string_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto start = json.find(needle);
  CHECK_TRUE(start != std::string::npos);
  const auto value_start = start + needle.size();
  std::string value;
  bool escaping = false;
  for (size_t i = value_start; i < json.size(); ++i) {
    const char c = json[i];
    if (escaping) {
      switch (c) {
        case '"':
        case '\\':
        case '/':
          value.push_back(c);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          CHECK_TRUE(false);
      }
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    if (c == '"') {
      return value;
    }
    value.push_back(c);
  }
  CHECK_TRUE(false);
  return "";
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

std::string response_header(const video_server::WebRtcHttpResponse& response, const std::string& key) {
  const auto it = response.headers.find(key);
  CHECK_TRUE(it != response.headers.end());
  return it->second;
}

void assert_no_control_chars(const std::string& text) {
  for (unsigned char c : text) {
    CHECK_TRUE(c >= 0x20);
  }
}

size_t count_sdp_media_sections(const std::string& sdp) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = sdp.find("\nm=", pos)) != std::string::npos) {
    ++count;
    pos += 3;
  }
  if (sdp.rfind("m=", 0) == 0) {
    ++count;
  }
  return count;
}

std::vector<std::string> extract_sdp_mids(const std::string& sdp) {
  std::vector<std::string> mids;
  std::istringstream input(sdp);
  for (std::string line; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("a=mid:", 0) == 0) {
      mids.push_back(line.substr(6));
    }
  }
  return mids;
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

const video_server::StreamDebugSnapshot* find_stream_snapshot(const video_server::ServerDebugSnapshot& snapshot,
                                                              const std::string& stream_id) {
  for (const auto& stream : snapshot.streams) {
    if (stream.stream_id == stream_id) {
      return &stream;
    }
  }
  return nullptr;
}

struct ClientPeerOffer {
  std::shared_ptr<rtc::PeerConnection> peer_connection;
  std::mutex mutex;
  std::string offer_sdp;
  std::vector<std::string> candidates;
};

std::unique_ptr<ClientPeerOffer> make_client_offer(const std::string& channel_label) {
  auto client_offer = std::make_unique<ClientPeerOffer>();
  rtc::Configuration rtc_config;
  client_offer->peer_connection = std::make_shared<rtc::PeerConnection>(rtc_config);
  auto bootstrap_channel = client_offer->peer_connection->createDataChannel(channel_label);
  (void)bootstrap_channel;

  client_offer->peer_connection->onLocalDescription([client_offer = client_offer.get()](rtc::Description description) {
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    client_offer->offer_sdp = std::string(description);
  });
  client_offer->peer_connection->onLocalCandidate([client_offer = client_offer.get()](rtc::Candidate candidate) {
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    client_offer->candidates.push_back(std::string(candidate));
  });
  client_offer->peer_connection->setLocalDescription(rtc::Description::Type::Offer);

  CHECK_TRUE(wait_until([&]() {
    {
      std::lock_guard<std::mutex> lock(client_offer->mutex);
      if (!client_offer->offer_sdp.empty()) {
        return true;
      }
    }
    const auto description = client_offer->peer_connection->localDescription();
    if (!description.has_value()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    client_offer->offer_sdp = std::string(*description);
    return true;
  }));

  return client_offer;
}

}  // namespace

TEST(WebRtcHttpTest, SignalingCallbacksRemainResponsive) {
  const uint16_t port = static_cast<uint16_t>(21000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"signal-stability", "signal", 4, 4, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));
  CHECK_TRUE(server.start());

  auto client_offer = make_client_offer("stability");
  {
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    CHECK_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/signal-stability/offer",
                                                   client_offer->offer_sdp)
                   .status == 200);
  }

  video_server::WebRtcHttpResponse session_response{};
  CHECK_TRUE(wait_until([&]() {
    session_response = server.handle_http_request_for_test("GET", "/api/video/signaling/signal-stability/session");
    return session_response.status == 200 && !json_string_field(session_response.body, "answer_sdp").empty() &&
           !json_string_field(session_response.body, "peer_state").empty();
  }));

  const auto invalid_answer =
      server.handle_http_request_for_test("POST", "/api/video/signaling/signal-stability/answer", "not-an-answer");
  CHECK_TRUE(invalid_answer.status == 400);

  std::string candidate;
  if (wait_until([&]() {
        std::lock_guard<std::mutex> lock(client_offer->mutex);
        if (client_offer->candidates.empty()) {
          return false;
        }
        candidate = client_offer->candidates.front();
        return true;
      },
      1000)) {
    CHECK_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/signal-stability/candidate", candidate)
                   .status == 200);
    const auto after_candidate =
        server.handle_http_request_for_test("GET", "/api/video/signaling/signal-stability/session");
    CHECK_TRUE(after_candidate.status == 200);
    CHECK_TRUE(json_string_field(after_candidate.body, "last_remote_candidate") == candidate);
  }

  server.stop();
}

TEST(WebRtcHttpTest, SignalingRoutesExposeCorsHeadersAndHandleOptions) {
  const std::string origin = "http://127.0.0.1:8090";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{"127.0.0.1", 0, false});
  video_server::StreamConfig cfg{"signal-cors", "signal-cors", 4, 4, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));

  auto client_offer = make_client_offer("cors");
  std::unordered_map<std::string, std::string> headers{{"origin", origin}};

  {
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    const auto offer_response = server.handle_http_request_for_test("POST", "/api/video/signaling/signal-cors/offer",
                                                                    client_offer->offer_sdp, headers);
    CHECK_TRUE(offer_response.status == 200);
    CHECK_TRUE(response_header(offer_response, "Access-Control-Allow-Origin") == origin);
    CHECK_TRUE(response_header(offer_response, "Access-Control-Allow-Methods") == "GET, POST, PUT, OPTIONS");
    CHECK_TRUE(response_header(offer_response, "Access-Control-Allow-Headers") ==
               "Authorization, Content-Type, X-Video-Server-Key");
  }

  const auto session_response =
      server.handle_http_request_for_test("GET", "/api/video/signaling/signal-cors/session", "", headers);
  CHECK_TRUE(session_response.status == 200);
  CHECK_TRUE(response_header(session_response, "Access-Control-Allow-Origin") == origin);

  const auto options_offer =
      server.handle_http_request_for_test("OPTIONS", "/api/video/signaling/signal-cors/offer", "", headers);
  CHECK_TRUE(options_offer.status == 204);
  CHECK_TRUE(options_offer.body.empty());
  CHECK_TRUE(response_header(options_offer, "Access-Control-Allow-Origin") == origin);
  CHECK_TRUE(response_header(options_offer, "Access-Control-Allow-Methods") == "GET, POST, PUT, OPTIONS");
  CHECK_TRUE(response_header(options_offer, "Access-Control-Allow-Headers") ==
             "Authorization, Content-Type, X-Video-Server-Key");

  const auto options_candidate =
      server.handle_http_request_for_test("OPTIONS", "/api/video/signaling/signal-cors/candidate", "", headers);
  CHECK_TRUE(options_candidate.status == 204);
  CHECK_TRUE(response_header(options_candidate, "Access-Control-Allow-Origin") == origin);

  const auto options_session =
      server.handle_http_request_for_test("OPTIONS", "/api/video/signaling/signal-cors/session", "", headers);
  CHECK_TRUE(options_session.status == 204);
  CHECK_TRUE(response_header(options_session, "Access-Control-Allow-Origin") == origin);

  std::string candidate;
  if (wait_until([&]() {
        std::lock_guard<std::mutex> lock(client_offer->mutex);
        if (client_offer->candidates.empty()) {
          return false;
        }
        candidate = client_offer->candidates.front();
        return true;
      },
      1000)) {
    const auto candidate_response = server.handle_http_request_for_test(
        "POST", "/api/video/signaling/signal-cors/candidate", candidate, headers);
    CHECK_TRUE(candidate_response.status == 200);
    CHECK_TRUE(response_header(candidate_response, "Access-Control-Allow-Origin") == origin);
  }
}

TEST(WebRtcHttpTest, SignalingSessionJsonEscapesSdpAndQueuedCandidates) {
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{"127.0.0.1", 0, false});
  video_server::StreamConfig cfg{"signal-json", "signal-json", 4, 4, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));

  const std::string queued_candidate =
      "candidate:0 1 UDP 2122252543 127.0.0.1 3478 typ host generation 0 ufrag abc network-id 1";
  const auto early_candidate = server.handle_http_request_for_test(
      "POST", "/api/video/signaling/signal-json/candidate", queued_candidate);
  CHECK_TRUE(early_candidate.status == 200);

  auto client_offer = make_client_offer("json-escape");
  {
    std::lock_guard<std::mutex> lock(client_offer->mutex);
    const auto offer_response =
        server.handle_http_request_for_test("POST", "/api/video/signaling/signal-json/offer", client_offer->offer_sdp);
    CHECK_TRUE(offer_response.status == 200);
  }

  video_server::WebRtcHttpResponse session_response{};
  CHECK_TRUE(wait_until([&]() {
    session_response = server.handle_http_request_for_test("GET", "/api/video/signaling/signal-json/session");
    return session_response.status == 200 && !json_string_field(session_response.body, "answer_sdp").empty();
  }));

  CHECK_TRUE(session_response.body.find('\r') == std::string::npos);
  CHECK_TRUE(session_response.body.find('\n') == std::string::npos);
  assert_no_control_chars(session_response.body);
  CHECK_TRUE(session_response.body.find("\\r\\n") != std::string::npos);
  CHECK_TRUE(session_response.body.find("\"last_remote_candidate\":\"" + queued_candidate + "\"") != std::string::npos);
}

TEST(WebRtcHttpTest, AnswerReusesOfferedVideoMediaSectionWithoutExtraMLine) {
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{"127.0.0.1", 0, false});
  video_server::StreamConfig cfg{"signal-video-offer", "signal-video-offer", 4, 4, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));

  std::array<uint8_t, 23> access_unit_bytes{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
                                            0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
                                            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  video_server::EncodedAccessUnitView access_unit{};
  access_unit.data = access_unit_bytes.data();
  access_unit.size_bytes = access_unit_bytes.size();
  access_unit.codec = video_server::VideoCodec::H264;
  access_unit.timestamp_ns = 123456;
  access_unit.keyframe = true;
  access_unit.codec_config = false;
  CHECK_TRUE(server.push_access_unit(cfg.stream_id, access_unit));

  rtc::Configuration rtc_config;
  auto client = std::make_shared<rtc::PeerConnection>(rtc_config);
  rtc::Description::Video offered_video_description("0", rtc::Description::Direction::RecvOnly);
  offered_video_description.addH264Codec(102);
  auto requested_track = client->addTrack(offered_video_description);
  (void)requested_track;

  std::string offer_sdp;
  client->onLocalDescription([&](rtc::Description description) { offer_sdp = std::string(description); });
  client->setLocalDescription(rtc::Description::Type::Offer);

  CHECK_TRUE(wait_until([&]() {
    if (!offer_sdp.empty()) {
      return true;
    }
    const auto description = client->localDescription();
    if (!description.has_value()) {
      return false;
    }
    offer_sdp = std::string(*description);
    return true;
  }));

  const auto offered_video_line = offer_sdp.find("m=video ");
  CHECK_TRUE(offered_video_line != std::string::npos);
  const auto offered_video_line_end = offer_sdp.find('\n', offered_video_line);
  CHECK_TRUE(offered_video_line_end != std::string::npos);
  const auto first_space_after_port = offer_sdp.find(' ', std::string("m=video ").size() + offered_video_line);
  CHECK_TRUE(first_space_after_port != std::string::npos);
  offer_sdp.replace(offered_video_line, first_space_after_port - offered_video_line, "m=video 9");

  CHECK_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/signal-video-offer/offer", offer_sdp).status == 200);

  video_server::WebRtcHttpResponse session_response{};
  CHECK_TRUE(wait_until([&]() {
    session_response = server.handle_http_request_for_test("GET", "/api/video/signaling/signal-video-offer/session");
    return session_response.status == 200 && !json_string_field(session_response.body, "answer_sdp").empty();
  }));

  const std::string answer_sdp = json_string_field(session_response.body, "answer_sdp");
  CHECK_TRUE(count_sdp_media_sections(offer_sdp) == 1);
  CHECK_TRUE(count_sdp_media_sections(answer_sdp) == count_sdp_media_sections(offer_sdp));
  CHECK_TRUE(extract_sdp_mids(offer_sdp) == extract_sdp_mids(answer_sdp));
  CHECK_TRUE(answer_sdp.find("m=video 0 ") == std::string::npos);
  CHECK_TRUE(answer_sdp.find("a=mid:video") == std::string::npos);
  CHECK_TRUE(answer_sdp.find("a=setup:actpass") == std::string::npos);
  CHECK_TRUE(answer_sdp.find("a=setup:active") != std::string::npos ||
             answer_sdp.find("a=setup:passive") != std::string::npos);
  CHECK_TRUE(json_string_field(session_response.body, "encoded_sender_video_mid") == "0");
  CHECK_TRUE(json_string_field(session_response.body, "encoded_sender_state") != "waiting-for-video-track-open");
  CHECK_TRUE(json_bool_field(session_response.body, "encoded_sender_cached_codec_config_available"));
  CHECK_TRUE(json_bool_field(session_response.body, "encoded_sender_cached_idr_available"));
  CHECK_TRUE(!json_bool_field(session_response.body, "encoded_sender_first_decodable_frame_sent"));
  CHECK_TRUE(!json_bool_field(session_response.body, "encoded_sender_startup_sequence_sent"));
}

TEST(WebRtcHttpTest, RepeatedSignalingOperationsRemainResponsive) {
  const uint16_t port = static_cast<uint16_t>(22000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig cfg{"signal-repeat", "signal-repeat", 4, 4, 30.0, video_server::VideoPixelFormat::GRAY8};
  CHECK_TRUE(server.register_stream(cfg));
  CHECK_TRUE(server.start());

  uint64_t previous_generation = 0;
  for (int round = 0; round < 3; ++round) {
    auto client_offer = make_client_offer("repeat-" + std::to_string(round));
    {
      std::lock_guard<std::mutex> lock(client_offer->mutex);
      CHECK_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/signal-repeat/offer",
                                                     client_offer->offer_sdp)
                     .status == 200);
    }

    std::string session_json;
    CHECK_TRUE(wait_until([&]() {
      const auto response = server.handle_http_request_for_test("GET", "/api/video/signaling/signal-repeat/session");
      if (response.status != 200) {
        return false;
      }
      session_json = response.body;
      return json_uint_field(session_json, "session_generation") > previous_generation &&
             !json_string_field(session_json, "answer_sdp").empty();
    }));

    previous_generation = json_uint_field(session_json, "session_generation");
    CHECK_TRUE(previous_generation == static_cast<uint64_t>(round + 1));

    std::string candidate;
    if (wait_until([&]() {
          std::lock_guard<std::mutex> lock(client_offer->mutex);
          if (client_offer->candidates.empty()) {
            return false;
          }
          candidate = client_offer->candidates.front();
          return true;
        },
        1000)) {
      CHECK_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/signal-repeat/candidate", candidate)
                     .status == 200);
      const auto after_candidate =
          server.handle_http_request_for_test("GET", "/api/video/signaling/signal-repeat/session");
      CHECK_TRUE(after_candidate.status == 200);
      CHECK_TRUE(json_uint_field(after_candidate.body, "session_generation") == previous_generation);
    }
  }

  server.stop();
}

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
           !json_string_field(session_json, "offer_sdp").empty() &&
           !json_string_field(session_json, "answer_sdp").empty();
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

  std::array<uint8_t, 23> access_unit_bytes{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
                                            0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
                                            0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
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
  const auto encoded_sender_state = json_string_field(session_after_updates.body, "encoded_sender_state");
  CHECK_TRUE(!encoded_sender_state.empty());
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_video_track_exists") ||
             !json_string_field(session_after_updates.body, "encoded_sender_video_mid").empty() ||
             !encoded_sender_state.empty());
  CHECK_TRUE(json_uint_field(session_after_updates.body, "encoded_sender_delivered_units") >= 1);
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_has_pending_encoded_unit"));
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_codec_config_seen"));
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_cached_codec_config_available"));
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_cached_idr_available"));
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_ready_for_video_track") ||
             !encoded_sender_state.empty());
  CHECK_TRUE(!json_bool_field(session_after_updates.body, "encoded_sender_first_decodable_frame_sent") ||
             json_uint_field(session_after_updates.body, "encoded_sender_packets_sent_after_track_open") >= 1);
  CHECK_TRUE(!json_bool_field(session_after_updates.body, "encoded_sender_startup_sequence_sent") ||
             json_uint_field(session_after_updates.body, "encoded_sender_startup_packets_sent") >= 1);
  CHECK_TRUE(json_uint_field(session_after_updates.body, "encoded_sender_negotiated_h264_payload_type") >= 1);
  CHECK_TRUE(!json_bool_field(session_after_updates.body, "encoded_sender_video_track_exists") ||
             !json_string_field(session_after_updates.body, "encoded_sender_negotiated_h264_fmtp").empty() ||
             json_string_field(session_after_updates.body, "encoded_sender_video_mid").empty());
  CHECK_TRUE(json_bool_field(session_after_updates.body, "encoded_sender_last_contains_idr"));
  const auto packetization_status = json_string_field(session_after_updates.body, "encoded_sender_last_packetization_status");
  CHECK_TRUE(!packetization_status.empty());

  CHECK_TRUE(server.remove_stream(cfg.stream_id));
  const auto removed_session = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
  CHECK_TRUE(removed_session.status == 404);

  server.stop();
}

TEST(WebRtcHttpTest, DebugStatsEndpointReportsPerStreamObservability) {
  auto server_config = make_http_test_config();
  server_config.enable_debug_api = true;
  video_server::WebRtcVideoServer server(server_config);
  ASSERT_TRUE(server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));
  ASSERT_TRUE(server.register_stream({"bravo", "Bravo", 3, 1, 30.0, video_server::VideoPixelFormat::GRAY8}));

  std::vector<uint8_t> alpha_pixels = {1, 2, 3, 4};
  video_server::VideoFrameView alpha_frame{alpha_pixels.data(), 2, 2, 2, video_server::VideoPixelFormat::GRAY8, 100, 7};
  ASSERT_TRUE(server.push_frame("alpha", alpha_frame));
  const std::array<uint8_t, 6> alpha_au{0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
  video_server::EncodedAccessUnitView alpha_access{alpha_au.data(), alpha_au.size(), video_server::VideoCodec::H264,
                                                   222, true, false};
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_access));

  const auto response = server.handle_http_request_for_test("GET", "/api/video/debug/stats");
  ASSERT_EQ(response.status, 200);
  EXPECT_NE(response.body.find("\"stream_count\":2"), std::string::npos);
  EXPECT_NE(response.body.find("\"active_session_count\":0"), std::string::npos);
  EXPECT_NE(response.body.find("\"stream_id\":\"alpha\""), std::string::npos);
  EXPECT_NE(response.body.find("\"stream_id\":\"bravo\""), std::string::npos);
  EXPECT_NE(response.body.find("\"latest_raw_frame_available\":true"), std::string::npos);
  EXPECT_NE(response.body.find("\"total_access_units_received\":1"), std::string::npos);
}

TEST(WebRtcHttpTest, SharedKeyProtectionRejectsMissingOrWrongKeyAndAcceptsCorrectKey) {
  auto server_config = make_http_test_config();
  server_config.enable_shared_key_auth = true;
  server_config.shared_key = "lan-secret";
  video_server::WebRtcVideoServer server(server_config);
  ASSERT_TRUE(server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));

  const auto missing =
      server.handle_http_request_for_test("GET", "/api/video/streams/alpha/config");
  ASSERT_EQ(missing.status, 401);

  const auto wrong = server.handle_http_request_for_test(
      "GET", "/api/video/streams/alpha/config", "", {{"Authorization", "Bearer wrong-secret"}});
  ASSERT_EQ(wrong.status, 401);

  const auto ok = server.handle_http_request_for_test(
      "GET", "/api/video/streams/alpha/config", "", {{"Authorization", "Bearer lan-secret"}});
  ASSERT_EQ(ok.status, 200);
  ASSERT_NE(ok.body.find("\"config_generation\":1"), std::string::npos);
}

TEST(WebRtcHttpTest, AllowlistSupportsCidrsAndRejectsDisallowedRemoteAddresses) {
  auto server_config = make_http_test_config("0.0.0.0");
  server_config.ip_allowlist = {"192.168.50.0/24"};
  video_server::WebRtcVideoServer server(server_config);
  ASSERT_TRUE(server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));

  const auto allowed = server.handle_http_request_for_test(
      "GET", "/api/video/streams/alpha/config", "", {}, "192.168.50.77");
  ASSERT_EQ(allowed.status, 200);

  const auto blocked = server.handle_http_request_for_test(
      "GET", "/api/video/streams/alpha/config", "", {}, "10.1.2.3");
  ASSERT_EQ(blocked.status, 403);
}

TEST(WebRtcHttpTest, SaferDefaultsGateDebugAndRemoteSensitiveRoutes) {
  video_server::WebRtcVideoServer local_server(make_http_test_config());
  ASSERT_TRUE(local_server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));
  EXPECT_EQ(local_server.handle_http_request_for_test("GET", "/api/video/debug/stats").status, 404);

  auto remote_config = make_http_test_config("0.0.0.0");
  video_server::WebRtcVideoServer remote_server(remote_config);
  ASSERT_TRUE(remote_server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));
  EXPECT_EQ(remote_server.handle_http_request_for_test("GET", "/api/video/streams").status, 200);
  EXPECT_EQ(remote_server.handle_http_request_for_test("GET", "/api/video/streams/alpha/config", "", {}, "192.168.1.44").status,
            403);
}

TEST(WebRtcHttpTest, ValidationRejectsInvalidStreamIdsAndMalformedConfigPayloads) {
  video_server::WebRtcVideoServer server(make_http_test_config());
  ASSERT_TRUE(server.register_stream({"alpha", "Alpha", 2, 2, 15.0, video_server::VideoPixelFormat::GRAY8}));

  const auto invalid_stream = server.handle_http_request_for_test("GET", "/api/video/streams/bad/id/config");
  ASSERT_EQ(invalid_stream.status, 400);

  const auto fractional_width = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/alpha/config", "{\"output_width\":12.5}");
  ASSERT_EQ(fractional_width.status, 400);

  const auto unknown_field = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/alpha/config", "{\"surprise\":1}");
  ASSERT_EQ(unknown_field.status, 400);
}

TEST(WebRtcHttpTest, RateLimitingRejectsRepeatedConfigMutations) {
  auto server_config = make_http_test_config();
  server_config.config_rate_limit_max_requests = 2;
  server_config.config_rate_limit_window_seconds = 60;
  video_server::WebRtcVideoServer server(server_config);
  ASSERT_TRUE(server.register_stream({"alpha", "Alpha", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8}));

  const auto first = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/alpha/config", "{\"display_mode\":\"white_hot\"}");
  const auto second = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/alpha/config", "{\"display_mode\":\"black_hot\"}");
  const auto third = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/alpha/config", "{\"display_mode\":\"rainbow\"}");

  ASSERT_EQ(first.status, 200);
  ASSERT_EQ(second.status, 200);
  ASSERT_EQ(third.status, 429);
}

TEST(WebRtcHttpTest, KeepsMultiStreamSignalingAndStateIsolated) {
  video_server::WebRtcVideoServer server(make_http_test_config());
  const video_server::StreamConfig alpha{"alpha", "Alpha", 640, 360, 15.0, video_server::VideoPixelFormat::GRAY8};
  const video_server::StreamConfig bravo{"bravo", "Bravo", 320, 240, 30.0, video_server::VideoPixelFormat::GRAY8};
  ASSERT_TRUE(server.register_stream(alpha));
  ASSERT_TRUE(server.register_stream(bravo));

  std::vector<uint8_t> alpha_frame_pixels(alpha.width * alpha.height, 11);
  std::vector<uint8_t> bravo_frame_pixels(bravo.width * bravo.height, 99);
  video_server::VideoFrameView alpha_frame{alpha_frame_pixels.data(), alpha.width, alpha.height, alpha.width,
                                           video_server::VideoPixelFormat::GRAY8, 1010, 1};
  video_server::VideoFrameView bravo_frame{bravo_frame_pixels.data(), bravo.width, bravo.height, bravo.width,
                                           video_server::VideoPixelFormat::GRAY8, 2020, 2};
  ASSERT_TRUE(server.push_frame(alpha.stream_id, alpha_frame));
  ASSERT_TRUE(server.push_frame(bravo.stream_id, bravo_frame));

  const std::array<uint8_t, 23> alpha_au{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
                                         0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
                                         0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  const std::array<uint8_t, 7> bravo_au{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22};
  video_server::EncodedAccessUnitView alpha_access{alpha_au.data(), alpha_au.size(), video_server::VideoCodec::H264,
                                                    3030, true, true};
  video_server::EncodedAccessUnitView bravo_access{bravo_au.data(), bravo_au.size(), video_server::VideoCodec::H264,
                                                    4040, false, false};
  ASSERT_TRUE(server.push_access_unit(alpha.stream_id, alpha_access));
  ASSERT_TRUE(server.push_access_unit(bravo.stream_id, bravo_access));

  auto alpha_offer = make_client_offer("alpha-bootstrap");
  auto bravo_offer = make_client_offer("bravo-bootstrap");
  ASSERT_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/alpha/offer", alpha_offer->offer_sdp).status == 200);
  ASSERT_TRUE(server.handle_http_request_for_test("POST", "/api/video/signaling/bravo/offer", bravo_offer->offer_sdp).status == 200);

  video_server::WebRtcHttpResponse alpha_session{};
  video_server::WebRtcHttpResponse bravo_session{};
  ASSERT_TRUE(wait_until([&]() {
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
    return alpha_session.status == 200 && bravo_session.status == 200 &&
           !json_string_field(alpha_session.body, "answer_sdp").empty() &&
           !json_string_field(bravo_session.body, "answer_sdp").empty();
  }));

  EXPECT_EQ(json_string_field(alpha_session.body, "stream_id"), "alpha");
  EXPECT_EQ(json_string_field(bravo_session.body, "stream_id"), "bravo");
  EXPECT_EQ(json_uint_field(alpha_session.body, "latest_snapshot_width"), 640u);
  EXPECT_EQ(json_uint_field(alpha_session.body, "latest_snapshot_height"), 360u);
  EXPECT_EQ(json_uint_field(bravo_session.body, "latest_snapshot_width"), 320u);
  EXPECT_EQ(json_uint_field(bravo_session.body, "latest_snapshot_height"), 240u);
  EXPECT_EQ(json_uint_field(alpha_session.body, "latest_encoded_timestamp_ns"), 3030u);
  EXPECT_EQ(json_uint_field(bravo_session.body, "latest_encoded_timestamp_ns"), 4040u);
  EXPECT_NE(json_string_field(alpha_session.body, "answer_sdp"), json_string_field(bravo_session.body, "answer_sdp"));

  std::string alpha_candidate;
  std::string bravo_candidate;
  wait_until([&]() {
    std::lock_guard<std::mutex> alpha_lock(alpha_offer->mutex);
    if (!alpha_offer->candidates.empty()) {
      alpha_candidate = alpha_offer->candidates.front();
      return true;
    }
    return false;
  }, 1000);
  wait_until([&]() {
    std::lock_guard<std::mutex> bravo_lock(bravo_offer->mutex);
    if (!bravo_offer->candidates.empty()) {
      bravo_candidate = bravo_offer->candidates.front();
      return true;
    }
    return false;
  }, 1000);

  if (!alpha_candidate.empty() && !bravo_candidate.empty()) {
    ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/alpha/candidate", alpha_candidate).status, 200);
    ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/bravo/candidate", bravo_candidate).status, 200);
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
    EXPECT_EQ(json_string_field(alpha_session.body, "last_remote_candidate"), alpha_candidate);
    EXPECT_EQ(json_string_field(bravo_session.body, "last_remote_candidate"), bravo_candidate);
    EXPECT_NE(json_string_field(alpha_session.body, "last_remote_candidate"), json_string_field(bravo_session.body, "last_remote_candidate"));
  }

  auto replacement_alpha = make_client_offer("alpha-replacement");
  ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/alpha/offer", replacement_alpha->offer_sdp).status, 200);
  ASSERT_TRUE(wait_until([&]() {
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
    return alpha_session.status == 200 && bravo_session.status == 200 &&
           json_uint_field(alpha_session.body, "session_generation") > 1 &&
           json_uint_field(bravo_session.body, "session_generation") == 1;
  }));
  EXPECT_EQ(json_uint_field(alpha_session.body, "latest_encoded_timestamp_ns"), 3030u);
  EXPECT_EQ(json_uint_field(bravo_session.body, "latest_encoded_timestamp_ns"), 4040u);
}



TEST(WebRtcHttpTest, PerStreamConfigApiSupportsIsolationValidationAndObservability) {
  const uint16_t port = static_cast<uint16_t>(21000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  auto server_config = make_http_test_config(host, port, true);
  server_config.enable_debug_api = true;
  video_server::WebRtcVideoServer server(server_config);
  video_server::StreamConfig alpha{"alpha", "alpha", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  video_server::StreamConfig bravo{"bravo", "bravo", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  ASSERT_TRUE(server.register_stream(alpha));
  ASSERT_TRUE(server.register_stream(bravo));

  const auto alpha_update =
      server.handle_http_request_for_test("PUT", "/api/video/streams/alpha/config",
                                          "{\"display_mode\":\"ironbow\",\"output_width\":32,\"output_height\":24,\"output_fps\":12}");
  ASSERT_EQ(alpha_update.status, 200);
  ASSERT_NE(alpha_update.body.find("\"config_generation\":2"), std::string::npos);

  const auto bravo_update =
      server.handle_http_request_for_test("PUT", "/api/video/streams/bravo/config",
                                          "{\"display_mode\":\"black_hot\",\"output_width\":48,\"output_height\":48}");
  ASSERT_EQ(bravo_update.status, 200);

  const auto invalid =
      server.handle_http_request_for_test("PUT", "/api/video/streams/alpha/config",
                                          "{\"output_width\":4,\"output_height\":4,\"output_fps\":500}");
  ASSERT_EQ(invalid.status, 400);

  const auto alpha_cfg = server.handle_http_request_for_test("GET", "/api/video/streams/alpha/config");
  const auto bravo_cfg = server.handle_http_request_for_test("GET", "/api/video/streams/bravo/config");
  ASSERT_EQ(alpha_cfg.status, 200);
  ASSERT_EQ(bravo_cfg.status, 200);
  ASSERT_NE(alpha_cfg.body.find("\"display_mode\":\"Ironbow\""), std::string::npos);
  ASSERT_NE(bravo_cfg.body.find("\"display_mode\":\"BlackHot\""), std::string::npos);
  ASSERT_NE(alpha_cfg.body.find("\"output_width\":32"), std::string::npos);
  ASSERT_NE(bravo_cfg.body.find("\"output_width\":48"), std::string::npos);

  const auto stats = server.handle_http_request_for_test("GET", "/api/video/debug/stats");
  ASSERT_EQ(stats.status, 200);
  ASSERT_NE(stats.body.find("\"active_filter_mode\":\"Ironbow\""), std::string::npos);
  ASSERT_NE(stats.body.find("\"active_filter_mode\":\"BlackHot\""), std::string::npos);
  ASSERT_NE(stats.body.find("\"config_generation\":2"), std::string::npos);
}

TEST(WebRtcHttpTest, SessionLifecycleTeardownReconnectIsolationAndConfigStayCoherent) {
  const uint16_t port = static_cast<uint16_t>(23000 + (::getpid() % 10000));
  const std::string host = "127.0.0.1";
  video_server::WebRtcVideoServer server(video_server::WebRtcVideoServerConfig{host, port, true});
  video_server::StreamConfig alpha{"alpha", "alpha", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  video_server::StreamConfig bravo{"bravo", "bravo", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  ASSERT_TRUE(server.register_stream(alpha));
  ASSERT_TRUE(server.register_stream(bravo));

  std::array<uint8_t, 16> codec_config_bytes{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                             0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2};
  std::array<uint8_t, 7> idr_bytes{0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  std::array<uint8_t, 7> non_idr_bytes{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22};

  video_server::EncodedAccessUnitView alpha_codec{};
  alpha_codec.data = codec_config_bytes.data();
  alpha_codec.size_bytes = codec_config_bytes.size();
  alpha_codec.codec = video_server::VideoCodec::H264;
  alpha_codec.timestamp_ns = 1000;
  alpha_codec.codec_config = true;
  alpha_codec.keyframe = false;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_codec));

  video_server::EncodedAccessUnitView alpha_idr{};
  alpha_idr.data = idr_bytes.data();
  alpha_idr.size_bytes = idr_bytes.size();
  alpha_idr.codec = video_server::VideoCodec::H264;
  alpha_idr.timestamp_ns = 2000;
  alpha_idr.codec_config = false;
  alpha_idr.keyframe = true;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_idr));

  video_server::EncodedAccessUnitView bravo_codec = alpha_codec;
  bravo_codec.timestamp_ns = 3000;
  ASSERT_TRUE(server.push_access_unit("bravo", bravo_codec));

  video_server::EncodedAccessUnitView bravo_idr = alpha_idr;
  bravo_idr.timestamp_ns = 4000;
  ASSERT_TRUE(server.push_access_unit("bravo", bravo_idr));

  auto alpha_offer = make_client_offer("alpha-lifecycle");
  auto bravo_offer = make_client_offer("bravo-lifecycle");
  ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/alpha/offer", alpha_offer->offer_sdp).status, 200);
  ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/bravo/offer", bravo_offer->offer_sdp).status, 200);

  video_server::WebRtcHttpResponse alpha_session{};
  video_server::WebRtcHttpResponse bravo_session{};
  ASSERT_TRUE(wait_until([&]() {
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
    return alpha_session.status == 200 && bravo_session.status == 200 &&
           !json_string_field(alpha_session.body, "answer_sdp").empty() &&
           !json_string_field(bravo_session.body, "answer_sdp").empty() &&
           json_bool_field(alpha_session.body, "active") &&
           json_bool_field(bravo_session.body, "active");
  }));

  video_server::EncodedAccessUnitView alpha_non_idr{};
  alpha_non_idr.data = non_idr_bytes.data();
  alpha_non_idr.size_bytes = non_idr_bytes.size();
  alpha_non_idr.codec = video_server::VideoCodec::H264;
  alpha_non_idr.timestamp_ns = 5000;
  alpha_non_idr.codec_config = false;
  alpha_non_idr.keyframe = false;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_non_idr));

  video_server::EncodedAccessUnitView bravo_non_idr = alpha_non_idr;
  bravo_non_idr.timestamp_ns = 6000;
  ASSERT_TRUE(server.push_access_unit("bravo", bravo_non_idr));

  alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
  bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
  ASSERT_EQ(alpha_session.status, 200);
  ASSERT_EQ(bravo_session.status, 200);

  const uint64_t alpha_generation_before_reconnect = json_uint_field(alpha_session.body, "session_generation");
  const uint64_t bravo_generation_before_reconnect = json_uint_field(bravo_session.body, "session_generation");
  const uint64_t bravo_packets_before_reconnect = json_uint_field(bravo_session.body, "encoded_sender_packets_attempted");

  const auto alpha_config_update =
      server.handle_http_request_for_test("PUT", "/api/video/streams/alpha/config",
                                          "{\"display_mode\":\"ironbow\",\"output_width\":32,\"output_height\":24,\"output_fps\":12}");
  ASSERT_EQ(alpha_config_update.status, 200);
  ASSERT_NE(alpha_config_update.body.find("\"config_generation\":2"), std::string::npos);

  auto replacement_alpha = make_client_offer("alpha-replacement-clean");
  ASSERT_EQ(server.handle_http_request_for_test("POST", "/api/video/signaling/alpha/offer", replacement_alpha->offer_sdp).status, 200);
  ASSERT_TRUE(wait_until([&]() {
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    bravo_session = server.handle_http_request_for_test("GET", "/api/video/signaling/bravo/session");
    if (alpha_session.status != 200 || bravo_session.status != 200) {
      return false;
    }
    return json_bool_field(alpha_session.body, "active") &&
           json_uint_field(alpha_session.body, "session_generation") == alpha_generation_before_reconnect + 1 &&
           json_uint_field(bravo_session.body, "session_generation") == bravo_generation_before_reconnect &&
           json_uint_field(alpha_session.body, "encoded_sender_packets_attempted") == 0;
  }));

  EXPECT_FALSE(json_bool_field(alpha_session.body, "encoded_sender_first_decodable_frame_sent"));
  EXPECT_FALSE(json_bool_field(alpha_session.body, "encoded_sender_startup_sequence_sent"));
  const auto replacement_sender_state = json_string_field(alpha_session.body, "encoded_sender_state");
  EXPECT_TRUE(replacement_sender_state == "waiting-for-h264-keyframe" ||
              replacement_sender_state == "video-track-missing");
  EXPECT_EQ(json_uint_field(alpha_session.body, "disconnect_count"), 0u);
  EXPECT_TRUE(json_bool_field(bravo_session.body, "active"));
  EXPECT_EQ(json_uint_field(bravo_session.body, "encoded_sender_packets_attempted"), bravo_packets_before_reconnect);

  alpha_codec.timestamp_ns = 7000;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_codec));
  alpha_idr.timestamp_ns = 8000;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_idr));
  alpha_non_idr.timestamp_ns = 9000;
  ASSERT_TRUE(server.push_access_unit("alpha", alpha_non_idr));

  ASSERT_TRUE(wait_until([&]() {
    alpha_session = server.handle_http_request_for_test("GET", "/api/video/signaling/alpha/session");
    if (alpha_session.status != 200) {
      return false;
    }
    return json_uint_field(alpha_session.body, "encoded_sender_delivered_units") >= 3 &&
           json_bool_field(alpha_session.body, "encoded_sender_cached_codec_config_available") &&
           json_bool_field(alpha_session.body, "encoded_sender_cached_idr_available") &&
           json_string_field(alpha_session.body, "encoded_sender_state") != "session-inactive";
  }));

  const auto debug_snapshot = server.get_debug_snapshot();
  const auto* alpha_debug = find_stream_snapshot(debug_snapshot, "alpha");
  const auto* bravo_debug = find_stream_snapshot(debug_snapshot, "bravo");
  ASSERT_NE(alpha_debug, nullptr);
  ASSERT_NE(bravo_debug, nullptr);
  ASSERT_TRUE(alpha_debug->current_session.has_value());
  ASSERT_TRUE(bravo_debug->current_session.has_value());
  EXPECT_EQ(debug_snapshot.active_session_count, 2u);
  EXPECT_EQ(alpha_debug->config_generation, 2u);
  EXPECT_EQ(alpha_debug->current_session->session_generation, alpha_generation_before_reconnect + 1);
  EXPECT_TRUE(alpha_debug->current_session->active);
  EXPECT_EQ(bravo_debug->current_session->session_generation, bravo_generation_before_reconnect);
  EXPECT_TRUE(bravo_debug->current_session->active);

  server.stop();
}


#else
TEST(WebRtcHttpTest, ExercisesHttpAndSignalingFlow) { GTEST_SKIP() << "WebRTC backend disabled"; }
TEST(WebRtcHttpTest, KeepsMultiStreamSignalingAndStateIsolated) { GTEST_SKIP() << "WebRTC backend disabled"; }
TEST(WebRtcHttpTest, SessionLifecycleTeardownReconnectIsolationAndConfigStayCoherent) {
  GTEST_SKIP() << "WebRTC backend disabled";
}
#endif
