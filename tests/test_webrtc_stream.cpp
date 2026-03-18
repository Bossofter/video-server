#include <gtest/gtest.h>

#include <memory>
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
  EXPECT_EQ(snapshot.media_source.encoded_sender.delivered_units, 1u);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.codec_config_seen);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_sps);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_pps);
  EXPECT_TRUE(snapshot.media_source.encoded_sender.last_contains_idr);
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
  EXPECT_TRUE(after_config.media_source.encoded_sender.codec_config_seen);
  EXPECT_FALSE(after_config.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(after_config.media_source.latest_encoded_codec_config);
  EXPECT_FALSE(after_config.media_source.latest_encoded_keyframe);

  session.on_encoded_access_unit(codec_config);
  auto after_duplicate = session.snapshot();
  EXPECT_EQ(after_duplicate.media_source.encoded_sender.delivered_units, 1u);
  EXPECT_EQ(after_duplicate.media_source.encoded_sender.duplicate_units_skipped, 1u);

  auto idr = make_encoded_unit({0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84}, 3000, true, false);
  session.on_encoded_access_unit(idr);
  auto after_idr = session.snapshot();
  EXPECT_EQ(after_idr.media_source.encoded_sender.delivered_units, 2u);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.ready_for_video_track);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.last_delivered_keyframe);
  EXPECT_FALSE(after_idr.media_source.encoded_sender.last_delivered_codec_config);
  EXPECT_TRUE(after_idr.media_source.encoded_sender.last_contains_idr);
  EXPECT_EQ(after_idr.media_source.encoded_sender.last_delivered_timestamp_ns, 3000u);
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
}

}  // namespace
