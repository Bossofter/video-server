#include "webrtc_stream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
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

constexpr uint8_t kH264PayloadType = 102;
constexpr uint64_t kH264ClockRate = 90000;
constexpr size_t kMaxRtpPayloadSize = 1200;

uint32_t timestamp_ns_to_h264_rtp_timestamp(uint64_t timestamp_ns) {
  constexpr uint64_t kClockRate = kH264ClockRate;
  constexpr uint64_t kNsPerSecond = 1000000000ull;
  return static_cast<uint32_t>((timestamp_ns * kClockRate) / kNsPerSecond);
}

uint32_t make_random_ssrc() {
  static std::mt19937 generator(std::random_device{}());
  static std::uniform_int_distribution<uint32_t> distribution(1u, 0xffffffffu);
  return distribution(generator);
}

std::vector<std::vector<uint8_t>> split_annex_b_nalus(const std::vector<uint8_t>& bytes) {
  std::vector<std::vector<uint8_t>> nalus;
  size_t nal_start = 0;
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
    if (nal_end > payload_offset) {
      nalus.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(nal_end));
    }
    nal_start = nal_end;
  }

  if (nalus.empty() && !bytes.empty()) {
    nalus.push_back(bytes);
  }
  return nalus;
}

size_t count_sdp_media_sections(const std::string& sdp) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = sdp.find("\nm=", pos)) != std::string::npos) {
    ++count;
    pos += 3;
  }
  if (sdp.rfind("m=", 0) == 0) {
    ++count;
  }
  return count;
}

std::vector<std::string> extract_sdp_mids(const std::string& sdp) {
  std::vector<std::string> mids;
  size_t pos = 0;
  while (pos < sdp.size()) {
    const size_t line_end = sdp.find('\n', pos);
    const size_t line_size = (line_end == std::string::npos ? sdp.size() : line_end) - pos;
    std::string_view line(sdp.data() + pos, line_size);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.rfind("a=mid:", 0) == 0) {
      mids.emplace_back(line.substr(6));
    }
    if (line_end == std::string::npos) {
      break;
    }
    pos = line_end + 1;
  }
  return mids;
}

size_t count_rejected_sdp_media_sections(const std::string& sdp) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = sdp.find("\nm=", pos)) != std::string::npos) {
    const size_t line_start = pos + 1;
    const size_t line_end = sdp.find('\n', line_start);
    const std::string line = sdp.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
    if (line.find("m=") == 0 && line.find(" 0 ") != std::string::npos) {
      ++count;
    }
    pos = line_start + 2;
  }
  if (sdp.rfind("m=", 0) == 0) {
    const size_t line_end = sdp.find('\n');
    const std::string line = sdp.substr(0, line_end == std::string::npos ? std::string::npos : line_end);
    if (line.find(" 0 ") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

std::string join_strings(const std::vector<std::string>& values) {
  std::ostringstream out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  return out.str();
}

std::string sanitize_answer_setup_role(std::string sdp) {
  const std::string from = "a=setup:actpass";
  const std::string to = "a=setup:active";
  size_t pos = 0;
  while ((pos = sdp.find(from, pos)) != std::string::npos) {
    sdp.replace(pos, from.size(), to);
    pos += to.size();
  }
  return sdp;
}

std::vector<uint8_t> make_rtp_packet(const uint8_t* payload, size_t payload_size, uint16_t sequence_number,
                                     uint32_t timestamp, uint32_t ssrc, bool marker) {
  std::vector<uint8_t> packet(12 + payload_size);
  packet[0] = 0x80;
  packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | kH264PayloadType);
  packet[2] = static_cast<uint8_t>(sequence_number >> 8);
  packet[3] = static_cast<uint8_t>(sequence_number & 0xff);
  packet[4] = static_cast<uint8_t>(timestamp >> 24);
  packet[5] = static_cast<uint8_t>((timestamp >> 16) & 0xff);
  packet[6] = static_cast<uint8_t>((timestamp >> 8) & 0xff);
  packet[7] = static_cast<uint8_t>(timestamp & 0xff);
  packet[8] = static_cast<uint8_t>(ssrc >> 24);
  packet[9] = static_cast<uint8_t>((ssrc >> 16) & 0xff);
  packet[10] = static_cast<uint8_t>((ssrc >> 8) & 0xff);
  packet[11] = static_cast<uint8_t>(ssrc & 0xff);
  std::copy(payload, payload + payload_size, packet.begin() + 12);
  return packet;
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
  H264EncodedVideoSender(std::shared_ptr<IEncodedVideoTrackSink> video_track_sink, uint32_t ssrc)
      : video_track_sink_(std::move(video_track_sink)), ssrc_(ssrc) {}

  void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) override {
    if (latest_encoded_unit == nullptr || !latest_encoded_unit->valid) {
      return;
    }

    if (latest_encoded_unit->codec != VideoCodec::H264 || latest_encoded_unit->bytes.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++failed_units_;
      sender_state_ = "rejected-non-h264-access-unit";
      last_packetization_status_ = "rejected-invalid-input";
      return;
    }

    const H264AccessUnitDescriptor descriptor = inspect_h264_access_unit(*latest_encoded_unit);
    const bool contains_codec_config = latest_encoded_unit->codec_config || (descriptor.has_sps && descriptor.has_pps);
    const bool is_keyframe = latest_encoded_unit->keyframe || descriptor.has_idr;
    bool should_send = false;
    bool has_track = false;
    bool track_open = false;
    std::shared_ptr<IEncodedVideoTrackSink> track_sink;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      has_pending_encoded_unit_ = true;
      codec_ = codec_to_string(latest_encoded_unit->codec);
      video_track_exists_ = video_track_sink_ && video_track_sink_->exists();
      video_track_open_ = video_track_sink_ && video_track_sink_->is_open();
      has_track = video_track_exists_;
      track_open = video_track_open_;
      track_sink = video_track_sink_;

      if (last_delivered_sequence_id_ == latest_encoded_unit->sequence_id) {
        ++duplicate_units_skipped_;
        last_packetization_status_ = "duplicate-sequence-skipped";
        return;
      }

      if (contains_codec_config) {
        cached_codec_config_ = latest_encoded_unit;
        cached_codec_config_available_ = true;
      }

      codec_config_seen_ = codec_config_seen_ || contains_codec_config;
      keyframe_seen_ = keyframe_seen_ || is_keyframe;
      ready_for_video_track_ = has_track && codec_config_seen_ && keyframe_seen_;

      last_delivered_sequence_id_ = latest_encoded_unit->sequence_id;
      last_delivered_timestamp_ns_ = latest_encoded_unit->timestamp_ns;
      last_delivered_size_bytes_ = latest_encoded_unit->bytes.size();
      last_delivered_keyframe_ = latest_encoded_unit->keyframe;
      last_delivered_codec_config_ = latest_encoded_unit->codec_config;
      last_contains_sps_ = descriptor.has_sps;
      last_contains_pps_ = descriptor.has_pps;
      last_contains_idr_ = descriptor.has_idr;
      last_contains_non_idr_ = descriptor.has_non_idr_slice;

      ++delivered_units_;
      should_send = ready_for_video_track_ && has_track && track_open;
      if (!has_track) {
        sender_state_ = "video-track-missing";
        last_packetization_status_ = "no-video-track";
      } else if (!codec_config_seen_) {
        sender_state_ = "waiting-for-h264-codec-config";
        last_packetization_status_ = "codec-config-required";
      } else if (!keyframe_seen_) {
        sender_state_ = "waiting-for-h264-keyframe";
        last_packetization_status_ = "keyframe-required";
      } else if (!track_open) {
        sender_state_ = "waiting-for-video-track-open";
        last_packetization_status_ = "track-not-open-yet";
      } else {
        sender_state_ = "sending-h264-rtp";
        last_packetization_status_ = "packetization-attempt-pending";
      }
    }

    if (!should_send || !track_sink) {
      return;
    }

    const uint32_t rtp_timestamp = timestamp_ns_to_h264_rtp_timestamp(latest_encoded_unit->timestamp_ns);
    std::vector<std::vector<uint8_t>> nalus_to_send;
    if (cached_codec_config_available_ && is_keyframe && !contains_codec_config && cached_codec_config_ != nullptr) {
      auto cached_nalus = split_annex_b_nalus(cached_codec_config_->bytes);
      nalus_to_send.insert(nalus_to_send.end(), cached_nalus.begin(), cached_nalus.end());
    }
    auto unit_nalus = split_annex_b_nalus(latest_encoded_unit->bytes);
    nalus_to_send.insert(nalus_to_send.end(), unit_nalus.begin(), unit_nalus.end());

    uint64_t packet_count = 0;
    for (size_t nal_index = 0; nal_index < nalus_to_send.size(); ++nal_index) {
      const auto& nal = nalus_to_send[nal_index];
      const bool is_last_nal = nal_index + 1 == nalus_to_send.size();
      if (nal.empty()) {
        continue;
      }

      if (nal.size() <= kMaxRtpPayloadSize) {
        auto packet = make_rtp_packet(nal.data(), nal.size(), sequence_number_++, rtp_timestamp, ssrc_, is_last_nal);
        track_sink->send(reinterpret_cast<const std::byte*>(packet.data()), packet.size());
        ++packet_count;
        continue;
      }

      const uint8_t nal_header = nal[0];
      const uint8_t fu_indicator = static_cast<uint8_t>((nal_header & 0xe0) | 28u);
      const uint8_t nal_type = static_cast<uint8_t>(nal_header & 0x1f);
      const size_t fragment_payload_capacity = kMaxRtpPayloadSize - 2;
      size_t offset = 1;
      while (offset < nal.size()) {
        const size_t remaining = nal.size() - offset;
        const size_t fragment_size = std::min(fragment_payload_capacity, remaining);
        const bool start = offset == 1;
        const bool end = fragment_size == remaining;
        std::vector<uint8_t> fu_payload(2 + fragment_size);
        fu_payload[0] = fu_indicator;
        fu_payload[1] = static_cast<uint8_t>((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type);
        std::copy(nal.begin() + static_cast<std::ptrdiff_t>(offset),
                  nal.begin() + static_cast<std::ptrdiff_t>(offset + fragment_size), fu_payload.begin() + 2);
        auto packet = make_rtp_packet(fu_payload.data(), fu_payload.size(), sequence_number_++, rtp_timestamp, ssrc_,
                                      is_last_nal && end);
        track_sink->send(reinterpret_cast<const std::byte*>(packet.data()), packet.size());
        ++packet_count;
        offset += fragment_size;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      packets_attempted_ += packet_count;
      h264_delivery_active_ = true;
      video_track_open_ = video_track_sink_ && video_track_sink_->is_open();
      last_packetization_status_ = packet_count > 0 ? "rtp-packets-sent" : "no-rtp-packets-generated";
    }
  }

  EncodedVideoSenderSnapshot snapshot() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    EncodedVideoSenderSnapshot snapshot;
    snapshot.sender_state = sender_state_;
    snapshot.codec = codec_;
    snapshot.has_pending_encoded_unit = has_pending_encoded_unit_;
    snapshot.codec_config_seen = codec_config_seen_;
    snapshot.video_track_exists = video_track_sink_ && video_track_sink_->exists();
    snapshot.video_track_open = video_track_sink_ && video_track_sink_->is_open();
    snapshot.ready_for_video_track = snapshot.video_track_exists && codec_config_seen_ && keyframe_seen_;
    snapshot.h264_delivery_active = h264_delivery_active_;
    snapshot.keyframe_seen = keyframe_seen_;
    snapshot.cached_codec_config_available = cached_codec_config_available_;
    snapshot.delivered_units = delivered_units_;
    snapshot.duplicate_units_skipped = duplicate_units_skipped_;
    snapshot.failed_units = failed_units_;
    snapshot.packets_attempted = packets_attempted_;
    snapshot.last_delivered_sequence_id = last_delivered_sequence_id_;
    snapshot.last_delivered_timestamp_ns = last_delivered_timestamp_ns_;
    snapshot.last_delivered_size_bytes = last_delivered_size_bytes_;
    snapshot.last_delivered_keyframe = last_delivered_keyframe_;
    snapshot.last_delivered_codec_config = last_delivered_codec_config_;
    snapshot.last_contains_sps = last_contains_sps_;
    snapshot.last_contains_pps = last_contains_pps_;
    snapshot.last_contains_idr = last_contains_idr_;
    snapshot.last_contains_non_idr = last_contains_non_idr_;
    snapshot.last_packetization_status = last_packetization_status_;
    snapshot.video_mid = video_track_sink_ ? video_track_sink_->mid() : "";
    if (snapshot.sender_state == "video-track-missing" && snapshot.video_track_exists) {
      if (!snapshot.codec_config_seen) {
        snapshot.sender_state = "waiting-for-h264-codec-config";
      } else if (!snapshot.keyframe_seen) {
        snapshot.sender_state = "waiting-for-h264-keyframe";
      } else if (!snapshot.video_track_open) {
        snapshot.sender_state = "waiting-for-video-track-open";
      } else {
        snapshot.sender_state = "sending-h264-rtp";
      }
    }
    return snapshot;
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<IEncodedVideoTrackSink> video_track_sink_;
  std::shared_ptr<const LatestEncodedUnit> cached_codec_config_;
  std::string sender_state_{"waiting-for-encoded-input"};
  std::string codec_;
  uint32_t ssrc_{0};
  uint16_t sequence_number_{0};
  bool has_pending_encoded_unit_{false};
  bool codec_config_seen_{false};
  bool ready_for_video_track_{false};
  bool video_track_exists_{false};
  bool video_track_open_{false};
  bool h264_delivery_active_{false};
  bool keyframe_seen_{false};
  bool cached_codec_config_available_{false};
  uint64_t delivered_units_{0};
  uint64_t duplicate_units_skipped_{0};
  uint64_t failed_units_{0};
  uint64_t packets_attempted_{0};
  uint64_t last_delivered_sequence_id_{0};
  uint64_t last_delivered_timestamp_ns_{0};
  size_t last_delivered_size_bytes_{0};
  bool last_delivered_keyframe_{false};
  bool last_delivered_codec_config_{false};
  bool last_contains_sps_{false};
  bool last_contains_pps_{false};
  bool last_contains_idr_{false};
  bool last_contains_non_idr_{false};
  std::string last_packetization_status_{"awaiting-encoded-input"};
};

class RtcTrackPacketSink : public IEncodedVideoTrackSink {
 public:
  explicit RtcTrackPacketSink(std::shared_ptr<rtc::Track> track) : track_(std::move(track)) {}

  bool exists() const override { return static_cast<bool>(track_); }
  bool is_open() const override { return track_ && track_->isOpen(); }
  std::string mid() const override { return track_ ? track_->mid() : ""; }

  void send(const std::byte* data, size_t size) override {
    if (track_) {
      track_->send(data, size);
    }
  }

 private:
  std::shared_ptr<rtc::Track> track_;
};

class AttachableRtcTrackSink : public IEncodedVideoTrackSink {
 public:
  bool exists() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(track_);
  }

  bool is_open() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return track_ && track_->isOpen();
  }

  std::string mid() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return track_ ? track_->mid() : "";
  }

  void send(const std::byte* data, size_t size) override {
    std::shared_ptr<rtc::Track> track;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      track = track_;
    }
    if (track) {
      track->send(data, size);
    }
  }

  void bind(std::shared_ptr<rtc::Track> track) {
    std::lock_guard<std::mutex> lock(mutex_);
    track_ = std::move(track);
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<rtc::Track> track_;
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

std::unique_ptr<IEncodedVideoSender> make_h264_encoded_video_sender(std::shared_ptr<IEncodedVideoTrackSink> video_track_sink,
                                                                    uint32_t ssrc) {
  return std::make_unique<H264EncodedVideoSender>(std::move(video_track_sink), ssrc);
}

WebRtcStreamSession::WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter,
                                         LatestEncodedUnitGetter latest_encoded_unit_getter)
    : stream_id_(std::move(stream_id)) {
  rtc::Configuration config;
  config.disableAutoNegotiation = false;
  peer_connection_ = std::make_shared<rtc::PeerConnection>(config);
  media_source_ = std::make_unique<StreamMediaSourceBridge>();
  video_ssrc_ = make_random_ssrc();
  video_track_sink_ = std::make_shared<AttachableRtcTrackSink>();
  encoded_sender_ = make_h264_encoded_video_sender(video_track_sink_, video_ssrc_);
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
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peer_connection = peer_connection_;
    }

    const auto offer_mids = extract_sdp_mids(offer_sdp);
    std::clog << "[signaling] applying offer stream=" << stream_id_
              << " media_sections=" << count_sdp_media_sections(offer_sdp)
              << " mids=" << join_strings(offer_mids) << '\n';

    // PeerConnection calls may synchronously invoke onLocalDescription/onLocalCandidate/onStateChange,
    // so the session mutex must not be held across libdatachannel API calls.
    peer_connection->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
    peer_connection->setLocalDescription();

    std::lock_guard<std::mutex> lock(mutex_);
    offer_sdp_ = offer_sdp;
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
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peer_connection = peer_connection_;
    }

    // PeerConnection callbacks may reenter the session synchronously here too.
    peer_connection->setRemoteDescription(rtc::Description(answer_sdp, "answer"));

    std::lock_guard<std::mutex> lock(mutex_);
    answer_sdp_ = answer_sdp;
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
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peer_connection = peer_connection_;
    }

    // Keep candidate insertion callback-safe for synchronous libdatachannel reentrancy.
    peer_connection->addRemoteCandidate(rtc::Candidate(candidate_sdp));

    std::lock_guard<std::mutex> lock(mutex_);
    last_remote_candidate_ = candidate_sdp;
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
    // close() may also trigger state callbacks, so do not hold the session mutex here.
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
  // These callbacks may run synchronously while PeerConnection API calls are still on the stack.
  // They only touch session-local state under mutex_ and therefore must stay independent.
  peer_connection_->onTrack([this](std::shared_ptr<rtc::Track> track) {
    if (!track) {
      std::clog << "[signaling] track callback stream=" << stream_id_ << " with null track\n";
      return;
    }

    const auto description = track->description();
    std::clog << "[signaling] track callback stream=" << stream_id_ << " mid=" << track->mid()
              << " type=" << description.type() << " direction=" << static_cast<int>(description.direction())
              << '\n';

    if (description.type() != "video") {
      return;
    }

    int payload_type = kH264PayloadType;
    std::string h264_profile = rtc::DEFAULT_H264_VIDEO_PROFILE;
    for (const int offered_payload_type : description.payloadTypes()) {
      const auto* rtp_map = description.rtpMap(offered_payload_type);
      if (!rtp_map || rtp_map->format != "H264") {
        continue;
      }
      payload_type = offered_payload_type;
      if (!rtp_map->fmtps.empty()) {
        h264_profile = rtp_map->fmtps.front();
      }
      break;
    }

    rtc::Description::Video local_video(track->mid(), rtc::Description::Direction::SendOnly);
    local_video.addH264Codec(payload_type, h264_profile);
    local_video.addSSRC(video_ssrc_, "video-server-video", stream_id_, "h264-track");
    track->setDescription(local_video);
    std::clog << "[signaling] activated negotiated video section stream=" << stream_id_
              << " mid=" << track->mid() << " payload_type=" << payload_type
              << " ssrc=" << video_ssrc_ << '\n';

    auto attachable_sink = std::dynamic_pointer_cast<AttachableRtcTrackSink>(video_track_sink_);
    if (!attachable_sink) {
      std::clog << "[signaling] failed to access attachable video track sink stream=" << stream_id_ << '\n';
      return;
    }

    attachable_sink->bind(track);
    std::clog << "[signaling] bound negotiated video track stream=" << stream_id_
              << " reused_mid=" << track->mid() << '\n';
  });

  peer_connection_->onLocalDescription([this](rtc::Description description) {
    std::string answer = std::string(description);
    answer = sanitize_answer_setup_role(std::move(answer));
    const auto answer_mids = extract_sdp_mids(answer);
    std::clog << "[signaling] answer generated stream=" << stream_id_ << " size=" << answer.size()
              << " media_sections=" << count_sdp_media_sections(answer)
              << " rejected_media_sections=" << count_rejected_sdp_media_sections(answer)
              << " mids=" << join_strings(answer_mids) << '\n';
    std::lock_guard<std::mutex> lock(mutex_);
    answer_sdp_ = answer;
  });

  peer_connection_->onLocalCandidate([this](rtc::Candidate candidate) {
    const std::string local_candidate = std::string(candidate);
    std::clog << "[signaling] local candidate generated stream=" << stream_id_
              << " size=" << local_candidate.size() << '\n';
    std::lock_guard<std::mutex> lock(mutex_);
    last_local_candidate_ = local_candidate;
  });

  peer_connection_->onStateChange([this](rtc::PeerConnection::State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_state_ = peer_state_to_string(state);
  });
}

}  // namespace video_server
