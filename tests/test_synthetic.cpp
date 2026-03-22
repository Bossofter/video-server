#include <gtest/gtest.h>

#include <cstring>

#include "../src/testing/synthetic_frame_generator.h"

TEST(SyntheticFrameGeneratorTest, ProducesSequentialFrames) {
  video_server::StreamConfig cfg{"synthetic", "synthetic", 16, 16, 20.0, video_server::VideoPixelFormat::RGB24};
  video_server::SyntheticFrameGenerator generator(cfg);

  auto f1 = generator.next_frame();
  auto f2 = generator.next_frame();

  EXPECT_NE(f1.data, nullptr);
  EXPECT_NE(f2.data, nullptr);
  EXPECT_EQ(f2.frame_id, f1.frame_id + 1);
  EXPECT_GE(f2.timestamp_ns, f1.timestamp_ns);
}

TEST(SyntheticFrameGeneratorTest, DifferentStreamsProduceDistinctPatternFamilies) {
  video_server::StreamConfig alpha{"alpha", "Synthetic demo alpha sweep", 64, 64, 30.0, video_server::VideoPixelFormat::RGB24};
  video_server::StreamConfig bravo{"bravo", "Synthetic demo bravo orbit", 64, 64, 30.0, video_server::VideoPixelFormat::RGB24};
  video_server::StreamConfig charlie{"charlie", "Synthetic demo charlie checker", 64, 64, 30.0, video_server::VideoPixelFormat::RGB24};

  video_server::SyntheticFrameGenerator alpha_generator(alpha);
  video_server::SyntheticFrameGenerator bravo_generator(bravo);
  video_server::SyntheticFrameGenerator charlie_generator(charlie);

  const auto alpha_frame = alpha_generator.next_frame();
  const auto bravo_frame = bravo_generator.next_frame();
  const auto charlie_frame = charlie_generator.next_frame();
  const size_t byte_count = static_cast<size_t>(alpha.width) * alpha.height * 3u;

  EXPECT_NE(std::memcmp(alpha_frame.data, bravo_frame.data, byte_count), 0);
  EXPECT_NE(std::memcmp(alpha_frame.data, charlie_frame.data, byte_count), 0);
  EXPECT_NE(std::memcmp(bravo_frame.data, charlie_frame.data, byte_count), 0);
}
