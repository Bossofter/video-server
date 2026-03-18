#include "webrtc_stream.h"

#include <exception>
#include <memory>
#include <utility>

#include <rtc/configuration.hpp>
#include <rtc/description.hpp>

#include "video_server/video_types.h"

namespace video_server {
namespace {

std::string codec_to_string(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264:
      return "H264";
  }
  return "Unknown";
}

class StreamMediaSourceBridge : public IWebRtcMediaSourceBridge {
 public:
  void on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_snapshot_available_ = latest_frame != nullptr && latest_frame->valid;
    if (!latest_snapshot_available_) {
      latest_snapshot_frame_id_ = 0;
      latest_snapshot_timestamp_ns_ = 0;
      latest_snapshot_width_ = 0;
      latest_snapshot_height_ = 0;
      return;
    }

    latest_snapshot_frame_id_ = latest_frame->frame_id;
    latest_snapshot_timestamp_ns_ = latest_frame->timestamp_ns;
    latest_snapshot_width_ = latest_frame->width;
    latest_snapshot_height_ = latest_frame->height;
  }

  void on_encoded_access_unit(const EncodedAccessUnitView& access_unit) override {
    if (access_unit.data == nullptr || access_unit.size_bytes == 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    latest_encoded_access_unit_available_ = true;
    latest_encoded_codec_ = codec_to_string(access_unit.codec);
    latest_encoded_timestamp_ns_ = access_unit.timestamp_ns;
    latest_encoded_size_bytes_ = access_unit.size_bytes;
    latest_encoded_keyframe_ = access_unit.keyframe;
    latest_encoded_codec_config_ = access_unit.codec_config;
  }

  WebRtcMediaSourceSnapshot snapshot() const override {
    WebRtcMediaSourceSnapshot snapshot;
    snapshot.bridge_state = "awaiting-video-track-bridge";
    snapshot.preferred_media_path = "latest-frame-snapshot";

    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot.latest_snapshot_available = latest_snapshot_available_;
      snapshot.latest_snapshot_frame_id = latest_snapshot_frame_id_;
      snapshot.latest_snapshot_timestamp_ns = latest_snapshot_timestamp_ns_;
      snapshot.latest_snapshot_width = latest_snapshot_width_;
      snapshot.latest_snapshot_height = latest_snapshot_height_;
      snapshot.latest_encoded_access_unit_available = latest_encoded_access_unit_available_;
      snapshot.latest_encoded_codec = latest_encoded_codec_;
      snapshot.latest_encoded_timestamp_ns = latest_encoded_timestamp_ns_;
      snapshot.latest_encoded_size_bytes = latest_encoded_size_bytes_;
      snapshot.latest_encoded_keyframe = latest_encoded_keyframe_;
      snapshot.latest_encoded_codec_config = latest_encoded_codec_config_;
    }

    if (snapshot.latest_encoded_access_unit_available) {
      snapshot.preferred_media_path = "encoded-access-unit";
      snapshot.bridge_state = "awaiting-h264-video-track-bridge";
    }

    return snapshot;
  }

 private:
  mutable std::mutex mutex_;
  bool latest_snapshot_available_{false};
  uint64_t latest_snapshot_frame_id_{0};
  uint64_t latest_snapshot_timestamp_ns_{0};
  uint32_t latest_snapshot_width_{0};
  uint32_t latest_snapshot_height_{0};
  bool latest_encoded_access_unit_available_{false};
  std::string latest_encoded_codec_;
  uint64_t latest_encoded_timestamp_ns_{0};
  size_t latest_encoded_size_bytes_{0};
  bool latest_encoded_keyframe_{false};
  bool latest_encoded_codec_config_{false};
};

}  // namespace

WebRtcStreamSession::WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter)
    : stream_id_(std::move(stream_id)) {
  rtc::Configuration config;
  config.disableAutoNegotiation = false;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(config);
  media_source_ = std::make_unique<StreamMediaSourceBridge>();
  media_source_->on_latest_frame(latest_frame_getter(stream_id_));
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

void WebRtcStreamSession::on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  media_source_->on_latest_frame(std::move(latest_frame));
}

void WebRtcStreamSession::on_encoded_access_unit(const EncodedAccessUnitView& access_unit) {
  std::lock_guard<std::mutex> lock(mutex_);
  media_source_->on_encoded_access_unit(access_unit);
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
