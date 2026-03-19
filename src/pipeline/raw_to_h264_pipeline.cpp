#include "video_server/raw_video_pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <string>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace video_server {
namespace {

const char* pixel_format_to_ffmpeg(VideoPixelFormat pixel_format) {
  switch (pixel_format) {
    case VideoPixelFormat::RGB24:
      return "rgb24";
    case VideoPixelFormat::BGR24:
      return "bgr24";
    case VideoPixelFormat::RGBA32:
      return "rgba";
    case VideoPixelFormat::BGRA32:
      return "bgra";
    case VideoPixelFormat::GRAY8:
      return "gray";
    case VideoPixelFormat::NV12:
      return "nv12";
    case VideoPixelFormat::I420:
      return "yuv420p";
  }
  return nullptr;
}

uint32_t bytes_per_pixel(VideoPixelFormat pixel_format) {
  switch (pixel_format) {
    case VideoPixelFormat::RGB24:
    case VideoPixelFormat::BGR24:
      return 3;
    case VideoPixelFormat::RGBA32:
    case VideoPixelFormat::BGRA32:
      return 4;
    case VideoPixelFormat::GRAY8:
      return 1;
    default:
      return 0;
  }
}

bool is_planar_pixel_format(VideoPixelFormat pixel_format) {
  return pixel_format == VideoPixelFormat::NV12 || pixel_format == VideoPixelFormat::I420;
}

size_t expected_frame_size(const RawVideoPipelineConfig& config) {
  switch (config.input_pixel_format) {
    case VideoPixelFormat::NV12:
    case VideoPixelFormat::I420:
      return static_cast<size_t>(config.input_width) * config.input_height * 3u / 2u;
    default:
      return static_cast<size_t>(config.input_width) * config.input_height *
             bytes_per_pixel(config.input_pixel_format);
  }
}

bool start_code_at(const std::vector<uint8_t>& bytes, size_t index, size_t* prefix_size) {
  if (index + 4 <= bytes.size() && bytes[index] == 0x00 && bytes[index + 1] == 0x00 &&
      bytes[index + 2] == 0x00 && bytes[index + 3] == 0x01) {
    *prefix_size = 4;
    return true;
  }
  if (index + 3 <= bytes.size() && bytes[index] == 0x00 && bytes[index + 1] == 0x00 &&
      bytes[index + 2] == 0x01) {
    *prefix_size = 3;
    return true;
  }
  return false;
}

uint8_t first_nal_type(const std::vector<uint8_t>& bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    size_t prefix = 0;
    if (!start_code_at(bytes, i, &prefix)) {
      continue;
    }
    if (i + prefix < bytes.size()) {
      return static_cast<uint8_t>(bytes[i + prefix] & 0x1f);
    }
  }
  return 0;
}

bool contains_nal_type(const std::vector<uint8_t>& bytes, uint8_t nal_type) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    size_t prefix = 0;
    if (!start_code_at(bytes, i, &prefix)) {
      continue;
    }
    if (i + prefix < bytes.size() && static_cast<uint8_t>(bytes[i + prefix] & 0x1f) == nal_type) {
      return true;
    }
    i += prefix;
  }
  return false;
}

std::string make_filter_chain(const RawVideoPipelineConfig& config) {
  std::vector<std::string> filters;
  if (config.scale_mode == RawPipelineScaleMode::Resize && config.output_width.has_value() &&
      config.output_height.has_value()) {
    std::ostringstream resize;
    resize << "scale=" << *config.output_width << ':' << *config.output_height;
    filters.push_back(resize.str());
  }
  if (config.output_fps.has_value() && *config.output_fps > 0.0) {
    std::ostringstream fps;
    fps << "fps=" << *config.output_fps;
    filters.push_back(fps.str());
  }
  if (filters.empty()) {
    return {};
  }
  std::ostringstream out;
  for (size_t i = 0; i < filters.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << filters[i];
  }
  return out.str();
}

class RawToH264Pipeline final : public IRawVideoPipeline {
 public:
  RawToH264Pipeline(std::string stream_id, RawVideoPipelineConfig config, EncodedAccessUnitSink sink)
      : stream_id_(std::move(stream_id)), config_(std::move(config)), sink_(std::move(sink)) {}

  ~RawToH264Pipeline() override { stop(); }

  const std::string& stream_id() const override { return stream_id_; }

  bool start(std::string* error_message = nullptr) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
      return true;
    }
    if (!validate_config(error_message)) {
      return false;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
      set_error(error_message, std::string("failed to create pipes: ") + std::strerror(errno));
      close_pipe(stdin_pipe);
      close_pipe(stdout_pipe);
      return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);

    auto args = build_ffmpeg_args();
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int spawn_result = ::posix_spawnp(&pid, config_.ffmpeg_path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);

    if (spawn_result != 0) {
      ::close(stdin_pipe[1]);
      ::close(stdout_pipe[0]);
      set_error(error_message, std::string("failed to start ffmpeg: ") + std::strerror(spawn_result));
      return false;
    }

    child_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    running_ = true;
    encoder_failed_ = false;
    reader_thread_ = std::thread([this]() { this->read_loop(); });
    return true;
  }

  bool push_frame(const VideoFrameView& frame, std::string* error_message = nullptr) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stdin_fd_ < 0) {
      set_error(error_message, "pipeline is not running");
      return false;
    }
    if (encoder_failed_) {
      set_error(error_message, last_error_);
      return false;
    }
    if (!validate_frame(frame, error_message)) {
      return false;
    }
    last_seen_timestamp_ns_ = frame.timestamp_ns;
    const size_t frame_size = expected_frame_size(config_);
    const uint8_t* ptr = static_cast<const uint8_t*>(frame.data);
    if (!write_all_locked(ptr, frame_size, error_message)) {
      encoder_failed_ = true;
      if (last_error_.empty() && error_message != nullptr) {
        last_error_ = *error_message;
      }
      return false;
    }
    return true;
  }

  void stop() override {
    int stdin_fd = -1;
    int stdout_fd = -1;
    pid_t child_pid = -1;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && reader_thread_.joinable() == false) {
        return;
      }
      stdin_fd = stdin_fd_;
      stdout_fd = stdout_fd_;
      child_pid = child_pid_;
      stdin_fd_ = -1;
      stdout_fd_ = -1;
      child_pid_ = -1;
      running_ = false;
    }

    if (stdin_fd >= 0) {
      ::close(stdin_fd);
    }
    if (stdout_fd >= 0) {
      ::close(stdout_fd);
    }

    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }

    if (child_pid > 0) {
      int status = 0;
      pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
      if (waited == 0) {
        ::kill(child_pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
          waited = ::waitpid(child_pid, &status, WNOHANG);
          if (waited == child_pid) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (waited == 0) {
          ::kill(child_pid, SIGKILL);
          ::waitpid(child_pid, &status, 0);
        }
      } else if (waited < 0 && errno == ECHILD) {
        // already reaped
      } else if (waited == 0) {
        ::waitpid(child_pid, &status, 0);
      }
    }
  }

 private:
  bool validate_config(std::string* error_message) const {
    if (stream_id_.empty()) {
      set_error(error_message, "stream_id is required");
      return false;
    }
    if (config_.input_width == 0 || config_.input_height == 0 || config_.input_fps <= 0.0) {
      set_error(error_message, "input dimensions and fps must be valid");
      return false;
    }
    if (pixel_format_to_ffmpeg(config_.input_pixel_format) == nullptr) {
      set_error(error_message, "unsupported ffmpeg input pixel format");
      return false;
    }
    if (is_planar_pixel_format(config_.input_pixel_format) && bytes_per_pixel(config_.input_pixel_format) == 0) {
      // allowed; frame size is handled separately
    }
    if (config_.scale_mode == RawPipelineScaleMode::Resize) {
      if (!config_.output_width.has_value() || !config_.output_height.has_value() || *config_.output_width == 0 ||
          *config_.output_height == 0) {
        set_error(error_message, "resize mode requires output width and height");
        return false;
      }
    }
    return true;
  }

  bool validate_frame(const VideoFrameView& frame, std::string* error_message) const {
    if (frame.data == nullptr || frame.width != config_.input_width || frame.height != config_.input_height ||
        frame.pixel_format != config_.input_pixel_format) {
      set_error(error_message, "frame does not match pipeline input contract");
      return false;
    }
    if (!is_planar_pixel_format(frame.pixel_format)) {
      const uint32_t min_stride = config_.input_width * bytes_per_pixel(frame.pixel_format);
      if (frame.stride_bytes < min_stride) {
        set_error(error_message, "frame stride is too small");
        return false;
      }
      if (frame.stride_bytes != min_stride) {
        set_error(error_message, "pipeline currently requires tightly packed raw frames");
        return false;
      }
    }
    return true;
  }

  bool write_all_locked(const uint8_t* data, size_t size, std::string* error_message) {
    size_t written = 0;
    while (written < size) {
      const ssize_t rc = ::write(stdin_fd_, data + written, size - written);
      if (rc > 0) {
        written += static_cast<size_t>(rc);
        continue;
      }
      if (rc < 0 && errno == EINTR) {
        continue;
      }
      const int local_errno = (rc < 0) ? errno : EPIPE;
      last_error_ = std::string("ffmpeg stdin write failed: ") + std::strerror(local_errno);
      set_error(error_message, last_error_);
      return false;
    }
    return true;
  }

  void read_loop() {
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> current_access_unit;
    std::array<uint8_t, 8192> chunk{};

    auto flush_current = [&](uint64_t timestamp_ns) {
      if (current_access_unit.empty()) {
        return;
      }
      EncodedAccessUnitView view{};
      view.data = current_access_unit.data();
      view.size_bytes = current_access_unit.size();
      view.codec = VideoCodec::H264;
      view.timestamp_ns = timestamp_ns;
      view.keyframe = contains_nal_type(current_access_unit, 5);
      view.codec_config = contains_nal_type(current_access_unit, 7) && contains_nal_type(current_access_unit, 8) && !view.keyframe;
      sink_(view);
      current_access_unit.clear();
    };

    while (true) {
      const ssize_t bytes_read = ::read(stdout_fd_, chunk.data(), chunk.size());
      if (bytes_read > 0) {
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + bytes_read);

        std::vector<size_t> starts;
        for (size_t i = 0; i < buffer.size();) {
          size_t prefix = 0;
          if (start_code_at(buffer, i, &prefix)) {
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
          std::vector<uint8_t> nal(buffer.begin() + static_cast<std::ptrdiff_t>(begin),
                                   buffer.begin() + static_cast<std::ptrdiff_t>(end));
          const uint8_t type = first_nal_type(nal);
          if (type == 9 && !current_access_unit.empty()) {
            flush_current(last_seen_timestamp_ns_);
          }
          current_access_unit.insert(current_access_unit.end(), nal.begin(), nal.end());
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(starts.back()));
        continue;
      }

      if (bytes_read == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (!buffer.empty()) {
      current_access_unit.insert(current_access_unit.end(), buffer.begin(), buffer.end());
    }
    if (!current_access_unit.empty()) {
      flush_current(last_seen_timestamp_ns_);
    }
  }

  std::vector<std::string> build_ffmpeg_args() const {
    std::vector<std::string> args;
    args.emplace_back(config_.ffmpeg_path);
    args.emplace_back("-hide_banner");
    args.emplace_back("-loglevel");
    args.emplace_back("error");
    args.emplace_back("-f");
    args.emplace_back("rawvideo");
    args.emplace_back("-pix_fmt");
    args.emplace_back(pixel_format_to_ffmpeg(config_.input_pixel_format));
    args.emplace_back("-s:v");
    args.emplace_back(std::to_string(config_.input_width) + "x" + std::to_string(config_.input_height));
    args.emplace_back("-r");
    args.emplace_back(std::to_string(config_.input_fps));
    args.emplace_back("-i");
    args.emplace_back("pipe:0");
    const std::string filter_chain = make_filter_chain(config_);
    if (!filter_chain.empty()) {
      args.emplace_back("-vf");
      args.emplace_back(filter_chain);
    }
    args.emplace_back("-an");
    args.emplace_back("-c:v");
    args.emplace_back("libx264");
    args.emplace_back("-pix_fmt");
    args.emplace_back("yuv420p");
    args.emplace_back("-preset");
    args.emplace_back(config_.encoder_preset);
    args.emplace_back("-tune");
    args.emplace_back(config_.encoder_tune);
    args.emplace_back("-profile:v");
    args.emplace_back(config_.encoder_profile);
    args.emplace_back("-bf");
    args.emplace_back("0");
    const int gop = std::max(1, static_cast<int>(config_.output_fps.value_or(config_.input_fps)));
    args.emplace_back("-g");
    args.emplace_back(std::to_string(gop));
    args.emplace_back("-keyint_min");
    args.emplace_back(std::to_string(gop));
    std::ostringstream x264;
    x264 << "scenecut=0";
    if (config_.emit_access_unit_delimiters) {
      x264 << ":aud=1";
    }
    if (config_.repeat_headers) {
      x264 << ":repeat-headers=1";
    }
    args.emplace_back("-x264-params");
    args.emplace_back(x264.str());
    args.emplace_back("-f");
    args.emplace_back("h264");
    args.emplace_back("pipe:1");
    return args;
  }

  static void close_pipe(int pipe_fds[2]) {
    if (pipe_fds[0] >= 0) {
      ::close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
      ::close(pipe_fds[1]);
    }
  }

  static void set_error(std::string* error_message, const std::string& value) {
    if (error_message != nullptr) {
      *error_message = value;
    }
  }

  const std::string stream_id_;
  const RawVideoPipelineConfig config_;
  EncodedAccessUnitSink sink_;

  mutable std::mutex mutex_;
  bool running_{false};
  int stdin_fd_{-1};
  int stdout_fd_{-1};
  pid_t child_pid_{-1};
  std::thread reader_thread_;
  bool encoder_failed_{false};
  std::string last_error_;
  std::atomic<uint64_t> last_seen_timestamp_ns_{0};
};

}  // namespace

std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline(std::string stream_id,
                                                             RawVideoPipelineConfig config,
                                                             EncodedAccessUnitSink sink) {
  return std::make_unique<RawToH264Pipeline>(std::move(stream_id), std::move(config), std::move(sink));
}

std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline_for_server(std::string stream_id,
                                                                        RawVideoPipelineConfig config,
                                                                        IVideoServer& server) {
  std::string target_stream_id = stream_id;
  return make_raw_to_h264_pipeline(
      std::move(stream_id), std::move(config),
      [&server, target_stream_id = std::move(target_stream_id)](const EncodedAccessUnitView& access_unit) {
        return server.push_access_unit(target_stream_id, access_unit);
      });
}

const char* to_string(RawPipelineScaleMode scale_mode) {
  switch (scale_mode) {
    case RawPipelineScaleMode::Passthrough:
      return "Passthrough";
    case RawPipelineScaleMode::Resize:
      return "Resize";
  }
  return "Unknown";
}

}  // namespace video_server
