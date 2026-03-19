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

#include "video_server/webrtc_video_server.h"
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
  std::cout << "Usage: " << argv0 << " [--host HOST] [--port PORT] [--stream-id ID] [--width W] [--height H] [--fps FPS] --ffmpeg PATH\n";
}

std::optional<Options> parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
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
      std::cerr << "Unknown argument: " << arg << "\n";
      return std::nullopt;
    }
  }

  if (options.ffmpeg_path.empty()) {
    std::cerr << "--ffmpeg is required\n";
    return std::nullopt;
  }
  return options;
}

size_t start_code_size(const std::vector<uint8_t>& buffer, size_t index) {
  if (index + 3 < buffer.size() && buffer[index] == 0x00 && buffer[index + 1] == 0x00 && buffer[index + 2] == 0x00 &&
      buffer[index + 3] == 0x01) {
    return 4;
  }
  if (index + 2 < buffer.size() && buffer[index] == 0x00 && buffer[index + 1] == 0x00 && buffer[index + 2] == 0x01) {
    return 3;
  }
  return 0;
}

int nal_type(const std::vector<uint8_t>& nal) {
  const size_t prefix = start_code_size(nal, 0);
  if (prefix == 0 || prefix >= nal.size()) {
    return -1;
  }
  return nal[prefix] & 0x1F;
}

bool contains_nal_type(const std::vector<uint8_t>& bytes, int type) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    const size_t prefix = start_code_size(bytes, i);
    if (prefix == 0) {
      continue;
    }
    if (i + prefix < bytes.size() && static_cast<int>(bytes[i + prefix] & 0x1F) == type) {
      return true;
    }
    i += prefix;
  }
  return false;
}

void emit_access_unit(video_server::WebRtcVideoServer& server, const Options& options, const std::vector<uint8_t>& bytes,
                      uint64_t sequence_id, uint64_t timestamp_ns) {
  if (bytes.empty()) {
    return;
  }
  video_server::EncodedAccessUnitView view{};
  view.data = bytes.data();
  view.size_bytes = bytes.size();
  view.codec = video_server::VideoCodec::H264;
  view.timestamp_ns = timestamp_ns;
  view.keyframe = contains_nal_type(bytes, 5);
  view.codec_config = contains_nal_type(bytes, 7) && contains_nal_type(bytes, 8) && !view.keyframe;
  if (!server.push_access_unit(options.stream_id, view)) {
    std::cerr << "Failed to push H264 access unit sequence=" << sequence_id << " size=" << bytes.size() << "\n";
  }
}

void run_ffmpeg_h264_loop(video_server::WebRtcVideoServer& server, const Options& options, std::atomic<bool>& stop_requested) {
  std::ostringstream command;
  command << '"' << options.ffmpeg_path << '"'
          << " -hide_banner -loglevel error"
          << " -f lavfi -re -i testsrc=size=" << options.width << 'x' << options.height << ":rate=" << options.fps
          << " -pix_fmt yuv420p -an -c:v libx264 -preset ultrafast -tune zerolatency"
          << " -profile:v baseline -bf 0 -g " << static_cast<int>(options.fps)
          << " -keyint_min " << static_cast<int>(options.fps)
          << " -x264-params aud=1:repeat-headers=1:scenecut=0"
          << " -f h264 -";

  std::cout << "[smoke-server] launching ffmpeg: " << command.str() << "\n";
  FILE* pipe = popen(command.str().c_str(), "r");
  if (!pipe) {
    std::cerr << "Failed to launch ffmpeg\n";
    return;
  }

  std::vector<uint8_t> buffer;
  std::vector<uint8_t> current_access_unit;
  uint64_t sequence_id = 1;
  const auto start = Clock::now();
  std::array<uint8_t, 8192> chunk{};

  auto flush_nal = [&](const std::vector<uint8_t>& nal) {
    if (nal.empty()) {
      return;
    }
    const int type = nal_type(nal);
    if (type == 9 && !current_access_unit.empty()) {
      const uint64_t elapsed_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
      emit_access_unit(server, options, current_access_unit, sequence_id++, elapsed_ns);
      current_access_unit.clear();
    }
    current_access_unit.insert(current_access_unit.end(), nal.begin(), nal.end());
  };

  while (!stop_requested.load()) {
    const size_t bytes_read = std::fread(chunk.data(), 1, chunk.size(), pipe);
    if (bytes_read == 0) {
      if (std::feof(pipe)) {
        break;
      }
      if (std::ferror(pipe)) {
        std::cerr << "ffmpeg pipe read error\n";
        break;
      }
      continue;
    }

    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(bytes_read));

    std::vector<size_t> starts;
    for (size_t i = 0; i < buffer.size();) {
      const size_t prefix = start_code_size(buffer, i);
      if (prefix > 0) {
        starts.push_back(i);
        i += prefix;
      } else {
        ++i;
      }
    }

    if (starts.size() < 2) {
      continue;
    }

    for (size_t i = 0; i + 1 < starts.size(); ++i) {
      const size_t begin = starts[i];
      const size_t end = starts[i + 1];
      flush_nal(std::vector<uint8_t>(buffer.begin() + static_cast<std::ptrdiff_t>(begin),
                                    buffer.begin() + static_cast<std::ptrdiff_t>(end)));
    }

    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(starts.back()));
  }

  if (!buffer.empty()) {
    flush_nal(buffer);
  }
  if (!current_access_unit.empty()) {
    const uint64_t elapsed_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    emit_access_unit(server, options, current_access_unit, sequence_id++, elapsed_ns);
  }

  pclose(pipe);
}

}  // namespace

int main(int argc, char** argv) {
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
    std::cerr << "Failed to register stream: " << options->stream_id << "\n";
    return 1;
  }
  if (!server.start()) {
    std::cerr << "Failed to start WebRTC video server\n";
    return 1;
  }

  std::cout << "[smoke-server] started HTTP/WebRTC server on http://" << options->host << ':' << options->port
            << " stream_id=" << options->stream_id << "\n";

  std::atomic<bool> stop_requested{false};
  video_server::SyntheticFrameGenerator generator(stream_config);

  std::thread frame_thread([&]() {
    const auto frame_interval = std::chrono::duration<double>(1.0 / options->fps);
    while (!stop_requested.load()) {
      const auto frame = generator.next_frame();
      if (!server.push_frame(options->stream_id, frame)) {
        std::cerr << "Failed to push synthetic frame\n";
      }
      std::this_thread::sleep_for(frame_interval);
    }
  });

  std::thread h264_thread([&]() { run_ffmpeg_h264_loop(server, *options, stop_requested); });

  std::cout << "[smoke-server] press ENTER to stop\n";
  std::string line;
  std::getline(std::cin, line);
  stop_requested = true;

  if (h264_thread.joinable()) {
    h264_thread.join();
  }
  if (frame_thread.joinable()) {
    frame_thread.join();
  }

  server.stop();
  std::cout << "[smoke-server] stopped\n";
  return 0;
}
