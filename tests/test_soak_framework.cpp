#include <gtest/gtest.h>

#include <chrono>

#include "../src/testing/soak_test_framework.h"

namespace {

using video_server::soak::FailureDetector;
using video_server::soak::FailureRecord;
using video_server::soak::MetricSample;
using video_server::soak::MetricsCollector;
using video_server::soak::RunnerOptions;
using video_server::soak::SessionHttpSnapshot;
using video_server::soak::SteadyClock;
using video_server::soak::StreamEvaluationState;

video_server::StreamDebugSnapshot make_stream_snapshot(const std::string& stream_id,
                                                       uint64_t generation,
                                                       uint64_t disconnects,
                                                       uint64_t packets_sent,
                                                       bool active,
                                                       bool sending_active,
                                                       const std::string& state) {
  video_server::StreamDebugSnapshot snapshot;
  snapshot.stream_id = stream_id;
  snapshot.config_generation = 1;
  snapshot.current_session = video_server::StreamSessionDebugSnapshot{};
  snapshot.current_session->stream_id = stream_id;
  snapshot.current_session->session_generation = generation;
  snapshot.current_session->disconnect_count = disconnects;
  snapshot.current_session->active = active;
  snapshot.current_session->sending_active = sending_active;
  snapshot.current_session->sender_state = state;
  snapshot.current_session->counters.packets_sent_after_track_open = packets_sent;
  snapshot.current_session->counters.packets_attempted = packets_sent;
  snapshot.current_session->last_packetization_status = "rtp-packets-sent";
  return snapshot;
}

}  // namespace

TEST(SoakFrameworkTest, ParseDurationSupportsSecondsMinutesHoursAndMillis) {
  ASSERT_EQ(video_server::soak::parse_duration_to_millis("1500ms"), std::chrono::milliseconds(1500));
  ASSERT_EQ(video_server::soak::parse_duration_to_millis("5s"), std::chrono::milliseconds(5000));
  ASSERT_EQ(video_server::soak::parse_duration_to_millis("2m"), std::chrono::milliseconds(120000));
  ASSERT_EQ(video_server::soak::parse_duration_to_millis("1h"), std::chrono::milliseconds(3600000));
  ASSERT_FALSE(video_server::soak::parse_duration_to_millis("bad").has_value());
}

TEST(SoakFrameworkTest, SessionHttpParserExtractsExpectedFields) {
  const auto snapshot = video_server::soak::parse_session_http_snapshot(
      "{\"session_generation\":7,\"disconnect_count\":3,\"active\":true,\"sending_active\":false,"
      "\"answer_sdp\":\"abc\",\"last_local_candidate\":\"cand\",\"encoded_sender_video_mid\":\"0\","
      "\"encoded_sender_packets_attempted\":55,\"encoded_sender_packets_sent_after_track_open\":21,"
      "\"encoded_sender_video_track_open\":true,\"encoded_sender_state\":\"sending-h264-rtp\","
      "\"encoded_sender_last_packetization_status\":\"rtp-packets-sent\"}");
  EXPECT_EQ(snapshot.session_generation, 7u);
  EXPECT_EQ(snapshot.disconnect_count, 3u);
  EXPECT_TRUE(snapshot.active);
  EXPECT_FALSE(snapshot.sending_active);
  EXPECT_EQ(snapshot.answer_sdp, "abc");
  EXPECT_EQ(snapshot.last_local_candidate, "cand");
  EXPECT_EQ(snapshot.encoded_sender_video_mid, "0");
  EXPECT_EQ(snapshot.encoded_sender_packets_attempted, 55u);
  EXPECT_EQ(snapshot.encoded_sender_packets_sent_after_track_open, 21u);
  EXPECT_TRUE(snapshot.encoded_sender_video_track_open);
  EXPECT_EQ(snapshot.encoded_sender_state, "sending-h264-rtp");
}

TEST(SoakFrameworkTest, MetricsCollectorBuildsPerStreamSummary) {
  MetricsCollector collector;
  collector.record(MetricSample{1.0, "alpha", 1, 0, 10, 10, 0, 0, 10, 0, 0, 1, true, true, "sending-h264-rtp", "ok"});
  collector.record(MetricSample{2.0, "alpha", 2, 1, 20, 22, 1, 0, 20, 0, 2, 2, true, true, "sending-h264-rtp", "ok"});
  collector.record(MetricSample{2.5, "bravo", 1, 0, 5, 5, 0, 0, 5, 0, 0, 1, true, true, "sending-h264-rtp", "ok"});

  const std::vector<FailureRecord> failures{{2.1, "alpha", "packet_progress_stalled", "stalled"}};
  const auto summary = collector.build_summary(3.0, failures);
  ASSERT_FALSE(summary.success);
  ASSERT_EQ(summary.failures.size(), 1u);
  ASSERT_EQ(summary.streams.size(), 2u);
  EXPECT_EQ(summary.streams[0].stream_id, "alpha");
  EXPECT_EQ(summary.streams[0].final_session_generation, 2u);
  EXPECT_EQ(summary.streams[0].final_disconnect_count, 1u);
  EXPECT_EQ(summary.streams[0].final_packets_sent, 20u);
  EXPECT_EQ(summary.streams[1].stream_id, "bravo");
}

TEST(SoakFrameworkTest, FailureDetectorFlagsPacketStallAndUnexpectedReset) {
  RunnerOptions options;
  options.progress_stall_threshold = std::chrono::seconds(2);
  options.startup_grace_period = std::chrono::seconds(1);

  StreamEvaluationState state;
  const auto start = SteadyClock::now();
  SessionHttpSnapshot session_http;
  session_http.session_generation = 1;
  session_http.sending_active = true;

  auto first = make_stream_snapshot("alpha", 1, 0, 10, true, true, "sending-h264-rtp");
  auto failures = FailureDetector::evaluate_stream(options, first, session_http, start, 0.0, state);
  EXPECT_TRUE(failures.empty());

  auto stalled = make_stream_snapshot("alpha", 1, 0, 10, true, true, "sending-h264-rtp");
  failures = FailureDetector::evaluate_stream(options, stalled, session_http, start + std::chrono::seconds(3), 3.0, state);
  ASSERT_FALSE(failures.empty());
  EXPECT_EQ(failures.front().code, "packet_progress_stalled");

  auto reset = make_stream_snapshot("alpha", 2, 0, 11, true, true, "sending-h264-rtp");
  session_http.session_generation = 2;
  failures = FailureDetector::evaluate_stream(options, reset, session_http, start + std::chrono::seconds(4), 4.0, state);
  ASSERT_FALSE(failures.empty());
  EXPECT_EQ(failures.front().code, "unexpected_session_reset");
}

TEST(SoakFrameworkTest, FailureDetectorTracksPendingConfigTimeout) {
  RunnerOptions options;
  options.config_apply_timeout = std::chrono::seconds(1);
  StreamEvaluationState state;
  state.pending_config = video_server::soak::PendingConfigExpectation{
      1, 2, video_server::StreamOutputConfig{}, SteadyClock::now() + std::chrono::seconds(1)};

  SessionHttpSnapshot session_http;
  session_http.session_generation = 1;
  auto snapshot = make_stream_snapshot("alpha", 1, 0, 5, true, true, "sending-h264-rtp");
  const auto failures = FailureDetector::evaluate_stream(options, snapshot, session_http,
                                                         SteadyClock::now() + std::chrono::seconds(2), 2.0, state);
  ASSERT_FALSE(failures.empty());
  EXPECT_EQ(failures.front().code, "config_apply_timeout");
}
