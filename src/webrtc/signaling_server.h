#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "webrtc_stream.h"

namespace video_server
{

    /** @brief Callback used to validate stream existence. */
    using StreamExistsFn = std::function<bool(const std::string &)>;
    /** @brief Getter alias used to fetch the configured per-stream subscriber cap. */
    using MaxSubscribersGetterFn = std::function<std::optional<uint32_t>(const std::string &)>;
    /** @brief Getter alias used to fetch the latest transformed frame. */
    using LatestFrameGetterFn = WebRtcStreamSession::LatestFrameGetter;
    /** @brief Getter alias used to fetch the latest encoded unit. */
    using LatestEncodedUnitGetterFn = WebRtcStreamSession::LatestEncodedUnitGetter;

    /**
     * @brief Public-facing session snapshot stored by the signaling layer.
     */
    struct SignalingSession
    {
        std::string session_id;                  /**< Stable signaling session identifier for one recipient. */
        uint64_t session_generation{0};         /**< Monotonic session generation for the stream. */
        std::string stream_id;                  /**< Identifier of the owning stream. */
        std::string offer_sdp;                  /**< Latest applied remote offer SDP. */
        std::string answer_sdp;                 /**< Latest generated or applied answer SDP. */
        std::string last_remote_candidate;      /**< Latest remote ICE candidate. */
        std::string last_local_candidate;       /**< Latest local ICE candidate. */
        std::string peer_state;                 /**< Current peer connection state string. */
        bool active{true};                      /**< True when the session is active. */
        bool sending_active{false};             /**< True when the session has an active sending path. */
        std::string teardown_reason;            /**< Reason the session became inactive, when applicable. */
        std::string last_transition_reason;     /**< Last recorded lifecycle transition reason. */
        uint64_t disconnect_count{0};           /**< Number of disconnect-like terminal transitions observed. */
        WebRtcMediaSourceSnapshot media_source; /**< Current media-source bridge snapshot. */
    };

    /**
     * @brief Stream-scoped signaling and session manager.
     */
    class SignalingServer
    {
    public:
        /**
         * @brief Creates a signaling server bound to stream existence and snapshot getters.
         *
         * @param stream_exists Callback used to validate stream ids.
         * @param max_subscribers_getter Callback used to fetch each stream's subscriber cap.
         * @param latest_frame_getter Callback used to fetch latest-frame snapshots.
         * @param latest_encoded_unit_getter Callback used to fetch latest encoded-unit snapshots.
         * @param max_pending_candidates_per_stream Maximum number of queued candidates before a session exists.
         */
        SignalingServer(StreamExistsFn stream_exists, MaxSubscribersGetterFn max_subscribers_getter,
                        LatestFrameGetterFn latest_frame_getter,
                        LatestEncodedUnitGetterFn latest_encoded_unit_getter,
                        size_t max_pending_candidates_per_stream = 32);
        ~SignalingServer();

        /**
         * @brief Creates or replaces the active session for a stream from a remote offer.
         *
         * @param stream_id Identifier of the target stream.
         * @param offer_sdp Remote offer SDP.
         * @param error_message Optional destination for a human-readable failure reason.
         * @return True when the offer was accepted, false otherwise.
         */
        bool set_offer(const std::string &stream_id, const std::string &offer_sdp, std::string *error_message = nullptr,
                       std::string *session_id_out = nullptr);

        /**
         * @brief Applies a remote answer to an active session.
         *
         * @param stream_id Identifier of the target stream.
         * @param answer_sdp Remote answer SDP.
         * @param error_message Optional destination for a human-readable failure reason.
         * @return True when the answer was accepted, false otherwise.
         */
        bool set_answer(const std::string &stream_id, const std::string &answer_sdp, std::string *error_message = nullptr,
                        std::optional<std::string> session_id = std::nullopt);

        /**
         * @brief Queues or applies a remote ICE candidate.
         *
         * @param stream_id Identifier of the target stream.
         * @param candidate Remote ICE candidate string.
         * @param error_message Optional destination for a human-readable failure reason.
         * @return True when the candidate was queued or applied, false otherwise.
         */
        bool add_ice_candidate(const std::string &stream_id, const std::string &candidate,
                               std::string *error_message = nullptr,
                               std::optional<std::string> session_id = std::nullopt);

        /**
         * @brief Publishes a latest-frame update to the active session for a stream.
         *
         * @param stream_id Identifier of the updated stream.
         * @param latest_frame Latest transformed frame snapshot.
         */
        void on_latest_frame(const std::string &stream_id, std::shared_ptr<const LatestFrame> latest_frame);

        /**
         * @brief Publishes an encoded-unit update to the active session for a stream.
         *
         * @param stream_id Identifier of the updated stream.
         * @param latest_encoded_unit Latest encoded access-unit snapshot.
         */
        void on_encoded_access_unit(const std::string &stream_id,
                                    std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit);

        /**
         * @brief Returns the current session snapshot for one stream.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Session snapshot when the stream has a session, otherwise `std::nullopt`.
         */
        std::optional<SignalingSession> get_session(const std::string &stream_id,
                                                    std::optional<std::string> session_id = std::nullopt) const;

        /**
         * @brief Returns all active session snapshots.
         *
         * @return List of tracked signaling sessions.
         */
        std::vector<SignalingSession> list_sessions() const;

        /**
         * @brief Removes a stream and stops its active session.
         *
         * @param stream_id Identifier of the stream to remove.
         */
        void remove_stream(const std::string &stream_id);

        /** @brief Stops all active sessions. */
        void stop();

    private:
        /**
         * @brief Session slot tracked for one stream id.
         */
        struct StreamSessionSlot
        {
            std::vector<std::string> pending_remote_candidates; /**< Candidates queued before session creation. */
            uint64_t next_session_generation{0};                /**< Monotonic generation assigned to each new recipient. */
            struct RecipientSessionSlot
            {
                uint64_t session_generation{0};               /**< Monotonic session generation for the recipient. */
                std::shared_ptr<WebRtcStreamSession> session; /**< Session instance when present. */
            };
            std::unordered_map<std::string, RecipientSessionSlot> sessions; /**< Active and recently inactive recipients. */
        };

        static void prune_inactive_recipients_locked(StreamSessionSlot &slot);

        StreamExistsFn stream_exists_;
        MaxSubscribersGetterFn max_subscribers_getter_;
        LatestFrameGetterFn latest_frame_getter_;
        LatestEncodedUnitGetterFn latest_encoded_unit_getter_;
        size_t max_pending_candidates_per_stream_;

        mutable std::mutex mutex_;
        mutable std::unordered_map<std::string, StreamSessionSlot> sessions_;
    };

} // namespace video_server
