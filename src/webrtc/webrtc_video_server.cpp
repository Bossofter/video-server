#include "video_server/webrtc_video_server.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../core/video_server_core.h"
#include "http_api_server.h"
#include "signaling_server.h"
#include "video_server/video_types.h"

namespace video_server {
namespace {

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
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
        http_server_(std::make_unique<HttpApiServer>(config_.http_host, config_.http_port)) {}

  bool start() {
    if (!config_.enable_http_api) {
      return true;
    }
    return http_server_->start([this](const HttpRequest& request) { return this->handle_http(request); });
  }

  void stop() {
    if (http_server_) {
      http_server_->stop();
    }
  }

  HttpResponse handle_http(const HttpRequest& request) {
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
      return HttpResponse{200, out.str(), "application/json"};
    }

    if (request.path.find("/api/video/streams/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/streams/").size());
      const auto output_pos = tail.find("/output");
      if (request.method == "GET" && output_pos == std::string::npos) {
        auto info = core_.get_stream_info(tail);
        if (!info.has_value()) {
          return json_error(404, "stream not found");
        }
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
            << "\"active\":" << bool_to_json(info->active) << '}';
        return HttpResponse{200, out.str(), "application/json"};
      }

      if (output_pos != std::string::npos) {
        const std::string stream_id = tail.substr(0, output_pos);
        if (request.method == "GET") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return json_error(404, "stream not found");
          }
          return HttpResponse{200, output_config_json(*cfg), "application/json"};
        }

        if (request.method == "PUT") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return json_error(404, "stream not found");
          }

          if (request.body.empty()) {
            return json_error(400, "invalid request body");
          }

          std::unordered_map<std::string, JsonValue> body_values;
          if (!parse_flat_json_object(request.body, body_values)) {
            return json_error(400, "invalid request body");
          }

          const auto mode_it = body_values.find("display_mode");
          const auto mirrored_it = body_values.find("mirrored");
          const auto rotation_it = body_values.find("rotation_degrees");
          const auto min_it = body_values.find("palette_min");
          const auto max_it = body_values.find("palette_max");

          if (mode_it == body_values.end() || mirrored_it == body_values.end() ||
              rotation_it == body_values.end() || min_it == body_values.end() ||
              max_it == body_values.end()) {
            return json_error(400, "invalid request body");
          }

          if (mode_it->second.type != JsonValue::Type::String ||
              mirrored_it->second.type != JsonValue::Type::Bool ||
              rotation_it->second.type != JsonValue::Type::Number ||
              min_it->second.type != JsonValue::Type::Number ||
              max_it->second.type != JsonValue::Type::Number) {
            return json_error(400, "invalid request body");
          }

          const auto parsed_mode = video_display_mode_from_string(mode_it->second.string_value.c_str());
          if (!parsed_mode.has_value()) {
            return json_error(400, "invalid display_mode");
          }

          StreamOutputConfig updated = *cfg;
          updated.display_mode = *parsed_mode;
          updated.mirrored = mirrored_it->second.bool_value;
          updated.rotation_degrees = static_cast<int>(rotation_it->second.number_value);
          updated.palette_min = static_cast<float>(min_it->second.number_value);
          updated.palette_max = static_cast<float>(max_it->second.number_value);

          if (!core_.set_stream_output_config(stream_id, updated)) {
            return json_error(400, "invalid output config");
          }

          return HttpResponse{200, output_config_json(updated), "application/json"};
        }
      }
    }

    if (request.path.find("/api/video/signaling/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/signaling/").size());
      const auto slash = tail.find('/');
      const std::string stream_id = slash == std::string::npos ? tail : tail.substr(0, slash);
      const std::string action = slash == std::string::npos ? "offer" : tail.substr(slash + 1);

      if (request.method == "POST" && action == "offer") {
        signaling_.set_offer(stream_id, request.body);
        return HttpResponse{200, "{\"ok\":true}", "application/json"};
      }
      if (request.method == "POST" && action == "answer") {
        signaling_.set_answer(stream_id, request.body);
        return HttpResponse{200, "{\"ok\":true}", "application/json"};
      }
      if (request.method == "POST" && action == "candidate") {
        signaling_.add_ice_candidate(stream_id, request.body);
        return HttpResponse{200, "{\"ok\":true}", "application/json"};
      }
      if (request.method == "GET" && action == "session") {
        auto session = signaling_.get_session(stream_id);
        if (!session.has_value()) {
          return json_error(404, "session not found");
        }
        std::ostringstream out;
        out << "{\"stream_id\":\"" << json_escape(session->stream_id) << "\","
            << "\"offer_sdp\":\"" << json_escape(session->offer_sdp) << "\","
            << "\"answer_sdp\":\"" << json_escape(session->answer_sdp) << "\","
            << "\"last_ice_candidate\":\"" << json_escape(session->last_ice_candidate) << "\"}";
        return HttpResponse{200, out.str(), "application/json"};
      }
    }

    return json_error(404, "not found");
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

bool WebRtcVideoServer::remove_stream(const std::string& stream_id) { return impl_->core_.remove_stream(stream_id); }

bool WebRtcVideoServer::push_frame(const std::string& stream_id, const VideoFrameView& frame) {
  return impl_->core_.push_frame(stream_id, frame);
}

bool WebRtcVideoServer::push_access_unit(const std::string& stream_id,
                                         const EncodedAccessUnitView& access_unit) {
  return impl_->core_.push_access_unit(stream_id, access_unit);
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
                                                                   const std::string& body) {
  const HttpResponse response = impl_->handle_http(HttpRequest{method, path, body});
  return WebRtcHttpResponse{response.status, response.body};
}

}  // namespace video_server
