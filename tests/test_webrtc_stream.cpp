#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "../src/core/video_server_core.h"
#include "../src/webrtc/webrtc_stream.h"

namespace {

std::shared_ptr<const video_server::LatestEncodedUnit> make_encoded_unit(std::vector<uint8_t> bytes,
                                                                         uint64_t timestamp_ns,
                                                                         bool keyframe,
                                                                         bool codec_config) {
  auto unit = std::make_shared<video_server::LatestEncodedUnit>();
  unit->bytes = std::move(bytes);
  unit->codec = video_server::VideoCodec::H264;
  unit->timestamp_ns = timestamp_ns;
  unit->sequence_id = timestamp_ns;
  unit->keyframe = keyframe;
  unit->codec_config = codec_config;
  unit->valid = true;
  return unit;
}

std::shared_ptr<const video_server::LatestEncodedUnit> make_large_idr_unit(uint64_t timestamp_ns, size_t payload_size) {
  std::vector<uint8_t> bytes{0x00, 0x00, 0x00, 0x01, 0x65};
  bytes.resize(bytes.size() + payload_size, 0xab);
  return make_encoded_unit(std::move(bytes), timestamp_ns, true, false);
}

std::shared_ptr<const video_server::LatestEncodedUnit> make_non_idr_unit(uint64_t timestamp_ns) {
  return make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x22}, timestamp_ns, false, false);
}

class FakeEncodedVideoTrackSink : public video_server::IEncodedVideoTrackSink {
 public:
  explicit FakeEncodedVideoTrackSink(bool open = true, std::string mid = "video", bool exists = true)
      : exists_(exists), open_(open), mid_(std::move(mid)) {}

  bool exists() const override { return exists_; }
  bool is_open() const override { return open_; }
  std::string mid() const override { return mid_; }
  void send(const std::byte* data, size_t size) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++send_attempts;
    if (close_on_send_attempt_ > 0 && send_attempts >= close_on_send_attempt_) {
      open_ = false;
    }
    if (throw_on_closed_ && !open_) {
      throw std::runtime_error("Track is closed");
    }
    sent_packets.emplace_back(data, data + size);
  }

  void set_exists(bool exists) { exists_ = exists; }
  void set_open(bool open) { open_ = open; }
  void set_throw_on_closed(bool throw_on_closed) { throw_on_closed_ = throw_on_closed; }
  void set_close_on_send_attempt(size_t send_attempt) { close_on_send_attempt_ = send_attempt; }

  std::mutex mutex;
  std::vector<std::vector<std::byte>> sent_packets;
  size_t send_attempts{0};
 private:
  bool exists_{true};
  bool open_{true};
  bool throw_on_closed_{false};
  size_t close_on_send_attempt_{0};
  std::string mid_;
};


TEST(WebRtcStreamSessionTest, SenderSnapshotsStayIsolatedAcrossIndependentSenders) {
  auto alpha_track = std::make_shared<FakeEncodedVideoTrackSink>();
  auto bravo_track = std::make_shared<FakeEncodedVideoTrackSink>(false, "video", true);
  auto alpha_sender = video_server::make_h264_encoded_video_sender(alpha_track, 1111);
  auto bravo_sender = video_server::make_h264_encoded_video_sender(bravo_track, 2222);

  alpha_sender->set_negotiated_h264_parameters(110, "packetization-mode=1;profile-level-id=42e01f");
  bravo_sender->set_negotiated_h264_parameters(111, "packetization-mode=1;profile-level-id=4d401f");

  alpha_sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                          0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                         1000, false, true));
  alpha_sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));

  bravo_sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x00, 0x1f,
                                                          0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80},
                                                         3000, false, true));
  bravo_sender->on_encoded_access_unit(make_non_idr_unit(4000));

  const auto alpha_snapshot = alpha_sender->snapshot();
  const auto bravo_snapshot = bravo_sender->snapshot();
  EXPECT_EQ(alpha_snapshot.sender_state, "sending-h264-rtp");
  EXPECT_TRUE(alpha_snapshot.video_track_open);
  EXPECT_TRUE(alpha_snapshot.first_decodable_frame_sent);
  EXPECT_EQ(alpha_snapshot.negotiated_h264_payload_type, 110);
  EXPECT_EQ(alpha_snapshot.last_delivered_timestamp_ns, 2000u);
  EXPECT_TRUE(alpha_snapshot.cached_idr_available);

  EXPECT_EQ(bravo_snapshot.sender_state, "waiting-for-h264-keyframe");
  EXPECT_FALSE(bravo_snapshot.video_track_open);
  EXPECT_FALSE(bravo_snapshot.first_decodable_frame_sent);
  EXPECT_EQ(bravo_snapshot.negotiated_h264_payload_type, 111);
  EXPECT_EQ(bravo_snapshot.last_delivered_timestamp_ns, 4000u);
  EXPECT_FALSE(bravo_snapshot.cached_idr_available);
  EXPECT_NE(alpha_snapshot.sender_state, bravo_snapshot.sender_state);
  EXPECT_NE(alpha_snapshot.last_delivered_timestamp_ns, bravo_snapshot.last_delivered_timestamp_ns);
}

TEST(WebRtcStreamSessionTest, EncodedUnitDeliveryPathConsumesLatestH264Unit) {
  auto latest = make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
                                   0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
                                   0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84},
                                  1000, true, true);

  video_server::WebRtcStreamSession session(
      "stream-a", [](const std::string&) { return std::shared_ptr<const video_server::LatestFrame>{}; },
      [latest](const std::string&) { return latest; });

  const auto snapshot = session.snapshot();
  EXPECT_TRUE(snapshot.media_source.latest_encoded_access_unit_available);
  EXPECT_EQ(snapshot.media_source.latest_encoded_timestamp_ns, 1000u);
  EXPECT_FALSE(snapshot.media_source.encoded_sender.video_track_exists);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.video_mid.empty());
  EXPECT_EQ(snapshot.media_source.encoded_sender.delivered_units, 1u);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.codec_config_seen);
  EXPECT_FALSE(snapshot.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_sps);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_pps);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_idr);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.packets_attempted == 0u ||
              snapshot.media_source.encoded_sender.packets_attempted >= 1u);
  EXPECT_FALSE(snapshot.media_source.encoded_sender.last_packetization_status.empty());
}

TEST(WebRtcStreamSessionTest, PacketizesFragmentedH264AfterCodecConfigAndKeyframe) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>();
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 1234);
  sender->set_negotiated_h264_parameters(110, "packetization-mode=1;profile-level-id=42e01f");

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  const auto after_codec_config = sender->snapshot();
  EXPECT_EQ(after_codec_config.packets_attempted, 0u);
  EXPECT_EQ(after_codec_config.last_packetization_status, "keyframe-required");

  sender->on_encoded_access_unit(make_large_idr_unit(2000, 2500));

  const auto after_idr = sender->snapshot();
  EXPECT_TRUE(after_idr.video_track_exists);
  EXPECT_TRUE(after_idr.video_track_open);
  EXPECT_TRUE(after_idr.ready_for_video_track);
  EXPECT_TRUE(after_idr.h264_delivery_active);
  EXPECT_EQ(after_idr.sender_state, "sending-h264-rtp");
  EXPECT_EQ(after_idr.last_packetization_status, "rtp-packets-sent");
  EXPECT_TRUE(after_idr.codec_config_seen);
  EXPECT_TRUE(after_idr.keyframe_seen);
  EXPECT_EQ(after_idr.delivered_units, 2u);
  EXPECT_GE(after_idr.packets_attempted, 4u);

  std::lock_guard<std::mutex> lock(track_sink->mutex);
  ASSERT_EQ(track_sink->sent_packets.size(), after_idr.packets_attempted);
  ASSERT_GE(track_sink->sent_packets.size(), 4u);
  for (const auto& packet : track_sink->sent_packets) {
    ASSERT_GE(packet.size(), 12u);
    EXPECT_EQ(static_cast<uint8_t>(packet[0]), 0x80);
    EXPECT_EQ(static_cast<uint8_t>(packet[1]) & 0x7f, 110u);
  }
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets.front()[12]) & 0x1f, 7u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[1][12]) & 0x1f, 8u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[2][12]) & 0x1f, 28u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[2][13]) & 0x80, 0x80);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets.back()[12]) & 0x1f, 28u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets.back()[13]) & 0x40, 0x40);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets.back()[1]) & 0x80, 0x80);
  EXPECT_EQ(after_idr.negotiated_h264_payload_type, 110);
  EXPECT_EQ(after_idr.negotiated_h264_fmtp, "packetization-mode=1;profile-level-id=42e01f");
}


TEST(WebRtcStreamSessionTest, SnapshotReflectsTrackAvailabilityAfterLateBind) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>(false, "video", false);
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 4321);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));

  const auto before_bind = sender->snapshot();
  EXPECT_EQ(before_bind.sender_state, "video-track-missing");
  EXPECT_FALSE(before_bind.video_track_exists);
  EXPECT_FALSE(before_bind.video_track_open);
  EXPECT_FALSE(before_bind.ready_for_video_track);

  track_sink->set_exists(true);

  const auto after_exists = sender->snapshot();
  EXPECT_EQ(after_exists.sender_state, "waiting-for-decoded-startup-idr");
  EXPECT_TRUE(after_exists.video_track_exists);
  EXPECT_FALSE(after_exists.video_track_open);
  EXPECT_TRUE(after_exists.ready_for_video_track);
  EXPECT_TRUE(after_exists.cached_idr_available);
  EXPECT_FALSE(after_exists.first_decodable_frame_sent);

  track_sink->set_open(true);

  const auto after_open = sender->snapshot();
  EXPECT_EQ(after_open.sender_state, "waiting-for-decoded-startup-idr");
  EXPECT_TRUE(after_open.video_track_exists);
  EXPECT_TRUE(after_open.video_track_open);
  EXPECT_TRUE(after_open.ready_for_video_track);
  EXPECT_FALSE(after_open.first_decodable_frame_sent);
}

TEST(WebRtcStreamSessionTest, SendsStartupSequenceFromCachedCodecConfigAndIdrAfterLateOpen) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>(false, "video", true);
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 5678);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));

  track_sink->set_open(true);
  sender->on_encoded_access_unit(make_non_idr_unit(3000));

  const auto snapshot = sender->snapshot();
  EXPECT_TRUE(snapshot.cached_codec_config_available);
  EXPECT_TRUE(snapshot.cached_idr_available);
  EXPECT_TRUE(snapshot.startup_sequence_sent);
  EXPECT_TRUE(snapshot.first_decodable_frame_sent);
  EXPECT_EQ(snapshot.sender_state, "sending-h264-rtp");
  EXPECT_EQ(snapshot.last_packetization_status, "rtp-packets-sent");
  EXPECT_GE(snapshot.packets_sent_after_track_open, 3u);
  EXPECT_GE(snapshot.startup_packets_sent, 2u);

  std::lock_guard<std::mutex> lock(track_sink->mutex);
  ASSERT_GE(track_sink->sent_packets.size(), 3u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[0][12]) & 0x1f, 7u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[1][12]) & 0x1f, 8u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets[2][12]) & 0x1f, 5u);
  EXPECT_EQ(static_cast<uint8_t>(track_sink->sent_packets.back()[12]) & 0x1f, 1u);
}

TEST(WebRtcStreamSessionTest, DoesNotSendWhileTrackExistsButIsClosed) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>(false, "video", true);
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 2468);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));
  sender->on_encoded_access_unit(make_non_idr_unit(3000));

  const auto snapshot = sender->snapshot();
  EXPECT_TRUE(snapshot.video_track_exists);
  EXPECT_FALSE(snapshot.video_track_open);
  EXPECT_TRUE(snapshot.cached_codec_config_available);
  EXPECT_TRUE(snapshot.cached_idr_available);
  EXPECT_FALSE(snapshot.first_decodable_frame_sent);
  EXPECT_FALSE(snapshot.startup_sequence_sent);
  EXPECT_EQ(snapshot.sender_state, "waiting-for-video-track-open");
  EXPECT_EQ(snapshot.last_packetization_status, "track-not-open-yet");
  EXPECT_EQ(snapshot.packets_attempted, 0u);

  std::lock_guard<std::mutex> lock(track_sink->mutex);
  EXPECT_TRUE(track_sink->sent_packets.empty());
}

TEST(WebRtcStreamSessionTest, ClosedTrackRaceDuringSendIsCaughtAndLaterRecoveryStillWorks) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>(true, "video", true);
  track_sink->set_throw_on_closed(true);
  track_sink->set_close_on_send_attempt(2);
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 2469);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));

  EXPECT_NO_THROW(sender->on_encoded_access_unit(make_non_idr_unit(3000)));

  const auto after_failure = sender->snapshot();
  EXPECT_TRUE(after_failure.video_track_exists);
  EXPECT_FALSE(after_failure.video_track_open);
  EXPECT_FALSE(after_failure.h264_delivery_active);
  EXPECT_EQ(after_failure.sender_state, "waiting-for-video-track-open");
  EXPECT_EQ(after_failure.last_packetization_status, "track-not-open-yet");
  EXPECT_EQ(after_failure.packets_attempted, 0u);
  EXPECT_FALSE(after_failure.first_decodable_frame_sent);

  {
    std::lock_guard<std::mutex> lock(track_sink->mutex);
    EXPECT_EQ(track_sink->send_attempts, 2u);
    EXPECT_EQ(track_sink->sent_packets.size(), 1u);
  }

  track_sink->set_open(true);
  track_sink->set_close_on_send_attempt(0);
  EXPECT_NO_THROW(sender->on_encoded_access_unit(make_non_idr_unit(4000)));

  const auto recovered = sender->snapshot();
  EXPECT_TRUE(recovered.video_track_open);
  EXPECT_TRUE(recovered.h264_delivery_active);
  EXPECT_TRUE(recovered.first_decodable_frame_sent);
  EXPECT_TRUE(recovered.startup_sequence_sent);
  EXPECT_EQ(recovered.sender_state, "sending-h264-rtp");
  EXPECT_EQ(recovered.last_packetization_status, "rtp-packets-sent");
  EXPECT_GT(recovered.packets_attempted, 0u);

  std::lock_guard<std::mutex> lock(track_sink->mutex);
  EXPECT_GT(track_sink->send_attempts, 0u);
  EXPECT_FALSE(track_sink->sent_packets.empty());
}

TEST(WebRtcStreamSessionTest, DoesNotStartFromNonIdrOnlyUnits) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>();
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 8765);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_non_idr_unit(2000));

  const auto snapshot = sender->snapshot();
  EXPECT_TRUE(snapshot.codec_config_seen);
  EXPECT_FALSE(snapshot.cached_idr_available);
  EXPECT_FALSE(snapshot.ready_for_video_track);
  EXPECT_FALSE(snapshot.first_decodable_frame_sent);
  EXPECT_EQ(snapshot.sender_state, "waiting-for-h264-keyframe");
  EXPECT_EQ(snapshot.last_packetization_status, "keyframe-required");
  EXPECT_EQ(snapshot.packets_attempted, 0u);

  std::lock_guard<std::mutex> lock(track_sink->mutex);
  EXPECT_TRUE(track_sink->sent_packets.empty());
}

TEST(WebRtcStreamSessionTest, RetainsCachedStartupStateAcrossSubsequentNonIdrUnits) {
  auto track_sink = std::make_shared<FakeEncodedVideoTrackSink>(false, "video", true);
  auto sender = video_server::make_h264_encoded_video_sender(track_sink, 9753);

  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                                    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                                   1000, false, true));
  sender->on_encoded_access_unit(make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 2000, true, false));

  auto before_follow_up = sender->snapshot();
  EXPECT_TRUE(before_follow_up.cached_codec_config_available);
  EXPECT_TRUE(before_follow_up.cached_idr_available);

  sender->on_encoded_access_unit(make_non_idr_unit(3000));

  auto while_closed = sender->snapshot();
  EXPECT_TRUE(while_closed.cached_codec_config_available);
  EXPECT_TRUE(while_closed.cached_idr_available);
  EXPECT_FALSE(while_closed.startup_sequence_sent);
  EXPECT_FALSE(while_closed.first_decodable_frame_sent);
  EXPECT_EQ(while_closed.sender_state, "waiting-for-video-track-open");
  EXPECT_EQ(while_closed.last_packetization_status, "track-not-open-yet");
  EXPECT_EQ(while_closed.packets_attempted, 0u);

  track_sink->set_open(true);
  sender->on_encoded_access_unit(make_non_idr_unit(4000));

  const auto after_follow_up = sender->snapshot();
  EXPECT_TRUE(after_follow_up.cached_codec_config_available);
  EXPECT_TRUE(after_follow_up.cached_idr_available);
  EXPECT_TRUE(after_follow_up.startup_sequence_sent);
  EXPECT_TRUE(after_follow_up.first_decodable_frame_sent);
  EXPECT_EQ(after_follow_up.sender_state, "sending-h264-rtp");
  EXPECT_GT(after_follow_up.packets_attempted, 0u);
}

TEST(WebRtcStreamSessionTest, CodecConfigKeyframeAndDuplicateBehaviorArePreserved) {
  video_server::WebRtcStreamSession session(
      "stream-b", [](const std::string&) { return std::shared_ptr<const video_server::LatestFrame>{}; },
      [](const std::string&) { return std::shared_ptr<const video_server::LatestEncodedUnit>{}; });

  auto codec_config = make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
                                         0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2},
                                        2000, false, true);
  session.on_encoded_access_unit(codec_config);

  auto after_config = session.snapshot();
  EXPECT_EQ(after_config.media_source.encoded_sender.delivered_units, 1u);
  EXPECT_EQ(after_config.media_source.encoded_sender.packets_attempted, 0u);
  EXPECT_TRUE(after_config.media_source.encoded_sender.codec_config_seen);
  EXPECT_FALSE(after_config.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(after_config.media_source.encoded_sender.cached_codec_config_available);
  EXPECT_TRUE(after_config.media_source.latest_encoded_codec_config);
  EXPECT_FALSE(after_config.media_source.latest_encoded_keyframe);
  EXPECT_EQ(after_config.media_source.encoded_sender.last_packetization_status, "no-video-track");

  session.on_encoded_access_unit(codec_config);
  auto after_duplicate = session.snapshot();
  EXPECT_EQ(after_duplicate.media_source.encoded_sender.delivered_units, 1u);
  EXPECT_EQ(after_duplicate.media_source.encoded_sender.duplicate_units_skipped, 1u);

  auto idr = make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 3000, true, false);
  session.on_encoded_access_unit(idr);
  auto after_idr = session.snapshot();
  EXPECT_EQ(after_idr.media_source.encoded_sender.delivered_units, 2u);
  EXPECT_FALSE(after_idr.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.keyframe_seen);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.last_delivered_keyframe);
  EXPECT_FALSE(after_idr.media_source.encoded_sender.last_delivered_codec_config);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.last_contains_idr);
  EXPECT_EQ(after_idr.media_source.encoded_sender.last_delivered_timestamp_ns, 3000u);
  EXPECT_FALSE(after_idr.media_source.encoded_sender.last_packetization_status.empty());
}

TEST(WebRtcStreamSessionTest, InvalidConditionsAndParserBehaviorStayClean) {
  video_server::LatestEncodedUnit invalid{};
  const auto invalid_desc = video_server::inspect_h264_access_unit(invalid);
  EXPECT_FALSE(invalid_desc.valid);

  video_server::LatestEncodedUnit unsupported{};
  unsupported.valid = true;
  unsupported.codec = static_cast<video_server::VideoCodec>(99);
  unsupported.bytes = {0x65, 0x88};
  const auto unsupported_desc = video_server::inspect_h264_access_unit(unsupported);
  EXPECT_FALSE(unsupported_desc.valid);

  auto non_idr = make_encoded_unit({0x00, 0x00, 0x01, 0x41, 0x9a, 0x22}, 4000, false, false);
  const auto non_idr_desc = video_server::inspect_h264_access_unit(*non_idr);
  EXPECT_TRUE(non_idr_desc.valid);
  EXPECT_TRUE(non_idr_desc.has_non_idr_slice);
  EXPECT_FALSE(non_idr_desc.has_idr);

  video_server::WebRtcStreamSession session(
      "stream-c", [](const std::string&) { return std::shared_ptr<const video_server::LatestFrame>{}; },
      [](const std::string&) { return std::shared_ptr<const video_server::LatestEncodedUnit>{}; });
  auto invalid_empty = std::make_shared<video_server::LatestEncodedUnit>();
  invalid_empty->valid = true;
  invalid_empty->codec = video_server::VideoCodec::H264;
  session.on_encoded_access_unit(invalid_empty);
  const auto snapshot = session.snapshot();
  EXPECT_EQ(snapshot.media_source.encoded_sender.failed_units, 1u);
  EXPECT_EQ(snapshot.media_source.encoded_sender.last_packetization_status, "rejected-invalid-input");
}

}  // namespace
