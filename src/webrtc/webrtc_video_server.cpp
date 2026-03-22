#include "video_server/webrtc_video_server.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

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

HttpResponse json_error(int status, const char* message) {
  std::ostringstream out;
  out << "{\"error\":\"" << message << "\"}";
  return HttpResponse{status, out.str(), "application/json"};
}

bool is_api_path(const std::string& path) { return path.rfind("/api/", 0) == 0; }

std::string cors_allow_origin(const HttpRequest& request) {
  const auto it = request.headers.find("origin");
  if (it != request.headers.end() && !it->second.empty()) {
    return it->second;
  }
  return "*";
}

void apply_api_cors_headers(const HttpRequest& request, HttpResponse& response) {
  if (!is_api_path(request.path)) {
    return;
  }
  response.headers["Access-Control-Allow-Origin"] = cors_allow_origin(request);
  response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, OPTIONS";
  response.headers["Access-Control-Allow-Headers"] = "Content-Type";
}


std::string encoded_sender_json(const EncodedVideoSenderSnapshot& sender) {
  std::ostringstream out;
  out << "{"
      << "\"bound_stream_id\":\"" << json_escape(sender.bound_stream_id) << "\","
      << "\"sender_state\":\"" << json_escape(sender.sender_state) << "\","
      << "\"codec\":\"" << json_escape(sender.codec) << "\","
      << "\"has_pending_encoded_unit\":" << bool_to_json(sender.has_pending_encoded_unit) << ','
      << "\"codec_config_seen\":" << bool_to_json(sender.codec_config_seen) << ','
      << "\"ready_for_video_track\":" << bool_to_json(sender.ready_for_video_track) << ','
      << "\"video_track_exists\":" << bool_to_json(sender.video_track_exists) << ','
      << "\"video_track_open\":" << bool_to_json(sender.video_track_open) << ','
      << "\"h264_delivery_active\":" << bool_to_json(sender.h264_delivery_active) << ','
      << "\"keyframe_seen\":" << bool_to_json(sender.keyframe_seen) << ','
      << "\"cached_codec_config_available\":" << bool_to_json(sender.cached_codec_config_available) << ','
      << "\"cached_idr_available\":" << bool_to_json(sender.cached_idr_available) << ','
      << "\"first_decodable_frame_sent\":" << bool_to_json(sender.first_decodable_frame_sent) << ','
      << "\"startup_sequence_sent\":" << bool_to_json(sender.startup_sequence_sent) << ','
      << "\"delivered_units\":" << sender.delivered_units << ','
      << "\"duplicate_units_skipped\":" << sender.duplicate_units_skipped << ','
      << "\"failed_units\":" << sender.failed_units << ','
      << "\"packets_attempted\":" << sender.packets_attempted << ','
      << "\"packets_sent_after_track_open\":" << sender.packets_sent_after_track_open << ','
      << "\"startup_packets_sent\":" << sender.startup_packets_sent << ','
      << "\"packetization_failures\":" << sender.packetization_failures << ','
      << "\"track_closed_events\":" << sender.track_closed_events << ','
      << "\"send_failures\":" << sender.send_failures << ','
      << "\"last_delivered_sequence_id\":" << sender.last_delivered_sequence_id << ','
      << "\"last_delivered_timestamp_ns\":" << sender.last_delivered_timestamp_ns << ','
      << "\"last_delivered_size_bytes\":" << sender.last_delivered_size_bytes << ','
      << "\"last_delivered_keyframe\":" << bool_to_json(sender.last_delivered_keyframe) << ','
      << "\"last_delivered_codec_config\":" << bool_to_json(sender.last_delivered_codec_config) << ','
      << "\"last_contains_sps\":" << bool_to_json(sender.last_contains_sps) << ','
      << "\"last_contains_pps\":" << bool_to_json(sender.last_contains_pps) << ','
      << "\"last_contains_idr\":" << bool_to_json(sender.last_contains_idr) << ','
      << "\"last_contains_non_idr\":" << bool_to_json(sender.last_contains_non_idr) << ','
      << "\"negotiated_h264_payload_type\":" << sender.negotiated_h264_payload_type << ','
      << "\"negotiated_h264_fmtp\":\"" << json_escape(sender.negotiated_h264_fmtp) << "\","
      << "\"last_packetization_status\":\"" << json_escape(sender.last_packetization_status) << "\","
      << "\"last_send_error\":\"" << json_escape(sender.last_send_error) << "\","
      << "\"video_mid\":\"" << json_escape(sender.video_mid) << "\"}";
  return out.str();
}

std::string signaling_session_json(const SignalingSession& session) {
  const auto& sender = session.media_source.encoded_sender;
  std::ostringstream out;
  out << "{"
      << "\"session_generation\":" << session.session_generation << ','
      << "\"stream_id\":\"" << json_escape(session.stream_id) << "\","
      << "\"offer_sdp\":\"" << json_escape(session.offer_sdp) << "\","
      << "\"answer_sdp\":\"" << json_escape(session.answer_sdp) << "\","
      << "\"last_remote_candidate\":\"" << json_escape(session.last_remote_candidate) << "\","
      << "\"last_local_candidate\":\"" << json_escape(session.last_local_candidate) << "\","
      << "\"peer_state\":\"" << json_escape(session.peer_state) << "\","
      << "\"media_bridge_state\":\"" << json_escape(session.media_source.bridge_state) << "\","
      << "\"preferred_media_path\":\"" << json_escape(session.media_source.preferred_media_path) << "\","
      << "\"latest_snapshot_available\":" << bool_to_json(session.media_source.latest_snapshot_available) << ','
      << "\"latest_snapshot_frame_id\":" << session.media_source.latest_snapshot_frame_id << ','
      << "\"latest_snapshot_timestamp_ns\":" << session.media_source.latest_snapshot_timestamp_ns << ','
      << "\"latest_snapshot_width\":" << session.media_source.latest_snapshot_width << ','
      << "\"latest_snapshot_height\":" << session.media_source.latest_snapshot_height << ','
      << "\"latest_encoded_access_unit_available\":" << bool_to_json(session.media_source.latest_encoded_access_unit_available) << ','
      << "\"latest_encoded_codec\":\"" << json_escape(session.media_source.latest_encoded_codec) << "\","
      << "\"latest_encoded_timestamp_ns\":" << session.media_source.latest_encoded_timestamp_ns << ','
      << "\"latest_encoded_sequence_id\":" << session.media_source.latest_encoded_sequence_id << ','
      << "\"latest_encoded_size_bytes\":" << session.media_source.latest_encoded_size_bytes << ','
      << "\"latest_encoded_keyframe\":" << bool_to_json(session.media_source.latest_encoded_keyframe) << ','
      << "\"latest_encoded_codec_config\":" << bool_to_json(session.media_source.latest_encoded_codec_config) << ','
      << "\"encoded_sender_state\":\"" << json_escape(sender.sender_state) << "\","
      << "\"encoded_sender_codec\":\"" << json_escape(sender.codec) << "\","
      << "\"encoded_sender_has_pending_encoded_unit\":" << bool_to_json(sender.has_pending_encoded_unit) << ','
      << "\"encoded_sender_codec_config_seen\":" << bool_to_json(sender.codec_config_seen) << ','
      << "\"encoded_sender_ready_for_video_track\":" << bool_to_json(sender.ready_for_video_track) << ','
      << "\"encoded_sender_video_track_exists\":" << bool_to_json(sender.video_track_exists) << ','
      << "\"encoded_sender_video_track_open\":" << bool_to_json(sender.video_track_open) << ','
      << "\"encoded_sender_h264_delivery_active\":" << bool_to_json(sender.h264_delivery_active) << ','
      << "\"encoded_sender_keyframe_seen\":" << bool_to_json(sender.keyframe_seen) << ','
      << "\"encoded_sender_cached_codec_config_available\":" << bool_to_json(sender.cached_codec_config_available) << ','
      << "\"encoded_sender_cached_idr_available\":" << bool_to_json(sender.cached_idr_available) << ','
      << "\"encoded_sender_first_decodable_frame_sent\":" << bool_to_json(sender.first_decodable_frame_sent) << ','
      << "\"encoded_sender_startup_sequence_sent\":" << bool_to_json(sender.startup_sequence_sent) << ','
      << "\"encoded_sender_delivered_units\":" << sender.delivered_units << ','
      << "\"encoded_sender_duplicate_units_skipped\":" << sender.duplicate_units_skipped << ','
      << "\"encoded_sender_failed_units\":" << sender.failed_units << ','
      << "\"encoded_sender_packets_attempted\":" << sender.packets_attempted << ','
      << "\"encoded_sender_packets_sent_after_track_open\":" << sender.packets_sent_after_track_open << ','
      << "\"encoded_sender_startup_packets_sent\":" << sender.startup_packets_sent << ','
      << "\"encoded_sender_packetization_failures\":" << sender.packetization_failures << ','
      << "\"encoded_sender_track_closed_events\":" << sender.track_closed_events << ','
      << "\"encoded_sender_send_failures\":" << sender.send_failures << ','
      << "\"encoded_sender_last_delivered_sequence_id\":" << sender.last_delivered_sequence_id << ','
      << "\"encoded_sender_last_delivered_timestamp_ns\":" << sender.last_delivered_timestamp_ns << ','
      << "\"encoded_sender_last_delivered_size_bytes\":" << sender.last_delivered_size_bytes << ','
      << "\"encoded_sender_last_delivered_keyframe\":" << bool_to_json(sender.last_delivered_keyframe) << ','
      << "\"encoded_sender_last_delivered_codec_config\":" << bool_to_json(sender.last_delivered_codec_config) << ','
      << "\"encoded_sender_last_contains_sps\":" << bool_to_json(sender.last_contains_sps) << ','
      << "\"encoded_sender_last_contains_pps\":" << bool_to_json(sender.last_contains_pps) << ','
      << "\"encoded_sender_last_contains_idr\":" << bool_to_json(sender.last_contains_idr) << ','
      << "\"encoded_sender_last_contains_non_idr\":" << bool_to_json(sender.last_contains_non_idr) << ','
      << "\"encoded_sender_negotiated_h264_payload_type\":" << sender.negotiated_h264_payload_type << ','
      << "\"encoded_sender_negotiated_h264_fmtp\":\"" << json_escape(sender.negotiated_h264_fmtp) << "\","
      << "\"encoded_sender_last_packetization_status\":\"" << json_escape(sender.last_packetization_status) << "\","
      << "\"encoded_sender_last_send_error\":\"" << json_escape(sender.last_send_error) << "\","
      << "\"encoded_sender_video_mid\":\"" << json_escape(sender.video_mid) << "\","
      << "\"encoded_sender\":" << encoded_sender_json(sender)
      << '}';
  return out.str();
}

std::string sender_observability_json(const SenderObservabilitySnapshot& sender) {
  std::ostringstream out;
  out << '{'
      << "\"session_present\":" << bool_to_json(sender.session_present) << ','
      << "\"session_generation\":" << sender.session_generation << ','
      << "\"peer_state\":\"" << json_escape(sender.peer_state) << "\","
      << "\"sender_state\":\"" << json_escape(sender.sender_state) << "\","
      << "\"last_packetization_status\":\"" << json_escape(sender.last_packetization_status) << "\","
      << "\"bound_stream_id\":\"" << json_escape(sender.bound_stream_id) << "\","
      << "\"video_track_exists\":" << bool_to_json(sender.video_track_exists) << ','
      << "\"video_track_open\":" << bool_to_json(sender.video_track_open) << ','
      << "\"startup_sequence_sent\":" << bool_to_json(sender.startup_sequence_sent) << ','
      << "\"first_decodable_frame_sent\":" << bool_to_json(sender.first_decodable_frame_sent) << ','
      << "\"access_units_considered_for_send\":" << sender.access_units_considered_for_send << ','
      << "\"packets_emitted\":" << sender.packets_emitted << ','
      << "\"startup_sequence_injections\":" << sender.startup_sequence_injections << ','
      << "\"duplicate_units_skipped\":" << sender.duplicate_units_skipped << ','
      << "\"packetization_failures\":" << sender.packetization_failures << ','
      << "\"track_closed_events\":" << sender.track_closed_events << ','
      << "\"send_failures\":" << sender.send_failures << ','
      << "\"last_send_error\":\"" << json_escape(sender.last_send_error) << "\"}";
  return out.str();
}

std::string stream_snapshot_json(const StreamObservabilitySnapshot& snapshot) {
  std::ostringstream out;
  out << '{'
      << "\"stream_id\":\"" << json_escape(snapshot.info.stream_id) << "\","
      << "\"label\":\"" << json_escape(snapshot.info.label) << "\","
      << "\"configured_width\":" << snapshot.info.config.width << ','
      << "\"configured_height\":" << snapshot.info.config.height << ','
      << "\"configured_fps\":" << snapshot.info.config.nominal_fps << ','
      << "\"input_pixel_format\":\"" << json_escape(to_string(snapshot.info.config.input_pixel_format)) << "\","
      << "\"active\":" << bool_to_json(snapshot.info.active) << ','
      << "\"frames_received\":" << snapshot.info.frames_received << ','
      << "\"frames_transformed\":" << snapshot.info.frames_transformed << ','
      << "\"frames_dropped\":" << snapshot.info.frames_dropped << ','
      << "\"access_units_received\":" << snapshot.info.access_units_received << ','
      << "\"last_input_timestamp_ns\":" << snapshot.info.last_input_timestamp_ns << ','
      << "\"last_output_timestamp_ns\":" << snapshot.info.last_output_timestamp_ns << ','
      << "\"last_frame_timestamp_ns\":" << snapshot.info.last_frame_timestamp_ns << ','
      << "\"last_frame_id\":" << snapshot.info.last_frame_id << ','
      << "\"latest_raw_frame_exists\":" << bool_to_json(snapshot.latest_raw_frame_exists) << ','
      << "\"latest_raw_width\":" << snapshot.latest_raw_width << ','
      << "\"latest_raw_height\":" << snapshot.latest_raw_height << ','
      << "\"latest_raw_pixel_format\":\"" << json_escape(snapshot.latest_raw_pixel_format) << "\","
      << "\"latest_raw_timestamp_ns\":" << snapshot.latest_raw_timestamp_ns << ','
      << "\"latest_raw_frame_id\":" << snapshot.latest_raw_frame_id << ','
      << "\"latest_encoded_access_unit_exists\":" << bool_to_json(snapshot.latest_encoded_access_unit_exists) << ','
      << "\"latest_encoded_codec\":\"" << json_escape(snapshot.latest_encoded_codec) << "\","
      << "\"latest_encoded_timestamp_ns\":" << snapshot.latest_encoded_timestamp_ns << ','
      << "\"latest_encoded_sequence_id\":" << snapshot.latest_encoded_sequence_id << ','
      << "\"latest_encoded_size_bytes\":" << snapshot.latest_encoded_size_bytes << ','
      << "\"latest_encoded_keyframe\":" << bool_to_json(snapshot.latest_encoded_keyframe) << ','
      << "\"latest_encoded_codec_config\":" << bool_to_json(snapshot.latest_encoded_codec_config) << ','
      << "\"sender\":" << sender_observability_json(snapshot.sender)
      << '}';
  return out.str();
}

std::string output_config_json(const StreamOutputConfig& cfg) {
  std::ostringstream out;
  out << "{"
      << "\"display_mode\":\"" << to_string(cfg.display_mode) << "\","
      << "\"mirrored\":" << bool_to_json(cfg.mirrored) << ","
      << "\"rotation_degrees\":" << cfg.rotation_degrees << ","
      << "\"palette_min\":" << cfg.palette_min << ","
      << "\"palette_max\":" << cfg.palette_max << "}";
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
                   }),
        http_server_(std::make_unique<HttpApiServer>(config_.http_host, config_.http_port)) {}

  bool start() {
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

  HttpResponse handle_http(const HttpRequest& request) {
    if (is_api_path(request.path) && request.method == "OPTIONS") {
      HttpResponse response{204, "", "application/json"};
      apply_api_cors_headers(request, response);
      return response;
    }

    auto finalize = [&](HttpResponse response) {
      apply_api_cors_headers(request, response);
      return response;
    };

    if (request.method == "GET" && request.path == "/api/video/debug/stats") {
      auto streams = core_.list_stream_snapshots();
      auto sessions = signaling_.list_sessions();
      std::unordered_map<std::string, SignalingSession> sessions_by_stream;
      for (const auto& session : sessions) {
        sessions_by_stream[session.stream_id] = session;
      }
      std::ostringstream out;
      out << "{\"stream_count\":" << streams.size() << ",\"session_count\":" << sessions.size() << ",\"streams\":[";
      for (size_t i = 0; i < streams.size(); ++i) {
        auto snapshot = streams[i];
        const auto session_it = sessions_by_stream.find(snapshot.info.stream_id);
        if (session_it != sessions_by_stream.end()) {
          const auto& sender = session_it->second.media_source.encoded_sender;
          snapshot.sender.session_present = true;
          snapshot.sender.session_generation = session_it->second.session_generation;
          snapshot.sender.peer_state = session_it->second.peer_state;
          snapshot.sender.sender_state = sender.sender_state;
          snapshot.sender.last_packetization_status = sender.last_packetization_status;
          snapshot.sender.bound_stream_id = sender.bound_stream_id;
          snapshot.sender.video_track_exists = sender.video_track_exists;
          snapshot.sender.video_track_open = sender.video_track_open;
          snapshot.sender.startup_sequence_sent = sender.startup_sequence_sent;
          snapshot.sender.first_decodable_frame_sent = sender.first_decodable_frame_sent;
          snapshot.sender.access_units_considered_for_send = sender.delivered_units;
          snapshot.sender.packets_emitted = sender.packets_attempted;
          snapshot.sender.startup_sequence_injections = sender.startup_sequence_sent ? 1u : 0u;
          snapshot.sender.duplicate_units_skipped = sender.duplicate_units_skipped;
          snapshot.sender.packetization_failures = sender.packetization_failures;
          snapshot.sender.track_closed_events = sender.track_closed_events;
          snapshot.sender.send_failures = sender.send_failures;
          snapshot.sender.last_send_error = sender.last_send_error;
        }
        if (i != 0) out << ',';
        out << stream_snapshot_json(snapshot);
      }
      out << "],\"sessions\":[";
      for (size_t i = 0; i < sessions.size(); ++i) {
        if (i != 0) out << ',';
        out << signaling_session_json(sessions[i]);
      }
      out << "]}";
      return finalize(HttpResponse{200, out.str(), "application/json"});
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

    if (request.path.find("/api/video/streams/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/streams/").size());
      const auto output_pos = tail.find("/output");
      const auto frame_pos = tail.find("/frame");
      const bool is_frame_request = frame_pos != std::string::npos && (frame_pos + 6) == tail.size();
      if (request.method == "GET" && output_pos == std::string::npos && !is_frame_request) {
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
            << "\"active\":" << bool_to_json(info->active) << '}';
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

      if (output_pos != std::string::npos) {
        const std::string stream_id = tail.substr(0, output_pos);
        if (request.method == "GET") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return finalize(json_error(404, "stream not found"));
          }
          return finalize(HttpResponse{200, output_config_json(*cfg), "application/json"});
        }

        if (request.method == "PUT") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return finalize(json_error(404, "stream not found"));
          }

          if (request.body.empty()) {
            return finalize(json_error(400, "invalid request body"));
          }

          std::unordered_map<std::string, JsonValue> body_values;
          if (!parse_flat_json_object(request.body, body_values)) {
            return finalize(json_error(400, "invalid request body"));
          }

          const auto mode_it = body_values.find("display_mode");
          const auto mirrored_it = body_values.find("mirrored");
          const auto rotation_it = body_values.find("rotation_degrees");
          const auto min_it = body_values.find("palette_min");
          const auto max_it = body_values.find("palette_max");

          if (mode_it == body_values.end() || mirrored_it == body_values.end() ||
              rotation_it == body_values.end() || min_it == body_values.end() ||
              max_it == body_values.end()) {
            return finalize(json_error(400, "invalid request body"));
          }

          if (mode_it->second.type != JsonValue::Type::String ||
              mirrored_it->second.type != JsonValue::Type::Bool ||
              rotation_it->second.type != JsonValue::Type::Number ||
              min_it->second.type != JsonValue::Type::Number ||
              max_it->second.type != JsonValue::Type::Number) {
            return finalize(json_error(400, "invalid request body"));
          }

          const auto parsed_mode = video_display_mode_from_string(mode_it->second.string_value.c_str());
          if (!parsed_mode.has_value()) {
            return finalize(json_error(400, "invalid display_mode"));
          }

          StreamOutputConfig updated = *cfg;
          updated.display_mode = *parsed_mode;
          updated.mirrored = mirrored_it->second.bool_value;
          updated.rotation_degrees = static_cast<int>(rotation_it->second.number_value);
          updated.palette_min = static_cast<float>(min_it->second.number_value);
          updated.palette_max = static_cast<float>(max_it->second.number_value);

          if (!core_.set_stream_output_config(stream_id, updated)) {
            return finalize(json_error(400, "invalid output config"));
          }

          return finalize(HttpResponse{200, output_config_json(updated), "application/json"});
        }
      }
    }

    if (request.path.find("/api/video/signaling/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/signaling/").size());
      const auto slash = tail.find('/');
      const std::string stream_id = slash == std::string::npos ? tail : tail.substr(0, slash);
      const std::string action = slash == std::string::npos ? "offer" : tail.substr(slash + 1);

      if (request.method == "POST" && action == "offer") {
        std::string error_message;
        if (!signaling_.set_offer(stream_id, request.body, &error_message)) {
          return finalize(json_error(error_message == "stream not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "POST" && action == "answer") {
        std::string error_message;
        if (!signaling_.set_answer(stream_id, request.body, &error_message)) {
          return finalize(json_error(error_message == "session not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "POST" && action == "candidate") {
        std::string error_message;
        if (!signaling_.add_ice_candidate(stream_id, request.body, &error_message)) {
          return finalize(json_error(error_message == "session not found" ? 404 : 400, error_message.c_str()));
        }
        return finalize(HttpResponse{200, "{\"ok\":true}", "application/json"});
      }
      if (request.method == "GET" && action == "session") {
        auto session = signaling_.get_session(stream_id);
        if (!session.has_value()) {
          return finalize(json_error(404, "session not found"));
        }
        return finalize(HttpResponse{200, signaling_session_json(*session), "application/json"});
      }
    }

    return finalize(json_error(404, "not found"));
  }

  WebRtcVideoServerConfig config_;
  VideoServerCore core_;
  SignalingServer signaling_;
  std::unique_ptr<HttpApiServer> http_server_;
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

WebRtcHttpResponse WebRtcVideoServer::handle_http_request_for_test(const std::string& method,
                                                                   const std::string& path,
                                                                   const std::string& body,
                                                                   std::unordered_map<std::string, std::string> headers) {
  const HttpResponse response = impl_->handle_http(HttpRequest{method, path, body, std::move(headers)});
  return WebRtcHttpResponse{response.status, response.body, response.headers};
}

}  // namespace video_server
