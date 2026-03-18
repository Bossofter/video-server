#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <rtc/peerconnection.hpp>

#include "../core/video_server_core.h"
#include "video_server/encoded_access_unit_view.h"

namespace video_server {

struct WebRtcMediaSourceSnapshot {
  std::string bridge_state;
  std::string preferred_media_path;
  bool latest_snapshot_available{false};
  uint64_t latest_snapshot_frame_id{0};
  uint64_t latest_snapshot_timestamp_ns{0};
  uint32_t latest_snapshot_width{0};
  uint32_t latest_snapshot_height{0};
  bool latest_encoded_access_unit_available{false};
  std::string latest_encoded_codec;
  uint64_t latest_encoded_timestamp_ns{0};
  size_t latest_encoded_size_bytes{0};
  bool latest_encoded_keyframe{false};
  bool latest_encoded_codec_config{false};
};

class IWebRtcMediaSourceBridge {
 public:
  virtual ~IWebRtcMediaSourceBridge() = default;
  virtual void on_encoded_access_unit(const EncodedAccessUnitView& access_unit) = 0;
  virtual WebRtcMediaSourceSnapshot snapshot() const = 0;
};

struct WebRtcSessionSnapshot {
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_remote_candidate;
  std::string last_local_candidate;
  std::string peer_state;
  WebRtcMediaSourceSnapshot media_source;
};

class WebRtcStreamSession {
 public:
  using LatestFrameGetter = std::function<std::shared_ptr<const LatestFrame>(const std::string&)>;

  WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter);
  ~WebRtcStreamSession();

  WebRtcStreamSession(const WebRtcStreamSession&) = delete;
  WebRtcStreamSession& operator=(const WebRtcStreamSession&) = delete;

  bool apply_offer(const std::string& offer_sdp, std::string* error_message = nullptr);
  bool apply_answer(const std::string& answer_sdp, std::string* error_message = nullptr);
  bool add_remote_candidate(const std::string& candidate_sdp, std::string* error_message = nullptr);
  void on_encoded_access_unit(const EncodedAccessUnitView& access_unit);
  WebRtcSessionSnapshot snapshot() const;
  void stop();

 private:
  static std::string peer_state_to_string(rtc::PeerConnection::State state);
  void configure_callbacks();

  const std::string stream_id_;

  mutable std::mutex mutex_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  std::unique_ptr<IWebRtcMediaSourceBridge> media_source_;
  std::string offer_sdp_;
  std::string answer_sdp_;
  std::string last_remote_candidate_;
  std::string last_local_candidate_;
  std::string peer_state_{"new"};
};

}  // namespace video_server
