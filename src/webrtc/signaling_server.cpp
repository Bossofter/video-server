#include "signaling_server.h"

#include <utility>

namespace video_server {

SignalingServer::SignalingServer(StreamExistsFn stream_exists, LatestFrameGetterFn latest_frame_getter)
    : stream_exists_(std::move(stream_exists)), latest_frame_getter_(std::move(latest_frame_getter)) {}

SignalingServer::~SignalingServer() { stop(); }

bool SignalingServer::set_offer(const std::string& stream_id, const std::string& offer_sdp,
                                std::string* error_message) {
  if (!stream_exists_(stream_id)) {
    if (error_message != nullptr) {
      *error_message = "stream not found";
    }
    return false;
  }

  auto session = std::make_shared<WebRtcStreamSession>(stream_id, latest_frame_getter_);
  if (!session->apply_offer(offer_sdp, error_message)) {
    session->stop();
    return false;
  }

  std::shared_ptr<WebRtcStreamSession> previous;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = sessions_[stream_id];
    previous = slot.session;
    ++slot.session_generation;
    slot.session = session;
  }
  if (previous) {
    previous->stop();
  }
  return true;
}

bool SignalingServer::set_answer(const std::string& stream_id, const std::string& answer_sdp,
                                 std::string* error_message) {
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      if (error_message != nullptr) {
        *error_message = "session not found";
      }
      return false;
    }
    session = it->second.session;
  }
  return session->apply_answer(answer_sdp, error_message);
}

bool SignalingServer::add_ice_candidate(const std::string& stream_id, const std::string& candidate,
                                        std::string* error_message) {
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      if (error_message != nullptr) {
        *error_message = "session not found";
      }
      return false;
    }
    session = it->second.session;
  }
  return session->add_remote_candidate(candidate, error_message);
}



void SignalingServer::on_latest_frame(const std::string& stream_id,
                                      std::shared_ptr<const LatestFrame> latest_frame) {
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      return;
    }
    session = it->second.session;
  }
  session->on_latest_frame(std::move(latest_frame));
}
void SignalingServer::on_encoded_access_unit(const std::string& stream_id,
                                             const EncodedAccessUnitView& access_unit) {
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      return;
    }
    session = it->second.session;
  }
  session->on_encoded_access_unit(access_unit);
}
std::optional<SignalingSession> SignalingServer::get_session(const std::string& stream_id) const {
  uint64_t session_generation = 0;
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      return std::nullopt;
    }
    session_generation = it->second.session_generation;
    session = it->second.session;
  }

  const auto snapshot = session->snapshot();
  return SignalingSession{session_generation,
                          snapshot.stream_id,
                          snapshot.offer_sdp,
                          snapshot.answer_sdp,
                          snapshot.last_remote_candidate,
                          snapshot.last_local_candidate,
                          snapshot.peer_state,
                          snapshot.media_source};
}

void SignalingServer::remove_stream(const std::string& stream_id) {
  std::shared_ptr<WebRtcStreamSession> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(stream_id);
    if (it == sessions_.end()) {
      return;
    }
    session = it->second.session;
    sessions_.erase(it);
  }
  session->stop();
}

void SignalingServer::stop() {
  std::unordered_map<std::string, StreamSessionSlot> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions.swap(sessions_);
  }

  for (auto& [_, slot] : sessions) {
    if (slot.session) {
      slot.session->stop();
    }
  }
}

}  // namespace video_server
