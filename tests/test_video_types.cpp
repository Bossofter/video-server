#include <cassert>
#include <string>

#include "video_server/video_types.h"

int test_video_types() {
  using video_server::VideoPixelFormat;

  assert(std::string(video_server::to_string(VideoPixelFormat::RGB24)) == "RGB24");
  assert(std::string(video_server::to_string(VideoPixelFormat::BGR24)) == "BGR24");
  assert(std::string(video_server::to_string(VideoPixelFormat::RGBA32)) == "RGBA32");
  assert(std::string(video_server::to_string(VideoPixelFormat::BGRA32)) == "BGRA32");
  assert(std::string(video_server::to_string(VideoPixelFormat::NV12)) == "NV12");
  assert(std::string(video_server::to_string(VideoPixelFormat::I420)) == "I420");
  assert(std::string(video_server::to_string(VideoPixelFormat::GRAY8)) == "GRAY8");

  return 0;
}
