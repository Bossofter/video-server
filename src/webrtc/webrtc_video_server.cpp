#include "video_server/webrtc_video_server.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>

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

bool extract_json_string(const std::string& body, const std::string& key, std::string& out) {
  const std::string token = "\"" + key + "\"";
  const auto key_pos = body.find(token);
  if (key_pos == std::string::npos) {
    return false;
  }
  const auto colon = body.find(':', key_pos + token.size());
  if (colon == std::string::npos) {
    return false;
  }
  const auto first_quote = body.find('"', colon + 1);
  if (first_quote == std::string::npos) {
    return false;
  }
  const auto second_quote = body.find('"', first_quote + 1);
  if (second_quote == std::string::npos) {
    return false;
  }
  out = body.substr(first_quote + 1, second_quote - first_quote - 1);
  return true;
}

bool extract_json_bool(const std::string& body, const std::string& key, bool& out) {
  const std::string token = "\"" + key + "\"";
  const auto key_pos = body.find(token);
  if (key_pos == std::string::npos) {
    return false;
  }
  const auto colon = body.find(':', key_pos + token.size());
  if (colon == std::string::npos) {
    return false;
  }
  const auto true_pos = body.find("true", colon + 1);
  const auto false_pos = body.find("false", colon + 1);
  if (true_pos != std::string::npos && (false_pos == std::string::npos || true_pos < false_pos)) {
    out = true;
    return true;
  }
  if (false_pos != std::string::npos) {
    out = false;
    return true;
  }
  return false;
}

template <typename T>
bool extract_json_number(const std::string& body, const std::string& key, T& out) {
  const std::string token = "\"" + key + "\"";
  const auto key_pos = body.find(token);
  if (key_pos == std::string::npos) {
    return false;
  }
  const auto colon = body.find(':', key_pos + token.size());
  if (colon == std::string::npos) {
    return false;
  }

  std::string number;
  for (size_t i = colon + 1; i < body.size(); ++i) {
    const char c = body[i];
    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      number.push_back(c);
    } else if (!number.empty()) {
      break;
    }
  }

  if (number.empty()) {
    return false;
  }

  std::istringstream in(number);
  in >> out;
  return !in.fail();
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
      : config_(config), http_server_(std::make_unique<HttpApiServer>(config.http_port)) {}

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
      return HttpResponse{200, out.str()};
    }

    if (request.path.find("/api/video/streams/") == 0) {
      const std::string tail = request.path.substr(std::string("/api/video/streams/").size());
      const auto output_pos = tail.find("/output");
      if (request.method == "GET" && output_pos == std::string::npos) {
        auto info = core_.get_stream_info(tail);
        if (!info.has_value()) {
          return HttpResponse{404, "{\"error\":\"stream not found\"}"};
        }
        std::ostringstream out;
        out << "{\"stream_id\":\"" << json_escape(info->stream_id) << "\","
            << "\"label\":\"" << json_escape(info->label) << "\","
            << "\"frames_received\":" << info->frames_received << ','
            << "\"frames_transformed\":" << info->frames_transformed << ','
            << "\"frames_dropped\":" << info->frames_dropped << ','
            << "\"access_units_received\":" << info->access_units_received << ','
            << "\"active\":" << bool_to_json(info->active) << '}';
        return HttpResponse{200, out.str()};
      }

      if (output_pos != std::string::npos) {
        const std::string stream_id = tail.substr(0, output_pos);
        if (request.method == "GET") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return HttpResponse{404, "{\"error\":\"stream not found\"}"};
          }
          return HttpResponse{200, output_config_json(*cfg)};
        }

        if (request.method == "PUT") {
          auto cfg = core_.get_stream_output_config(stream_id);
          if (!cfg.has_value()) {
            return HttpResponse{404, "{\"error\":\"stream not found\"}"};
          }

          StreamOutputConfig updated = *cfg;
          std::string mode;
          if (extract_json_string(request.body, "display_mode", mode)) {
            updated.display_mode = video_display_mode_from_string(mode.c_str());
          }
          extract_json_bool(request.body, "mirrored", updated.mirrored);
          extract_json_number(request.body, "rotation_degrees", updated.rotation_degrees);
          extract_json_number(request.body, "palette_min", updated.palette_min);
          extract_json_number(request.body, "palette_max", updated.palette_max);

          if (!core_.set_stream_output_config(stream_id, updated)) {
            return HttpResponse{400, "{\"error\":\"invalid output config\"}"};
          }

          return HttpResponse{200, output_config_json(updated)};
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
        return HttpResponse{200, "{\"ok\":true}"};
      }
      if (request.method == "POST" && action == "answer") {
        signaling_.set_answer(stream_id, request.body);
        return HttpResponse{200, "{\"ok\":true}"};
      }
      if (request.method == "POST" && action == "candidate") {
        signaling_.add_ice_candidate(stream_id, request.body);
        return HttpResponse{200, "{\"ok\":true}"};
      }
      if (request.method == "GET" && action == "session") {
        auto session = signaling_.get_session(stream_id);
        if (!session.has_value()) {
          return HttpResponse{404, "{\"error\":\"session not found\"}"};
        }
        std::ostringstream out;
        out << "{\"stream_id\":\"" << json_escape(session->stream_id) << "\","
            << "\"offer_sdp\":\"" << json_escape(session->offer_sdp) << "\","
            << "\"answer_sdp\":\"" << json_escape(session->answer_sdp) << "\","
            << "\"last_ice_candidate\":\"" << json_escape(session->last_ice_candidate) << "\"}";
        return HttpResponse{200, out.str()};
      }
    }

    return HttpResponse{404, "{\"error\":\"not found\"}"};
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
