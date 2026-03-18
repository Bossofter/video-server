#include <cassert>
#include <cstdint>
#include <vector>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_types.h"
#include "../src/core/video_server_core.h"

namespace {

video_server::VideoFrameView make_gray_frame(const std::vector<uint8_t>& data, uint32_t width, uint32_t height,
                                             uint64_t timestamp_ns, uint64_t frame_id) {
  return video_server::VideoFrameView{data.data(), width, height, width, video_server::VideoPixelFormat::GRAY8,
                                      timestamp_ns, frame_id};
}

}  // namespace

int test_core() {
  video_server::VideoServerCore server;

  video_server::StreamConfig cfg{"stream-a", "primary", 2, 2, 30.0, video_server::VideoPixelFormat::GRAY8};
  assert(server.register_stream(cfg));
  assert(!server.register_stream(cfg));

  auto info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(!info->has_latest_frame);

  std::vector<uint8_t> frame_data = {10, 20, 30, 40};
  auto frame = make_gray_frame(frame_data, cfg.width, cfg.height, 100, 1);
  assert(server.push_frame(cfg.stream_id, frame));

  info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->frames_received == 1);
  assert(info->frames_transformed == 1);
  assert(info->frames_dropped == 0);
  assert(info->last_input_timestamp_ns == 100);
  assert(info->last_output_timestamp_ns == 100);
  assert(info->last_frame_id == 1);
  assert(info->has_latest_frame);

  auto latest = server.get_latest_frame_for_stream(cfg.stream_id);
  assert(latest != nullptr);
  assert(latest->valid);
  assert(latest->pixel_format == video_server::VideoPixelFormat::RGB24);
  assert(latest->width == 2);
  assert(latest->height == 2);
  assert(latest->timestamp_ns == 100);
  assert(latest->frame_id == 1);
  assert(latest->bytes.size() == 12);

  // Passthrough for GRAY8 produces RGB triplets with equal channels.
  assert(latest->bytes[0] == 10 && latest->bytes[1] == 10 && latest->bytes[2] == 10);
  assert(latest->bytes[9] == 40 && latest->bytes[10] == 40 && latest->bytes[11] == 40);

  // Snapshot access should not copy: multiple gets without publish return same object.
  auto latest_again = server.get_latest_frame_for_stream(cfg.stream_id);
  assert(latest_again != nullptr);
  assert(latest_again.get() == latest.get());

  video_server::StreamOutputConfig out_cfg;
  out_cfg.display_mode = video_server::VideoDisplayMode::Rainbow;
  out_cfg.rotation_degrees = 90;
  out_cfg.mirrored = true;
  assert(server.set_stream_output_config(cfg.stream_id, out_cfg));

  std::vector<uint8_t> frame_data_2 = {0, 64, 128, 255};
  auto frame2 = make_gray_frame(frame_data_2, cfg.width, cfg.height, 200, 2);
  assert(server.push_frame(cfg.stream_id, frame2));

  auto newest = server.get_latest_frame_for_stream(cfg.stream_id);
  assert(newest != nullptr);
  assert(newest->frame_id == 2);
  assert(newest->timestamp_ns == 200);
  assert(newest->width == 2);
  assert(newest->height == 2);
  // Rainbow mapping for grayscale should produce colored output for mid values.
  assert(!(newest->bytes[3] == newest->bytes[4] && newest->bytes[4] == newest->bytes[5]));

  // Previously retrieved snapshot remains valid after a newer publish.
  assert(latest->frame_id == 1);
  assert(latest->timestamp_ns == 100);
  assert(latest->bytes[0] == 10);
  assert(latest.get() != newest.get());

  info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->frames_received == 2);
  assert(info->frames_transformed == 2);
  assert(info->frames_dropped == 0);

  // Invalid stream path fails cleanly.
  assert(!server.push_frame("missing", frame));
  assert(server.get_latest_frame_for_stream("missing") == nullptr);

  // Invalid shape increments dropped counter and keeps latest frame unchanged.
  std::vector<uint8_t> bad_data = {1, 2, 3};
  auto bad_frame = make_gray_frame(bad_data, 3, 1, 300, 3);
  assert(!server.push_frame(cfg.stream_id, bad_frame));
  info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->frames_dropped == 1);
  newest = server.get_latest_frame_for_stream(cfg.stream_id);
  assert(newest != nullptr);
  assert(newest->frame_id == 2);

  std::vector<uint8_t> encoded_bytes = {0x00, 0x00, 0x00, 0x01, 0x67, 0x64};
  video_server::EncodedAccessUnitView encoded{};
  encoded.data = encoded_bytes.data();
  encoded.size_bytes = encoded_bytes.size();
  encoded.codec = video_server::VideoCodec::H264;
  encoded.timestamp_ns = 250;
  encoded.keyframe = true;
  encoded.codec_config = true;
  assert(server.push_access_unit(cfg.stream_id, encoded));

  auto latest_encoded = server.get_latest_encoded_unit_for_stream(cfg.stream_id);
  assert(latest_encoded != nullptr);
  assert(latest_encoded->valid);
  assert(latest_encoded->codec == video_server::VideoCodec::H264);
  assert(latest_encoded->timestamp_ns == 250);
  assert(latest_encoded->sequence_id == 250);
  assert(latest_encoded->keyframe);
  assert(latest_encoded->codec_config);
  assert(latest_encoded->bytes == encoded_bytes);

  info = server.get_stream_info(cfg.stream_id);
  assert(info.has_value());
  assert(info->access_units_received == 1);
  assert(info->has_latest_encoded_unit);
  assert(info->last_encoded_codec == video_server::VideoCodec::H264);
  assert(info->last_encoded_timestamp_ns == 250);
  assert(info->last_encoded_sequence_id == 250);
  assert(info->last_encoded_size_bytes == encoded_bytes.size());
  assert(info->last_encoded_keyframe);
  assert(info->last_encoded_codec_config);

  // Encoded snapshots keep immutable publication semantics too.
  auto latest_encoded_again = server.get_latest_encoded_unit_for_stream(cfg.stream_id);
  assert(latest_encoded_again != nullptr);
  assert(latest_encoded_again.get() == latest_encoded.get());

  assert(!server.push_access_unit("missing", encoded));
  video_server::EncodedAccessUnitView empty_encoded{};
  assert(!server.push_access_unit(cfg.stream_id, empty_encoded));
  video_server::EncodedAccessUnitView invalid_codec = encoded;
  invalid_codec.codec = static_cast<video_server::VideoCodec>(99);
  assert(!server.push_access_unit(cfg.stream_id, invalid_codec));

  assert(server.remove_stream(cfg.stream_id));
  assert(!server.remove_stream(cfg.stream_id));

  return 0;
}
