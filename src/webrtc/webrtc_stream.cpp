#include "webrtc_stream.h"

#include <exception>
#include <memory>
#include <utility>

#include <rtc/configuration.hpp>
#include <rtc/description.hpp>

namespace video_server {
namespace {

class LatestFrameVideoSourceBridge : public IWebRtcMediaSource {
 public:
  using LatestFrameGetter = WebRtcStreamSession::LatestFrameGetter;

  LatestFrameVideoSourceBridge(std::string stream_id, LatestFrameGetter latest_frame_getter)
      : stream_id_(std::move(stream_id)), latest_frame_getter_(std::move(latest_frame_getter)) {}

  WebRtcMediaSourceSnapshot snapshot() const override {
    WebRtcMediaSourceSnapshot snapshot;
    snapshot.bridge_state = "awaiting-encoded-video-bridge";

    const auto latest_frame = latest_frame_getter_(stream_id_);
    if (!latest_frame || !latest_frame->valid) {
      return snapshot;
    }

    snapshot.latest_snapshot_available = true;
    snapshot.latest_snapshot_frame_id = latest_frame->frame_id;
    snapshot.latest_snapshot_timestamp_ns = latest_frame->timestamp_ns;
    snapshot.latest_snapshot_width = latest_frame->width;
    snapshot.latest_snapshot_height = latest_frame->height;
    return snapshot;
  }

 private:
  std::string stream_id_;
  LatestFrameGetter latest_frame_getter_;
};

}  // namespace

WebRtcStreamSession::WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter)
    : stream_id_(std::move(stream_id)) {
  rtc::Configuration config;
  config.disableAutoNegotiation = false;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(config);
  media_source_ = std::make_unique<LatestFrameVideoSourceBridge>(stream_id_, std::move(latest_frame_getter));
  peer_state_ = peer_state_to_string(peer_connection_->state());
  configure_callbacks();
}

WebRtcStreamSession::~WebRtcStreamSession() { stop(); }

bool WebRtcStreamSession::apply_offer(const std::string& offer_sdp, std::string* error_message) {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    offer_sdp_ = offer_sdp;
    peer_connection_->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
    peer_connection_->setLocalDescription();
    return true;
  } catch (const std::exception& e) {
    if (error_message != nullptr) {
      *error_message = e.what();
    }
    return false;
  }
}

bool WebRtcStreamSession::apply_answer(const std::string& answer_sdp, std::string* error_message) {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    answer_sdp_ = answer_sdp;
    peer_connection_->setRemoteDescription(rtc::Description(answer_sdp, "answer"));
    return true;
  } catch (const std::exception& e) {
    if (error_message != nullptr) {
      *error_message = e.what();
    }
    return false;
  }
}

bool WebRtcStreamSession::add_remote_candidate(const std::string& candidate_sdp, std::string* error_message) {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    last_remote_candidate_ = candidate_sdp;
    peer_connection_->addRemoteCandidate(rtc::Candidate(candidate_sdp));
    return true;
  } catch (const std::exception& e) {
    if (error_message != nullptr) {
      *error_message = e.what();
    }
    return false;
  }
}

WebRtcSessionSnapshot WebRtcStreamSession::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return WebRtcSessionSnapshot{stream_id_,
                               offer_sdp_,
                               answer_sdp_,
                               last_remote_candidate_,
                               last_local_candidate_,
                               peer_state_,
                               media_source_->snapshot()};
}

void WebRtcStreamSession::stop() {
  std::shared_ptr<rtc::PeerConnection> peer_connection;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_connection = peer_connection_;
  }

  if (peer_connection) {
    peer_connection->close();
  }
}

std::string WebRtcStreamSession::peer_state_to_string(rtc::PeerConnection::State state) {
  switch (state) {
    case rtc::PeerConnection::State::New:
      return "new";
    case rtc::PeerConnection::State::Connecting:
      return "connecting";
    case rtc::PeerConnection::State::Connected:
      return "connected";
    case rtc::PeerConnection::State::Disconnected:
      return "disconnected";
    case rtc::PeerConnection::State::Failed:
      return "failed";
    case rtc::PeerConnection::State::Closed:
      return "closed";
  }
  return "unknown";
}

void WebRtcStreamSession::configure_callbacks() {
  peer_connection_->onLocalDescription([this](rtc::Description description) {
    std::lock_guard<std::mutex> lock(mutex_);
    answer_sdp_ = std::string(description);
  });

  peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_local_candidate_ = std::string(candidate);
  });

  peer_connection_->onStateChange([this](rtc::PeerConnection::State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_state_ = peer_state_to_string(state);
  });
}

}  // namespace video_server
