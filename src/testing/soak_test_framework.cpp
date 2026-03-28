#include "soak_test_framework.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include <rtc/description.hpp>

#include <spdlog/spdlog.h>

#include "../transforms/display_transform.h"
#include "../webrtc/logging_utils.h"

namespace video_server {
namespace soak {
namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(5),
                std::chrono::milliseconds sleep_interval = std::chrono::milliseconds(25)) {
  const auto deadline = SteadyClock::now() + timeout;
  while (SteadyClock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(sleep_interval);
  }
  return predicate();
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  static constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (c < 0x20) {
          escaped += "\\u00";
          escaped.push_back(kHex[(c >> 4) & 0x0f]);
          escaped.push_back(kHex[c & 0x0f]);
        } else {
          escaped.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return escaped;
}

std::string http_request(const std::string& method, const std::string& host, const std::string& path,
                         const std::string& body, const std::string& content_type) {
  std::ostringstream out;
  out << method << " " << path << " HTTP/1.1\r\n";
  out << "Host: " << host << "\r\n";
  out << "Connection: close\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") {
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
  }
  out << "\r\n";
  out << body;
  return out.str();
}

std::string http_body(const std::string& response) {
  const auto header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return {};
  }
  return response.substr(header_end + 4);
}

int http_status(const std::string& response) {
  const auto line_end = response.find("\r\n");
  if (line_end == std::string::npos) {
    return 0;
  }
  std::istringstream in(response.substr(0, line_end));
  std::string version;
  int status = 0;
  in >> version >> status;
  return status;
}

bool json_bool_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto start = json.find(needle);
  if (start == std::string::npos) {
    return false;
  }
  const size_t pos = start + needle.size();
  if (json.compare(pos, 4, "true") == 0) {
    return true;
  }
  return false;
}

uint64_t json_uint_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto start = json.find(needle);
  if (start == std::string::npos) {
    return 0;
  }
  size_t pos = start + needle.size();
  size_t end = pos;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') {
    ++end;
  }
  return end > pos ? std::stoull(json.substr(pos, end - pos)) : 0;
}

std::string json_string_field(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto start = json.find(needle);
  if (start == std::string::npos) {
    return {};
  }
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
          value.push_back(c);
          break;
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
  return {};
}

std::optional<uint16_t> parse_port(const char* value) {
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(parsed);
}

std::optional<size_t> parse_size_t_value(const char* value) {
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<size_t>(parsed);
}

std::vector<VideoDisplayMode> display_modes() {
  return {
      VideoDisplayMode::Passthrough,
      VideoDisplayMode::Ironbow,
      VideoDisplayMode::BlackHot,
      VideoDisplayMode::Rainbow,
      VideoDisplayMode::Arctic,
  };
}

StreamOutputConfig config_variant_for(const StreamSpec& spec, size_t variant_index) {
  const auto modes = display_modes();
  const uint32_t base_width = spec.width;
  const uint32_t base_height = spec.height;
  const std::array<double, 4> fps_values{spec.fps, std::max(5.0, spec.fps / 2.0), std::min(60.0, spec.fps + 5.0), std::max(10.0, spec.fps - 10.0)};

  const size_t size_variant = variant_index % 3;
  StreamOutputConfig config;
  config.display_mode = modes[variant_index % modes.size()];
  if (size_variant == 0) {
    config.output_width = base_width;
    config.output_height = base_height;
  } else if (size_variant == 1) {
    config.output_width = std::max<uint32_t>(16, ((base_width / 2u) / 16u) * 16u);
    config.output_height = std::max<uint32_t>(16, ((base_height / 2u) / 16u) * 16u);
  } else {
    config.output_width = std::min<uint32_t>(1920, std::max<uint32_t>(16, ((base_width * 3u / 2u) / 16u) * 16u));
    config.output_height = std::min<uint32_t>(1080, std::max<uint32_t>(16, ((base_height * 3u / 2u) / 16u) * 16u));
  }
  config.output_fps = fps_values[variant_index % fps_values.size()];
  return config;
}

std::string stream_config_json(const StreamOutputConfig& config) {
  std::ostringstream out;
  out << "{\"display_mode\":\"" << to_string(config.display_mode) << "\","
      << "\"output_width\":" << config.output_width << ','
      << "\"output_height\":" << config.output_height << ','
      << "\"output_fps\":" << std::fixed << std::setprecision(2) << config.output_fps << "}";
  return out.str();
}

bool stream_config_matches(const StreamDebugSnapshot& snapshot, const StreamOutputConfig& config) {
  return snapshot.active_filter_mode == to_string(config.display_mode) &&
         snapshot.active_output_width == config.output_width &&
         snapshot.active_output_height == config.output_height &&
         std::fabs(snapshot.active_output_fps - config.output_fps) < 0.01;
}

bool known_sender_state(const std::string& state) {
  static const std::set<std::string> known_states{
      "waiting-for-encoded-input",
      "video-track-missing",
      "waiting-for-video-track-open",
      "waiting-for-h264-codec-config",
      "waiting-for-h264-keyframe",
      "waiting-for-decoded-startup-idr",
      "sending-h264-rtp",
      "session-inactive",
      "rejected-non-h264-access-unit",
  };
  return known_states.find(state) != known_states.end();
}

bool open_output_file(const std::string& path, std::ofstream& out) {
  out.open(path, std::ios::out | std::ios::trunc);
  return out.is_open();
}

}  // namespace

std::vector<StreamSpec> default_stream_specs() {
  return {
      StreamSpec{"alpha", "Alpha soak grayscale 640x360", 640, 360, 30.0, VideoPixelFormat::GRAY8},
      StreamSpec{"bravo", "Bravo soak RGB 1280x720", 1280, 720, 24.0, VideoPixelFormat::RGB24},
      StreamSpec{"charlie", "Charlie soak RGB 320x240", 320, 240, 15.0, VideoPixelFormat::RGB24},
  };
}

std::optional<std::chrono::milliseconds> parse_duration_to_millis(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double numeric = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || numeric <= 0.0) {
    return std::nullopt;
  }
  std::string suffix = end ? std::string(end) : std::string();
  double multiplier = 1000.0;
  if (suffix.empty() || suffix == "s") {
    multiplier = 1000.0;
  } else if (suffix == "m") {
    multiplier = 60.0 * 1000.0;
  } else if (suffix == "h") {
    multiplier = 60.0 * 60.0 * 1000.0;
  } else if (suffix == "ms") {
    multiplier = 1.0;
  } else {
    return std::nullopt;
  }
  return std::chrono::milliseconds(static_cast<int64_t>(numeric * multiplier));
}

SessionHttpSnapshot parse_session_http_snapshot(const std::string& json) {
  SessionHttpSnapshot snapshot;
  snapshot.session_generation = json_uint_field(json, "session_generation");
  snapshot.disconnect_count = json_uint_field(json, "disconnect_count");
  snapshot.encoded_sender_packets_attempted = json_uint_field(json, "encoded_sender_packets_attempted");
  snapshot.encoded_sender_packets_sent_after_track_open = json_uint_field(json, "encoded_sender_packets_sent_after_track_open");
  snapshot.active = json_bool_field(json, "active");
  snapshot.sending_active = json_bool_field(json, "sending_active");
  snapshot.encoded_sender_video_track_open = json_bool_field(json, "encoded_sender_video_track_open");
  snapshot.answer_sdp = json_string_field(json, "answer_sdp");
  snapshot.last_local_candidate = json_string_field(json, "last_local_candidate");
  snapshot.encoded_sender_video_mid = json_string_field(json, "encoded_sender_video_mid");
  snapshot.encoded_sender_state = json_string_field(json, "encoded_sender_state");
  snapshot.encoded_sender_last_packetization_status = json_string_field(json, "encoded_sender_last_packetization_status");
  return snapshot;
}

void MetricsCollector::record(const MetricSample& sample) { samples_.push_back(sample); }

RunSummary MetricsCollector::build_summary(double total_duration_seconds,
                                          const std::vector<FailureRecord>& failures) const {
  RunSummary summary;
  summary.total_duration_seconds = total_duration_seconds;
  summary.success = failures.empty();
  summary.failures = failures;

  std::unordered_map<std::string, StreamRunSummary> by_stream;
  for (const auto& sample : samples_) {
    auto& stream = by_stream[sample.stream_id];
    stream.stream_id = sample.stream_id;
    stream.samples += 1;
    stream.final_session_generation = sample.session_generation;
    stream.final_disconnect_count = sample.disconnect_count;
    stream.final_packets_sent = sample.packets_sent;
    stream.final_packets_attempted = sample.packets_attempted;
    stream.final_send_failures = sample.send_failures;
    stream.final_packetization_failures = sample.packetization_failures;
    stream.final_total_frames_dropped = sample.total_frames_dropped;
  }
  for (const auto& failure : failures) {
    if (failure.stream_id.empty()) {
      continue;
    }
    by_stream[failure.stream_id].stream_id = failure.stream_id;
  }
  for (const auto& [stream_id, stream] : by_stream) {
    (void)stream_id;
    summary.streams.push_back(stream);
  }
  std::sort(summary.streams.begin(), summary.streams.end(),
            [](const StreamRunSummary& lhs, const StreamRunSummary& rhs) { return lhs.stream_id < rhs.stream_id; });
  return summary;
}

bool MetricsCollector::write_json_report(const std::string& path,
                                         double total_duration_seconds,
                                         const std::vector<FailureRecord>& failures) const {
  std::ofstream out;
  if (!open_output_file(path, out)) {
    return false;
  }

  const auto summary = build_summary(total_duration_seconds, failures);
  out << "{";
  out << "\"success\":" << (summary.success ? "true" : "false") << ',';
  out << "\"total_duration_seconds\":" << std::fixed << std::setprecision(3) << total_duration_seconds << ',';
  out << "\"streams\":[";
  for (size_t i = 0; i < summary.streams.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    const auto& stream = summary.streams[i];
    out << "{\"stream_id\":\"" << json_escape(stream.stream_id) << "\","
        << "\"samples\":" << stream.samples << ','
        << "\"final_session_generation\":" << stream.final_session_generation << ','
        << "\"final_disconnect_count\":" << stream.final_disconnect_count << ','
        << "\"final_packets_sent\":" << stream.final_packets_sent << ','
        << "\"final_packets_attempted\":" << stream.final_packets_attempted << ','
        << "\"final_send_failures\":" << stream.final_send_failures << ','
        << "\"final_packetization_failures\":" << stream.final_packetization_failures << ','
        << "\"final_total_frames_dropped\":" << stream.final_total_frames_dropped << "}";
  }
  out << "],\"failures\":[";
  for (size_t i = 0; i < failures.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    const auto& failure = failures[i];
    out << "{\"elapsed_seconds\":" << std::fixed << std::setprecision(3) << failure.elapsed_seconds << ','
        << "\"stream_id\":\"" << json_escape(failure.stream_id) << "\","
        << "\"code\":\"" << json_escape(failure.code) << "\","
        << "\"message\":\"" << json_escape(failure.message) << "\"}";
  }
  out << "],\"samples\":[";
  for (size_t i = 0; i < samples_.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    const auto& sample = samples_[i];
    out << "{\"elapsed_seconds\":" << std::fixed << std::setprecision(3) << sample.elapsed_seconds << ','
        << "\"stream_id\":\"" << json_escape(sample.stream_id) << "\","
        << "\"session_generation\":" << sample.session_generation << ','
        << "\"disconnect_count\":" << sample.disconnect_count << ','
        << "\"packets_sent\":" << sample.packets_sent << ','
        << "\"packets_attempted\":" << sample.packets_attempted << ','
        << "\"send_failures\":" << sample.send_failures << ','
        << "\"packetization_failures\":" << sample.packetization_failures << ','
        << "\"delivered_units\":" << sample.delivered_units << ','
        << "\"failed_units\":" << sample.failed_units << ','
        << "\"total_frames_dropped\":" << sample.total_frames_dropped << ','
        << "\"config_generation\":" << sample.config_generation << ','
        << "\"session_active\":" << (sample.session_active ? "true" : "false") << ','
        << "\"sending_active\":" << (sample.sending_active ? "true" : "false") << ','
        << "\"sender_state\":\"" << json_escape(sample.sender_state) << "\","
        << "\"last_packetization_status\":\"" << json_escape(sample.last_packetization_status) << "\"}";
  }
  out << "]}";
  return true;
}

bool MetricsCollector::write_csv_report(const std::string& path) const {
  std::ofstream out;
  if (!open_output_file(path, out)) {
    return false;
  }
  out << "elapsed_seconds,stream_id,session_generation,disconnect_count,packets_sent,packets_attempted,send_failures,"
         "packetization_failures,delivered_units,failed_units,total_frames_dropped,config_generation,session_active,"
         "sending_active,sender_state,last_packetization_status\n";
  for (const auto& sample : samples_) {
    out << std::fixed << std::setprecision(3) << sample.elapsed_seconds << ','
        << sample.stream_id << ','
        << sample.session_generation << ','
        << sample.disconnect_count << ','
        << sample.packets_sent << ','
        << sample.packets_attempted << ','
        << sample.send_failures << ','
        << sample.packetization_failures << ','
        << sample.delivered_units << ','
        << sample.failed_units << ','
        << sample.total_frames_dropped << ','
        << sample.config_generation << ','
        << (sample.session_active ? "true" : "false") << ','
        << (sample.sending_active ? "true" : "false") << ','
        << '"' << json_escape(sample.sender_state) << '"' << ','
        << '"' << json_escape(sample.last_packetization_status) << '"' << '\n';
  }
  return true;
}

std::vector<FailureRecord> FailureDetector::evaluate_stream(const RunnerOptions& options,
                                                            const StreamDebugSnapshot& debug_snapshot,
                                                            const SessionHttpSnapshot& session_http_snapshot,
                                                            const SteadyClock::time_point& now,
                                                            double elapsed_seconds,
                                                            StreamEvaluationState& state) {
  std::vector<FailureRecord> failures;
  const auto add_failure = [&](const std::string& code, const std::string& message) {
    failures.push_back(FailureRecord{elapsed_seconds, debug_snapshot.stream_id, code, message});
  };

  const auto current_generation = debug_snapshot.current_session ? debug_snapshot.current_session->session_generation : 0;
  const auto current_disconnect_count = debug_snapshot.current_session ? debug_snapshot.current_session->disconnect_count : 0;
  const auto current_packets_sent = debug_snapshot.current_session ? debug_snapshot.current_session->counters.packets_sent_after_track_open : 0;
  const bool session_active = debug_snapshot.current_session ? debug_snapshot.current_session->active : false;
  const bool sending_active = debug_snapshot.current_session ? debug_snapshot.current_session->sending_active : false;
  const std::string sender_state = debug_snapshot.current_session ? debug_snapshot.current_session->sender_state : std::string("no-session");

  if (debug_snapshot.current_session && state.last_session_generation.has_value() &&
      current_generation > *state.last_session_generation && !state.expecting_reconnect) {
    add_failure("unexpected_session_reset", "session_generation increased without an explicit reconnect");
  }

  if (!state.expecting_reconnect && state.last_disconnect_count.has_value() &&
      current_disconnect_count > *state.last_disconnect_count) {
    add_failure("unexpected_disconnect", "disconnect_count increased without an explicit reconnect");
  }

  if (state.expecting_reconnect && state.reconnect_disconnect_phase && !session_active) {
    state.saw_expected_disconnect = true;
    state.reconnect_disconnect_phase = false;
  }

  if (state.expecting_reconnect && state.last_session_generation.has_value() &&
      current_generation > *state.last_session_generation) {
    state.expecting_reconnect = false;
    state.reconnect_disconnect_phase = false;
    state.saw_expected_disconnect = false;
    state.reconnect_resume_at.reset();
    state.reconnect_deadline.reset();
    state.first_sending_active_at.reset();
    state.last_packet_progress_at = now;
    state.last_packets_sent = current_packets_sent;
    state.reconnect_events.push_back(now);
    while (!state.reconnect_events.empty() && now - state.reconnect_events.front() > options.rapid_reconnect_window) {
      state.reconnect_events.pop_front();
    }
    if (state.reconnect_events.size() > options.max_rapid_reconnects) {
      add_failure("rapid_reconnect_loop", "reconnect count exceeded the configured rapid reconnect threshold");
    }
  }

  if (state.expecting_reconnect && !state.saw_expected_disconnect &&
      state.reconnect_deadline.has_value() && now > *state.reconnect_deadline) {
    add_failure("ghost_session_after_disconnect", "session stayed active after an explicit disconnect");
    state.reconnect_deadline.reset();
  }

  if (debug_snapshot.current_session && !known_sender_state(sender_state)) {
    add_failure("invalid_sender_state", "sender entered an unknown state: " + sender_state);
  }

  if (debug_snapshot.current_session && session_active && sending_active) {
    if (!state.first_sending_active_at.has_value()) {
      state.first_sending_active_at = now;
    }
    if (!state.last_packets_sent.has_value() || current_packets_sent > *state.last_packets_sent) {
      state.last_packet_progress_at = now;
    }
    if (current_packets_sent == 0 &&
        state.first_sending_active_at.has_value() &&
        now - *state.first_sending_active_at > options.startup_grace_period) {
      add_failure("sending_without_packets", "sending_active stayed true without any packets being emitted");
    }
    if (state.last_packets_sent.has_value() && current_packets_sent == *state.last_packets_sent &&
        state.last_packet_progress_at.has_value() &&
        now - *state.last_packet_progress_at > options.progress_stall_threshold) {
      add_failure("packet_progress_stalled", "packets stopped increasing while the session remained active");
    }
    if (sender_state == "session-inactive") {
      add_failure("invalid_sender_state", "active session reported sender_state=session-inactive");
    }
  } else {
    state.first_sending_active_at.reset();
    state.last_packet_progress_at = now;
  }

  if (state.pending_config.has_value()) {
    const auto& pending = *state.pending_config;
    if (debug_snapshot.config_generation >= pending.expected_generation &&
        stream_config_matches(debug_snapshot, pending.expected_config)) {
      state.pending_config.reset();
    } else if (now > pending.deadline) {
      add_failure("config_apply_timeout", "config churn did not apply within the expected timeout");
      state.pending_config.reset();
    }
  }

  if (debug_snapshot.current_session &&
      debug_snapshot.current_session->sending_active != session_http_snapshot.sending_active) {
    add_failure("session_endpoint_mismatch", "HTTP session poll disagreed with the debug snapshot sending state");
  }

  if (debug_snapshot.current_session &&
      debug_snapshot.current_session->session_generation != session_http_snapshot.session_generation) {
    add_failure("session_endpoint_mismatch", "HTTP session poll disagreed with the debug snapshot generation");
  }

  state.last_session_generation = current_generation;
  state.last_disconnect_count = current_disconnect_count;
  state.last_packets_sent = current_packets_sent;
  return failures;
}

HttpJsonClient::HttpJsonClient(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

HttpJsonClient::Response HttpJsonClient::get(const std::string& path) const {
  return send("GET", path, "", "application/json");
}

HttpJsonClient::Response HttpJsonClient::post(const std::string& path, const std::string& body, const std::string& content_type) const {
  return send("POST", path, body, content_type);
}

HttpJsonClient::Response HttpJsonClient::put(const std::string& path, const std::string& body, const std::string& content_type) const {
  return send("PUT", path, body, content_type);
}

HttpJsonClient::Response HttpJsonClient::send(const std::string& method,
                                              const std::string& path,
                                              const std::string& body,
                                              const std::string& content_type) const {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("failed to create HTTP socket");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    throw std::runtime_error("failed to parse HTTP host");
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string error = "failed to connect HTTP socket: " + std::string(std::strerror(errno));
    ::close(fd);
    throw std::runtime_error(error);
  }

  const std::string request = http_request(method, host_, path, body, content_type);
  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      throw std::runtime_error("failed to write HTTP request");
    }
    sent += static_cast<size_t>(n);
  }

  std::string response;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error("failed to read HTTP response");
    }
    if (n == 0) {
      break;
    }
    response.append(buffer.data(), static_cast<size_t>(n));
  }
  ::close(fd);
  return Response{http_status(response), http_body(response)};
}

WebRtcClientSession::WebRtcClientSession(std::string stream_id) : stream_id_(std::move(stream_id)) {}

WebRtcClientSession::~WebRtcClientSession() { disconnect(); }

bool WebRtcClientSession::connect(const HttpJsonClient& http_client, std::string* error_message) {
  disconnect();

  callback_state_ = std::make_shared<CallbackState>();
  rtc::Configuration configuration;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(configuration);
  rtc::Description::Video offered_video_description("0", rtc::Description::Direction::RecvOnly);
  offered_video_description.addH264Codec(102);
  auto requested_track = peer_connection_->addTrack(offered_video_description);
  (void)requested_track;

  peer_connection_->onLocalDescription([state = callback_state_](rtc::Description description) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->offer_sdp = std::string(description);
  });
  peer_connection_->onLocalCandidate([state = callback_state_](rtc::Candidate candidate) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->local_candidates.push_back(std::string(candidate));
  });
  peer_connection_->setLocalDescription(rtc::Description::Type::Offer);

  if (!wait_until([this]() {
        {
          std::lock_guard<std::mutex> lock(callback_state_->mutex);
          if (!callback_state_->offer_sdp.empty()) {
            return true;
          }
        }
        const auto description = peer_connection_->localDescription();
        if (!description.has_value()) {
          return false;
        }
        std::lock_guard<std::mutex> lock(callback_state_->mutex);
        callback_state_->offer_sdp = std::string(*description);
        return true;
      })) {
    if (error_message) {
      *error_message = "timed out waiting for local offer SDP";
    }
    disconnect();
    return false;
  }

  std::string offer_sdp;
  {
    std::lock_guard<std::mutex> lock(callback_state_->mutex);
    offer_sdp = callback_state_->offer_sdp;
  }

  const auto response = http_client.post("/api/video/signaling/" + stream_id_ + "/offer", offer_sdp, "application/sdp");
  if (response.status != 200) {
    if (error_message) {
      *error_message = "offer POST failed with HTTP " + std::to_string(response.status) + ": " + response.body;
    }
    disconnect();
    return false;
  }

  remote_description_applied_ = false;
  last_applied_backend_candidate_.clear();
  return true;
}

void WebRtcClientSession::disconnect() {
  if (peer_connection_) {
    peer_connection_.reset();
  }
  callback_state_.reset();
  remote_description_applied_ = false;
  last_applied_backend_candidate_.clear();
}

void WebRtcClientSession::poll(const HttpJsonClient& http_client, std::string* error_message) {
  if (!peer_connection_ || !callback_state_) {
    return;
  }

  const auto response = http_client.get("/api/video/signaling/" + stream_id_ + "/session");
  if (response.status != 200) {
    if (error_message) {
      *error_message = "session GET failed with HTTP " + std::to_string(response.status) + ": " + response.body;
    }
    return;
  }

  const auto session = parse_session_http_snapshot(response.body);
  if (!remote_description_applied_ && !session.answer_sdp.empty()) {
    peer_connection_->setRemoteDescription(rtc::Description(session.answer_sdp, "answer"));
    remote_description_applied_ = true;
  }

  if (remote_description_applied_ && !session.last_local_candidate.empty() &&
      session.last_local_candidate != last_applied_backend_candidate_) {
    rtc::Candidate candidate(session.last_local_candidate, session.encoded_sender_video_mid.empty() ? "0" : session.encoded_sender_video_mid);
    peer_connection_->addRemoteCandidate(candidate);
    last_applied_backend_candidate_ = session.last_local_candidate;
  }

  std::vector<std::string> candidates_to_send;
  {
    std::lock_guard<std::mutex> lock(callback_state_->mutex);
    for (const auto& candidate : callback_state_->local_candidates) {
      if (candidate != callback_state_->last_posted_candidate) {
        candidates_to_send.push_back(candidate);
      }
    }
  }
  for (const auto& candidate : candidates_to_send) {
    const auto post_response = http_client.post("/api/video/signaling/" + stream_id_ + "/candidate", candidate, "application/trickle-ice-sdpfrag");
    if (post_response.status != 200) {
      if (error_message) {
        *error_message = "candidate POST failed with HTTP " + std::to_string(post_response.status) + ": " + post_response.body;
      }
      return;
    }
    std::lock_guard<std::mutex> lock(callback_state_->mutex);
    callback_state_->last_posted_candidate = candidate;
  }
}

SyntheticServerHarness::RunningStream::RunningStream(const StreamSpec& spec)
    : config{spec.stream_id, spec.label, spec.width, spec.height, spec.fps, spec.pixel_format},
      generator(config) {}

RawVideoPipelineConfig SyntheticServerHarness::make_pipeline_config(const RunningStream& stream,
                                                                   const StreamOutputConfig& output_config,
                                                                   uint32_t width,
                                                                   uint32_t height) {
  RawVideoPipelineConfig config;
  config.input_width = width;
  config.input_height = height;
  config.input_pixel_format = VideoPixelFormat::RGB24;
  config.input_fps = stream.config.nominal_fps;
  if (output_config.output_fps > 0.0) {
    config.output_fps = output_config.output_fps;
  }
  return config;
}

SyntheticServerHarness::SyntheticServerHarness(RunnerOptions options)
    : options_(std::move(options)),
      server_([this]() {
        WebRtcVideoServerConfig config;
        config.http_host = options_.host;
        config.http_port = options_.port;
        config.enable_http_api = true;
        config.enable_debug_api = options_.enable_debug_api;
        return config;
      }()) {}

SyntheticServerHarness::~SyntheticServerHarness() { stop(); }

bool SyntheticServerHarness::start(std::string* error_message) {
  ensure_default_logging_config();
  if (options_.streams.empty()) {
    if (error_message) {
      *error_message = "no streams configured";
    }
    return false;
  }
  for (const auto& spec : options_.streams) {
    streams_.push_back(std::make_unique<RunningStream>(spec));
    if (!server_.register_stream(streams_.back()->config)) {
      if (error_message) {
        *error_message = "failed to register stream " + spec.stream_id;
      }
      return false;
    }
  }
  if (!server_.start()) {
    if (error_message) {
      *error_message = "failed to start embedded WebRTC server";
    }
    return false;
  }

  stop_requested_ = false;
  background_error_.reset();
  for (auto& stream : streams_) {
    stream->frame_thread = std::thread([this, stream = stream.get()]() {
      try {
        const auto frame_interval = std::chrono::duration<double>(1.0 / stream->config.nominal_fps);
        while (!stop_requested_.load()) {
          const auto raw_frame = stream->generator.next_frame();
          if (!server_.push_frame(stream->config.stream_id, raw_frame)) {
            throw std::runtime_error("failed to push synthetic frame");
          }

          const auto output_config = server_.get_stream_output_config(stream->config.stream_id);
          if (!output_config.has_value()) {
            throw std::runtime_error("missing output config");
          }

          RgbImage transformed;
          if (!apply_display_transform(raw_frame, *output_config, transformed)) {
            throw std::runtime_error("display transform failed");
          }

          if (!stream->pipeline || stream->pipeline_config_generation != output_config->config_generation) {
            if (stream->pipeline) {
              stream->pipeline->stop();
              stream->pipeline.reset();
            }
            stream->pipeline_config = make_pipeline_config(*stream, *output_config, transformed.width, transformed.height);
            stream->pipeline = make_raw_to_h264_pipeline_for_server(stream->config.stream_id, stream->pipeline_config, server_);
            if (!stream->pipeline->start(&stream->pipeline_error)) {
              throw std::runtime_error("failed to start raw-to-h264 pipeline: " + stream->pipeline_error);
            }
            stream->pipeline_config_generation = output_config->config_generation;
          }

          const VideoFrameView transformed_frame{
              transformed.rgb.data(), transformed.width, transformed.height, transformed.width * 3u,
              VideoPixelFormat::RGB24, raw_frame.timestamp_ns, raw_frame.frame_id};
          if (!stream->pipeline->push_frame(transformed_frame, &stream->pipeline_error)) {
            throw std::runtime_error("failed to push transformed frame: " + stream->pipeline_error);
          }

          std::this_thread::sleep_for(frame_interval);
        }
      } catch (const std::exception& ex) {
        {
          std::lock_guard<std::mutex> lock(background_error_mutex_);
          background_error_ = stream->config.stream_id + ": " + ex.what();
        }
        stop_requested_ = true;
      }
    });
  }
  return true;
}

void SyntheticServerHarness::stop() {
  stop_requested_ = true;
  for (auto& stream : streams_) {
    if (stream->frame_thread.joinable()) {
      stream->frame_thread.join();
    }
  }
  for (auto& stream : streams_) {
    if (stream->pipeline) {
      stream->pipeline->stop();
      stream->pipeline.reset();
    }
  }
  server_.stop();
}

std::optional<std::string> SyntheticServerHarness::background_error() const {
  std::lock_guard<std::mutex> lock(background_error_mutex_);
  return background_error_;
}

SoakRunner::SoakRunner(RunnerOptions options)
    : options_(std::move(options)),
      harness_(options_),
      http_client_(options_.host, options_.port) {
  for (const auto& spec : options_.streams) {
    clients_[spec.stream_id] = std::make_unique<WebRtcClientSession>(spec.stream_id);
  }
}

MetricSample SoakRunner::make_sample(double elapsed_seconds, const StreamDebugSnapshot& snapshot) const {
  MetricSample sample;
  sample.elapsed_seconds = elapsed_seconds;
  sample.stream_id = snapshot.stream_id;
  sample.config_generation = snapshot.config_generation;
  sample.total_frames_dropped = snapshot.total_frames_dropped;
  if (snapshot.current_session.has_value()) {
    const auto& session = *snapshot.current_session;
    sample.session_generation = session.session_generation;
    sample.disconnect_count = session.disconnect_count;
    sample.packets_sent = session.counters.packets_sent_after_track_open;
    sample.packets_attempted = session.counters.packets_attempted;
    sample.send_failures = session.counters.send_failures;
    sample.packetization_failures = session.counters.packetization_failures;
    sample.delivered_units = session.counters.delivered_units;
    sample.failed_units = session.counters.failed_units;
    sample.session_active = session.active;
    sample.sending_active = session.sending_active;
    sample.sender_state = session.sender_state;
    sample.last_packetization_status = session.last_packetization_status;
  }
  return sample;
}

SessionHttpSnapshot SoakRunner::fetch_session_snapshot(const std::string& stream_id) const {
  const auto response = http_client_.get("/api/video/signaling/" + stream_id + "/session");
  if (response.status == 404) {
    return SessionHttpSnapshot{};
  }
  if (response.status != 200) {
    throw std::runtime_error("session endpoint failed for " + stream_id + " with HTTP " + std::to_string(response.status) + ": " + response.body);
  }
  return parse_session_http_snapshot(response.body);
}

void SoakRunner::record_failures(const std::vector<FailureRecord>& failures) {
  failures_.insert(failures_.end(), failures.begin(), failures.end());
}

void SoakRunner::connect_all_clients() {
  for (const auto& spec : options_.streams) {
    std::string error_message;
    if (!clients_.at(spec.stream_id)->connect(http_client_, &error_message)) {
      throw std::runtime_error("failed to connect client for " + spec.stream_id + ": " + error_message);
    }
  }
}

void SoakRunner::disconnect_all_clients() {
  for (auto& [stream_id, client] : clients_) {
    (void)stream_id;
    client->disconnect();
  }
}

void SoakRunner::poll_clients() {
  for (const auto& spec : options_.streams) {
    std::string error_message;
    clients_.at(spec.stream_id)->poll(http_client_, &error_message);
    if (!error_message.empty()) {
      throw std::runtime_error("client poll failed for " + spec.stream_id + ": " + error_message);
    }
  }
}

void SoakRunner::trigger_reconnect(StreamSpec& spec) {
  auto& state = evaluation_states_[spec.stream_id];
  state.expecting_reconnect = true;
  state.reconnect_disconnect_phase = true;
  state.saw_expected_disconnect = false;
  state.reconnect_resume_at = SteadyClock::now() + std::chrono::milliseconds(500);
  state.reconnect_deadline = SteadyClock::now() + options_.reconnect_grace_period;
  clients_.at(spec.stream_id)->disconnect();
}

void SoakRunner::trigger_config_update(StreamSpec& spec) {
  const auto current_config = harness_.server().get_stream_output_config(spec.stream_id);
  if (!current_config.has_value()) {
    throw std::runtime_error("failed to load current output config for " + spec.stream_id);
  }
  size_t& index = config_variant_index_[spec.stream_id];
  ++index;
  StreamOutputConfig next = config_variant_for(spec, index);
  const auto response = http_client_.put("/api/video/streams/" + spec.stream_id + "/config", stream_config_json(next));
  if (response.status != 200) {
    throw std::runtime_error("config update failed for " + spec.stream_id + " with HTTP " + std::to_string(response.status) + ": " + response.body);
  }
  auto& state = evaluation_states_[spec.stream_id];
  state.pending_config = PendingConfigExpectation{
      current_config->config_generation,
      current_config->config_generation + 1,
      next,
      SteadyClock::now() + options_.config_apply_timeout,
  };
}

RunSummary SoakRunner::run() {
  const auto run_started_at = SteadyClock::now();
  std::string startup_error;
  if (!harness_.start(&startup_error)) {
    throw std::runtime_error(startup_error);
  }

  try {
    connect_all_clients();

    auto next_poll = run_started_at;
    auto next_summary = run_started_at + options_.summary_interval;
    auto next_reconnect = run_started_at + options_.reconnect_interval;
    auto next_config = run_started_at + options_.config_interval;
    size_t reconnect_index = 0;
    size_t config_index = 0;

    while (SteadyClock::now() - run_started_at < options_.duration && !has_failures()) {
      const auto now = SteadyClock::now();
      if (now >= next_poll) {
        if (const auto thread_error = harness_.background_error(); thread_error.has_value()) {
          throw std::runtime_error("background stream worker failed: " + *thread_error);
        }
        poll_clients();
        const auto debug_probe = http_client_.get("/api/video/debug/stats");
        if (debug_probe.status != 200) {
          throw std::runtime_error("debug stats endpoint failed with HTTP " + std::to_string(debug_probe.status) + ": " + debug_probe.body);
        }

        const auto snapshot = harness_.server().get_debug_snapshot();
        const double elapsed_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - run_started_at).count();
        for (const auto& stream : snapshot.streams) {
          const auto session_http = fetch_session_snapshot(stream.stream_id);
          metrics_.record(make_sample(elapsed_seconds, stream));
          auto failures = FailureDetector::evaluate_stream(options_, stream, session_http, now, elapsed_seconds,
                                                           evaluation_states_[stream.stream_id]);
          record_failures(failures);
        }
        next_poll = now + options_.poll_interval;
      }

      for (auto& spec : options_.streams) {
        auto& state = evaluation_states_[spec.stream_id];
        if (state.expecting_reconnect && state.reconnect_resume_at.has_value() && now >= *state.reconnect_resume_at) {
          std::string error_message;
          if (!clients_.at(spec.stream_id)->connect(http_client_, &error_message)) {
            throw std::runtime_error("failed to reconnect " + spec.stream_id + ": " + error_message);
          }
          state.reconnect_resume_at.reset();
        }
      }

      if (options_.enable_lifecycle_churn && !options_.streams.empty() && now >= next_reconnect) {
        trigger_reconnect(options_.streams[reconnect_index % options_.streams.size()]);
        reconnect_index += 1;
        next_reconnect = now + options_.reconnect_interval;
      }

      if (options_.enable_config_churn && !options_.streams.empty() && now >= next_config) {
        trigger_config_update(options_.streams[config_index % options_.streams.size()]);
        config_index += 1;
        next_config = now + options_.config_interval;
      }

      if (now >= next_summary) {
        const auto summary = harness_.server().get_debug_snapshot();
        spdlog::info("[soak] elapsed={:.1f}s streams={} active_sessions={} failures={}",
                     std::chrono::duration_cast<std::chrono::duration<double>>(now - run_started_at).count(),
                     summary.stream_count,
                     summary.active_session_count,
                     failures_.size());
        for (const auto& stream : summary.streams) {
          if (!stream.current_session.has_value()) {
            spdlog::info("[soak] stream={} cfg_gen={} no-session dropped_frames={}", stream.stream_id,
                         stream.config_generation, stream.total_frames_dropped);
            continue;
          }
          const auto& session = *stream.current_session;
          spdlog::info("[soak] stream={} gen={} cfg_gen={} state={} active={} sending={} packets={} attempted={} disconnects={} dropped_frames={}",
                       stream.stream_id,
                       session.session_generation,
                       stream.config_generation,
                       session.sender_state,
                       session.active,
                       session.sending_active,
                       session.counters.packets_sent_after_track_open,
                       session.counters.packets_attempted,
                       session.disconnect_count,
                       stream.total_frames_dropped);
        }
        next_summary = now + options_.summary_interval;
      }

      std::this_thread::sleep_for(100ms);
    }
  } catch (...) {
    disconnect_all_clients();
    harness_.stop();
    throw;
  }

  disconnect_all_clients();
  harness_.stop();

  const double total_duration_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(SteadyClock::now() - run_started_at).count();
  if (options_.write_json_report) {
    metrics_.write_json_report(options_.json_report_path, total_duration_seconds, failures_);
  }
  if (options_.write_csv_report) {
    metrics_.write_csv_report(options_.csv_report_path);
  }
  return metrics_.build_summary(total_duration_seconds, failures_);
}

std::optional<RunnerOptions> parse_runner_options(int argc, char** argv, std::string* error_message) {
  RunnerOptions options;
  options.streams = default_stream_specs();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        if (error_message) {
          *error_message = std::string("missing value for ") + name;
        }
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--host") {
      const char* value = require_value("--host");
      if (!value) return std::nullopt;
      options.host = value;
    } else if (arg == "--port") {
      const char* value = require_value("--port");
      if (!value) return std::nullopt;
      const auto parsed = parse_port(value);
      if (!parsed.has_value()) {
        if (error_message) {
          *error_message = "invalid --port value";
        }
        return std::nullopt;
      }
      options.port = *parsed;
    } else if (arg == "--duration") {
      const char* value = require_value("--duration");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --duration value";
        }
        return std::nullopt;
      }
      options.duration = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--poll-interval") {
      const char* value = require_value("--poll-interval");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --poll-interval value";
        }
        return std::nullopt;
      }
      options.poll_interval = *duration;
    } else if (arg == "--summary-interval") {
      const char* value = require_value("--summary-interval");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --summary-interval value";
        }
        return std::nullopt;
      }
      options.summary_interval = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--reconnect-interval") {
      const char* value = require_value("--reconnect-interval");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --reconnect-interval value";
        }
        return std::nullopt;
      }
      options.reconnect_interval = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--config-interval") {
      const char* value = require_value("--config-interval");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --config-interval value";
        }
        return std::nullopt;
      }
      options.config_interval = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--stall-threshold") {
      const char* value = require_value("--stall-threshold");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --stall-threshold value";
        }
        return std::nullopt;
      }
      options.progress_stall_threshold = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--startup-grace") {
      const char* value = require_value("--startup-grace");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --startup-grace value";
        }
        return std::nullopt;
      }
      options.startup_grace_period = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--rapid-reconnect-window") {
      const char* value = require_value("--rapid-reconnect-window");
      if (!value) return std::nullopt;
      const auto duration = parse_duration_to_millis(value);
      if (!duration.has_value()) {
        if (error_message) {
          *error_message = "invalid --rapid-reconnect-window value";
        }
        return std::nullopt;
      }
      options.rapid_reconnect_window = std::chrono::duration_cast<std::chrono::seconds>(*duration);
    } else if (arg == "--max-rapid-reconnects") {
      const char* value = require_value("--max-rapid-reconnects");
      if (!value) return std::nullopt;
      const auto parsed = parse_size_t_value(value);
      if (!parsed.has_value()) {
        if (error_message) {
          *error_message = "invalid --max-rapid-reconnects value";
        }
        return std::nullopt;
      }
      options.max_rapid_reconnects = *parsed;
    } else if (arg == "--stream") {
      const char* value = require_value("--stream");
      if (!value) return std::nullopt;
      std::stringstream input(value);
      std::string part;
      std::vector<std::string> parts;
      while (std::getline(input, part, ':')) {
        parts.push_back(part);
      }
      if (parts.size() < 4 || parts.size() > 5) {
        if (error_message) {
          *error_message = "invalid --stream value; expected id:width:height:fps[:label]";
        }
        return std::nullopt;
      }
      StreamSpec spec;
      spec.stream_id = parts[0];
      spec.label = parts.size() == 5 ? parts[4] : parts[0];
      spec.width = static_cast<uint32_t>(std::stoul(parts[1]));
      spec.height = static_cast<uint32_t>(std::stoul(parts[2]));
      spec.fps = std::stod(parts[3]);
      options.streams.push_back(spec);
    } else if (arg == "--clear-default-streams") {
      options.streams.clear();
    } else if (arg == "--disable-lifecycle-churn") {
      options.enable_lifecycle_churn = false;
    } else if (arg == "--disable-config-churn") {
      options.enable_config_churn = false;
    } else if (arg == "--json-report") {
      const char* value = require_value("--json-report");
      if (!value) return std::nullopt;
      options.write_json_report = true;
      options.json_report_path = value;
    } else if (arg == "--csv-report") {
      const char* value = require_value("--csv-report");
      if (!value) return std::nullopt;
      options.write_csv_report = true;
      options.csv_report_path = value;
    } else if (arg == "--help" || arg == "-h") {
      if (error_message) {
        *error_message =
            "Usage: video_server_soak_runner [--duration 10m] [--poll-interval 1s] [--summary-interval 5s] "
            "[--reconnect-interval 25s] [--config-interval 12s] [--stream id:width:height:fps[:label]] "
            "[--clear-default-streams] [--json-report PATH] [--csv-report PATH]";
      }
      return std::nullopt;
    } else {
      if (error_message) {
        *error_message = "unknown argument: " + arg;
      }
      return std::nullopt;
    }
  }

  if (options.port == 0) {
    options.port = static_cast<uint16_t>(25000 + (::getpid() % 10000));
  }
  if (options.streams.empty()) {
    if (error_message) {
      *error_message = "at least one stream must be configured";
    }
    return std::nullopt;
  }
  return options;
}

}  // namespace soak
}  // namespace video_server
