#include "signaling_server.h"

namespace video_server {

bool SignalingServer::set_offer(const std::string& stream_id, const std::string& offer_sdp) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& session = sessions_[stream_id];
  session.stream_id = stream_id;
  session.offer_sdp = offer_sdp;
  return true;
}

bool SignalingServer::set_answer(const std::string& stream_id, const std::string& answer_sdp) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& session = sessions_[stream_id];
  session.stream_id = stream_id;
  session.answer_sdp = answer_sdp;
  return true;
}

bool SignalingServer::add_ice_candidate(const std::string& stream_id, const std::string& candidate) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& session = sessions_[stream_id];
  session.stream_id = stream_id;
  session.last_ice_candidate = candidate;
  return true;
}

std::optional<SignalingSession> SignalingServer::get_session(const std::string& stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(stream_id);
  if (it == sessions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace video_server
