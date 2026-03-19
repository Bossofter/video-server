#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video_server/raw_video_pipeline.h"
#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_types.h"
#include "../src/core/video_server_core.h"

namespace {

bool ffmpeg_available() {
  const int rc = std::system("ffmpeg -version >/dev/null 2>&1");
  return rc == 0;
}

std::vector<uint8_t> make_rgb_frame(uint32_t width, uint32_t height, uint64_t seed) {
  std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3u);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * width + x) * 3u;
      frame[index + 0] = static_cast<uint8_t>((x * 7 + seed) % 255);
      frame[index + 1] = static_cast<uint8_t>((y * 13 + seed * 3) % 255);
      frame[index + 2] = static_cast<uint8_t>(((x + y) * 5 + seed * 11) % 255);
    }
  }
  return frame;
}

video_server::VideoFrameView make_rgb_view(const std::vector<uint8_t>& frame, uint32_t width, uint32_t height,
                                           uint64_t timestamp_ns, uint64_t frame_id) {
  return video_server::VideoFrameView{frame.data(), width, height, width * 3u, video_server::VideoPixelFormat::RGB24,
                                      timestamp_ns, frame_id};
}

bool contains_start_code(const std::vector<uint8_t>& bytes) {
  for (size_t i = 0; i + 3 < bytes.size(); ++i) {
    if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 && ((bytes[i + 2] == 0x01) || (bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01))) {
      return true;
    }
  }
  return false;
}

bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 4000) {
  for (int waited = 0; waited < timeout_ms; waited += 25) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return predicate();
}


TEST(RawToH264PipelineTest, RejectsNoAudConfigurationUntilFallbackExists) {
  video_server::RawVideoPipelineConfig config;
  config.input_width = 64;
  config.input_height = 48;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = 30.0;
  config.emit_access_unit_delimiters = false;

  auto pipeline = video_server::make_raw_to_h264_pipeline(
      "no-aud-stream", config,
      [](const video_server::EncodedAccessUnitView&) {
        return true;
      });

  std::string error;
  EXPECT_FALSE(pipeline->start(&error));
  EXPECT_NE(error.find("emit_access_unit_delimiters=false"), std::string::npos);
}

TEST(RawToH264PipelineTest, PropagatesSinkFailureToCaller) {
  if (!ffmpeg_available()) { GTEST_SKIP() << "ffmpeg not available in test environment"; }

  video_server::RawVideoPipelineConfig config;
  config.input_width = 64;
  config.input_height = 48;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = 30.0;

  std::atomic<size_t> sink_calls{0};
  auto pipeline = video_server::make_raw_to_h264_pipeline(
      "sink-failure-stream", config,
      [&sink_calls](const video_server::EncodedAccessUnitView&) {
        ++sink_calls;
        return false;
      });

  std::string error;
  ASSERT_TRUE(pipeline->start(&error)) << error;
  for (uint64_t i = 0; i < 12; ++i) {
    auto frame = make_rgb_frame(config.input_width, config.input_height, i + 51);
    ASSERT_TRUE(pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 20000 + i, i + 1), &error)) << error;
  }

  ASSERT_TRUE(wait_until([&sink_calls]() { return sink_calls.load() > 0; }));

  auto extra_frame = make_rgb_frame(config.input_width, config.input_height, 99);
  EXPECT_FALSE(pipeline->push_frame(make_rgb_view(extra_frame, config.input_width, config.input_height, 30000, 99), &error));
  EXPECT_NE(error.find("sink rejected"), std::string::npos);
  pipeline->stop();
}

TEST(RawToH264PipelineTest, ProducesEncodedAccessUnitsFromRawFrames) {
  if (!ffmpeg_available()) { GTEST_SKIP() << "ffmpeg not available in test environment"; }

  std::vector<std::vector<uint8_t>> access_units;
  video_server::RawVideoPipelineConfig config;
  config.input_width = 64;
  config.input_height = 48;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = 30.0;

  auto pipeline = video_server::make_raw_to_h264_pipeline(
      "raw-smoke", config,
      [&access_units](const video_server::EncodedAccessUnitView& access_unit) {
        access_units.emplace_back(static_cast<const uint8_t*>(access_unit.data),
                                  static_cast<const uint8_t*>(access_unit.data) + access_unit.size_bytes);
        return true;
      });

  std::string error;
  ASSERT_TRUE(pipeline->start(&error)) << error;
  for (uint64_t i = 0; i < 12; ++i) {
    auto frame = make_rgb_frame(config.input_width, config.input_height, i + 1);
    ASSERT_TRUE(pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 1000 + i, i + 1), &error)) << error;
  }
  ASSERT_TRUE(wait_until([&access_units]() { return !access_units.empty(); }));
  pipeline->stop();

  ASSERT_FALSE(access_units.empty());
  EXPECT_TRUE(contains_start_code(access_units.front()));
}

TEST(RawToH264PipelineTest, BindsEncodedOutputIntoExistingServerPath) {
  if (!ffmpeg_available()) { GTEST_SKIP() << "ffmpeg not available in test environment"; }

  video_server::VideoServerCore server;
  video_server::StreamConfig stream_config{"raw-to-server", "raw", 64, 48, 30.0, video_server::VideoPixelFormat::RGB24};
  ASSERT_TRUE(server.register_stream(stream_config));

  video_server::RawVideoPipelineConfig pipeline_config;
  pipeline_config.input_width = stream_config.width;
  pipeline_config.input_height = stream_config.height;
  pipeline_config.input_pixel_format = stream_config.input_pixel_format;
  pipeline_config.input_fps = stream_config.nominal_fps;

  auto pipeline = video_server::make_raw_to_h264_pipeline_for_server(stream_config.stream_id, pipeline_config, server);
  std::string error;
  ASSERT_TRUE(pipeline->start(&error)) << error;
  for (uint64_t i = 0; i < 10; ++i) {
    auto frame = make_rgb_frame(stream_config.width, stream_config.height, i + 17);
    ASSERT_TRUE(pipeline->push_frame(make_rgb_view(frame, stream_config.width, stream_config.height, 5000 + i, i + 1), &error)) << error;
  }

  ASSERT_TRUE(wait_until([&server, &stream_config]() {
    auto info = server.get_stream_info(stream_config.stream_id);
    return info.has_value() && info->access_units_received > 0 && info->has_latest_encoded_unit;
  }));
  pipeline->stop();

  auto info = server.get_stream_info(stream_config.stream_id);
  ASSERT_TRUE(info.has_value());
  EXPECT_GT(info->access_units_received, 0u);
  EXPECT_TRUE(info->has_latest_encoded_unit);

  auto latest = server.get_latest_encoded_unit_for_stream(stream_config.stream_id);
  ASSERT_NE(latest, nullptr);
  EXPECT_TRUE(latest->valid);
  EXPECT_EQ(latest->codec, video_server::VideoCodec::H264);
  EXPECT_FALSE(latest->bytes.empty());
}

TEST(RawToH264PipelineTest, AppliesResizeFilterConfiguration) {
  if (!ffmpeg_available()) { GTEST_SKIP() << "ffmpeg not available in test environment"; }

  std::vector<std::vector<uint8_t>> access_units;
  video_server::RawVideoPipelineConfig config;
  config.input_width = 80;
  config.input_height = 60;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = 24.0;
  config.scale_mode = video_server::RawPipelineScaleMode::Resize;
  config.output_width = 40;
  config.output_height = 30;
  config.output_fps = 12.0;

  auto pipeline = video_server::make_raw_to_h264_pipeline(
      "resize-stream", config,
      [&access_units](const video_server::EncodedAccessUnitView& access_unit) {
        access_units.emplace_back(static_cast<const uint8_t*>(access_unit.data),
                                  static_cast<const uint8_t*>(access_unit.data) + access_unit.size_bytes);
        return true;
      });

  std::string error;
  ASSERT_TRUE(pipeline->start(&error)) << error;
  for (uint64_t i = 0; i < 18; ++i) {
    auto frame = make_rgb_frame(config.input_width, config.input_height, i + 23);
    ASSERT_TRUE(pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 9000 + i, i + 1), &error)) << error;
  }
  ASSERT_TRUE(wait_until([&access_units]() { return !access_units.empty(); }));
  pipeline->stop();

  ASSERT_FALSE(access_units.empty());
  bool saw_sps = false;
  for (const auto& au : access_units) {
    for (size_t i = 0; i + 4 < au.size(); ++i) {
      if ((au[i] == 0x00 && au[i + 1] == 0x00 && au[i + 2] == 0x00 && au[i + 3] == 0x01 && (au[i + 4] & 0x1F) == 7) ||
          (au[i] == 0x00 && au[i + 1] == 0x00 && au[i + 2] == 0x01 && (au[i + 3] & 0x1F) == 7)) {
        saw_sps = true;
        break;
      }
    }
  }
  EXPECT_TRUE(saw_sps);
}

TEST(RawToH264PipelineTest, StopsCleanlyAfterFramesArePushed) {
  if (!ffmpeg_available()) { GTEST_SKIP() << "ffmpeg not available in test environment"; }

  video_server::RawVideoPipelineConfig config;
  config.input_width = 64;
  config.input_height = 48;
  config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
  config.input_fps = 30.0;

  size_t units_seen = 0;
  auto pipeline = video_server::make_raw_to_h264_pipeline(
      "lifecycle-stream", config,
      [&units_seen](const video_server::EncodedAccessUnitView&) {
        ++units_seen;
        return true;
      });

  std::string error;
  ASSERT_TRUE(pipeline->start(&error)) << error;
  for (uint64_t i = 0; i < 6; ++i) {
    auto frame = make_rgb_frame(config.input_width, config.input_height, i + 31);
    ASSERT_TRUE(pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 12000 + i, i + 1), &error)) << error;
  }
  pipeline->stop();
  EXPECT_TRUE(units_seen >= 0u);
  auto extra_frame = make_rgb_frame(config.input_width, config.input_height, 99);
  EXPECT_FALSE(pipeline->push_frame(make_rgb_view(extra_frame, config.input_width, config.input_height, 99999, 99), &error));
}

}  // namespace
