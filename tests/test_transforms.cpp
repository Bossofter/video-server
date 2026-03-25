#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "../src/transforms/display_transform.h"

namespace {

std::array<uint8_t, 3> at_rgb(const video_server::RgbImage& img, uint32_t x, uint32_t y) {
  const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
  return {img.rgb[i], img.rgb[i + 1], img.rgb[i + 2]};
}

TEST(DisplayTransformTest, AppliesModesRotationAndMirroring) {
  std::vector<uint8_t> gray = {0, 64, 128, 255};
  video_server::VideoFrameView frame{gray.data(), 2, 2, 2, video_server::VideoPixelFormat::GRAY8, 1, 1};

  video_server::RgbImage out;
  video_server::StreamOutputConfig cfg;

  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  ASSERT_EQ(out.rgb.size(), 12u);
  EXPECT_EQ(at_rgb(out, 0, 0), (std::array<uint8_t, 3>{0, 0, 0}));
  EXPECT_EQ(at_rgb(out, 1, 1), (std::array<uint8_t, 3>{255, 255, 255}));

  cfg.display_mode = video_server::VideoDisplayMode::Grayscale;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(at_rgb(out, 1, 0), (std::array<uint8_t, 3>{64, 64, 64}));

  cfg.display_mode = video_server::VideoDisplayMode::WhiteHot;
  cfg.palette_min = 0.0F;
  cfg.palette_max = 1.0F;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_LE(out.rgb[0], out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::BlackHot;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_GE(out.rgb[0], out.rgb[9]);

  cfg.display_mode = video_server::VideoDisplayMode::Ironbow;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  const auto ironbow_mid = at_rgb(out, 1, 0);
  EXPECT_FALSE(ironbow_mid[0] == ironbow_mid[1] && ironbow_mid[1] == ironbow_mid[2]);

  cfg.display_mode = video_server::VideoDisplayMode::Arctic;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  const auto arctic_mid = at_rgb(out, 1, 0);
  EXPECT_FALSE(arctic_mid[0] == arctic_mid[1] && arctic_mid[1] == arctic_mid[2]);

  cfg.display_mode = video_server::VideoDisplayMode::Passthrough;
  cfg.rotation_degrees = 90;
  cfg.mirrored = false;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(out.width, 2);
  EXPECT_EQ(out.height, 2);
  EXPECT_EQ(at_rgb(out, 0, 0), (std::array<uint8_t, 3>{128, 128, 128}));

  cfg.rotation_degrees = 180;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(at_rgb(out, 0, 0), (std::array<uint8_t, 3>{255, 255, 255}));

  cfg.rotation_degrees = 270;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(at_rgb(out, 0, 0), (std::array<uint8_t, 3>{64, 64, 64}));

  cfg.rotation_degrees = 0;
  cfg.mirrored = true;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(at_rgb(out, 0, 0), (std::array<uint8_t, 3>{64, 64, 64}));

  cfg.rotation_degrees = 45;
  EXPECT_FALSE(video_server::apply_display_transform(frame, cfg, out));

  cfg.rotation_degrees = 0;
  cfg.output_width = 4;
  cfg.output_height = 4;
  ASSERT_TRUE(video_server::apply_display_transform(frame, cfg, out));
  EXPECT_EQ(out.width, 4u);
  EXPECT_EQ(out.height, 4u);
}

}  // namespace
