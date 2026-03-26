#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "webrtc_stream.h"

namespace video_server {

using StreamExistsFn = std::function<bool(const std::string&)>;
using LatestFrameGetterFn = WebRtcStreamSession::LatestFrameGetter;
using LatestEncodedUnitGetterFn = WebRtcStreamSession::LatestEncodedUnitGetter;

struct SignalingSession {
  uint64_t session_generation{0};
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_remote_candidate;
  std::string last_local_candidate;
  std::string peer_state;
  bool active{true};
  bool sending_active{false};
  std::string teardown_reason;
  std::string last_transition_reason;
  uint64_t disconnect_count{0};
  WebRtcMediaSourceSnapshot media_source;
};

class SignalingServer {
 public:
  SignalingServer(StreamExistsFn stream_exists, LatestFrameGetterFn latest_frame_getter,
                  LatestEncodedUnitGetterFn latest_encoded_unit_getter,
                  size_t max_pending_candidates_per_stream = 32);
  ~SignalingServer();

  bool set_offer(const std::string& stream_id, const std::string& offer_sdp, std::string* error_message = nullptr);
  bool set_answer(const std::string& stream_id, const std::string& answer_sdp, std::string* error_message = nullptr);
  bool add_ice_candidate(const std::string& stream_id, const std::string& candidate,
                         std::string* error_message = nullptr);
  void on_latest_frame(const std::string& stream_id, std::shared_ptr<const LatestFrame> latest_frame);
  void on_encoded_access_unit(const std::string& stream_id,
                              std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit);
  std::optional<SignalingSession> get_session(const std::string& stream_id) const;
  std::vector<SignalingSession> list_sessions() const;
  void remove_stream(const std::string& stream_id);
  void stop();

 private:
  struct StreamSessionSlot {
    uint64_t session_generation{0};
    std::shared_ptr<WebRtcStreamSession> session;
    std::vector<std::string> pending_remote_candidates;
  };

  StreamExistsFn stream_exists_;
  LatestFrameGetterFn latest_frame_getter_;
  LatestEncodedUnitGetterFn latest_encoded_unit_getter_;
  size_t max_pending_candidates_per_stream_;

  // Temporary limitation: only one active WebRTC session slot is tracked per stream.
  // TODO: extend this to support multiple simultaneous peer sessions per stream.
  mutable std::mutex mutex_;
  std::unordered_map<std::string, StreamSessionSlot> sessions_;
};

}  // namespace video_server
