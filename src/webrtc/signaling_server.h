#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace video_server {

struct SignalingSession {
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_ice_candidate;
};

class SignalingServer {
 public:
  bool set_offer(const std::string& stream_id, const std::string& offer_sdp);
  bool set_answer(const std::string& stream_id, const std::string& answer_sdp);
  bool add_ice_candidate(const std::string& stream_id, const std::string& candidate);
  std::optional<SignalingSession> get_session(const std::string& stream_id) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, SignalingSession> sessions_;
};

}  // namespace video_server
