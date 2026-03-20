#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
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

struct Options {
  std::string host{"127.0.0.1"};
  uint16_t port{8080};
  std::string stream_id{"synthetic-h264"};
  uint32_t width{640};
  uint32_t height{360};
  double fps{30.0};
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

void print_usage(const char* argv0) {
  spdlog::info("Usage: {} [--host HOST] [--port PORT] [--stream-id ID] [--width W] [--height H] [--fps FPS] --ffmpeg PATH",
               argv0);
}

std::optional<Options> parse_args(int argc, char** argv) {
  Options options;
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
      options.stream_id = value;
    } else if (arg == "--width") {
      const char* value = require_value("--width");
      if (!value || !parse_uint32(value, options.width)) return std::nullopt;
    } else if (arg == "--height") {
      const char* value = require_value("--height");
      if (!value || !parse_uint32(value, options.height)) return std::nullopt;
    } else if (arg == "--fps") {
      const char* value = require_value("--fps");
      if (!value || !parse_double(value, options.fps)) return std::nullopt;
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
  return options;
}

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
  const video_server::StreamConfig stream_config{options->stream_id,
                                                 "NiceGUI smoke synthetic H264 stream",
                                                 options->width,
                                                 options->height,
                                                 options->fps,
                                                 video_server::VideoPixelFormat::RGB24};

  if (!server.register_stream(stream_config)) {
    spdlog::error("Failed to register stream: {}", options->stream_id);
    return 1;
  }
  if (!server.start()) {
    spdlog::error("Failed to start WebRTC video server");
    return 1;
  }

  spdlog::info("[smoke-server] started HTTP/WebRTC server on http://{}:{} stream_id={}", options->host,
               options->port, options->stream_id);

  std::atomic<bool> stop_requested{false};
  video_server::SyntheticFrameGenerator generator(stream_config);

  video_server::RawVideoPipelineConfig pipeline_config;
  pipeline_config.ffmpeg_path = options->ffmpeg_path;
  pipeline_config.input_width = options->width;
  pipeline_config.input_height = options->height;
  pipeline_config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  pipeline_config.input_fps = options->fps;
  auto pipeline = video_server::make_raw_to_h264_pipeline_for_server(options->stream_id, pipeline_config, server);
  std::string pipeline_error;
  if (!pipeline->start(&pipeline_error)) {
    spdlog::error("Failed to start raw-to-H264 pipeline: {}", pipeline_error);
    stop_requested = true;
  }

  std::thread frame_thread([&]() {
    const auto frame_interval = std::chrono::duration<double>(1.0 / options->fps);
    while (!stop_requested.load()) {
      const auto frame = generator.next_frame();
      if (!server.push_frame(options->stream_id, frame)) {
        spdlog::warn("Failed to push synthetic frame");
      }
      if (!pipeline->push_frame(frame, &pipeline_error)) {
        spdlog::error("Failed to push raw frame into H264 pipeline: {}", pipeline_error);
        stop_requested = true;
        break;
      }
      std::this_thread::sleep_for(frame_interval);
    }
  });


  spdlog::info("[smoke-server] press ENTER to stop");
  std::string line;
  std::getline(std::cin, line);
  stop_requested = true;

  pipeline->stop();
  if (frame_thread.joinable()) {
    frame_thread.join();
  }

  server.stop();
  spdlog::info("[smoke-server] stopped");
  return 0;
}
