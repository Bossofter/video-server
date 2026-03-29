#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"
#include "video_server/video_types.h"

namespace video_server
{

    /**
     * @brief Public stream status snapshot returned by the server APIs.
     */
    struct VideoStreamInfo
    {
        std::string stream_id;                           /**< Stable stream identifier. */
        std::string label;                               /**< Human-readable stream label. */
        StreamConfig config;                             /**< Registration-time stream configuration. */
        StreamOutputConfig output_config;                /**< Active runtime output configuration. */
        bool active{false};                              /**< True when the stream is currently registered and active. */
        uint64_t frames_received{0};                     /**< Total raw frames accepted for the stream. */
        uint64_t frames_transformed{0};                  /**< Total frames emitted by the transform stage. */
        uint64_t frames_dropped{0};                      /**< Total frames dropped by output throttling or gating. */
        uint64_t access_units_received{0};               /**< Total encoded access units accepted for the stream. */
        uint64_t last_frame_timestamp_ns{0};             /**< Timestamp of the latest raw frame. */
        uint64_t last_input_timestamp_ns{0};             /**< Timestamp of the latest admitted input. */
        uint64_t last_output_timestamp_ns{0};            /**< Timestamp of the latest transformed output. */
        uint64_t last_frame_id{0};                       /**< Producer frame id of the latest raw frame. */
        bool has_latest_frame{false};                    /**< True when a transformed latest-frame snapshot is available. */
        bool has_latest_encoded_unit{false};             /**< True when an encoded latest-unit snapshot is available. */
        VideoCodec last_encoded_codec{VideoCodec::H264}; /**< Codec of the latest encoded unit. */
        uint64_t last_encoded_timestamp_ns{0};           /**< Timestamp of the latest encoded unit. */
        uint64_t last_encoded_sequence_id{0};            /**< Sequence id of the latest encoded unit. */
        size_t last_encoded_size_bytes{0};               /**< Size in bytes of the latest encoded unit. */
        bool last_encoded_keyframe{false};               /**< True when the latest encoded unit is a keyframe. */
        bool last_encoded_codec_config{false};           /**< True when the latest encoded unit carries codec configuration. */
    };

} // namespace video_server
