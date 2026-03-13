#pragma once

#include <optional>
#include <string>

#include "../core/video_server_core.h"

namespace video_server {

struct EncodedFramePayload {
  std::string body;
  std::string content_type;
};

std::optional<EncodedFramePayload> encode_latest_frame_as_ppm(const LatestFrame& frame);

}  // namespace video_server

