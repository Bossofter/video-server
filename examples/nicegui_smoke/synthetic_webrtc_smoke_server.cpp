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
};

struct Options {
  std::string host{"127.0.0.1"};
  uint16_t port{8080};
  std::vector<StreamSpec> streams;
  std::string ffmpeg_path;
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
      StreamSpec{"alpha", "Synthetic demo alpha", 640, 360, 15.0},
      StreamSpec{"bravo", "Synthetic demo bravo", 1280, 720, 30.0},
      StreamSpec{"charlie", "Synthetic demo charlie", 320, 240, 10.0},
  };
}

void print_usage(const char* argv0) {
  spdlog::info(
      "Usage: {} [--host HOST] [--port PORT] [--stream-id ID --width W --height H --fps FPS] [--stream ID:W:H:FPS[:LABEL]] [--multi-stream-demo] --ffmpeg PATH",
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
    } else if (arg == "--ffmpeg") {
      const char* value = require_value("--ffmpeg");
      if (!value) return std::nullopt;
      options.ffmpeg_path = value;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      spdlog::error("Unknown argument: {}", arg);
      return std::nullopt;
    }
  }

  if (options.ffmpeg_path.empty()) {
    spdlog::error("--ffmpeg is required");
    return std::nullopt;
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

  explicit RunningStream(const StreamSpec& spec)
      : config{spec.stream_id, spec.label, spec.width, spec.height, spec.fps, video_server::VideoPixelFormat::RGB24},
        generator(config) {}
};

}  // namespace

int main(int argc, char** argv) {
  video_server::ensure_default_logging_config();
  const auto options = parse_args(argc, argv);
  if (!options.has_value()) {
    print_usage(argv[0]);
    return 1;
  }

  const video_server::WebRtcVideoServerConfig server_config{options->host, options->port, true};
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

  for (auto& stream : streams) {
    stream->pipeline_config.ffmpeg_path = options->ffmpeg_path;
    stream->pipeline_config.input_width = stream->config.width;
    stream->pipeline_config.input_height = stream->config.height;
    stream->pipeline_config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
    stream->pipeline_config.input_fps = stream->config.nominal_fps;
    stream->pipeline = video_server::make_raw_to_h264_pipeline_for_server(stream->config.stream_id, stream->pipeline_config, server);

    if (!stream->pipeline->start(&stream->pipeline_error)) {
      spdlog::error("Failed to start raw-to-H264 pipeline for {}: {}", stream->config.stream_id, stream->pipeline_error);
      stop_requested = true;
      break;
    }

    stream->frame_thread = std::thread([&server, &stop_requested, stream = stream.get()]() {
      const auto frame_interval = std::chrono::duration<double>(1.0 / stream->config.nominal_fps);
      while (!stop_requested.load()) {
        const auto frame = stream->generator.next_frame();
        if (!server.push_frame(stream->config.stream_id, frame)) {
          spdlog::warn("Failed to push synthetic frame for {}", stream->config.stream_id);
        }
        if (!stream->pipeline->push_frame(frame, &stream->pipeline_error)) {
          spdlog::error("Failed to push raw frame into H264 pipeline for {}: {}", stream->config.stream_id,
                        stream->pipeline_error);
          stop_requested = true;
          break;
        }
        std::this_thread::sleep_for(frame_interval);
      }
    });

    spdlog::info("[smoke-server] stream={} label='{}' {}x{} @ {} fps", stream->config.stream_id, stream->config.label,
                 stream->config.width, stream->config.height, stream->config.nominal_fps);
  }

  spdlog::info("[smoke-server] started HTTP/WebRTC server on http://{}:{} with {} stream(s)", options->host,
               options->port, streams.size());
  spdlog::info("[smoke-server] press ENTER to stop");

  std::string line;
  std::getline(std::cin, line);
  stop_requested = true;

  for (auto& stream : streams) {
    if (stream->pipeline) {
      stream->pipeline->stop();
    }
  }
  for (auto& stream : streams) {
    if (stream->frame_thread.joinable()) {
      stream->frame_thread.join();
    }
  }

  server.stop();
  spdlog::info("[smoke-server] stopped");
  return 0;
}
