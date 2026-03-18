#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "webrtc_stream.h"

namespace video_server {

using StreamExistsFn = std::function<bool(const std::string&)>;
using LatestFrameGetterFn = WebRtcStreamSession::LatestFrameGetter;

struct SignalingSession {
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_remote_candidate;
  std::string last_local_candidate;
  std::string peer_state;
  WebRtcMediaSourceSnapshot media_source;
};

class SignalingServer {
 public:
  SignalingServer(StreamExistsFn stream_exists, LatestFrameGetterFn latest_frame_getter);
  ~SignalingServer();

  bool set_offer(const std::string& stream_id, const std::string& offer_sdp, std::string* error_message = nullptr);
  bool set_answer(const std::string& stream_id, const std::string& answer_sdp, std::string* error_message = nullptr);
  bool add_ice_candidate(const std::string& stream_id, const std::string& candidate,
                         std::string* error_message = nullptr);
  std::optional<SignalingSession> get_session(const std::string& stream_id) const;
  void remove_stream(const std::string& stream_id);
  void stop();

 private:
  StreamExistsFn stream_exists_;
  LatestFrameGetterFn latest_frame_getter_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<WebRtcStreamSession>> sessions_;
};

}  // namespace video_server
