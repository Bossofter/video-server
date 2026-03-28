#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rtc/peerconnection.hpp>

#include "video_server/observability.h"
#include "video_server/raw_video_pipeline.h"
#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"
#include "video_server/video_types.h"
#include "video_server/webrtc_video_server.h"
#include "synthetic_frame_generator.h"

namespace video_server {
namespace soak {

using SteadyClock = std::chrono::steady_clock;

struct StreamSpec {
  std::string stream_id;
  std::string label;
  uint32_t width{0};
  uint32_t height{0};
  double fps{0.0};
  VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
};

struct RunnerOptions {
  std::string host{"127.0.0.1"};
  uint16_t port{0};
  std::chrono::seconds duration{std::chrono::minutes(10)};
  std::chrono::milliseconds poll_interval{1000};
  std::chrono::seconds summary_interval{std::chrono::seconds(5)};
  std::chrono::seconds reconnect_interval{std::chrono::seconds(25)};
  std::chrono::seconds reconnect_grace_period{std::chrono::seconds(3)};
  std::chrono::seconds config_interval{std::chrono::seconds(12)};
  std::chrono::seconds progress_stall_threshold{std::chrono::seconds(10)};
  std::chrono::seconds startup_grace_period{std::chrono::seconds(6)};
  std::chrono::seconds rapid_reconnect_window{std::chrono::seconds(30)};
  std::chrono::seconds config_apply_timeout{std::chrono::seconds(8)};
  size_t max_rapid_reconnects{4};
  bool enable_debug_api{true};
  bool enable_lifecycle_churn{true};
  bool enable_config_churn{true};
  bool write_json_report{false};
  bool write_csv_report{false};
  std::string json_report_path;
  std::string csv_report_path;
  std::vector<StreamSpec> streams;
};

struct SessionHttpSnapshot {
  uint64_t session_generation{0};
  uint64_t disconnect_count{0};
  uint64_t encoded_sender_packets_attempted{0};
  uint64_t encoded_sender_packets_sent_after_track_open{0};
  bool active{false};
  bool sending_active{false};
  bool encoded_sender_video_track_open{false};
  std::string answer_sdp;
  std::string last_local_candidate;
  std::string encoded_sender_video_mid;
  std::string encoded_sender_state;
  std::string encoded_sender_last_packetization_status;
};

struct MetricSample {
  double elapsed_seconds{0.0};
  std::string stream_id;
  uint64_t session_generation{0};
  uint64_t disconnect_count{0};
  uint64_t packets_sent{0};
  uint64_t packets_attempted{0};
  uint64_t send_failures{0};
  uint64_t packetization_failures{0};
  uint64_t delivered_units{0};
  uint64_t failed_units{0};
  uint64_t total_frames_dropped{0};
  uint64_t config_generation{0};
  bool session_active{false};
  bool sending_active{false};
  std::string sender_state;
  std::string last_packetization_status;
};

struct FailureRecord {
  double elapsed_seconds{0.0};
  std::string stream_id;
  std::string code;
  std::string message;
};

struct StreamRunSummary {
  std::string stream_id;
  uint64_t samples{0};
  uint64_t reconnect_count{0};
  uint64_t config_updates{0};
  uint64_t final_session_generation{0};
  uint64_t final_disconnect_count{0};
  uint64_t final_packets_sent{0};
  uint64_t final_packets_attempted{0};
  uint64_t final_send_failures{0};
  uint64_t final_packetization_failures{0};
  uint64_t final_total_frames_dropped{0};
};

struct RunSummary {
  double total_duration_seconds{0.0};
  bool success{false};
  std::vector<StreamRunSummary> streams;
  std::vector<FailureRecord> failures;
};

struct PendingConfigExpectation {
  uint64_t previous_generation{0};
  uint64_t expected_generation{0};
  StreamOutputConfig expected_config;
  SteadyClock::time_point deadline;
};

struct StreamEvaluationState {
  bool expecting_reconnect{false};
  bool reconnect_disconnect_phase{false};
  bool saw_expected_disconnect{false};
  std::optional<SteadyClock::time_point> reconnect_resume_at;
  std::optional<SteadyClock::time_point> reconnect_deadline;
  uint64_t expected_previous_generation{0};
  std::optional<uint64_t> last_session_generation;
  std::optional<uint64_t> last_disconnect_count;
  std::optional<uint64_t> last_packets_sent;
  std::optional<SteadyClock::time_point> last_packet_progress_at;
  std::optional<SteadyClock::time_point> first_sending_active_at;
  std::deque<SteadyClock::time_point> reconnect_events;
  std::optional<PendingConfigExpectation> pending_config;
};

class MetricsCollector {
 public:
  void record(const MetricSample& sample);
  const std::vector<MetricSample>& samples() const { return samples_; }
  RunSummary build_summary(double total_duration_seconds,
                           const std::vector<FailureRecord>& failures) const;
  bool write_json_report(const std::string& path,
                         double total_duration_seconds,
                         const std::vector<FailureRecord>& failures) const;
  bool write_csv_report(const std::string& path) const;

 private:
  std::vector<MetricSample> samples_;
};

class FailureDetector {
 public:
  static std::vector<FailureRecord> evaluate_stream(const RunnerOptions& options,
                                                    const StreamDebugSnapshot& debug_snapshot,
                                                    const SessionHttpSnapshot& session_http_snapshot,
                                                    const SteadyClock::time_point& now,
                                                    double elapsed_seconds,
                                                    StreamEvaluationState& state);
};

class HttpJsonClient {
 public:
  struct Response {
    int status{0};
    std::string body;
  };

  HttpJsonClient(std::string host, uint16_t port);

  Response get(const std::string& path) const;
  Response post(const std::string& path, const std::string& body, const std::string& content_type = "application/json") const;
  Response put(const std::string& path, const std::string& body, const std::string& content_type = "application/json") const;

 private:
  Response send(const std::string& method,
                const std::string& path,
                const std::string& body,
                const std::string& content_type) const;

  std::string host_;
  uint16_t port_{0};
};

class WebRtcClientSession {
 public:
  explicit WebRtcClientSession(std::string stream_id);
  ~WebRtcClientSession();

  WebRtcClientSession(const WebRtcClientSession&) = delete;
  WebRtcClientSession& operator=(const WebRtcClientSession&) = delete;

  bool connect(const HttpJsonClient& http_client, std::string* error_message);
  void disconnect();
  void poll(const HttpJsonClient& http_client, std::string* error_message);

 private:
  struct CallbackState {
    std::mutex mutex;
    std::string offer_sdp;
    std::vector<std::string> local_candidates;
    std::string last_posted_candidate;
  };

  std::string stream_id_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  std::shared_ptr<CallbackState> callback_state_;
  bool remote_description_applied_{false};
  std::string last_applied_backend_candidate_;
};

class SyntheticServerHarness {
 public:
  explicit SyntheticServerHarness(RunnerOptions options);
  ~SyntheticServerHarness();

  SyntheticServerHarness(const SyntheticServerHarness&) = delete;
  SyntheticServerHarness& operator=(const SyntheticServerHarness&) = delete;

  bool start(std::string* error_message);
  void stop();
  std::optional<std::string> background_error() const;

  WebRtcVideoServer& server() { return server_; }
  const RunnerOptions& options() const { return options_; }

 private:
  struct RunningStream {
    explicit RunningStream(const StreamSpec& spec);

    StreamConfig config;
    SyntheticFrameGenerator generator;
    RawVideoPipelineConfig pipeline_config;
    std::unique_ptr<IRawVideoPipeline> pipeline;
    std::thread frame_thread;
    std::string pipeline_error;
    uint64_t pipeline_config_generation{0};
  };

  static RawVideoPipelineConfig make_pipeline_config(const RunningStream& stream,
                                                     const StreamOutputConfig& output_config,
                                                     uint32_t width,
                                                     uint32_t height);

  RunnerOptions options_;
  WebRtcVideoServer server_;
  std::vector<std::unique_ptr<RunningStream>> streams_;
  std::atomic<bool> stop_requested_{false};
  mutable std::mutex background_error_mutex_;
  std::optional<std::string> background_error_;
};

class SoakRunner {
 public:
  explicit SoakRunner(RunnerOptions options);

  RunSummary run();

 private:
  void connect_all_clients();
  void disconnect_all_clients();
  void poll_clients();
  void trigger_reconnect(StreamSpec& spec);
  void trigger_config_update(StreamSpec& spec);
  MetricSample make_sample(double elapsed_seconds, const StreamDebugSnapshot& snapshot) const;
  SessionHttpSnapshot fetch_session_snapshot(const std::string& stream_id) const;
  void record_failures(const std::vector<FailureRecord>& failures);
  bool has_failures() const { return !failures_.empty(); }

  RunnerOptions options_;
  SyntheticServerHarness harness_;
  HttpJsonClient http_client_;
  MetricsCollector metrics_;
  std::vector<FailureRecord> failures_;
  std::unordered_map<std::string, std::unique_ptr<WebRtcClientSession>> clients_;
  std::unordered_map<std::string, StreamEvaluationState> evaluation_states_;
  std::unordered_map<std::string, size_t> config_variant_index_;
};

std::optional<RunnerOptions> parse_runner_options(int argc, char** argv, std::string* error_message);
std::vector<StreamSpec> default_stream_specs();
std::optional<std::chrono::milliseconds> parse_duration_to_millis(const std::string& value);
SessionHttpSnapshot parse_session_http_snapshot(const std::string& json);

}  // namespace soak
}  // namespace video_server
