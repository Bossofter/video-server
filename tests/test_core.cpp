#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_types.h"
#include "../src/core/video_server_core.h"

namespace {

video_server::VideoFrameView make_gray_frame(const std::vector<uint8_t>& data, uint32_t width, uint32_t height,
                                             uint64_t timestamp_ns, uint64_t frame_id) {
  return video_server::VideoFrameView{data.data(), width, height, width, video_server::VideoPixelFormat::GRAY8,
                                      timestamp_ns, frame_id};
}

TEST(VideoServerCoreTest, ManagesFramesAndEncodedUnits) {
  video_server::VideoServerCore server;

  video_server::StreamConfig cfg{"stream-a", "primary", 2, 2, 30.0, video_server::VideoPixelFormat::GRAY8};
  EXPECT_TRUE(server.register_stream(cfg));
  EXPECT_FALSE(server.register_stream(cfg));

  auto info = server.get_stream_info(cfg.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_FALSE(info->has_latest_frame);

  std::vector<uint8_t> frame_data = {10, 20, 30, 40};
  auto frame = make_gray_frame(frame_data, cfg.width, cfg.height, 100, 1);
  EXPECT_TRUE(server.push_frame(cfg.stream_id, frame));

  info = server.get_stream_info(cfg.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->frames_received, 1);
  EXPECT_EQ(info->frames_transformed, 1);
  EXPECT_EQ(info->frames_dropped, 0);
  EXPECT_EQ(info->last_input_timestamp_ns, 100);
  EXPECT_EQ(info->last_output_timestamp_ns, 100);
  EXPECT_EQ(info->last_frame_id, 1);
  EXPECT_TRUE(info->has_latest_frame);

  auto latest = server.get_latest_frame_for_stream(cfg.stream_id);
  ASSERT_NE(latest, nullptr);
  EXPECT_TRUE(latest->valid);
  EXPECT_EQ(latest->pixel_format, video_server::VideoPixelFormat::RGB24);
  EXPECT_EQ(latest->width, 2);
  EXPECT_EQ(latest->height, 2);
  EXPECT_EQ(latest->timestamp_ns, 100);
  EXPECT_EQ(latest->frame_id, 1);
  ASSERT_EQ(latest->bytes.size(), 12u);
  EXPECT_EQ(latest->bytes[0], 10);
  EXPECT_EQ(latest->bytes[1], 10);
  EXPECT_EQ(latest->bytes[2], 10);
  EXPECT_EQ(latest->bytes[9], 40);
  EXPECT_EQ(latest->bytes[10], 40);
  EXPECT_EQ(latest->bytes[11], 40);

  auto latest_again = server.get_latest_frame_for_stream(cfg.stream_id);
  ASSERT_NE(latest_again, nullptr);
  EXPECT_EQ(latest_again.get(), latest.get());

  video_server::StreamOutputConfig out_cfg;
  out_cfg.display_mode = video_server::VideoDisplayMode::Rainbow;
  out_cfg.rotation_degrees = 90;
  out_cfg.mirrored = true;
  EXPECT_TRUE(server.set_stream_output_config(cfg.stream_id, out_cfg));

  std::vector<uint8_t> frame_data_2 = {0, 64, 128, 255};
  auto frame2 = make_gray_frame(frame_data_2, cfg.width, cfg.height, 200, 2);
  EXPECT_TRUE(server.push_frame(cfg.stream_id, frame2));

  auto newest = server.get_latest_frame_for_stream(cfg.stream_id);
  ASSERT_NE(newest, nullptr);
  EXPECT_EQ(newest->frame_id, 2);
  EXPECT_EQ(newest->timestamp_ns, 200);
  EXPECT_EQ(newest->width, 2);
  EXPECT_EQ(newest->height, 2);
  EXPECT_FALSE(newest->bytes[3] == newest->bytes[4] && newest->bytes[4] == newest->bytes[5]);

  EXPECT_EQ(latest->frame_id, 1);
  EXPECT_EQ(latest->timestamp_ns, 100);
  EXPECT_EQ(latest->bytes[0], 10);
  EXPECT_NE(latest.get(), newest.get());

  info = server.get_stream_info(cfg.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->frames_received, 2);
  EXPECT_EQ(info->frames_transformed, 2);
  EXPECT_EQ(info->frames_dropped, 0);

  EXPECT_FALSE(server.push_frame("missing", frame));
  EXPECT_EQ(server.get_latest_frame_for_stream("missing"), nullptr);

  std::vector<uint8_t> bad_data = {1, 2, 3};
  auto bad_frame = make_gray_frame(bad_data, 3, 1, 300, 3);
  EXPECT_FALSE(server.push_frame(cfg.stream_id, bad_frame));
  info = server.get_stream_info(cfg.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->frames_dropped, 1);
  newest = server.get_latest_frame_for_stream(cfg.stream_id);
  ASSERT_NE(newest, nullptr);
  EXPECT_EQ(newest->frame_id, 2);

  std::vector<uint8_t> encoded_bytes = {0x00, 0x00, 0x00, 0x01, 0x67, 0x64};
  video_server::EncodedAccessUnitView encoded{};
  encoded.data = encoded_bytes.data();
  encoded.size_bytes = encoded_bytes.size();
  encoded.codec = video_server::VideoCodec::H264;
  encoded.timestamp_ns = 250;
  encoded.keyframe = true;
  encoded.codec_config = true;
  EXPECT_TRUE(server.push_access_unit(cfg.stream_id, encoded));

  auto latest_encoded = server.get_latest_encoded_unit_for_stream(cfg.stream_id);
  ASSERT_NE(latest_encoded, nullptr);
  EXPECT_TRUE(latest_encoded->valid);
  EXPECT_EQ(latest_encoded->codec, video_server::VideoCodec::H264);
  EXPECT_EQ(latest_encoded->timestamp_ns, 250);
  EXPECT_EQ(latest_encoded->sequence_id, 250);
  EXPECT_TRUE(latest_encoded->keyframe);
  EXPECT_TRUE(latest_encoded->codec_config);
  EXPECT_EQ(latest_encoded->bytes, encoded_bytes);

  info = server.get_stream_info(cfg.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->access_units_received, 1);
  EXPECT_TRUE(info->has_latest_encoded_unit);
  EXPECT_EQ(info->last_encoded_codec, video_server::VideoCodec::H264);
  EXPECT_EQ(info->last_encoded_timestamp_ns, 250);
  EXPECT_EQ(info->last_encoded_sequence_id, 250);
  EXPECT_EQ(info->last_encoded_size_bytes, encoded_bytes.size());
  EXPECT_TRUE(info->last_encoded_keyframe);
  EXPECT_TRUE(info->last_encoded_codec_config);

  auto latest_encoded_again = server.get_latest_encoded_unit_for_stream(cfg.stream_id);
  ASSERT_NE(latest_encoded_again, nullptr);
  EXPECT_EQ(latest_encoded_again.get(), latest_encoded.get());

  EXPECT_FALSE(server.push_access_unit("missing", encoded));
  video_server::EncodedAccessUnitView empty_encoded{};
  EXPECT_FALSE(server.push_access_unit(cfg.stream_id, empty_encoded));
  video_server::EncodedAccessUnitView invalid_codec = encoded;
  invalid_codec.codec = static_cast<video_server::VideoCodec>(99);
  EXPECT_FALSE(server.push_access_unit(cfg.stream_id, invalid_codec));

  EXPECT_TRUE(server.remove_stream(cfg.stream_id));
  EXPECT_FALSE(server.remove_stream(cfg.stream_id));
}

}  // namespace
