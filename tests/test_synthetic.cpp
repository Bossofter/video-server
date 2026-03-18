#include <gtest/gtest.h>

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
