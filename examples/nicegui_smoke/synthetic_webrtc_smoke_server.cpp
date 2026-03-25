#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "video_server/raw_video_pipeline.h"
#include "video_server/webrtc_video_server.h"
#include "../../src/transforms/display_transform.h"
#include "../../src/webrtc/logging_utils.h"
#include "../../src/testing/synthetic_frame_generator.h"

namespace {

using Clock = std::chrono::steady_clock;

struct StreamSpec {
  std::string stream_id;
  std::string label;
  uint32_t width{0};
  uint32_t height{0};
  double fps{0.0};
  video_server::VideoPixelFormat pixel_format{video_server::VideoPixelFormat::RGB24};
};

struct Options {
  std::string host{"127.0.0.1"};
  uint16_t port{8080};
  bool enable_debug_api{false};
  std::string shared_key;
  std::vector<StreamSpec> streams;
  double duration_seconds{0.0};
  double stats_interval_seconds{5.0};
  bool print_observability_summary{false};
};

bool parse_uint16(const char* value, uint16_t& out) {
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
    return false;
  }
  out = static_cast<uint16_t>(parsed);
  return true;
}

bool parse_uint32(const char* value, uint32_t& out) {
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0 || parsed > 100000) {
    return false;
  }
  out = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_double(const char* value, double& out) {
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || *end != '\0' || parsed <= 0.0 || parsed > 240.0) {
    return false;
  }
  out = parsed;
  return true;
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::stringstream input(value);
  for (std::string part; std::getline(input, part, delimiter);) {
    parts.push_back(part);
  }
  return parts;
}

std::optional<StreamSpec> parse_stream_spec(const std::string& value) {
  const auto parts = split(value, ':');
  if (parts.size() < 4 || parts.size() > 5) {
    return std::nullopt;
  }

  StreamSpec spec;
  spec.stream_id = parts[0];
  if (spec.stream_id.empty()) {
    return std::nullopt;
  }
  if (!parse_uint32(parts[1].c_str(), spec.width) || !parse_uint32(parts[2].c_str(), spec.height) ||
      !parse_double(parts[3].c_str(), spec.fps)) {
    return std::nullopt;
  }
  spec.label = parts.size() == 5 ? parts[4] : ("Synthetic demo stream " + spec.stream_id);
  return spec;
}

std::vector<StreamSpec> default_demo_streams() {
  return {
      StreamSpec{"alpha", "Synthetic demo alpha sweep grayscale", 640, 360, 30.0, video_server::VideoPixelFormat::GRAY8},
      StreamSpec{"bravo", "Synthetic demo bravo orbit", 1280, 720, 30.0, video_server::VideoPixelFormat::RGB24},
      StreamSpec{"charlie", "Synthetic demo charlie checker", 320, 240, 30.0, video_server::VideoPixelFormat::RGB24},
  };
}

void print_usage(const char* argv0) {
  spdlog::info(
      "Usage: {} [--host HOST] [--port PORT] [--enable-debug-api] [--shared-key TOKEN] [--stream-id ID --width W --height H --fps FPS] [--stream ID:W:H:FPS[:LABEL]] [--multi-stream-demo]",
      argv0);
}

std::optional<Options> parse_args(int argc, char** argv) {
  Options options;
  std::optional<std::string> legacy_stream_id;
  std::optional<uint32_t> legacy_width;
  std::optional<uint32_t> legacy_height;
  std::optional<double> legacy_fps;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        spdlog::error("Missing value for {}", name);
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
      if (!value || !parse_uint16(value, options.port)) return std::nullopt;
    } else if (arg == "--enable-debug-api") {
      options.enable_debug_api = true;
    } else if (arg == "--shared-key") {
      const char* value = require_value("--shared-key");
      if (!value) return std::nullopt;
      options.shared_key = value;
    } else if (arg == "--stream-id") {
      const char* value = require_value("--stream-id");
      if (!value) return std::nullopt;
      legacy_stream_id = value;
    } else if (arg == "--width") {
      const char* value = require_value("--width");
      uint32_t parsed = 0;
      if (!value || !parse_uint32(value, parsed)) return std::nullopt;
      legacy_width = parsed;
    } else if (arg == "--height") {
      const char* value = require_value("--height");
      uint32_t parsed = 0;
      if (!value || !parse_uint32(value, parsed)) return std::nullopt;
      legacy_height = parsed;
    } else if (arg == "--fps") {
      const char* value = require_value("--fps");
      double parsed = 0.0;
      if (!value || !parse_double(value, parsed)) return std::nullopt;
      legacy_fps = parsed;
    } else if (arg == "--stream") {
      const char* value = require_value("--stream");
      if (!value) return std::nullopt;
      auto parsed = parse_stream_spec(value);
      if (!parsed.has_value()) {
        spdlog::error("Invalid --stream value: {}", value);
        return std::nullopt;
      }
      options.streams.push_back(*parsed);
    } else if (arg == "--multi-stream-demo") {
      options.streams = default_demo_streams();
    } else if (arg == "--duration-seconds") {
      const char* value = require_value("--duration-seconds");
      if (!value || !parse_double(value, options.duration_seconds)) return std::nullopt;
    } else if (arg == "--stats-interval-seconds") {
      const char* value = require_value("--stats-interval-seconds");
      if (!value || !parse_double(value, options.stats_interval_seconds)) return std::nullopt;
    } else if (arg == "--print-observability-summary") {
      options.print_observability_summary = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      spdlog::error("Unknown argument: {}", arg);
      return std::nullopt;
    }
  }


  if (options.streams.empty()) {
    StreamSpec legacy{legacy_stream_id.value_or("synthetic-h264"),
                      "NiceGUI smoke synthetic H264 stream",
                      legacy_width.value_or(640),
                      legacy_height.value_or(360),
                      legacy_fps.value_or(30.0)};
    options.streams.push_back(std::move(legacy));
  }

  return options;
}

struct RunningStream {
  video_server::StreamConfig config;
  video_server::SyntheticFrameGenerator generator;
  video_server::RawVideoPipelineConfig pipeline_config;
  std::unique_ptr<video_server::IRawVideoPipeline> pipeline;
  std::thread frame_thread;
  std::string pipeline_error;
  uint64_t pipeline_config_generation{0};

  explicit RunningStream(const StreamSpec& spec)
      : config{spec.stream_id, spec.label, spec.width, spec.height, spec.fps, spec.pixel_format},
        generator(config) {}
};

video_server::RawVideoPipelineConfig make_pipeline_config(const RunningStream& stream,
                                                          const video_server::StreamOutputConfig& output_config,
                                                          uint32_t width,
                                                          uint32_t height) {
  video_server::RawVideoPipelineConfig config;
  config.input_width = width;
  config.input_height = height;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = stream.config.nominal_fps;
  if (output_config.output_fps > 0.0) {
    config.output_fps = output_config.output_fps;
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  video_server::ensure_default_logging_config();
  const auto options = parse_args(argc, argv);
  if (!options.has_value()) {
    print_usage(argv[0]);
    return 1;
  }

  auto server_config = video_server::WebRtcVideoServerConfig{};
  server_config.http_host = options->host;
  server_config.http_port = options->port;
  server_config.enable_http_api = true;
  server_config.enable_debug_api = options->enable_debug_api;
  if (!options->shared_key.empty()) {
    server_config.enable_shared_key_auth = true;
    server_config.shared_key = options->shared_key;
  }
  video_server::WebRtcVideoServer server(server_config);

  std::vector<std::unique_ptr<RunningStream>> streams;
  streams.reserve(options->streams.size());
  for (const auto& spec : options->streams) {
    streams.push_back(std::make_unique<RunningStream>(spec));
    if (!server.register_stream(streams.back()->config)) {
      spdlog::error("Failed to register stream: {}", streams.back()->config.stream_id);
      return 1;
    }
  }

  if (!server.start()) {
    spdlog::error("Failed to start WebRTC video server");
    return 1;
  }

  std::atomic<bool> stop_requested{false};
  const auto run_started_at = Clock::now();

  for (auto& stream : streams) {
    stream->frame_thread = std::thread([&server, &stop_requested, stream = stream.get()]() {
      const auto frame_interval = std::chrono::duration<double>(1.0 / stream->config.nominal_fps);
      while (!stop_requested.load()) {
        const auto raw_frame = stream->generator.next_frame();
        if (!server.push_frame(stream->config.stream_id, raw_frame)) {
          spdlog::warn("Failed to push synthetic frame for {}", stream->config.stream_id);
        }

        const auto output_config = server.get_stream_output_config(stream->config.stream_id);
        if (!output_config.has_value()) {
          spdlog::error("Failed to load stream output config for {}", stream->config.stream_id);
          stop_requested = true;
          break;
        }

        video_server::RgbImage transformed;
        if (!video_server::apply_display_transform(raw_frame, *output_config, transformed)) {
          spdlog::error("Failed to apply display transform for {} generation={}", stream->config.stream_id,
                        output_config->config_generation);
          stop_requested = true;
          break;
        }

        if (!stream->pipeline || stream->pipeline_config_generation != output_config->config_generation) {
          if (stream->pipeline) {
            stream->pipeline->stop();
            stream->pipeline.reset();
          }
          stream->pipeline_config = make_pipeline_config(*stream, *output_config, transformed.width, transformed.height);
          stream->pipeline = video_server::make_raw_to_h264_pipeline_for_server(stream->config.stream_id, stream->pipeline_config, server);
          if (!stream->pipeline->start(&stream->pipeline_error)) {
            spdlog::error("Failed to start raw-to-H264 pipeline for {} generation={}: {}", stream->config.stream_id,
                          output_config->config_generation, stream->pipeline_error);
            stop_requested = true;
            break;
          }
          stream->pipeline_config_generation = output_config->config_generation;
          spdlog::info(
              "[smoke-server] restarted encoded pipeline stream={} generation={} transformed={}x{} output_fps={}",
              stream->config.stream_id, stream->pipeline_config_generation, transformed.width, transformed.height,
              output_config->output_fps);
        }

        const video_server::VideoFrameView transformed_frame{
            transformed.rgb.data(), transformed.width, transformed.height, transformed.width * 3u,
            video_server::VideoPixelFormat::RGB24, raw_frame.timestamp_ns, raw_frame.frame_id};
        if (!stream->pipeline->push_frame(transformed_frame, &stream->pipeline_error)) {
          spdlog::error("Failed to push transformed frame into H264 pipeline for {} generation={}: {}",
                        stream->config.stream_id, stream->pipeline_config_generation, stream->pipeline_error);
          stop_requested = true;
          break;
        }

        std::this_thread::sleep_for(frame_interval);
      }
    });

    spdlog::info("[smoke-server] stream={} label='{}' {}x{} @ {} fps input={}", stream->config.stream_id,
                 stream->config.label, stream->config.width, stream->config.height, stream->config.nominal_fps,
                 video_server::to_string(stream->config.input_pixel_format));
  }

  spdlog::info("[smoke-server] started HTTP/WebRTC server on http://{}:{} with {} stream(s)", options->host,
               options->port, streams.size());
  if (options->duration_seconds > 0.0) {
    spdlog::info("[smoke-server] soak mode active for {:.2f}s (summary={}, stats every {:.2f}s)",
                 options->duration_seconds, options->print_observability_summary, options->stats_interval_seconds);
    auto next_stats_at = run_started_at + std::chrono::duration<double>(options->stats_interval_seconds);
    const auto stop_at = run_started_at + std::chrono::duration<double>(options->duration_seconds);
    while (!stop_requested.load() && Clock::now() < stop_at) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (options->print_observability_summary && Clock::now() >= next_stats_at) {
        const auto snapshot = server.get_debug_snapshot();
        spdlog::info("[smoke-server] observability summary streams={} active_sessions={}", snapshot.stream_count,
                     snapshot.active_session_count);
        for (const auto& stream : snapshot.streams) {
          const auto& session = stream.current_session;
          spdlog::info(
              "[smoke-server] stream={} raw={} encoded={} au={} sender={} packets={} startup_injections={} track_closed={}",
              stream.stream_id, stream.latest_raw_frame_available, stream.latest_encoded_access_unit_available,
              stream.total_access_units_received, session ? session->sender_state : std::string("no-session"),
              session ? session->counters.packets_sent_after_track_open : 0,
              session ? session->counters.startup_sequence_injections : 0,
              session ? session->counters.track_closed_events : 0);
        }
        next_stats_at = Clock::now() + std::chrono::duration<double>(options->stats_interval_seconds);
      }
    }
    stop_requested = true;
  } else {
    spdlog::info("[smoke-server] press ENTER to stop");
    std::string line;
    std::getline(std::cin, line);
    stop_requested = true;
  }

  for (auto& stream : streams) {
    if (stream->frame_thread.joinable()) {
      stream->frame_thread.join();
    }
  }
  for (auto& stream : streams) {
    if (stream->pipeline) {
      stream->pipeline->stop();
    }
  }

  server.stop();
  spdlog::info("[smoke-server] stopped");
  return 0;
}
