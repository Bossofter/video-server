#include "webrtc_stream.h"

#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <rtc/configuration.hpp>
#include <rtc/description.hpp>

#include "frame_http_encoder.h"

namespace video_server {
namespace {

std::string json_escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

}  // namespace

WebRtcStreamSession::WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter)
    : stream_id_(std::move(stream_id)), latest_frame_getter_(std::move(latest_frame_getter)) {
  rtc::Configuration config;
  config.disableAutoNegotiation = false;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(config);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel_ = peer_connection_->createDataChannel("video-snapshots");
    peer_state_ = peer_state_to_string(peer_connection_->state());
  }

  configure_callbacks();
  delivery_thread_ = std::thread([this]() { run_delivery_loop(); });
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
  return WebRtcSessionSnapshot{stream_id_,       offer_sdp_,           answer_sdp_,       last_remote_candidate_,
                               last_local_candidate_, peer_state_,     data_channel_open_, frames_sent_,
                               last_frame_id_};
}

void WebRtcStreamSession::stop() {
  const bool was_running = running_.exchange(false);
  if (!was_running) {
    if (delivery_thread_.joinable()) {
      delivery_thread_.join();
    }
    return;
  }

  std::shared_ptr<rtc::DataChannel> data_channel;
  std::shared_ptr<rtc::PeerConnection> peer_connection;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel = data_channel_;
    peer_connection = peer_connection_;
  }

  if (data_channel) {
    data_channel->close();
  }
  if (peer_connection) {
    peer_connection->close();
  }
  if (delivery_thread_.joinable()) {
    delivery_thread_.join();
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
  auto peer_connection = peer_connection_;
  auto data_channel = data_channel_;

  peer_connection->onLocalDescription([this](rtc::Description description) {
    std::lock_guard<std::mutex> lock(mutex_);
    answer_sdp_ = std::string(description);
  });

  peer_connection->onLocalCandidate([this](rtc::Candidate candidate) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_local_candidate_ = std::string(candidate);
  });

  peer_connection->onStateChange([this](rtc::PeerConnection::State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_state_ = peer_state_to_string(state);
  });

  data_channel->onOpen([this]() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel_open_ = true;
  });

  data_channel->onClosed([this]() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_channel_open_ = false;
  });
}

void WebRtcStreamSession::run_delivery_loop() {
  while (running_.load()) {
    std::shared_ptr<rtc::DataChannel> data_channel;
    bool data_channel_open = false;
    uint64_t last_frame_id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      data_channel = data_channel_;
      data_channel_open = data_channel_open_;
      last_frame_id = last_frame_id_;
    }

    if (data_channel && data_channel_open) {
      auto frame_snapshot = latest_frame_getter_(stream_id_);
      if (frame_snapshot && frame_snapshot->valid && frame_snapshot->frame_id != last_frame_id) {
        send_latest_frame_snapshot(std::move(data_channel), frame_snapshot);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
}

bool WebRtcStreamSession::send_latest_frame_snapshot(std::shared_ptr<rtc::DataChannel> data_channel,
                                                     const std::shared_ptr<const LatestFrame>& frame_snapshot) {
  auto encoded = encode_latest_frame_as_ppm(*frame_snapshot);
  if (!encoded.has_value()) {
    return false;
  }

  std::ostringstream metadata;
  metadata << "{"
           << "\"type\":\"latest-frame\"," 
           << "\"stream_id\":\"" << json_escape(stream_id_) << "\"," 
           << "\"frame_id\":" << frame_snapshot->frame_id << ','
           << "\"timestamp_ns\":" << frame_snapshot->timestamp_ns << ','
           << "\"width\":" << frame_snapshot->width << ','
           << "\"height\":" << frame_snapshot->height << ','
           << "\"pixel_format\":\"RGB24\"," 
           << "\"content_type\":\"" << encoded->content_type << "\"," 
           << "\"size_bytes\":" << encoded->body.size() << "}";

  if (!data_channel->send(metadata.str())) {
    return false;
  }
  if (!data_channel->send(reinterpret_cast<const rtc::byte*>(encoded->body.data()), encoded->body.size())) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  ++frames_sent_;
  last_frame_id_ = frame_snapshot->frame_id;
  return true;
}

}  // namespace video_server
