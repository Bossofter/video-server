#include "webrtc_stream.h"

#include <algorithm>
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

bool is_start_code_at(const std::vector<uint8_t>& bytes, size_t offset, size_t* start_code_size) {
  if (offset + 3 <= bytes.size() && bytes[offset] == 0x00 && bytes[offset + 1] == 0x00 && bytes[offset + 2] == 0x01) {
    *start_code_size = 3;
    return true;
  }
  if (offset + 4 <= bytes.size() && bytes[offset] == 0x00 && bytes[offset + 1] == 0x00 && bytes[offset + 2] == 0x00 &&
      bytes[offset + 3] == 0x01) {
    *start_code_size = 4;
    return true;
  }
  return false;
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

  void on_latest_encoded_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) override {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_encoded_unit_ = std::move(latest_encoded_unit);
    latest_encoded_access_unit_available_ = latest_encoded_unit_ != nullptr && latest_encoded_unit_->valid;
    if (!latest_encoded_access_unit_available_) {
      latest_encoded_codec_.clear();
      latest_encoded_timestamp_ns_ = 0;
      latest_encoded_sequence_id_ = 0;
      latest_encoded_size_bytes_ = 0;
      latest_encoded_keyframe_ = false;
      latest_encoded_codec_config_ = false;
      return;
    }

    latest_encoded_codec_ = codec_to_string(latest_encoded_unit_->codec);
    latest_encoded_timestamp_ns_ = latest_encoded_unit_->timestamp_ns;
    latest_encoded_sequence_id_ = latest_encoded_unit_->sequence_id;
    latest_encoded_size_bytes_ = latest_encoded_unit_->bytes.size();
    latest_encoded_keyframe_ = latest_encoded_unit_->keyframe;
    latest_encoded_codec_config_ = latest_encoded_unit_->codec_config;
  }

  std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_encoded_unit_;
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
      snapshot.latest_encoded_sequence_id = latest_encoded_sequence_id_;
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
  uint64_t latest_encoded_sequence_id_{0};
  size_t latest_encoded_size_bytes_{0};
  std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit_;
  bool latest_encoded_keyframe_{false};
  bool latest_encoded_codec_config_{false};
};

class H264EncodedVideoSender : public IEncodedVideoSender {
 public:
  void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) override {
    if (latest_encoded_unit == nullptr || !latest_encoded_unit->valid) {
      return;
    }

    const H264AccessUnitDescriptor descriptor = inspect_h264_access_unit(*latest_encoded_unit);

    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_encoded_unit_ = true;
    codec_ = codec_to_string(latest_encoded_unit->codec);

    if (last_delivered_sequence_id_ == latest_encoded_unit->sequence_id) {
      ++duplicate_units_skipped_;
      return;
    }

    last_delivered_sequence_id_ = latest_encoded_unit->sequence_id;
    last_delivered_timestamp_ns_ = latest_encoded_unit->timestamp_ns;
    last_delivered_size_bytes_ = latest_encoded_unit->bytes.size();
    last_delivered_keyframe_ = latest_encoded_unit->keyframe;
    last_delivered_codec_config_ = latest_encoded_unit->codec_config;
    last_contains_sps_ = descriptor.has_sps;
    last_contains_pps_ = descriptor.has_pps;
    last_contains_idr_ = descriptor.has_idr;
    last_contains_non_idr_ = descriptor.has_non_idr_slice;
    codec_config_seen_ = codec_config_seen_ || latest_encoded_unit->codec_config ||
                         (descriptor.has_sps && descriptor.has_pps);
    ready_for_video_track_ = codec_config_seen_ && (latest_encoded_unit->keyframe || descriptor.has_idr);
    ++delivered_units_;

    if (ready_for_video_track_) {
      sender_state_ = "ready-for-h264-rtp-packetization";
    } else if (codec_config_seen_) {
      sender_state_ = "waiting-for-h264-keyframe";
    } else {
      sender_state_ = "waiting-for-h264-codec-config";
    }
  }

  EncodedVideoSenderSnapshot snapshot() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    EncodedVideoSenderSnapshot snapshot;
    snapshot.sender_state = sender_state_;
    snapshot.codec = codec_;
    snapshot.has_pending_encoded_unit = has_pending_encoded_unit_;
    snapshot.codec_config_seen = codec_config_seen_;
    snapshot.ready_for_video_track = ready_for_video_track_;
    snapshot.delivered_units = delivered_units_;
    snapshot.duplicate_units_skipped = duplicate_units_skipped_;
    snapshot.last_delivered_sequence_id = last_delivered_sequence_id_;
    snapshot.last_delivered_timestamp_ns = last_delivered_timestamp_ns_;
    snapshot.last_delivered_size_bytes = last_delivered_size_bytes_;
    snapshot.last_delivered_keyframe = last_delivered_keyframe_;
    snapshot.last_delivered_codec_config = last_delivered_codec_config_;
    snapshot.last_contains_sps = last_contains_sps_;
    snapshot.last_contains_pps = last_contains_pps_;
    snapshot.last_contains_idr = last_contains_idr_;
    snapshot.last_contains_non_idr = last_contains_non_idr_;
    return snapshot;
  }

 private:
  mutable std::mutex mutex_;
  std::string sender_state_{"waiting-for-encoded-input"};
  std::string codec_;
  bool has_pending_encoded_unit_{false};
  bool codec_config_seen_{false};
  bool ready_for_video_track_{false};
  uint64_t delivered_units_{0};
  uint64_t duplicate_units_skipped_{0};
  uint64_t last_delivered_sequence_id_{0};
  uint64_t last_delivered_timestamp_ns_{0};
  size_t last_delivered_size_bytes_{0};
  bool last_delivered_keyframe_{false};
  bool last_delivered_codec_config_{false};
  bool last_contains_sps_{false};
  bool last_contains_pps_{false};
  bool last_contains_idr_{false};
  bool last_contains_non_idr_{false};
};

}  // namespace

H264AccessUnitDescriptor inspect_h264_access_unit(const LatestEncodedUnit& access_unit) {
  H264AccessUnitDescriptor descriptor;
  if (!access_unit.valid || access_unit.codec != VideoCodec::H264 || access_unit.bytes.empty()) {
    return descriptor;
  }

  const auto& bytes = access_unit.bytes;
  size_t nal_start = 0;
  bool found_any = false;
  while (nal_start < bytes.size()) {
    size_t start_code_size = 0;
    while (nal_start < bytes.size() && !is_start_code_at(bytes, nal_start, &start_code_size)) {
      ++nal_start;
    }
    if (nal_start >= bytes.size()) {
      break;
    }

    const size_t payload_offset = nal_start + start_code_size;
    if (payload_offset >= bytes.size()) {
      break;
    }

    size_t nal_end = payload_offset;
    size_t next_start_code_size = 0;
    while (nal_end < bytes.size() && !is_start_code_at(bytes, nal_end, &next_start_code_size)) {
      ++nal_end;
    }

    if (nal_end <= payload_offset) {
      nal_start = std::min(bytes.size(), payload_offset + 1);
      continue;
    }

    const uint8_t nal_type = static_cast<uint8_t>(bytes[payload_offset] & 0x1f);
    descriptor.nal_units.push_back(H264NalUnitInfo{nal_type, payload_offset, nal_end - payload_offset});
    descriptor.valid = true;
    found_any = true;

    switch (nal_type) {
      case 7:
        descriptor.has_sps = true;
        break;
      case 8:
        descriptor.has_pps = true;
        break;
      case 5:
        descriptor.has_idr = true;
        break;
      case 1:
        descriptor.has_non_idr_slice = true;
        break;
      case 9:
        descriptor.has_aud = true;
        break;
      default:
        break;
    }

    nal_start = nal_end;
  }

  if (!found_any && !bytes.empty()) {
    const uint8_t nal_type = static_cast<uint8_t>(bytes.front() & 0x1f);
    descriptor.nal_units.push_back(H264NalUnitInfo{nal_type, 0, bytes.size()});
    descriptor.valid = true;
    descriptor.has_sps = nal_type == 7;
    descriptor.has_pps = nal_type == 8;
    descriptor.has_idr = nal_type == 5;
    descriptor.has_non_idr_slice = nal_type == 1;
    descriptor.has_aud = nal_type == 9;
  }

  return descriptor;
}

WebRtcStreamSession::WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter,
                                         LatestEncodedUnitGetter latest_encoded_unit_getter)
    : stream_id_(std::move(stream_id)) {
  rtc::Configuration config;
  config.disableAutoNegotiation = false;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(config);
  media_source_ = std::make_unique<StreamMediaSourceBridge>();
  encoded_sender_ = std::make_unique<H264EncodedVideoSender>();
  media_source_->on_latest_frame(latest_frame_getter(stream_id_));
  auto latest_encoded = latest_encoded_unit_getter(stream_id_);
  media_source_->on_latest_encoded_unit(latest_encoded);
  encoded_sender_->on_encoded_access_unit(std::move(latest_encoded));
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
  std::unique_ptr<IWebRtcMediaSourceBridge>* media_source = &media_source_;
  std::lock_guard<std::mutex> lock(mutex_);
  (*media_source)->on_latest_frame(std::move(latest_frame));
}

void WebRtcStreamSession::on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) {
  std::unique_lock<std::mutex> lock(mutex_);
  media_source_->on_latest_encoded_unit(latest_encoded_unit);
  auto sender = encoded_sender_.get();
  lock.unlock();
  sender->on_encoded_access_unit(std::move(latest_encoded_unit));
}

WebRtcSessionSnapshot WebRtcStreamSession::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  WebRtcMediaSourceSnapshot media_snapshot = media_source_->snapshot();
  media_snapshot.encoded_sender = encoded_sender_->snapshot();
  return WebRtcSessionSnapshot{stream_id_,
                               offer_sdp_,
                               answer_sdp_,
                               last_remote_candidate_,
                               last_local_candidate_,
                               peer_state_,
                               std::move(media_snapshot)};
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
