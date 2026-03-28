#include <exception>
#include <iostream>

#include <spdlog/spdlog.h>

#include "../src/testing/soak_test_framework.h"
#include "../src/webrtc/logging_utils.h"

int main(int argc, char** argv) {
  video_server::ensure_default_logging_config();

  std::string parse_error;
  const auto options = video_server::soak::parse_runner_options(argc, argv, &parse_error);
  if (!options.has_value()) {
    if (!parse_error.empty()) {
      std::cerr << parse_error << '\n';
    }
    return parse_error.rfind("Usage:", 0) == 0 ? 0 : 1;
  }

  try {
    video_server::soak::SoakRunner runner(*options);
    const auto summary = runner.run();
    std::cout << "[soak] completed duration=" << summary.total_duration_seconds
              << "s success=" << (summary.success ? "true" : "false")
              << " failures=" << summary.failures.size() << '\n';
    for (const auto& stream : summary.streams) {
      std::cout << "[soak] stream=" << stream.stream_id
                << " samples=" << stream.samples
                << " final_generation=" << stream.final_session_generation
                << " disconnects=" << stream.final_disconnect_count
                << " packets_sent=" << stream.final_packets_sent
                << " attempted=" << stream.final_packets_attempted
                << " send_failures=" << stream.final_send_failures
                << " packetization_failures=" << stream.final_packetization_failures
                << " dropped_frames=" << stream.final_total_frames_dropped << '\n';
    }
    for (const auto& failure : summary.failures) {
      std::cout << "[soak][failure] t=" << failure.elapsed_seconds
                << "s stream=" << failure.stream_id
                << " code=" << failure.code
                << " message=" << failure.message << '\n';
    }
    return summary.success ? 0 : 2;
  } catch (const std::exception& ex) {
    spdlog::error("[soak] fatal error: {}", ex.what());
    return 1;
  }
}
