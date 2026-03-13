#include <cassert>
#include <cstdint>
#include <vector>

#if VIDEO_SERVER_TEST_WEBRTC
#include "video_server/webrtc_video_server.h"

int test_webrtc_http() {
  video_server::WebRtcVideoServer server;
  video_server::StreamConfig cfg{"stream-1", "label", 8, 8, 30.0, video_server::VideoPixelFormat::GRAY8};
  assert(server.register_stream(cfg));

  auto list = server.handle_http_request_for_test("GET", "/api/video/streams");
  assert(list.status == 200);
  assert(list.body.find("stream-1") != std::string::npos);

  auto output = server.handle_http_request_for_test("GET", "/api/video/streams/stream-1/output");
  assert(output.status == 200);
  assert(output.body.find("Passthrough") != std::string::npos);

  auto updated = server.handle_http_request_for_test(
      "PUT", "/api/video/streams/stream-1/output",
      "{\"display_mode\":\"WhiteHot\",\"mirrored\":true,\"rotation_degrees\":90,\"palette_min\":0.1,\"palette_max\":0.9}");
  assert(updated.status == 200);
  assert(updated.body.find("WhiteHot") != std::string::npos);

  auto offer = server.handle_http_request_for_test("POST", "/api/video/signaling/stream-1/offer", "offer-sdp");
  assert(offer.status == 200);
  auto candidate =
      server.handle_http_request_for_test("POST", "/api/video/signaling/stream-1/candidate", "candidate-a");
  assert(candidate.status == 200);
  auto session = server.handle_http_request_for_test("GET", "/api/video/signaling/stream-1/session");
  assert(session.status == 200);
  assert(session.body.find("offer-sdp") != std::string::npos);
  assert(session.body.find("candidate-a") != std::string::npos);

  return 0;
}
#else
int test_webrtc_http() { return 0; }
#endif
