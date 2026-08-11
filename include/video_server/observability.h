#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_server
{

    /**
     * @brief Cumulative wall-clock timing counters for a repeated processing stage.
     */
    struct StageTimingCounters
    {
        uint64_t samples{0};       /**< Number of timed stage executions. */
        uint64_t total_time_us{0}; /**< Total wall-clock time spent in this stage. */
        uint64_t last_time_us{0};  /**< Most recent stage duration. */
        uint64_t max_time_us{0};   /**< Largest observed stage duration. */
    };

    /**
     * @brief Sender-side counters exposed for diagnostics and soak reporting.
     */
    struct SenderDebugCounters
    {
        uint64_t delivered_units{0};               /**< Total encoded units delivered to the track sink. */
        uint64_t duplicate_units_skipped{0};       /**< Total duplicate encoded units skipped by sequence id. */
        uint64_t failed_units{0};                  /**< Total encoded units that failed before delivery. */
        uint64_t packetization_failures{0};        /**< Total packetization failures. */
        uint64_t track_closed_events{0};           /**< Total send attempts blocked by a closed track. */
        uint64_t send_failures{0};                 /**< Total transport send failures. */
        uint64_t packets_attempted{0};             /**< Total RTP packets attempted. */
        uint64_t packets_sent_after_track_open{0}; /**< Total RTP packets sent after the track was open. */
        uint64_t startup_packets_sent{0};          /**< Total startup-sequence RTP packets sent. */
        uint64_t startup_sequence_injections{0};   /**< Total startup sequences injected into the stream. */
        uint64_t first_decodable_transitions{0};   /**< Total transitions into the first-decodable state. */
        uint64_t skipped_no_track{0};              /**< Total units skipped because no track existed. */
        uint64_t skipped_track_not_open{0};        /**< Total units skipped because the track was not open. */
        uint64_t skipped_codec_config_wait{0};     /**< Total units skipped while waiting for codec config. */
        uint64_t skipped_keyframe_wait{0};         /**< Total units skipped while waiting for a keyframe. */
        uint64_t skipped_startup_idr_wait{0};      /**< Total units skipped while waiting for startup IDR delivery. */
        StageTimingCounters rtp_send_timing;       /**< Wall-clock timing for RTP packetization and track send. */
    };

    /**
     * @brief Debug view of the current WebRTC session state for a stream.
     */
    struct StreamSessionDebugSnapshot
    {
        uint64_t session_generation{0};         /**< Monotonic session generation for the stream. */
        std::string stream_id;                  /**< Identifier of the owning stream. */
        std::string peer_state;                 /**< Current peer connection state string. */
        bool active{true};                      /**< True when the session is active. */
        bool sending_active{false};             /**< True when a video track is currently bound for sending. */
        std::string sender_state;               /**< Current encoded-sender state string. */
        std::string last_packetization_status;  /**< Most recent packetization status string. */
        std::string teardown_reason;            /**< Reason the session became inactive, when applicable. */
        std::string last_transition_reason;     /**< Last recorded lifecycle transition reason. */
        uint64_t disconnect_count{0};           /**< Number of disconnect-like terminal transitions observed. */
        bool track_exists{false};               /**< True when a negotiated video track exists. */
        bool track_open{false};                 /**< True when the negotiated video track is open. */
        bool startup_sequence_sent{false};      /**< True when startup packets have been emitted. */
        bool first_decodable_frame_sent{false}; /**< True when the first decodable frame has been sent. */
        bool codec_config_seen{false};          /**< True when codec configuration has been observed. */
        bool cached_idr_available{false};       /**< True when a cached IDR frame is available. */
        std::string video_mid;                  /**< Negotiated MID for the video section. */
        SenderDebugCounters counters;           /**< Sender-side counter snapshot. */
    };

    /**
     * @brief Debug snapshot for one stream, including runtime config and session state.
     */
    struct StreamDebugSnapshot
    {
        std::string stream_id;                                     /**< Identifier of the stream. */
        std::string label;                                         /**< Human-readable stream label. */
        uint32_t configured_width{0};                              /**< Registered stream width. */
        uint32_t configured_height{0};                             /**< Registered stream height. */
        double configured_fps{0.0};                                /**< Registered nominal stream FPS. */
        std::string active_filter_mode;                            /**< Active output display/filter mode. */
        uint32_t active_output_width{0};                           /**< Active transformed output width. */
        uint32_t active_output_height{0};                          /**< Active transformed output height. */
        double active_output_fps{0.0};                             /**< Active transformed output FPS. */
        uint64_t config_generation{0};                             /**< Monotonic runtime config generation. */
        std::string config_state;                                  /**< Human-readable config state string. */
        bool latest_raw_frame_available{false};                    /**< True when a latest transformed frame snapshot exists. */
        uint64_t latest_raw_frame_id{0};                           /**< Frame id of the latest transformed frame. */
        uint64_t latest_raw_timestamp_ns{0};                       /**< Timestamp of the latest transformed frame. */
        uint32_t latest_raw_width{0};                              /**< Width of the latest transformed frame. */
        uint32_t latest_raw_height{0};                             /**< Height of the latest transformed frame. */
        bool latest_encoded_access_unit_available{false};          /**< True when a latest encoded-unit snapshot exists. */
        uint64_t latest_encoded_timestamp_ns{0};                   /**< Timestamp of the latest encoded unit. */
        uint64_t latest_encoded_sequence_id{0};                    /**< Sequence id of the latest encoded unit. */
        size_t latest_encoded_size_bytes{0};                       /**< Size of the latest encoded unit in bytes. */
        bool latest_encoded_keyframe{false};                       /**< True when the latest encoded unit is a keyframe. */
        bool latest_encoded_codec_config{false};                   /**< True when the latest encoded unit carries codec config. */
        std::string requested_encoder;                             /**< Encoder mode requested by the raw pipeline, when managed. */
        std::string active_encoder;                                /**< Active encoder implementation, when managed. */
        bool hardware_accelerated{false};                          /**< True when the active encoder uses hardware acceleration. */
        std::string hardware_device;                               /**< Hardware encoder device path, when any. */
        uint64_t encoded_handoff_dropped{0};                       /**< Encoded units overwritten before API/WebRTC delivery. */
        uint64_t stream_step_count{0};                             /**< Managed stream steps that consumed a raw frame. */
        double stream_step_total_ms{0.0};                           /**< Cumulative managed stream-step wall time. */
        double stream_step_max_ms{0.0};                             /**< Maximum managed stream-step wall time. */
        double stream_step_last_ms{0.0};                            /**< Most recent managed stream-step wall time. */
        double stream_step_transform_total_ms{0.0};                 /**< Cumulative display-transform time in managed stepping. */
        double stream_step_transform_max_ms{0.0};                   /**< Maximum display-transform time in managed stepping. */
        double stream_step_transform_last_ms{0.0};                  /**< Most recent display-transform time in managed stepping. */
        double stream_step_pipeline_setup_total_ms{0.0};            /**< Cumulative pipeline create/reconfigure time in managed stepping. */
        double stream_step_pipeline_setup_max_ms{0.0};              /**< Maximum pipeline create/reconfigure time in managed stepping. */
        double stream_step_pipeline_setup_last_ms{0.0};             /**< Most recent pipeline create/reconfigure time in managed stepping. */
        double stream_step_encode_total_ms{0.0};                    /**< Cumulative raw pipeline push/encode time in managed stepping. */
        double stream_step_encode_max_ms{0.0};                      /**< Maximum raw pipeline push/encode time in managed stepping. */
        double stream_step_encode_last_ms{0.0};                     /**< Most recent raw pipeline push/encode time in managed stepping. */
        double stream_step_publish_total_ms{0.0};                   /**< Cumulative transformed-frame publish time in managed stepping. */
        double stream_step_publish_max_ms{0.0};                     /**< Maximum transformed-frame publish time in managed stepping. */
        double stream_step_publish_last_ms{0.0};                    /**< Most recent transformed-frame publish time in managed stepping. */
        uint64_t total_frames_received{0};                         /**< Total raw frames received. */
        uint64_t total_frames_transformed{0};                      /**< Total frames emitted by the transform stage. */
        uint64_t total_frames_dropped{0};                          /**< Total frames dropped by throttling or gating. */
        uint64_t total_access_units_received{0};                   /**< Total encoded access units received. */
        double running_fps{0.0};                                   /**< Rolling output FPS, preferring encoded units when available. */
        double running_input_fps{0.0};                             /**< Rolling raw-frame ingest FPS. */
        double running_output_fps{0.0};                            /**< Rolling transformed-frame publish FPS. */
        double running_encoded_fps{0.0};                           /**< Rolling encoded access-unit ingest FPS. */
        StageTimingCounters transform_timing;                      /**< Wall-clock timing for display transform work. */
        StageTimingCounters encode_timing;                         /**< Wall-clock timing for raw-to-H.264 encode work. */
        std::optional<StreamSessionDebugSnapshot> current_session; /**< Current session snapshot when present. */
    };

    /**
     * @brief Debug snapshot for the whole server instance.
     */
    struct ServerDebugSnapshot
    {
        uint64_t stream_count{0};                             /**< Number of registered streams. */
        uint64_t active_session_count{0};                     /**< Number of active WebRTC sessions. */
        bool security_access_control_enabled{false};          /**< True when any access-control guard is enabled. */
        bool security_allowlist_enabled{false};               /**< True when IP allowlisting is enabled. */
        bool security_debug_api_enabled{false};               /**< True when the debug API is enabled. */
        bool security_runtime_config_api_enabled{false};      /**< True when the runtime config API is enabled. */
        bool security_remote_sensitive_routes_allowed{false}; /**< True when sensitive routes are exposed remotely. */
        uint64_t security_rejected_unauthorized{0};           /**< Count of requests rejected as unauthorized. */
        uint64_t security_rejected_forbidden{0};              /**< Count of requests rejected as forbidden. */
        uint64_t security_rejected_disabled{0};               /**< Count of requests rejected because the route is disabled. */
        uint64_t security_rejected_invalid{0};                /**< Count of requests rejected as invalid. */
        uint64_t security_rejected_rate_limited{0};           /**< Count of requests rejected by rate limits. */
        std::vector<StreamDebugSnapshot> streams;             /**< Per-stream debug snapshots. */
    };

} // namespace video_server
