#include "video_server/webrtc_video_server.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>

#include <spdlog/spdlog.h>

#include "../core/video_server_core.h"
#include "frame_http_encoder.h"
#include "http_api_server.h"
#include "signaling_server.h"
#include "video_server/video_types.h"

namespace video_server {
namespace {

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  static constexpr char kHex[] = "0123456789abcdef";
  for (unsigned char c : value) {
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

std::string bool_to_json(bool value) { return value ? "true" : "false"; }

struct JsonValue {
  enum class Type { String, Bool, Number } type{Type::String};
  std::string string_value;
  bool bool_value{false};
  double number_value{0.0};
};

bool skip_ws(const std::string& in, size_t& pos) {
  while (pos < in.size() && std::isspace(static_cast<unsigned char>(in[pos])) != 0) {
    ++pos;
  }
  return pos < in.size();
}

bool parse_json_string_token(const std::string& in, size_t& pos, std::string& out) {
  if (pos >= in.size() || in[pos] != '"') {
    return false;
  }
  ++pos;
  out.clear();
  while (pos < in.size()) {
    const char c = in[pos++];
    if (c == '"') {
      return true;
    }
    if (c == '\\') {
      if (pos >= in.size()) {
        return false;
      }
      out.push_back(in[pos++]);
    } else {
      out.push_back(c);
    }
  }
  return false;
}

bool parse_json_bool_token(const std::string& in, size_t& pos, bool& out) {
  if (in.compare(pos, 4, "true") == 0) {
    out = true;
    pos += 4;
    return true;
  }
  if (in.compare(pos, 5, "false") == 0) {
    out = false;
    pos += 5;
    return true;
  }
  return false;
}

bool parse_json_number_token(const std::string& in, size_t& pos, double& out) {
  const size_t start = pos;
  if (pos < in.size() && (in[pos] == '-' || in[pos] == '+')) {
    ++pos;
  }
  bool any = false;
  while (pos < in.size() && std::isdigit(static_cast<unsigned char>(in[pos])) != 0) {
    any = true;
    ++pos;
  }
  if (pos < in.size() && in[pos] == '.') {
    ++pos;
    while (pos < in.size() && std::isdigit(static_cast<unsigned char>(in[pos])) != 0) {
      any = true;
      ++pos;
    }
  }
  if (!any) {
    return false;
  }

  const std::string token = in.substr(start, pos - start);
  char* end = nullptr;
  out = std::strtod(token.c_str(), &end);
  return end != nullptr && *end == '\0';
}

bool parse_flat_json_object(const std::string& body, std::unordered_map<std::string, JsonValue>& out) {
  out.clear();
  size_t pos = 0;
  if (!skip_ws(body, pos) || body[pos] != '{') {
    return false;
  }
  ++pos;

  if (!skip_ws(body, pos)) {
    return false;
  }
  if (body[pos] == '}') {
    ++pos;
    skip_ws(body, pos);
    return pos == body.size();
  }

  while (pos < body.size()) {
    skip_ws(body, pos);
    std::string key;
    if (!parse_json_string_token(body, pos, key)) {
      return false;
    }

    if (!skip_ws(body, pos) || body[pos] != ':') {
      return false;
    }
    ++pos;
    if (!skip_ws(body, pos)) {
      return false;
    }

    JsonValue value;
    if (body[pos] == '"') {
      value.type = JsonValue::Type::String;
      if (!parse_json_string_token(body, pos, value.string_value)) {
        return false;
      }
    } else if (body[pos] == 't' || body[pos] == 'f') {
      value.type = JsonValue::Type::Bool;
      if (!parse_json_bool_token(body, pos, value.bool_value)) {
        return false;
      }
    } else {
      value.type = JsonValue::Type::Number;
      if (!parse_json_number_token(body, pos, value.number_value)) {
        return false;
      }
    }

    out[key] = value;

    if (!skip_ws(body, pos)) {
      return false;
    }
    if (body[pos] == '}') {
      ++pos;
      skip_ws(body, pos);
      return pos == body.size();
    }
    if (body[pos] != ',') {
      return false;
    }
    ++pos;
  }

  return false;
}

HttpResponse json_error(int status, const std::string& message) {
  std::ostringstream out;
  out << "{\"error\":\"" << json_escape(message) << "\"}";
  return HttpResponse{status, out.str(), "application/json"};
}

HttpResponse json_error(int status, const char* message) { return json_error(status, std::string(message)); }

std::string lowercase_ascii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string trim_ascii(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::optional<std::string> find_header_value(const HttpRequest& request, const std::string& key) {
  const std::string lower_key = lowercase_ascii(key);
  for (const auto& [header_key, header_value] : request.headers) {
    if (lowercase_ascii(header_key) == lower_key) {
      return header_value;
    }
  }
  return std::nullopt;
}

bool is_loopback_host(const std::string& host) {
  const std::string lower = lowercase_ascii(host);
  return lower.empty() || lower == "127.0.0.1" || lower == "::1" || lower == "localhost";
}

std::string extract_origin_host(const std::string& origin) {
  const auto scheme_pos = origin.find("://");
  if (scheme_pos == std::string::npos) {
    return "";
  }
  size_t host_start = scheme_pos + 3;
  size_t host_end = origin.find('/', host_start);
  std::string authority = origin.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  if (authority.empty()) {
    return "";
  }
  if (authority.front() == '[') {
    const auto bracket_end = authority.find(']');
    if (bracket_end == std::string::npos) {
      return "";
    }
    return authority.substr(1, bracket_end - 1);
  }
  const auto colon = authority.find(':');
  return colon == std::string::npos ? authority : authority.substr(0, colon);
}

bool is_loopback_origin(const std::string& origin) {
  const std::string host = extract_origin_host(origin);
  return !host.empty() && is_loopback_host(host);
}

bool is_valid_stream_id(const std::string& stream_id) {
  if (stream_id.empty() || stream_id.size() > 64) {
    return false;
  }
  return std::all_of(stream_id.begin(), stream_id.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
  });
}

bool is_finite_number(double value) { return std::isfinite(value); }

bool json_number_is_non_negative_integer(const JsonValue& value) {
  return value.type == JsonValue::Type::Number && is_finite_number(value.number_value) && value.number_value >= 0.0 &&
         std::floor(value.number_value) == value.number_value;
}

enum class RouteGroup {
  None,
  StreamsList,
  StreamInfo,
  StreamFrame,
  RuntimeConfig,
  Signaling,
  Debug,
};

struct RoutePolicy {
  RouteGroup group{RouteGroup::None};
  std::string stream_id;
  bool valid{false};
  bool debug_route{false};
  bool runtime_config_route{false};
  bool sensitive{false};
  bool mutating{false};
  bool rate_limited{false};
  std::string rate_limit_bucket;
};

RoutePolicy classify_route(const HttpRequest& request) {
  RoutePolicy policy;

  if (request.method == "GET" && request.path == "/api/video/streams") {
    policy.group = RouteGroup::StreamsList;
    policy.valid = true;
    return policy;
  }
  if (request.method == "GET" && request.path == "/api/video/debug/stats") {
    policy.group = RouteGroup::Debug;
    policy.valid = true;
    policy.debug_route = true;
    policy.sensitive = true;
    policy.rate_limited = true;
    policy.rate_limit_bucket = "debug";
    return policy;
  }
  if (request.path.rfind("/api/video/streams/", 0) == 0) {
    const std::string tail = request.path.substr(std::string("/api/video/streams/").size());
    const auto output_pos = tail.find("/output");
    const auto config_pos = tail.find("/config");
    const auto frame_pos = tail.find("/frame");
    const bool is_frame_request = frame_pos != std::string::npos && (frame_pos + 6) == tail.size();
    const auto config_segment_pos = output_pos != std::string::npos ? output_pos : config_pos;
    if (request.method == "GET" && config_segment_pos == std::string::npos && !is_frame_request) {
      policy.group = RouteGroup::StreamInfo;
      policy.stream_id = tail;
      policy.valid = true;
      return policy;
    }
    if (request.method == "GET" && is_frame_request) {
      policy.group = RouteGroup::StreamFrame;
      policy.stream_id = tail.substr(0, frame_pos);
      policy.valid = true;
      policy.sensitive = true;
      return policy;
    }
    if ((request.method == "GET" || request.method == "PUT") && config_segment_pos != std::string::npos) {
      policy.group = RouteGroup::RuntimeConfig;
      policy.stream_id = tail.substr(0, config_segment_pos);
      policy.valid = true;
      policy.runtime_config_route = true;
      policy.sensitive = true;
      policy.mutating = request.method == "PUT";
      policy.rate_limited = request.method == "PUT";
      policy.rate_limit_bucket = "config";
      return policy;
    }
  }
  if (request.path.rfind("/api/video/signaling/", 0) == 0) {
    const std::string tail = request.path.substr(std::string("/api/video/signaling/").size());
    const auto slash = tail.find('/');
    policy.group = RouteGroup::Signaling;
    policy.stream_id = slash == std::string::npos ? tail : tail.substr(0, slash);
    policy.valid = true;
    policy.sensitive = true;
    policy.rate_limited = request.method == "POST";
    policy.rate_limit_bucket = "signaling";
    return policy;
  }

  return policy;
}

struct IpSubnet {
  int family{AF_UNSPEC};
  std::array<uint8_t, 16> bytes{};
  uint8_t prefix_length{0};
};

std::optional<IpSubnet> parse_allowlist_entry(const std::string& entry) {
  const std::string trimmed = trim_ascii(entry);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  const auto slash = trimmed.find('/');
  const std::string address_part = slash == std::string::npos ? trimmed : trimmed.substr(0, slash);
  const std::string prefix_part = slash == std::string::npos ? "" : trimmed.substr(slash + 1);

  IpSubnet subnet;
  try {
    if (::inet_pton(AF_INET, address_part.c_str(), subnet.bytes.data()) == 1) {
      subnet.family = AF_INET;
      subnet.prefix_length = prefix_part.empty() ? 32U : static_cast<uint8_t>(std::stoi(prefix_part));
      if (subnet.prefix_length > 32U) {
        return std::nullopt;
      }
      return subnet;
    }
    if (::inet_pton(AF_INET6, address_part.c_str(), subnet.bytes.data()) == 1) {
      subnet.family = AF_INET6;
      subnet.prefix_length = prefix_part.empty() ? 128U : static_cast<uint8_t>(std::stoi(prefix_part));
      if (subnet.prefix_length > 128U) {
        return std::nullopt;
      }
      return subnet;
    }
  } catch (const std::exception&) {
    return std::nullopt;
  }
  return std::nullopt;
}

bool subnet_contains(const IpSubnet& subnet, const std::string& remote_address) {
  std::array<uint8_t, 16> remote_bytes{};
  if (::inet_pton(subnet.family, remote_address.c_str(), remote_bytes.data()) != 1) {
    return false;
  }

  const size_t total_bytes = subnet.family == AF_INET ? 4U : 16U;
  const size_t full_bytes = subnet.prefix_length / 8U;
  const uint8_t remaining_bits = static_cast<uint8_t>(subnet.prefix_length % 8U);
  for (size_t i = 0; i < full_bytes; ++i) {
    if (subnet.bytes[i] != remote_bytes[i]) {
      return false;
    }
  }
  if (remaining_bits > 0U && full_bytes < total_bytes) {
    const uint8_t mask = static_cast<uint8_t>(0xFFU << (8U - remaining_bits));
    if ((subnet.bytes[full_bytes] & mask) != (remote_bytes[full_bytes] & mask)) {
      return false;
    }
  }
  return true;
}

struct RateLimitWindow {
  std::chrono::steady_clock::time_point window_start{};
  uint32_t count{0};
};

bool is_api_path(const std::string& path) { return path.rfind("/api/", 0) == 0; }

void apply_api_cors_headers(const HttpRequest& request, const WebRtcVideoServerConfig& config, HttpResponse& response) {
  if (!is_api_path(request.path)) {
    return;
  }
  const auto origin = find_header_value(request, "Origin");
  if (!origin.has_value() || origin->empty()) {
    return;
  }

  bool allow_origin = false;
  if (!config.cors_allowed_origins.empty()) {
    for (const auto& allowed_origin : config.cors_allowed_origins) {
      if (allowed_origin == "*" || allowed_origin == *origin) {
        allow_origin = true;
        break;
      }
    }
  } else if (is_loopback_host(config.http_host) && is_loopback_origin(*origin)) {
    allow_origin = true;
  }

  if (!allow_origin) {
    return;
  }

  response.headers["Access-Control-Allow-Origin"] = *origin;
  response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, OPTIONS";
  response.headers["Access-Control-Allow-Headers"] = "Authorization, Content-Type, X-Video-Server-Key";
  response.headers["Vary"] = "Origin";
}

std::string output_config_json(const StreamOutputConfig& cfg) {
  std::ostringstream out;
  out << "{"
      << "\"display_mode\":\"" << to_string(cfg.display_mode) << "\","
      << "\"mirrored\":" << bool_to_json(cfg.mirrored) << ","
      << "\"rotation_degrees\":" << cfg.rotation_degrees << ","
      << "\"palette_min\":" << cfg.palette_min << ","
      << "\"palette_max\":" << cfg.palette_max << ","
      << "\"output_width\":" << cfg.output_width << ","
      << "\"output_height\":" << cfg.output_height << ","
      << "\"output_fps\":" << cfg.output_fps << ","
      << "\"config_generation\":" << cfg.config_generation << ","
      << "\"apply_status\":\"active\","
      << "\"raw_pipeline_reconfigure\":\"stream-local-restart\"}";
  return out.str();
}

std::string sender_debug_counters_json(const SenderDebugCounters& counters) {
  std::ostringstream out;
  out << "{"
      << "\"delivered_units\":" << counters.delivered_units << ","
      << "\"duplicate_units_skipped\":" << counters.duplicate_units_skipped << ","
      << "\"failed_units\":" << counters.failed_units << ","
      << "\"packetization_failures\":" << counters.packetization_failures << ","
      << "\"track_closed_events\":" << counters.track_closed_events << ","
      << "\"send_failures\":" << counters.send_failures << ","
      << "\"packets_attempted\":" << counters.packets_attempted << ","
      << "\"packets_sent_after_track_open\":" << counters.packets_sent_after_track_open << ","
      << "\"startup_packets_sent\":" << counters.startup_packets_sent << ","
      << "\"startup_sequence_injections\":" << counters.startup_sequence_injections << ","
      << "\"first_decodable_transitions\":" << counters.first_decodable_transitions << ","
      << "\"skipped_no_track\":" << counters.skipped_no_track << ","
      << "\"skipped_track_not_open\":" << counters.skipped_track_not_open << ","
      << "\"skipped_codec_config_wait\":" << counters.skipped_codec_config_wait << ","
      << "\"skipped_keyframe_wait\":" << counters.skipped_keyframe_wait << ","
      << "\"skipped_startup_idr_wait\":" << counters.skipped_startup_idr_wait << "}";
  return out.str();
}

std::string session_debug_snapshot_json(const StreamSessionDebugSnapshot& session) {
  std::ostringstream out;
  out << "{"
      << "\"session_generation\":" << session.session_generation << ","
      << "\"stream_id\":\"" << json_escape(session.stream_id) << "\","
      << "\"peer_state\":\"" << json_escape(session.peer_state) << "\","
      << "\"active\":" << bool_to_json(session.active) << ","
      << "\"sending_active\":" << bool_to_json(session.sending_active) << ","
      << "\"sender_state\":\"" << json_escape(session.sender_state) << "\","
      << "\"last_packetization_status\":\"" << json_escape(session.last_packetization_status) << "\","
      << "\"teardown_reason\":\"" << json_escape(session.teardown_reason) << "\","
      << "\"last_transition_reason\":\"" << json_escape(session.last_transition_reason) << "\","
      << "\"disconnect_count\":" << session.disconnect_count << ","
      << "\"track_exists\":" << bool_to_json(session.track_exists) << ","
      << "\"track_open\":" << bool_to_json(session.track_open) << ","
      << "\"startup_sequence_sent\":" << bool_to_json(session.startup_sequence_sent) << ","
      << "\"first_decodable_frame_sent\":" << bool_to_json(session.first_decodable_frame_sent) << ","
      << "\"codec_config_seen\":" << bool_to_json(session.codec_config_seen) << ","
      << "\"cached_idr_available\":" << bool_to_json(session.cached_idr_available) << ","
      << "\"video_mid\":\"" << json_escape(session.video_mid) << "\","
      << "\"counters\":" << sender_debug_counters_json(session.counters) << "}";
  return out.str();
}

std::string stream_debug_snapshot_json(const StreamDebugSnapshot& stream) {
  std::ostringstream out;
  out << "{"
      << "\"stream_id\":\"" << json_escape(stream.stream_id) << "\","
      << "\"label\":\"" << json_escape(stream.label) << "\","
      << "\"configured_width\":" << stream.configured_width << ","
      << "\"configured_height\":" << stream.configured_height << ","
      << "\"configured_fps\":" << stream.configured_fps << ","
      << "\"active_filter_mode\":\"" << json_escape(stream.active_filter_mode) << "\","
      << "\"active_output_width\":" << stream.active_output_width << ","
      << "\"active_output_height\":" << stream.active_output_height << ","
      << "\"active_output_fps\":" << stream.active_output_fps << ","
      << "\"config_generation\":" << stream.config_generation << ","
      << "\"config_state\":\"" << json_escape(stream.config_state) << "\","
      << "\"latest_raw_frame_available\":" << bool_to_json(stream.latest_raw_frame_available) << ","
      << "\"latest_raw_frame_id\":" << stream.latest_raw_frame_id << ","
      << "\"latest_raw_timestamp_ns\":" << stream.latest_raw_timestamp_ns << ","
      << "\"latest_raw_width\":" << stream.latest_raw_width << ","
      << "\"latest_raw_height\":" << stream.latest_raw_height << ","
      << "\"latest_encoded_access_unit_available\":" << bool_to_json(stream.latest_encoded_access_unit_available) << ","
      << "\"latest_encoded_timestamp_ns\":" << stream.latest_encoded_timestamp_ns << ","
      << "\"latest_encoded_sequence_id\":" << stream.latest_encoded_sequence_id << ","
      << "\"latest_encoded_size_bytes\":" << stream.latest_encoded_size_bytes << ","
      << "\"latest_encoded_keyframe\":" << bool_to_json(stream.latest_encoded_keyframe) << ","
      << "\"latest_encoded_codec_config\":" << bool_to_json(stream.latest_encoded_codec_config) << ","
      << "\"total_frames_received\":" << stream.total_frames_received << ","
      << "\"total_frames_transformed\":" << stream.total_frames_transformed << ","
      << "\"total_frames_dropped\":" << stream.total_frames_dropped << ","
      << "\"total_access_units_received\":" << stream.total_access_units_received << ","
      << "\"current_session\":";
  if (stream.current_session.has_value()) {
    out << session_debug_snapshot_json(*stream.current_session);
  } else {
    out << "null";
  }
  out << "}";
  return out.str();
}

}  // namespace

class WebRtcVideoServer::Impl {
 public:
  explicit Impl(WebRtcVideoServerConfig config)
      : config_(std::move(config)),
        signaling_([this](const std::string& stream_id) { return core_.get_stream_info(stream_id).has_value(); },
                   [this](const std::string& stream_id) { return core_.get_latest_frame_for_stream(stream_id); },
                   [this](const std::string& stream_id) {
                     return core_.get_latest_encoded_unit_for_stream(stream_id);
                   },
                   config_.max_pending_candidates_per_stream),
        http_server_(std::make_unique<HttpApiServer>(config_.http_host, config_.http_port, config_.max_http_request_bytes)) {
    for (const auto& entry : config_.ip_allowlist) {
      auto subnet = parse_allowlist_entry(entry);
      if (!subnet.has_value()) {
        allowlist_parse_error_ = "invalid allowlist entry: " + entry;
        break;
      }
      allowlist_.push_back(*subnet);
    }
  }

  struct SecurityCounters {
    uint64_t rejected_unauthorized{0};
    uint64_t rejected_forbidden{0};
    uint64_t rejected_disabled{0};
    uint64_t rejected_invalid{0};
    uint64_t rejected_rate_limited{0};
  };

  struct AuthResult {
    bool allowed{true};
    HttpResponse response{200, "", "application/json"};
  };

  bool start() {
    if (!allowlist_parse_error_.empty()) {
      spdlog::error("[security] {}", allowlist_parse_error_);
      return false;
    }
    if (!config_.enable_http_api) {
      return true;
    }
    return http_server_->start([this](const HttpRequest& request) { return this->handle_http(request); });
  }

  void stop() {
    signaling_.stop();
    if (http_server_) {
      http_server_->stop();
    }
  }

  ServerDebugSnapshot get_debug_snapshot() const {
    ServerDebugSnapshot snapshot;
    snapshot.streams = core_.list_stream_debug_snapshots();

    const auto sessions = signaling_.list_sessions();
    snapshot.stream_count = snapshot.streams.size();
    snapshot.security_access_control_enabled = shared_key_auth_enabled();
    snapshot.security_allowlist_enabled = allowlist_enabled();
    snapshot.security_debug_api_enabled = debug_api_effectively_enabled();
    snapshot.security_runtime_config_api_enabled = runtime_config_api_effectively_enabled();
    snapshot.security_remote_sensitive_routes_allowed = remote_sensitive_routes_allowed();
    {
      std::lock_guard<std::mutex> lock(security_mutex_);
      snapshot.security_rejected_unauthorized = security_counters_.rejected_unauthorized;
      snapshot.security_rejected_forbidden = security_counters_.rejected_forbidden;
      snapshot.security_rejected_disabled = security_counters_.rejected_disabled;
      snapshot.security_rejected_invalid = security_counters_.rejected_invalid;
      snapshot.security_rejected_rate_limited = security_counters_.rejected_rate_limited;
    }
    for (const auto& session : sessions) {
      snapshot.active_session_count += session.active ? 1u : 0u;
    }

    std::unordered_map<std::string, StreamSessionDebugSnapshot> sessions_by_stream;
    for (const auto& session : sessions) {
      StreamSessionDebugSnapshot session_snapshot;
      session_snapshot.session_generation = session.session_generation;
      session_snapshot.stream_id = session.stream_id;
      session_snapshot.peer_state = session.peer_state;
      session_snapshot.active = session.active;
      session_snapshot.sending_active = session.sending_active;
      session_snapshot.sender_state = session.media_source.encoded_sender.sender_state;
      session_snapshot.last_packetization_status = session.media_source.encoded_sender.last_packetization_status;
      session_snapshot.teardown_reason = session.teardown_reason;
      session_snapshot.last_transition_reason = session.last_transition_reason;
      session_snapshot.disconnect_count = session.disconnect_count;
      session_snapshot.track_exists = session.media_source.encoded_sender.video_track_exists;
      session_snapshot.track_open = session.media_source.encoded_sender.video_track_open;
      session_snapshot.startup_sequence_sent = session.media_source.encoded_sender.startup_sequence_sent;
      session_snapshot.first_decodable_frame_sent = session.media_source.encoded_sender.first_decodable_frame_sent;
      session_snapshot.codec_config_seen = session.media_source.encoded_sender.codec_config_seen;
      session_snapshot.cached_idr_available = session.media_source.encoded_sender.cached_idr_available;
      session_snapshot.video_mid = session.media_source.encoded_sender.video_mid;
      session_snapshot.counters = SenderDebugCounters{
          session.media_source.encoded_sender.delivered_units,
          session.media_source.encoded_sender.duplicate_units_skipped,
          session.media_source.encoded_sender.failed_units,
          session.media_source.encoded_sender.packetization_failures,
          session.media_source.encoded_sender.track_closed_events,
          session.media_source.encoded_sender.send_failures,
          session.media_source.encoded_sender.packets_attempted,
          session.media_source.encoded_sender.packets_sent_after_track_open,
          session.media_source.encoded_sender.startup_packets_sent,
          session.media_source.encoded_sender.startup_sequence_injections,
          session.media_source.encoded_sender.first_decodable_transitions,
          session.media_source.encoded_sender.skipped_no_track,
          session.media_source.encoded_sender.skipped_track_not_open,
          session.media_source.encoded_sender.skipped_codec_config_wait,
          session.media_source.encoded_sender.skipped_keyframe_wait,
          session.media_source.encoded_sender.skipped_startup_idr_wait};
      sessions_by_stream[session.stream_id] = std::move(session_snapshot);
    }

    for (auto& stream : snapshot.streams) {
      const auto it = sessions_by_stream.find(stream.stream_id);
      if (it != sessions_by_stream.end()) {
        stream.current_session = it->second;
      }
    }
    return snapshot;
  }

  bool shared_key_auth_enabled() const { return config_.enable_shared_key_auth && !config_.shared_key.empty(); }
  bool allowlist_enabled() const { return !allowlist_.empty(); }
  bool loopback_bind_only() const { return is_loopback_host(config_.http_host); }
  bool has_any_remote_guard() const { return shared_key_auth_enabled() || allowlist_enabled(); }
  bool remote_sensitive_routes_allowed() const {
    return loopback_bind_only() || config_.allow_unsafe_public_routes || has_any_remote_guard();
  }
  bool debug_api_effectively_enabled() const { return config_.enable_debug_api; }
  bool runtime_config_api_effectively_enabled() const { return config_.enable_runtime_config_api; }

  void record_rejected_unauthorized() {
    std::lock_guard<std::mutex> lock(security_mutex_);
    ++security_counters_.rejected_unauthorized;
  }
  void record_rejected_forbidden() {
    std::lock_guard<std::mutex> lock(security_mutex_);
    ++security_counters_.rejected_forbidden;
  }
  void record_rejected_disabled() {
    std::lock_guard<std::mutex> lock(security_mutex_);
    ++security_counters_.rejected_disabled;
  }
  void record_rejected_invalid() {
    std::lock_guard<std::mutex> lock(security_mutex_);
    ++security_counters_.rejected_invalid;
  }
  void record_rejected_rate_limited() {
    std::lock_guard<std::mutex> lock(security_mutex_);
    ++security_counters_.rejected_rate_limited;
  }

  bool is_remote_allowed(const std::string& remote_address) const {
    if (!allowlist_enabled()) {
      return true;
    }
    for (const auto& subnet : allowlist_) {
      if (subnet_contains(subnet, remote_address)) {
        return true;
      }
    }
    return false;
  }

  bool request_has_valid_shared_key(const HttpRequest& request) const {
    if (!shared_key_auth_enabled()) {
      return true;
    }
    if (const auto auth = find_header_value(request, "Authorization"); auth.has_value()) {
      const std::string trimmed = trim_ascii(*auth);
      static constexpr char kBearerPrefix[] = "Bearer ";
      if (trimmed.rfind(kBearerPrefix, 0) == 0 && trimmed.substr(sizeof(kBearerPrefix) - 1) == config_.shared_key) {
        return true;
      }
    }
    if (const auto key = find_header_value(request, "X-Video-Server-Key"); key.has_value()) {
      return trim_ascii(*key) == config_.shared_key;
    }
    return false;
  }

  bool consume_rate_limit(const HttpRequest& request, const RoutePolicy& policy) {
    if (!policy.rate_limited) {
      return true;
    }

    uint32_t max_requests = 0;
    uint32_t window_seconds = 0;
    if (policy.rate_limit_bucket == "signaling") {
      max_requests = config_.signaling_rate_limit_max_requests;
      window_seconds = config_.signaling_rate_limit_window_seconds;
    } else if (policy.rate_limit_bucket == "config") {
      max_requests = config_.config_rate_limit_max_requests;
      window_seconds = config_.config_rate_limit_window_seconds;
    } else if (policy.rate_limit_bucket == "debug") {
      max_requests = config_.debug_rate_limit_max_requests;
      window_seconds = config_.debug_rate_limit_window_seconds;
    }
    if (max_requests == 0 || window_seconds == 0) {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string bucket_key =
        policy.rate_limit_bucket + "|" + request.remote_address + "|" + request.path + "|" + request.method;
    std::lock_guard<std::mutex> lock(security_mutex_);
    auto& window = rate_limit_windows_[bucket_key];
    if (window.count == 0 || now - window.window_start >= std::chrono::seconds(window_seconds)) {
      window.window_start = now;
      window.count = 1;
      return true;
    }
    if (window.count >= max_requests) {
      return false;
    }
    ++window.count;
    return true;
  }

  AuthResult authorize_request(const HttpRequest& request, const RoutePolicy& policy) {
    if (policy.runtime_config_route && !runtime_config_api_effectively_enabled()) {
      record_rejected_disabled();
      return AuthResult{false, json_error(404, "runtime config endpoint disabled")};
    }
    if (policy.debug_route && !debug_api_effectively_enabled()) {
      record_rejected_disabled();
      return AuthResult{false, json_error(404, "debug endpoint disabled")};
    }
    if (!policy.stream_id.empty() && !is_valid_stream_id(policy.stream_id)) {
      record_rejected_invalid();
      return AuthResult{false, json_error(400, "invalid stream id")};
    }
    if (policy.sensitive && !remote_sensitive_routes_allowed()) {
      record_rejected_forbidden();
      return AuthResult{false, json_error(403, "sensitive endpoint requires access control, allowlist, or unsafe opt-in on non-loopback binds")};
    }
    if (!is_remote_allowed(request.remote_address)) {
      record_rejected_forbidden();
      return AuthResult{false, json_error(403, "remote address not allowed")};
    }
    if (policy.sensitive && shared_key_auth_enabled() && !request_has_valid_shared_key(request)) {
      record_rejected_unauthorized();
      HttpResponse response = json_error(401, "missing or invalid shared key");
      response.headers["WWW-Authenticate"] = "Bearer realm=\"video-server\"";
      return AuthResult{false, std::move(response)};
    }
    if (!consume_rate_limit(request, policy)) {
      record_rejected_rate_limited();
      return AuthResult{false, json_error(429, "rate limit exceeded")};
    }
    return AuthResult{};
  }

  HttpResponse handle_http(const HttpRequest& request) {
    const RoutePolicy route = classify_route(request);
    if (is_api_path(request.path) && request.method == "OPTIONS") {
      HttpResponse response{204, "", "application/json"};
      apply_api_cors_headers(request, config_, response);
      return response;
    }

    auto finalize = [&](HttpResponse response) {
      apply_api_cors_headers(request, config_, response);
      return response;
    };

    if (route.valid) {
      const AuthResult auth = authorize_request(request, route);
      if (!auth.allowed) {
        return finalize(auth.response);
      }
    }

    if (request.method == "GET" && request.path == "/api/video/streams") {
      auto streams = core_.list_streams();
      std::ostringstream out;
      out << "{\"streams\":[";
      for (size_t i = 0; i < streams.size(); ++i) {
        if (i != 0) out << ',';
        out << "{\"stream_id\":\"" << json_escape(streams[i].stream_id) << "\",\"label\":\""
            << json_escape(streams[i].label) << "\",\"active\":" << bool_to_json(streams[i].active) << '}';
      }
      out << "]}";
      return finalize(HttpResponse{200, out.str(), "application/json"});
    }

    if (request.method == "GET" && request.path == "/api/video/debug/stats") {
      const auto snapshot = get_debug_snapshot();
      std::ostringstream out;
      out << "{"
          << "\"stream_count\":" << snapshot.stream_count << ","
          << "\"active_session_count\":" << snapshot.active_session_count << ","
          << "\"security\":{"
          << "\"access_control_enabled\":" << bool_to_json(snapshot.security_access_control_enabled) << ","
          << "\"allowlist_enabled\":" << bool_to_json(snapshot.security_allowlist_enabled) << ","
          << "\"debug_api_enabled\":" << bool_to_json(snapshot.security_debug_api_enabled) << ","
          << "\"runtime_config_api_enabled\":" << bool_to_json(snapshot.security_runtime_config_api_enabled) << ","
          << "\"remote_sensitive_routes_allowed\":"
          << bool_to_json(snapshot.security_remote_sensitive_routes_allowed) << ","
          << "\"rejected_unauthorized\":" << snapshot.security_rejected_unauthorized << ","
          << "\"rejected_forbidden\":" << snapshot.security_rejected_forbidden << ","
          << "\"rejected_disabled\":" << snapshot.security_rejected_disabled << ","
          << "\"rejected_invalid\":" << snapshot.security_rejected_invalid << ","
          << "\"rejected_rate_limited\":" << snapshot.security_rejected_rate_limited << "},"
          << "\"streams\":[";
      for (size_t i = 0; i < snapshot.streams.size(); ++i) {
        if (i != 0) out << ",";
        out << stream_debug_snapshot_json(snapshot.streams[i]);
      }
      out << "]}";
      return finalize(HttpResponse{200, out.str(), "application/json"});
    }

    if (request.path.find("/api/video/streams/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/streams/").size());
      const auto output_pos = tail.find("/output");
      const auto config_pos = tail.find("/config");
      const auto frame_pos = tail.find("/frame");
      const bool is_frame_request = frame_pos != std::string::npos && (frame_pos + 6) == tail.size();
      if (request.method == "GET" && output_pos == std::string::npos && config_pos == std::string::npos && !is_frame_request) {
        auto info = core_.get_stream_info(tail);
        if (!info.has_value()) {
          return finalize(json_error(404, "stream not found"));
        }
        auto latest = core_.get_latest_frame_for_stream(tail);
        auto latest_encoded = core_.get_latest_encoded_unit_for_stream(tail);
        std::ostringstream out;
        out << "{\"stream_id\":\"" << json_escape(info->stream_id) << "\","
            << "\"label\":\"" << json_escape(info->label) << "\","
            << "\"frames_received\":" << info->frames_received << ','
            << "\"frames_transformed\":" << info->frames_transformed << ','
            << "\"frames_dropped\":" << info->frames_dropped << ','
            << "\"access_units_received\":" << info->access_units_received << ','
            << "\"last_input_timestamp_ns\":" << info->last_input_timestamp_ns << ','
            << "\"last_output_timestamp_ns\":" << info->last_output_timestamp_ns << ','
            << "\"last_frame_id\":" << info->last_frame_id << ','
            << "\"has_latest_frame\":" << bool_to_json(info->has_latest_frame) << ','
            << "\"has_latest_encoded_unit\":" << bool_to_json(info->has_latest_encoded_unit) << ','
            << "\"latest_frame_width\":" << (latest ? latest->width : 0) << ','
            << "\"latest_frame_height\":" << (latest ? latest->height : 0) << ','
            << "\"latest_frame_pixel_format\":\""
            << (latest ? to_string(latest->pixel_format) : "") << "\","
            << "\"latest_frame_timestamp_ns\":" << (latest ? latest->timestamp_ns : 0) << ','
            << "\"latest_encoded_codec\":\""
            << (latest_encoded ? to_string(latest_encoded->codec) : "") << "\","
            << "\"latest_encoded_timestamp_ns\":"
            << (latest_encoded ? latest_encoded->timestamp_ns : 0) << ','
            << "\"latest_encoded_sequence_id\":"
            << (latest_encoded ? latest_encoded->sequence_id : 0) << ','
            << "\"latest_encoded_size_bytes\":"
            << (latest_encoded ? latest_encoded->bytes.size() : 0) << ','
            << "\"latest_encoded_keyframe\":"
            << bool_to_json(latest_encoded ? latest_encoded->keyframe : false) << ','
            << "\"latest_encoded_codec_config\":"
            << bool_to_json(latest_encoded ? latest_encoded->codec_config : false) << ','
            << "\"active\":" << bool_to_json(info->active) << ","
            << "\"output_config\":" << output_config_json(info->output_config) << '}';
        return finalize(HttpResponse{200, out.str(), "application/json"});
      }


      if (request.method == "GET" && is_frame_request) {
        const std::string stream_id = tail.substr(0, frame_pos);
        auto info = core_.get_stream_info(stream_id);
        if (!info.has_value()) {
          return finalize(json_error(404, "stream not found"));
        }

        auto latest = core_.get_latest_frame_for_stream(stream_id);
        if (!latest || !latest->valid) {
          return finalize(json_error(404, "latest frame not found"));
        }

        auto encoded = encode_latest_frame_as_ppm(*latest);
        if (!encoded.has_value()) {
          return finalize(json_error(500, "failed to encode latest frame"));
        }
        return finalize(HttpResponse{200, std::move(encoded->body), encoded->content_type});
      }

      const auto config_segment_pos = output_pos != std::string::npos ? output_pos : config_pos;
      if (config_segment_pos != std::string::npos) {
        const std::string stream_id = tail.substr(0, config_segment_pos);
        if (request.method == "GET") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            spdlog::debug("[api] config-get stream={} result=not-found", stream_id);
            return finalize(json_error(404, "stream not found"));
          }
          const auto info = core_.get_stream_info(stream_id);
          spdlog::debug("[api] config-get stream={} input_format={} current={}", stream_id,
                        info.has_value() ? to_string(info->config.input_pixel_format) : "unknown",
                        output_config_json(*cfg));
          return finalize(HttpResponse{200, output_config_json(*cfg), "application/json"});
        }

        if (request.method == "PUT") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            spdlog::debug("[api] config-put stream={} result=not-found body={}", stream_id, request.body);
            return finalize(json_error(404, "stream not found"));
          }
          const auto info = core_.get_stream_info(stream_id);
          spdlog::debug("[api] config-put stream={} input_format={} body={} current={}", stream_id,
                        info.has_value() ? to_string(info->config.input_pixel_format) : "unknown", request.body,
                        output_config_json(*cfg));

          if (request.body.empty()) {
            record_rejected_invalid();
            spdlog::debug("[api] config-put stream={} result=invalid-empty-body", stream_id);
            return finalize(json_error(400, "invalid request body"));
          }
          if (request.body.size() > config_.max_json_body_bytes) {
            record_rejected_invalid();
            return finalize(json_error(413, "config request body too large"));
          }

          std::unordered_map<std::string, JsonValue> body_values;
          if (!parse_flat_json_object(request.body, body_values)) {
            record_rejected_invalid();
            spdlog::debug("[api] config-put stream={} result=invalid-json body={}", stream_id, request.body);
            return finalize(json_error(400, "invalid request body"));
          }
          static const std::unordered_set<std::string> kAllowedKeys = {"display_mode", "mirrored", "rotation_degrees",
                                                                       "palette_min", "palette_max", "output_width",
                                                                       "output_height", "output_fps"};
          for (const auto& [key, _] : body_values) {
            if (kAllowedKeys.find(key) == kAllowedKeys.end()) {
              record_rejected_invalid();
              return finalize(json_error(400, "unknown config field: " + key));
            }
          }

          StreamOutputConfig updated = *cfg;
          if (const auto mode_it = body_values.find("display_mode"); mode_it != body_values.end()) {
            if (mode_it->second.type != JsonValue::Type::String) {
              record_rejected_invalid();
              return finalize(json_error(400, "display_mode must be a string"));
            }
            const auto parsed_mode = video_display_mode_from_string(mode_it->second.string_value.c_str());
            if (!parsed_mode.has_value()) {
              record_rejected_invalid();
              return finalize(json_error(400, "invalid display_mode"));
            }
            updated.display_mode = *parsed_mode;
          }
          if (const auto mirrored_it = body_values.find("mirrored"); mirrored_it != body_values.end()) {
            if (mirrored_it->second.type != JsonValue::Type::Bool) {
              record_rejected_invalid();
              return finalize(json_error(400, "mirrored must be a boolean"));
            }
            updated.mirrored = mirrored_it->second.bool_value;
          }
          if (const auto rotation_it = body_values.find("rotation_degrees"); rotation_it != body_values.end()) {
            if (!json_number_is_non_negative_integer(rotation_it->second)) {
              record_rejected_invalid();
              return finalize(json_error(400, "rotation_degrees must be an integer"));
            }
            updated.rotation_degrees = static_cast<int>(rotation_it->second.number_value);
          }
          if (const auto min_it = body_values.find("palette_min"); min_it != body_values.end()) {
            if (min_it->second.type != JsonValue::Type::Number || !is_finite_number(min_it->second.number_value)) {
              record_rejected_invalid();
              return finalize(json_error(400, "palette_min must be numeric"));
            }
            updated.palette_min = static_cast<float>(min_it->second.number_value);
          }
          if (const auto max_it = body_values.find("palette_max"); max_it != body_values.end()) {
            if (max_it->second.type != JsonValue::Type::Number || !is_finite_number(max_it->second.number_value)) {
              record_rejected_invalid();
              return finalize(json_error(400, "palette_max must be numeric"));
            }
            updated.palette_max = static_cast<float>(max_it->second.number_value);
          }
          if (const auto width_it = body_values.find("output_width"); width_it != body_values.end()) {
            if (!json_number_is_non_negative_integer(width_it->second)) {
              record_rejected_invalid();
              return finalize(json_error(400, "output_width must be an integer"));
            }
            updated.output_width = static_cast<uint32_t>(width_it->second.number_value);
          }
          if (const auto height_it = body_values.find("output_height"); height_it != body_values.end()) {
            if (!json_number_is_non_negative_integer(height_it->second)) {
              record_rejected_invalid();
              return finalize(json_error(400, "output_height must be an integer"));
            }
            updated.output_height = static_cast<uint32_t>(height_it->second.number_value);
          }
          if (const auto fps_it = body_values.find("output_fps"); fps_it != body_values.end()) {
            if (fps_it->second.type != JsonValue::Type::Number || !is_finite_number(fps_it->second.number_value)) {
              record_rejected_invalid();
              return finalize(json_error(400, "output_fps must be numeric"));
            }
            updated.output_fps = fps_it->second.number_value;
          }

          if (!core_.set_stream_output_config(stream_id, updated)) {
            record_rejected_invalid();
            spdlog::debug("[api] config-put stream={} result=invalid-output-config candidate={}", stream_id,
                          output_config_json(updated));
            return finalize(json_error(400, "invalid output config; expected known filter and width/height 16..3840 and fps 1..120"));
          }

          auto persisted = core_.get_stream_output_config(stream_id);
          if (!persisted.has_value()) {
            spdlog::debug("[api] config-put stream={} result=missing-after-update", stream_id);
            return finalize(json_error(404, "stream not found"));
          }
          spdlog::debug("[api] config-put stream={} result=applied persisted={} note=updates-core-transform-state", stream_id,
                        output_config_json(*persisted));
          return finalize(HttpResponse{200, output_config_json(*persisted), "application/json"});
        }
      }
    }

    if (request.path.find("/api/video/signaling/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/signaling/").size());
      const auto slash = tail.find('/');
      const std::string stream_id = slash == std::string::npos ? tail : tail.substr(0, slash);
      const std::string action = slash == std::string::npos ? "offer" : tail.substr(slash + 1);

      if (request.method == "POST" && action == "offer") {
        if (request.body.empty()) {
          record_rejected_invalid();
          return finalize(json_error(400, "offer body required"));
        }
        if (request.body.size() > config_.max_signaling_sdp_bytes) {
          record_rejected_invalid();
          return finalize(json_error(413, "offer body too large"));
        }
        std::string error_message;
        if (!signaling_.set_offer(stream_id, request.body, &error_message)) {
          if (error_message != "stream not found") {
            record_rejected_invalid();
          }
          return finalize(json_error(error_message == "stream not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "POST" && action == "answer") {
        if (request.body.empty()) {
          record_rejected_invalid();
          return finalize(json_error(400, "answer body required"));
        }
        if (request.body.size() > config_.max_signaling_sdp_bytes) {
          record_rejected_invalid();
          return finalize(json_error(413, "answer body too large"));
        }
        std::string error_message;
        if (!signaling_.set_answer(stream_id, request.body, &error_message)) {
          if (error_message != "session not found") {
            record_rejected_invalid();
          }
          return finalize(json_error(error_message == "session not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "POST" && action == "candidate") {
        if (request.body.empty()) {
          record_rejected_invalid();
          return finalize(json_error(400, "candidate body required"));
        }
        if (request.body.size() > config_.max_signaling_candidate_bytes) {
          record_rejected_invalid();
          return finalize(json_error(413, "candidate body too large"));
        }
        std::string error_message;
        if (!signaling_.add_ice_candidate(stream_id, request.body, &error_message)) {
          if (error_message != "session not found" && error_message != "stream not found") {
            record_rejected_invalid();
          }
          return finalize(json_error(error_message == "session not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "GET" && action == "session") {
        auto session = signaling_.get_session(stream_id);
        if (!session.has_value()) {
          return finalize(json_error(404, "session not found"));
        }
        std::ostringstream out;
        out << "{\"session_generation\":" << session->session_generation << ','
            << "\"stream_id\":\"" << json_escape(session->stream_id) << "\","
            << "\"offer_sdp\":\"" << json_escape(session->offer_sdp) << "\","
            << "\"answer_sdp\":\"" << json_escape(session->answer_sdp) << "\","
            << "\"last_remote_candidate\":\"" << json_escape(session->last_remote_candidate) << "\","
            << "\"last_local_candidate\":\"" << json_escape(session->last_local_candidate) << "\","
            << "\"peer_state\":\"" << json_escape(session->peer_state) << "\","
            << "\"active\":" << bool_to_json(session->active) << ','
            << "\"sending_active\":" << bool_to_json(session->sending_active) << ','
            << "\"teardown_reason\":\"" << json_escape(session->teardown_reason) << "\","
            << "\"last_transition_reason\":\"" << json_escape(session->last_transition_reason) << "\","
            << "\"disconnect_count\":" << session->disconnect_count << ','
            << "\"media_bridge_state\":\"" << json_escape(session->media_source.bridge_state) << "\","
            << "\"preferred_media_path\":\"" << json_escape(session->media_source.preferred_media_path) << "\","
            << "\"latest_snapshot_available\":" << bool_to_json(session->media_source.latest_snapshot_available)
            << ','
            << "\"latest_snapshot_frame_id\":" << session->media_source.latest_snapshot_frame_id << ','
            << "\"latest_snapshot_timestamp_ns\":" << session->media_source.latest_snapshot_timestamp_ns << ','
            << "\"latest_snapshot_width\":" << session->media_source.latest_snapshot_width << ','
            << "\"latest_snapshot_height\":" << session->media_source.latest_snapshot_height << ','
            << "\"latest_encoded_access_unit_available\":"
            << bool_to_json(session->media_source.latest_encoded_access_unit_available) << ','
            << "\"latest_encoded_codec\":\"" << json_escape(session->media_source.latest_encoded_codec) << "\","
            << "\"latest_encoded_timestamp_ns\":" << session->media_source.latest_encoded_timestamp_ns << ','
            << "\"latest_encoded_sequence_id\":" << session->media_source.latest_encoded_sequence_id << ','
            << "\"latest_encoded_size_bytes\":" << session->media_source.latest_encoded_size_bytes << ','
            << "\"latest_encoded_keyframe\":" << bool_to_json(session->media_source.latest_encoded_keyframe) << ','
            << "\"latest_encoded_codec_config\":"
            << bool_to_json(session->media_source.latest_encoded_codec_config) << ','
            << "\"encoded_sender_state\":\"" << json_escape(session->media_source.encoded_sender.sender_state)
            << "\","
            << "\"encoded_sender_session_active\":"
            << bool_to_json(session->media_source.encoded_sender.session_active) << ','
            << "\"encoded_sender_session_teardown_reason\":\""
            << json_escape(session->media_source.encoded_sender.session_teardown_reason) << "\","
            << "\"encoded_sender_last_lifecycle_event\":\""
            << json_escape(session->media_source.encoded_sender.last_lifecycle_event) << "\","
            << "\"encoded_sender_codec\":\"" << json_escape(session->media_source.encoded_sender.codec) << "\","
            << "\"encoded_sender_has_pending_encoded_unit\":"
            << bool_to_json(session->media_source.encoded_sender.has_pending_encoded_unit) << ','
            << "\"encoded_sender_codec_config_seen\":"
            << bool_to_json(session->media_source.encoded_sender.codec_config_seen) << ','
            << "\"encoded_sender_ready_for_video_track\":"
            << bool_to_json(session->media_source.encoded_sender.ready_for_video_track) << ','
            << "\"encoded_sender_video_track_exists\":"
            << bool_to_json(session->media_source.encoded_sender.video_track_exists) << ','
            << "\"encoded_sender_video_track_open\":"
            << bool_to_json(session->media_source.encoded_sender.video_track_open) << ','
            << "\"encoded_sender_h264_delivery_active\":"
            << bool_to_json(session->media_source.encoded_sender.h264_delivery_active) << ','
            << "\"encoded_sender_keyframe_seen\":"
            << bool_to_json(session->media_source.encoded_sender.keyframe_seen) << ','
            << "\"encoded_sender_cached_codec_config_available\":"
            << bool_to_json(session->media_source.encoded_sender.cached_codec_config_available) << ','
            << "\"encoded_sender_cached_idr_available\":"
            << bool_to_json(session->media_source.encoded_sender.cached_idr_available) << ','
            << "\"encoded_sender_first_decodable_frame_sent\":"
            << bool_to_json(session->media_source.encoded_sender.first_decodable_frame_sent) << ','
            << "\"encoded_sender_startup_sequence_sent\":"
            << bool_to_json(session->media_source.encoded_sender.startup_sequence_sent) << ','
            << "\"encoded_sender_delivered_units\":" << session->media_source.encoded_sender.delivered_units << ','
            << "\"encoded_sender_duplicate_units_skipped\":"
            << session->media_source.encoded_sender.duplicate_units_skipped << ','
            << "\"encoded_sender_failed_units\":" << session->media_source.encoded_sender.failed_units << ','
            << "\"encoded_sender_packets_attempted\":" << session->media_source.encoded_sender.packets_attempted << ','
            << "\"encoded_sender_packets_sent_after_track_open\":"
            << session->media_source.encoded_sender.packets_sent_after_track_open << ','
            << "\"encoded_sender_startup_packets_sent\":"
            << session->media_source.encoded_sender.startup_packets_sent << ','
            << "\"encoded_sender_startup_sequence_injections\":"
            << session->media_source.encoded_sender.startup_sequence_injections << ','
            << "\"encoded_sender_first_decodable_transitions\":"
            << session->media_source.encoded_sender.first_decodable_transitions << ','
            << "\"encoded_sender_packetization_failures\":"
            << session->media_source.encoded_sender.packetization_failures << ','
            << "\"encoded_sender_track_closed_events\":"
            << session->media_source.encoded_sender.track_closed_events << ','
            << "\"encoded_sender_send_failures\":"
            << session->media_source.encoded_sender.send_failures << ','
            << "\"encoded_sender_skipped_no_track\":"
            << session->media_source.encoded_sender.skipped_no_track << ','
            << "\"encoded_sender_skipped_track_not_open\":"
            << session->media_source.encoded_sender.skipped_track_not_open << ','
            << "\"encoded_sender_skipped_codec_config_wait\":"
            << session->media_source.encoded_sender.skipped_codec_config_wait << ','
            << "\"encoded_sender_skipped_keyframe_wait\":"
            << session->media_source.encoded_sender.skipped_keyframe_wait << ','
            << "\"encoded_sender_skipped_startup_idr_wait\":"
            << session->media_source.encoded_sender.skipped_startup_idr_wait << ','
            << "\"encoded_sender_last_delivered_sequence_id\":"
            << session->media_source.encoded_sender.last_delivered_sequence_id << ','
            << "\"encoded_sender_last_delivered_timestamp_ns\":"
            << session->media_source.encoded_sender.last_delivered_timestamp_ns << ','
            << "\"encoded_sender_last_delivered_size_bytes\":"
            << session->media_source.encoded_sender.last_delivered_size_bytes << ','
            << "\"encoded_sender_last_delivered_keyframe\":"
            << bool_to_json(session->media_source.encoded_sender.last_delivered_keyframe) << ','
            << "\"encoded_sender_last_delivered_codec_config\":"
            << bool_to_json(session->media_source.encoded_sender.last_delivered_codec_config) << ','
            << "\"encoded_sender_last_contains_sps\":"
            << bool_to_json(session->media_source.encoded_sender.last_contains_sps) << ','
            << "\"encoded_sender_last_contains_pps\":"
            << bool_to_json(session->media_source.encoded_sender.last_contains_pps) << ','
            << "\"encoded_sender_last_contains_idr\":"
            << bool_to_json(session->media_source.encoded_sender.last_contains_idr) << ','
            << "\"encoded_sender_last_contains_non_idr\":"
            << bool_to_json(session->media_source.encoded_sender.last_contains_non_idr) << ','
            << "\"encoded_sender_negotiated_h264_payload_type\":"
            << session->media_source.encoded_sender.negotiated_h264_payload_type << ','
            << "\"encoded_sender_negotiated_h264_fmtp\":\""
            << json_escape(session->media_source.encoded_sender.negotiated_h264_fmtp) << "\","
            << "\"encoded_sender_last_packetization_status\":\""
            << json_escape(session->media_source.encoded_sender.last_packetization_status) << "\","
            << "\"encoded_sender_video_mid\":\""
            << json_escape(session->media_source.encoded_sender.video_mid) << "\"}";
        return finalize(HttpResponse{200, out.str(), "application/json"});
      }
    }

    return finalize(json_error(404, "not found"));
  }

  WebRtcVideoServerConfig config_;
  VideoServerCore core_;
  SignalingServer signaling_;
  std::unique_ptr<HttpApiServer> http_server_;
  std::vector<IpSubnet> allowlist_;
  std::string allowlist_parse_error_;
  mutable std::mutex security_mutex_;
  SecurityCounters security_counters_;
  std::unordered_map<std::string, RateLimitWindow> rate_limit_windows_;
};

WebRtcVideoServer::WebRtcVideoServer(WebRtcVideoServerConfig config) : impl_(std::make_unique<Impl>(config)) {}
WebRtcVideoServer::~WebRtcVideoServer() = default;

bool WebRtcVideoServer::start() { return impl_->start(); }
void WebRtcVideoServer::stop() { impl_->stop(); }

bool WebRtcVideoServer::register_stream(const StreamConfig& config) { return impl_->core_.register_stream(config); }

bool WebRtcVideoServer::remove_stream(const std::string& stream_id) {
  impl_->signaling_.remove_stream(stream_id);
  return impl_->core_.remove_stream(stream_id);
}

bool WebRtcVideoServer::push_frame(const std::string& stream_id, const VideoFrameView& frame) {
  if (!impl_->core_.push_frame(stream_id, frame)) {
    return false;
  }
  impl_->signaling_.on_latest_frame(stream_id, impl_->core_.get_latest_frame_for_stream(stream_id));
  return true;
}

bool WebRtcVideoServer::push_access_unit(const std::string& stream_id,
                                         const EncodedAccessUnitView& access_unit) {
  if (!impl_->core_.push_access_unit(stream_id, access_unit)) {
    return false;
  }
  impl_->signaling_.on_encoded_access_unit(stream_id, impl_->core_.get_latest_encoded_unit_for_stream(stream_id));
  return true;
}

std::vector<VideoStreamInfo> WebRtcVideoServer::list_streams() const { return impl_->core_.list_streams(); }

std::optional<VideoStreamInfo> WebRtcVideoServer::get_stream_info(const std::string& stream_id) const {
  return impl_->core_.get_stream_info(stream_id);
}

bool WebRtcVideoServer::set_stream_output_config(const std::string& stream_id,
                                                 const StreamOutputConfig& output_config) {
  return impl_->core_.set_stream_output_config(stream_id, output_config);
}

std::optional<StreamOutputConfig> WebRtcVideoServer::get_stream_output_config(
    const std::string& stream_id) const {
  return impl_->core_.get_stream_output_config(stream_id);
}

ServerDebugSnapshot WebRtcVideoServer::get_debug_snapshot() const { return impl_->get_debug_snapshot(); }

WebRtcHttpResponse WebRtcVideoServer::handle_http_request_for_test(const std::string& method,
                                                                   const std::string& path,
                                                                   const std::string& body,
                                                                   std::unordered_map<std::string, std::string> headers,
                                                                   std::string remote_address) {
  const HttpResponse response = impl_->handle_http(HttpRequest{method, path, body, std::move(remote_address), std::move(headers)});
  return WebRtcHttpResponse{response.status, response.body, response.headers};
}

}  // namespace video_server
