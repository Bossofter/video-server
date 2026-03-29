#pragma once

#include <optional>
#include <string>

#include "../core/video_server_core.h"

namespace video_server {

// Encoded HTTP payload returned by the latest-frame endpoint.
struct EncodedFramePayload {
  std::string body;
  std::string content_type;
};

// Encodes a transformed latest frame snapshot as a PPM payload.
std::optional<EncodedFramePayload> encode_latest_frame_as_ppm(const LatestFrame& frame);

}  // namespace video_server
