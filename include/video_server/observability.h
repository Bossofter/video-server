#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_server {

//Sender-side counters exposed for diagnostics and soak reporting.
struct SenderDebugCounters {
  uint64_t delivered_units{0};
  uint64_t duplicate_units_skipped{0};
  uint64_t failed_units{0};
  uint64_t packetization_failures{0};
  uint64_t track_closed_events{0};
  uint64_t send_failures{0};
  uint64_t packets_attempted{0};
  uint64_t packets_sent_after_track_open{0};
  uint64_t startup_packets_sent{0};
  uint64_t startup_sequence_injections{0};
  uint64_t first_decodable_transitions{0};
  uint64_t skipped_no_track{0};
  uint64_t skipped_track_not_open{0};
  uint64_t skipped_codec_config_wait{0};
  uint64_t skipped_keyframe_wait{0};
  uint64_t skipped_startup_idr_wait{0};
};

//Debug view of the current WebRTC session state for a stream.
struct StreamSessionDebugSnapshot {
  uint64_t session_generation{0};
  std::string stream_id;
  std::string peer_state;
  bool active{true};
  bool sending_active{false};
  std::string sender_state;
  std::string last_packetization_status;
  std::string teardown_reason;
  std::string last_transition_reason;
  uint64_t disconnect_count{0};
  bool track_exists{false};
  bool track_open{false};
  bool startup_sequence_sent{false};
  bool first_decodable_frame_sent{false};
  bool codec_config_seen{false};
  bool cached_idr_available{false};
  std::string video_mid;
  SenderDebugCounters counters;
};

//Debug snapshot for one stream, including runtime config and session state.
struct StreamDebugSnapshot {
  std::string stream_id;
  std::string label;
  uint32_t configured_width{0};
  uint32_t configured_height{0};
  double configured_fps{0.0};
  std::string active_filter_mode;
  uint32_t active_output_width{0};
  uint32_t active_output_height{0};
  double active_output_fps{0.0};
  uint64_t config_generation{0};
  std::string config_state;
  bool latest_raw_frame_available{false};
  uint64_t latest_raw_frame_id{0};
  uint64_t latest_raw_timestamp_ns{0};
  uint32_t latest_raw_width{0};
  uint32_t latest_raw_height{0};
  bool latest_encoded_access_unit_available{false};
  uint64_t latest_encoded_timestamp_ns{0};
  uint64_t latest_encoded_sequence_id{0};
  size_t latest_encoded_size_bytes{0};
  bool latest_encoded_keyframe{false};
  bool latest_encoded_codec_config{false};
  uint64_t total_frames_received{0};
  uint64_t total_frames_transformed{0};
  uint64_t total_frames_dropped{0};
  uint64_t total_access_units_received{0};
  std::optional<StreamSessionDebugSnapshot> current_session;
};

//Debug snapshot for the whole server instance.
struct ServerDebugSnapshot {
  uint64_t stream_count{0};
  uint64_t active_session_count{0};
  bool security_access_control_enabled{false};
  bool security_allowlist_enabled{false};
  bool security_debug_api_enabled{false};
  bool security_runtime_config_api_enabled{false};
  bool security_remote_sensitive_routes_allowed{false};
  uint64_t security_rejected_unauthorized{0};
  uint64_t security_rejected_forbidden{0};
  uint64_t security_rejected_disabled{0};
  uint64_t security_rejected_invalid{0};
  uint64_t security_rejected_rate_limited{0};
  std::vector<StreamDebugSnapshot> streams;
};

}  // namespace video_server
