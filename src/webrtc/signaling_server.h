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

// Callback used to validate stream existence.
using StreamExistsFn = std::function<bool(const std::string&)>;
// Getter alias used to fetch the latest transformed frame.
using LatestFrameGetterFn = WebRtcStreamSession::LatestFrameGetter;
// Getter alias used to fetch the latest encoded unit.
using LatestEncodedUnitGetterFn = WebRtcStreamSession::LatestEncodedUnitGetter;

// Public-facing session snapshot stored by the signaling layer.
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

// Stream-scoped signaling and session manager.
class SignalingServer {
 public:
  // Creates a signaling server bound to stream existence and snapshot getters.
  SignalingServer(StreamExistsFn stream_exists, LatestFrameGetterFn latest_frame_getter,
                  LatestEncodedUnitGetterFn latest_encoded_unit_getter,
                  size_t max_pending_candidates_per_stream = 32);
  ~SignalingServer();

  // Creates or replaces the active session for a stream from a remote offer.
  bool set_offer(const std::string& stream_id, const std::string& offer_sdp, std::string* error_message = nullptr);
  // Applies a remote answer to an active session.
  bool set_answer(const std::string& stream_id, const std::string& answer_sdp, std::string* error_message = nullptr);
  // Queues or applies a remote ICE candidate.
  bool add_ice_candidate(const std::string& stream_id, const std::string& candidate,
                         std::string* error_message = nullptr);
  // Publishes a latest-frame update to the active session for a stream.
  void on_latest_frame(const std::string& stream_id, std::shared_ptr<const LatestFrame> latest_frame);
  // Publishes an encoded-unit update to the active session for a stream.
  void on_encoded_access_unit(const std::string& stream_id,
                              std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit);
  // Returns the current session snapshot for one stream.
  std::optional<SignalingSession> get_session(const std::string& stream_id) const;
  // Returns all active session snapshots.
  std::vector<SignalingSession> list_sessions() const;
  // Removes a stream and stops its active session.
  void remove_stream(const std::string& stream_id);
  // Stops all active sessions.
  void stop();

 private:
  // Session slot tracked for one stream id.
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
