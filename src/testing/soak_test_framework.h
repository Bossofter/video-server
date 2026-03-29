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

namespace video_server
{
    namespace soak
    {

        /** @brief Monotonic clock used by the soak framework. */
        using SteadyClock = std::chrono::steady_clock;

        /** @brief Static description of one stream used during a soak run. */
        struct StreamSpec
        {
            std::string stream_id;
            std::string label;
            uint32_t width{0};
            uint32_t height{0};
            double fps{0.0};
            VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
        };

        /** @brief Runner configuration for the embedded server, churn, and reports. */
        struct RunnerOptions
        {
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

        /** @brief Parsed subset of the session HTTP endpoint used by the runner. */
        struct SessionHttpSnapshot
        {
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

        /** @brief One time-series sample recorded during a soak run. */
        struct MetricSample
        {
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

        /** @brief Failure emitted by the runner's automatic evaluation logic. */
        struct FailureRecord
        {
            double elapsed_seconds{0.0};
            std::string stream_id;
            std::string code;
            std::string message;
        };

        /** @brief Final per-stream summary for a soak run. */
        struct StreamRunSummary
        {
            std::string stream_id;
            uint64_t samples{0};
            uint64_t reconnect_count{0};
            uint64_t config_updates{0};
            bool reconnect_churn_observed{false};
            bool config_churn_observed{false};
            uint64_t final_session_generation{0};
            uint64_t final_config_generation{0};
            uint64_t final_disconnect_count{0};
            uint64_t final_packets_sent{0};
            uint64_t final_packets_attempted{0};
            uint64_t final_send_failures{0};
            uint64_t final_packetization_failures{0};
            uint64_t final_total_frames_dropped{0};
        };

        /** @brief Final run summary including stream coverage and failures. */
        struct RunSummary
        {
            double total_duration_seconds{0.0};
            bool success{false};
            bool all_streams_saw_reconnect_churn{false};
            bool all_streams_saw_config_churn{false};
            std::vector<StreamRunSummary> streams;
            std::vector<std::string> streams_missing_reconnect_churn;
            std::vector<std::string> streams_missing_config_churn;
            std::vector<FailureRecord> failures;
        };

        /** @brief Reconnect and config churn coverage for one stream. */
        struct StreamChurnSummary
        {
            uint64_t reconnect_count{0};
            uint64_t config_update_count{0};
            bool reconnect_churn_observed{false};
            bool config_churn_observed{false};
        };

        /** @brief Expected config transition tracked while waiting for application. */
        struct PendingConfigExpectation
        {
            uint64_t previous_generation{0};
            uint64_t expected_generation{0};
            StreamOutputConfig expected_config;
            SteadyClock::time_point deadline;
        };

        /** @brief Internal state used when evaluating one stream over time. */
        struct StreamEvaluationState
        {
            bool expecting_reconnect{false};
            bool reconnect_disconnect_phase{false};
            bool saw_expected_disconnect{false};
            std::optional<SteadyClock::time_point> reconnect_resume_at;
            std::optional<SteadyClock::time_point> reconnect_deadline;
            uint64_t observed_reconnect_count{0};
            uint64_t observed_config_update_count{0};
            uint64_t expected_previous_generation{0};
            std::optional<uint64_t> last_session_generation;
            std::optional<uint64_t> last_disconnect_count;
            std::optional<uint64_t> last_packets_sent;
            std::optional<SteadyClock::time_point> last_packet_progress_at;
            std::optional<SteadyClock::time_point> first_sending_active_at;
            std::deque<SteadyClock::time_point> reconnect_events;
            std::optional<PendingConfigExpectation> pending_config;
        };

        /** @brief Stores metric samples and writes summaries or reports. */
        class MetricsCollector
        {
        public:
            /** @brief Appends one metric sample. */
            void record(const MetricSample &sample);
            /** @brief Returns all recorded metric samples. */
            const std::vector<MetricSample> &samples() const { return samples_; }
            /** @brief Builds a final run summary from samples and failures. */
            RunSummary build_summary(double total_duration_seconds,
                                     const std::vector<FailureRecord> &failures,
                                     const std::unordered_map<std::string, StreamChurnSummary> &churn_by_stream = {}) const;
            /** @brief Writes a JSON report to disk. */
            bool write_json_report(const std::string &path,
                                   double total_duration_seconds,
                                   const std::vector<FailureRecord> &failures,
                                   const std::unordered_map<std::string, StreamChurnSummary> &churn_by_stream = {}) const;
            /** @brief Writes a CSV sample report to disk. */
            bool write_csv_report(const std::string &path) const;

        private:
            std::vector<MetricSample> samples_;
        };

        /** @brief Evaluates stream snapshots and emits failure records. */
        class FailureDetector
        {
        public:
            /** @brief Evaluates one stream sample and updates the caller-owned state. */
            static std::vector<FailureRecord> evaluate_stream(const RunnerOptions &options,
                                                              const StreamDebugSnapshot &debug_snapshot,
                                                              const SessionHttpSnapshot &session_http_snapshot,
                                                              const SteadyClock::time_point &now,
                                                              double elapsed_seconds,
                                                              StreamEvaluationState &state);
        };

        /** @brief Minimal HTTP client used by the soak runner. */
        class HttpJsonClient
        {
        public:
            /** @brief HTTP response wrapper used by the soak framework. */
            struct Response
            {
                int status{0};
                std::string body;
            };

            /** @brief Creates a client for the embedded server host and port. */
            HttpJsonClient(std::string host, uint16_t port);

            /** @brief Sends a GET request. */
            Response get(const std::string &path) const;
            /** @brief Sends a POST request. */
            Response post(const std::string &path, const std::string &body, const std::string &content_type = "application/json") const;
            /** @brief Sends a PUT request. */
            Response put(const std::string &path, const std::string &body, const std::string &content_type = "application/json") const;

        private:
            Response send(const std::string &method,
                          const std::string &path,
                          const std::string &body,
                          const std::string &content_type) const;

            std::string host_;
            uint16_t port_{0};
        };

        /** @brief Real libdatachannel client session used to exercise the backend. */
        class WebRtcClientSession
        {
        public:
            /** @brief Creates a client bound to one stream id. */
            explicit WebRtcClientSession(std::string stream_id);
            ~WebRtcClientSession();

            WebRtcClientSession(const WebRtcClientSession &) = delete;
            WebRtcClientSession &operator=(const WebRtcClientSession &) = delete;

            /** @brief Connects to the backend using the signaling HTTP API. */
            bool connect(const HttpJsonClient &http_client, std::string *error_message);
            /** @brief Tears down the peer connection. */
            void disconnect();
            /** @brief Polls for answer and ICE updates. */
            void poll(const HttpJsonClient &http_client, std::string *error_message);

        private:
            /** @brief Shared callback state captured by async libdatachannel handlers. */
            struct CallbackState
            {
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

        /** @brief Embedded synthetic server plus frame generation threads used by the runner. */
        class SyntheticServerHarness
        {
        public:
            /** @brief Creates a harness from runner options. */
            explicit SyntheticServerHarness(RunnerOptions options);
            ~SyntheticServerHarness();

            SyntheticServerHarness(const SyntheticServerHarness &) = delete;
            SyntheticServerHarness &operator=(const SyntheticServerHarness &) = delete;

            /** @brief Starts the server and all synthetic streams. */
            bool start(std::string *error_message);
            /** @brief Stops the server and joins worker threads. */
            void stop();
            /** @brief Returns an asynchronous background error when one occurred. */
            std::optional<std::string> background_error() const;

            /** @brief Returns the embedded server instance. */
            WebRtcVideoServer &server() { return server_; }
            /** @brief Returns the options used to configure the harness. */
            const RunnerOptions &options() const { return options_; }

        private:
            /** @brief Runtime state for one synthetic stream. */
            struct RunningStream
            {
                explicit RunningStream(const StreamSpec &spec);

                StreamConfig config;
                SyntheticFrameGenerator generator;
                RawVideoPipelineConfig pipeline_config;
                std::unique_ptr<IRawVideoPipeline> pipeline;
                std::thread frame_thread;
                std::string pipeline_error;
                uint64_t pipeline_config_generation{0};
            };

            static RawVideoPipelineConfig make_pipeline_config(const RunningStream &stream,
                                                               const StreamOutputConfig &output_config,
                                                               uint32_t width,
                                                               uint32_t height);

            RunnerOptions options_;
            WebRtcVideoServer server_;
            std::vector<std::unique_ptr<RunningStream>> streams_;
            std::atomic<bool> stop_requested_{false};
            mutable std::mutex background_error_mutex_;
            std::optional<std::string> background_error_;
        };

        /** @brief End-to-end soak runner that coordinates the harness, clients, churn, and reports. */
        class SoakRunner
        {
        public:
            /** @brief Creates a runner from the supplied options. */
            explicit SoakRunner(RunnerOptions options);

            /** @brief Executes the run and returns the final summary. */
            RunSummary run();

        private:
            /** @brief Connects all client sessions to the embedded server. */
            void connect_all_clients();
            /** @brief Disconnects all active client sessions. */
            void disconnect_all_clients();
            /** @brief Polls all active client sessions. */
            void poll_clients();
            /** @brief Forces a reconnect cycle for one stream. */
            void trigger_reconnect(StreamSpec &spec);
            /** @brief Applies the next config variant for one stream. */
            void trigger_config_update(StreamSpec &spec);
            /** @brief Builds one metric sample from the latest debug snapshot. */
            MetricSample make_sample(double elapsed_seconds, const StreamDebugSnapshot &snapshot) const;
            /** @brief Fetches and parses the signaling session snapshot for one stream. */
            SessionHttpSnapshot fetch_session_snapshot(const std::string &stream_id) const;
            /** @brief Records newly detected failures. */
            void record_failures(const std::vector<FailureRecord> &failures);
            /** @brief Returns true when the run has already failed. */
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

        /** @brief Parses runner CLI options or returns an error description. */
        std::optional<RunnerOptions> parse_runner_options(int argc, char **argv, std::string *error_message);
        /** @brief Returns the default stream set used by the runner. */
        std::vector<StreamSpec> default_stream_specs();
        /** @brief Parses a duration like 5s, 1m, or 2h. */
        std::optional<std::chrono::milliseconds> parse_duration_to_millis(const std::string &value);
        /** @brief Parses the session endpoint JSON into the runner's HTTP snapshot type. */
        SessionHttpSnapshot parse_session_http_snapshot(const std::string &json);

    } // namespace soak
} // namespace video_server
