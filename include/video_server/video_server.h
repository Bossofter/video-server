#pragma once

#include <optional>
#include <string>
#include <vector>

#include "video_server/encoded_access_unit_view.h"
#include "video_server/stream_config.h"
#include "video_server/stream_output_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_server_result.h"
#include "video_server/video_stream_info.h"

namespace video_server
{

    /**
     * @brief Execution policy used by stepped and managed server implementations.
     */
    enum class ExecutionMode
    {
        ManualStep,  /**< Caller drives all progression explicitly via `step()`. */
        InlineOnPush,/**< Raw-frame ingestion also progresses the stream inline. */
        WorkerThread /**< Internal worker thread progresses owned subsystems. */
    };

    /**
     * @brief Common video server interface used by the core and WebRTC-backed implementations.
     */
    class IVideoServer
    {
    public:
        virtual ~IVideoServer() = default;

        /**
         * @brief Registers a new stream definition.
         *
         * @param config Stream registration parameters.
         * @return Success or a typed configuration/duplicate-stream error.
         */
        virtual VideoServerResult register_stream(const StreamConfig &config) = 0;

        /**
         * @brief Removes an existing stream and any associated backend state.
         *
         * @param stream_id Identifier of the stream to remove.
         * @return Success or `NotFound` when the stream does not exist.
         */
        virtual VideoServerResult remove_stream(const std::string &stream_id) = 0;

        /**
         * @brief Pushes one raw frame into the named stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param frame Raw frame view to ingest.
         * @return Success or a typed stream/frame/transform error.
         */
        virtual VideoServerResult push_frame(const std::string &stream_id, const VideoFrameView &frame) = 0;

        /**
         * @brief Pushes one encoded access unit into the named stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param access_unit Encoded access unit to ingest.
         * @return Success or a typed stream/payload/codec error.
         */
        virtual VideoServerResult push_access_unit(const std::string &stream_id, const EncodedAccessUnitView &access_unit) = 0;

        /**
         * @brief Returns a snapshot of all registered streams.
         *
         * @return Current stream information for every registered stream.
         */
        virtual std::vector<VideoStreamInfo> list_streams() const = 0;

        /**
         * @brief Returns stream information for one stream when present.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Stream information when the stream exists, otherwise `std::nullopt`.
         */
        virtual std::optional<VideoStreamInfo> get_stream_info(const std::string &stream_id) const = 0;

        /**
         * @brief Updates the runtime output config for a stream.
         *
         * @param stream_id Identifier of the stream to update.
         * @param output_config New runtime output configuration.
         * @return Success or a typed stream/configuration error.
         */
        virtual VideoServerResult set_stream_output_config(const std::string &stream_id,
                                                           const StreamOutputConfig &output_config) = 0;

        /**
         * @brief Returns the runtime output config for a stream when present.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Runtime output configuration when the stream exists, otherwise `std::nullopt`.
         */
        virtual std::optional<StreamOutputConfig> get_stream_output_config(
            const std::string &stream_id) const = 0;
    };

} // namespace video_server
