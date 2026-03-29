#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>

#include "../core/video_server_core.h"
#include "video_server/encoded_access_unit_view.h"

namespace video_server
{

    /**
     * @brief One parsed H.264 NAL unit inside an access unit.
     */
    struct H264NalUnitInfo
    {
        uint8_t nal_type{0};
        size_t offset{0};
        size_t size_bytes{0};
    };

    /**
     * @brief Parsed summary of an H.264 access unit.
     */
    struct H264AccessUnitDescriptor
    {
        bool valid{false};
        bool has_sps{false};
        bool has_pps{false};
        bool has_idr{false};
        bool has_non_idr_slice{false};
        bool has_aud{false};
        std::vector<H264NalUnitInfo> nal_units;
    };

    /**
     * @brief Snapshot of the encoded sender state for diagnostics.
     */
    struct EncodedVideoSenderSnapshot
    {
        std::string sender_state;
        std::string codec;
        bool session_active{true};
        std::string session_teardown_reason;
        std::string last_lifecycle_event;
        bool has_pending_encoded_unit{false};
        bool codec_config_seen{false};
        bool ready_for_video_track{false};
        bool video_track_exists{false};
        bool video_track_open{false};
        bool h264_delivery_active{false};
        bool keyframe_seen{false};
        bool cached_codec_config_available{false};
        bool cached_idr_available{false};
        bool first_decodable_frame_sent{false};
        bool startup_sequence_sent{false};
        uint64_t delivered_units{0};
        uint64_t duplicate_units_skipped{0};
        uint64_t failed_units{0};
        uint64_t packets_attempted{0};
        uint64_t packets_sent_after_track_open{0};
        uint64_t startup_packets_sent{0};
        uint64_t startup_sequence_injections{0};
        uint64_t first_decodable_transitions{0};
        uint64_t packetization_failures{0};
        uint64_t track_closed_events{0};
        uint64_t send_failures{0};
        uint64_t skipped_no_track{0};
        uint64_t skipped_track_not_open{0};
        uint64_t skipped_codec_config_wait{0};
        uint64_t skipped_keyframe_wait{0};
        uint64_t skipped_startup_idr_wait{0};
        uint64_t last_delivered_sequence_id{0};
        uint64_t last_delivered_timestamp_ns{0};
        size_t last_delivered_size_bytes{0};
        bool last_delivered_keyframe{false};
        bool last_delivered_codec_config{false};
        bool last_contains_sps{false};
        bool last_contains_pps{false};
        bool last_contains_idr{false};
        bool last_contains_non_idr{false};
        int negotiated_h264_payload_type{0};
        std::string negotiated_h264_fmtp;
        std::string last_packetization_status;
        std::string video_mid;
    };

    /**
     * @brief Snapshot of the media-source bridge state for one session.
     */
    struct WebRtcMediaSourceSnapshot
    {
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

    /**
     * @brief Bridge interface that exposes latest frame and encoded-unit snapshots to a session.
     */
    class IWebRtcMediaSourceBridge
    {
    public:
        virtual ~IWebRtcMediaSourceBridge() = default;

        /** @brief Publishes the latest transformed frame snapshot. */
        virtual void on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame) = 0;
        /** @brief Publishes the latest encoded access-unit snapshot. */
        virtual void on_latest_encoded_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) = 0;
        /** @brief Returns the most recent encoded access-unit snapshot. */
        virtual std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit() const = 0;

        /** @brief Returns a diagnostic snapshot of the bridge state. */
        virtual WebRtcMediaSourceSnapshot snapshot() const = 0;
    };

    /**
     * @brief Snapshot of one WebRTC session exposed through signaling inspection.
     */
    struct WebRtcSessionSnapshot
    {
        std::string stream_id;
        std::string offer_sdp;
        std::string answer_sdp;
        std::string last_remote_candidate;
        std::string last_local_candidate;
        std::string peer_state;
        bool active{true};
        bool sending_active{false};
        std::string teardown_reason;
        std::string last_transition_reason;
        uint64_t disconnect_count{0};
        WebRtcMediaSourceSnapshot media_source;
    };

    /**
     * @brief Session-side encoded video sender boundary.
     */
    class IEncodedVideoSender
    {
    public:
        virtual ~IEncodedVideoSender() = default;
        /** @brief Consumes one encoded access-unit snapshot. */
        virtual void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit) = 0;
        /** @brief Applies negotiated H.264 parameters from SDP. */
        virtual void set_negotiated_h264_parameters(int payload_type, std::string fmtp) = 0;
        /** @brief Deactivates the sender permanently for this session. */
        virtual void deactivate(std::string reason) = 0;
        /** @brief Returns a diagnostic sender snapshot. */
        virtual EncodedVideoSenderSnapshot snapshot() const = 0;
    };

    /**
     * @brief Minimal sink used to write packetized media to the negotiated video track.
     */
    class IEncodedVideoTrackSink
    {
    public:
        virtual ~IEncodedVideoTrackSink() = default;
        /** @brief Returns true when a track object exists. */
        virtual bool exists() const = 0;
        /** @brief Returns true when the bound track is open for sending. */
        virtual bool is_open() const = 0;
        /** @brief Returns the MID for the negotiated track when available. */
        virtual std::string mid() const = 0;
        /** @brief Sends one already-packetized payload. */
        virtual void send(const std::byte *data, size_t size) = 0;
    };

    /**
     * @brief One stream-scoped WebRTC session and sender pipeline.
     */
    class WebRtcStreamSession
    {
    public:
        /** @brief Callback used to fetch the latest transformed frame. */
        using LatestFrameGetter = std::function<std::shared_ptr<const LatestFrame>(const std::string &)>;
        /** @brief Callback used to fetch the latest encoded access unit. */
        using LatestEncodedUnitGetter = std::function<std::shared_ptr<const LatestEncodedUnit>(const std::string &)>;

        /** @brief Creates a session for one stream. */
        WebRtcStreamSession(std::string stream_id, LatestFrameGetter latest_frame_getter,
                            LatestEncodedUnitGetter latest_encoded_unit_getter);
        ~WebRtcStreamSession();

        WebRtcStreamSession(const WebRtcStreamSession &) = delete;
        WebRtcStreamSession &operator=(const WebRtcStreamSession &) = delete;

        /** @brief Applies a remote SDP offer. */
        bool apply_offer(const std::string &offer_sdp, std::string *error_message = nullptr);
        /** @brief Applies a remote SDP answer. */
        bool apply_answer(const std::string &answer_sdp, std::string *error_message = nullptr);
        /** @brief Adds a remote ICE candidate. */
        bool add_remote_candidate(const std::string &candidate_sdp, std::string *error_message = nullptr);
        /** @brief Publishes the latest transformed frame snapshot to the session. */
        void on_latest_frame(std::shared_ptr<const LatestFrame> latest_frame);
        /** @brief Publishes the latest encoded access-unit snapshot to the session. */
        void on_encoded_access_unit(std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit);
        /** @brief Returns the current session snapshot. */
        WebRtcSessionSnapshot snapshot() const;
        /** @brief Stops the session and disconnects the peer. */
        void stop();
        /** @brief Returns true while the session is active. */
        bool is_active() const;

    private:
        /** @brief Converts a libdatachannel peer state enum to a readable string. */
        static std::string peer_state_to_string(rtc::PeerConnection::State state);
        /** @brief Transitions the session to an inactive terminal state. */
        void transition_to_inactive_locked(std::string reason, std::string peer_state_override = "");
        /** @brief Installs peer-connection callbacks. */
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
        bool active_{true};
        bool sending_active_{false};
        std::string teardown_reason_{"not-terminated"};
        std::string last_transition_reason_{"session-created"};
        uint64_t disconnect_count_{0};
        uint32_t video_ssrc_{0};
        std::shared_ptr<std::atomic_bool> callbacks_enabled_;
    };

    /**
     * @brief Inspects one encoded unit and summarizes its contained NAL units.
     */
    H264AccessUnitDescriptor inspect_h264_access_unit(const LatestEncodedUnit &access_unit);
    /**
     * @brief Creates the H.264 sender implementation used by stream sessions.
     */
    std::unique_ptr<IEncodedVideoSender> make_h264_encoded_video_sender(std::shared_ptr<IEncodedVideoTrackSink> video_track_sink,
                                                                        uint32_t ssrc);

} // namespace video_server
