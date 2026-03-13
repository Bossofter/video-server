#include <cassert>

#include "../src/testing/synthetic_frame_generator.h"

int test_synthetic() {
  video_server::StreamConfig cfg{"synthetic", "synthetic", 16, 16, 20.0, video_server::VideoPixelFormat::RGB24};
  video_server::SyntheticFrameGenerator generator(cfg);

  auto f1 = generator.next_frame();
  auto f2 = generator.next_frame();

  assert(f1.data != nullptr);
  assert(f2.data != nullptr);
  assert(f2.frame_id == f1.frame_id + 1);
  assert(f2.timestamp_ns >= f1.timestamp_ns);

  return 0;
}
