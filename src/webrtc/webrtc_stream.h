#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>

#include "../core/video_server_core.h"
#include "video_server/encoded_access_unit_view.h"

namespace video_server {

struct H264NalUnitInfo {
  uint8_t nal_type{0};
  size_t offset{0};
  size_t size_bytes{0};
};

struct H264AccessUnitDescriptor {
  bool valid{false};
  bool has_sps{false};
  bool has_pps{false};
  bool has_idr{false};
  bool has_non_idr_slice{false};
  bool has_aud{false};
  std::vector<H264NalUnitInfo> nal_units;
};

struct EncodedVideoSenderSnapshot {
  std::string sender_state;
  std::string codec;
  bool has_pending_encoded_unit{false};
  bool codec_config_seen{false};
  bool ready_for_video_track{false};
  bool video_track_exists{false};
  bool video_track_open{false};
  bool h264_delivery_active{false};
  bool keyframe_seen{false};
  bool cached_codec_config_available{false};
  uint64_t delivered_units{0};
  uint64_t duplicate_units_skipped{0};
  uint64_t failed_units{0};
  uint64_t packets_attempted{0};
  uint64_t last_delivered_sequence_id{0};
  uint64_t last_delivered_timestamp_ns{0};
  size_t last_delivered_size_bytes{0};
  bool last_delivered_keyframe{false};
  bool last_delivered_codec_config{false};
  bool last_contains_sps{false};
  bool last_contains_pps{false};
  bool last_contains_idr{false};
  bool last_contains_non_idr{false};
  std::string last_packetization_status;
  std::string video_mid;
};

struct WebRtcMediaSourceSnapshot {
  std::string bridge_state;
  std::string preferred_media_path;
  bool latest_snapshot_available{false};
  uint64_t latest_snapshot_frame_id{0};
  uint64_t latest_snapshot_timestamp_ns{0};
  uint32_t latest_snapshot_width{0};
  uint32_t latest_snapshot_height{0};
  bool latest_encoded_access_unit_available{false};
  std::string latest_encoded_codec;
  uint64_t latest_encoded_timestamp_ns{0};
  uint64_t latest_encoded_sequence_id{0};
  size_t latest_encoded_size_bytes{0};
  bool latest_encoded_keyframe{false};
  bool latest_encoded_codec_config{false};
  EncodedVideoSenderSnapshot encoded_sender;
};

class IWebRtcMediaSourceBridge {
 public:
  virtual ~IWebRtcMediaSourceBridge() = default;

  virtual void on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame) = 0;
  virtual void on_latest_encoded_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) = 0;
  virtual std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit() const = 0;

  virtual WebRtcMediaSourceSnapshot snapshot() const = 0;
};

struct WebRtcSessionSnapshot {
  std::string stream_id;
  std::string offer_sdp;
  std::string answer_sdp;
  std::string last_remote_candidate;
  std::string last_local_candidate;
  std::string peer_state;
  WebRtcMediaSourceSnapshot media_source;
};

class IEncodedVideoSender {
 public:
  virtual ~IEncodedVideoSender() = default;
  // Session-side delivery/packetization boundary for encoded units feeding the browser-facing track.
  virtual void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) = 0;
  virtual EncodedVideoSenderSnapshot snapshot() const = 0;
};

class IEncodedVideoTrackSink {
 public:
  virtual ~IEncodedVideoTrackSink() = default;
  virtual bool exists() const = 0;
  virtual bool is_open() const = 0;
  virtual std::string mid() const = 0;
  virtual void send(const std::byte* data, size_t size) = 0;
};

class WebRtcStreamSession {
 public:
  using LatestFrameGetter = std::function<std::shared_ptr<const LatestFrame>(const std::string&)>;
  using LatestEncodedUnitGetter = std::function<std::shared_ptr<const LatestEncodedUnit>(const std::string&)>;

  WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter,
                      LatestEncodedUnitGetter latest_encoded_unit_getter);
  ~WebRtcStreamSession();

  WebRtcStreamSession(const WebRtcStreamSession&) = delete;
  WebRtcStreamSession& operator=(const WebRtcStreamSession&) = delete;

  bool apply_offer(const std::string& offer_sdp, std::string* error_message = nullptr);
  bool apply_answer(const std::string& answer_sdp, std::string* error_message = nullptr);
  bool add_remote_candidate(const std::string& candidate_sdp, std::string* error_message = nullptr);
  void on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame);
  void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit);
  WebRtcSessionSnapshot snapshot() const;
  void stop();

 private:
  static std::string peer_state_to_string(rtc::PeerConnection::State state);
  void configure_callbacks();

  const std::string stream_id_;

  mutable std::mutex mutex_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
  std::unique_ptr<IWebRtcMediaSourceBridge> media_source_;
  std::shared_ptr<IEncodedVideoTrackSink> video_track_sink_;
  std::unique_ptr<IEncodedVideoSender> encoded_sender_;
  std::string offer_sdp_;
  std::string answer_sdp_;
  std::string last_remote_candidate_;
  std::string last_local_candidate_;
  std::string peer_state_{"new"};
  uint32_t video_ssrc_{0};
};

H264AccessUnitDescriptor inspect_h264_access_unit(const LatestEncodedUnit& access_unit);
std::unique_ptr<IEncodedVideoSender> make_h264_encoded_video_sender(std::shared_ptr<IEncodedVideoTrackSink> video_track_sink,
                                                                    uint32_t ssrc);

}  // namespace video_server
