#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rtc/datachannel.hpp>
#include <rtc/peerconnection.hpp>

#include "../core/video_server_core.h"

namespace video_server {

struct WebRtcSessionSnapshot {
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_remote_candidate;
  std::string last_local_candidate;
  std::string peer_state;
  bool data_channel_open{false};
  uint64_t frames_sent{0};
  uint64_t last_frame_id{0};
};

class WebRtcStreamSession : public std::enable_shared_from_this<WebRtcStreamSession> {
 public:
  using LatestFrameGetter = std::function<std::shared_ptr<const LatestFrame>(const std::string&)>;

  WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter);
  ~WebRtcStreamSession();

  WebRtcStreamSession(const WebRtcStreamSession&) = delete;
  WebRtcStreamSession& operator=(const WebRtcStreamSession&) = delete;

  bool apply_offer(const std::string& offer_sdp, std::string* error_message = nullptr);
  bool apply_answer(const std::string& answer_sdp, std::string* error_message = nullptr);
  bool add_remote_candidate(const std::string& candidate_sdp, std::string* error_message = nullptr);
  WebRtcSessionSnapshot snapshot() const;
  void stop();

 private:
  static std::string peer_state_to_string(rtc::PeerConnection::State state);
  void configure_callbacks();
  void run_delivery_loop();
  bool send_latest_frame_snapshot(std::shared_ptr<rtc::DataChannel> data_channel,
                                  const std::shared_ptr<const LatestFrame>& frame_snapshot);

  const std::string stream_id_;
  const LatestFrameGetter latest_frame_getter_;

  mutable std::mutex mutex_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::string offer_sdp_;
  std::string answer_sdp_;
  std::string last_remote_candidate_;
  std::string last_local_candidate_;
  std::string peer_state_{"new"};
  uint64_t frames_sent_{0};
  uint64_t last_frame_id_{0};
  bool data_channel_open_{false};

  std::atomic<bool> running_{true};
  std::thread delivery_thread_;
};

}  // namespace video_server
