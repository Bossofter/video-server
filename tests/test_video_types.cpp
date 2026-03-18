#include <string>

#include <gtest/gtest.h>

#include "video_server/video_types.h"

TEST(VideoTypesTest, ConvertsPixelFormatsToStrings) {
  using video_server::VideoPixelFormat;

  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::RGB24)), "RGB24");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::BGR24)), "BGR24");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::RGBA32)), "RGBA32");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::BGRA32)), "BGRA32");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::NV12)), "NV12");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::I420)), "I420");
  EXPECT_EQ(std::string(video_server::to_string(VideoPixelFormat::GRAY8)), "GRAY8");
}
