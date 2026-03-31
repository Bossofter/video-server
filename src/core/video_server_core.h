#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/observability.h"
#include "video_server/video_server.h"

namespace video_server
{

    /**
     * @brief Immutable snapshot of the latest encoded unit for a stream.
     */
    struct LatestEncodedUnit
    {
        std::vector<uint8_t> bytes;
        VideoCodec codec{VideoCodec::H264};
        uint64_t timestamp_ns{0};
        uint64_t sequence_id{0};
        bool keyframe{false};
        bool codec_config{false};
        bool valid{false};
    };

    /**
     * @brief Immutable snapshot of the latest transformed frame for a stream.
     */
    struct LatestFrame
    {
        std::vector<uint8_t> bytes;
        uint32_t width{0};
        uint32_t height{0};
        VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
        uint64_t timestamp_ns{0};
        uint64_t frame_id{0};
        bool valid{false};
    };

    /**
     * @brief Core in-memory implementation of stream state, transforms, and latest snapshots.
     */
    class VideoServerCore : public IVideoServer
    {
    public:
        /**
         * @brief Registers a new stream in the core state store.
         *
         * @param config Registration-time stream configuration.
         * @return True when the stream was registered, false otherwise.
         */
        bool register_stream(const StreamConfig &config) override;

        /**
         * @brief Removes a stream and its latest-frame/latest-unit snapshots.
         *
         * @param stream_id Identifier of the stream to remove.
         * @return True when the stream was removed, false otherwise.
         */
        bool remove_stream(const std::string &stream_id) override;

        /**
         * @brief Ingests a raw frame for a registered stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param frame Raw frame view to validate, transform, and publish.
         * @return True when the frame was accepted, false otherwise.
         */
        bool push_frame(const std::string &stream_id, const VideoFrameView &frame) override;

        /**
         * @brief Ingests an encoded access unit for a registered stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param access_unit Encoded access unit to publish.
         * @return True when the access unit was accepted, false otherwise.
         */
        bool push_access_unit(const std::string &stream_id, const EncodedAccessUnitView &access_unit) override;

        /**
         * @brief Validates a raw input frame against the registered stream contract.
         *
         * @param stream_id Identifier of the target stream.
         * @param frame Raw frame view to validate.
         * @param config_out Optional destination for the registered stream config.
         * @param output_config_out Optional destination for the active output config.
         * @return True when the frame matches the stream contract, false otherwise.
         */
        bool validate_raw_frame_input(const std::string &stream_id,
                                      const VideoFrameView &frame,
                                      StreamConfig *config_out = nullptr,
                                      StreamOutputConfig *output_config_out = nullptr) const;

        /**
         * @brief Records that one valid raw input frame was received for the stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param timestamp_ns Frame timestamp in nanoseconds.
         * @return True when the stream exists, false otherwise.
         */
        bool note_frame_received(const std::string &stream_id, uint64_t timestamp_ns);

        /**
         * @brief Records that one raw frame was dropped for the stream.
         *
         * @param stream_id Identifier of the target stream.
         * @return True when the stream exists, false otherwise.
         */
        bool note_frame_dropped(const std::string &stream_id);

        /**
         * @brief Publishes a transformed RGB latest-frame snapshot for one stream.
         *
         * @param stream_id Identifier of the target stream.
         * @param rgb_bytes RGB24 payload.
         * @param width Transformed frame width.
         * @param height Transformed frame height.
         * @param timestamp_ns Source timestamp in nanoseconds.
         * @param frame_id Source frame identifier.
         * @return True when the frame was published, false otherwise.
         */
        bool publish_transformed_frame(const std::string &stream_id,
                                       std::vector<uint8_t> rgb_bytes,
                                       uint32_t width,
                                       uint32_t height,
                                       uint64_t timestamp_ns,
                                       uint64_t frame_id);

        /**
         * @brief Returns a snapshot of every registered stream.
         *
         * @return Stream info records for all registered streams.
         */
        std::vector<VideoStreamInfo> list_streams() const override;

        /**
         * @brief Returns stream information for one stream when present.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Stream info when the stream exists, otherwise `std::nullopt`.
         */
        std::optional<VideoStreamInfo> get_stream_info(const std::string &stream_id) const override;

        /**
         * @brief Updates the runtime output config for one stream.
         *
         * @param stream_id Identifier of the stream to update.
         * @param output_config Runtime output configuration to apply.
         * @return True when the configuration was accepted, false otherwise.
         */
        bool set_stream_output_config(const std::string &stream_id,
                                      const StreamOutputConfig &output_config) override;

        /**
         * @brief Returns the runtime output config for one stream when present.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Output configuration when the stream exists, otherwise `std::nullopt`.
         */
        std::optional<StreamOutputConfig> get_stream_output_config(const std::string &stream_id) const override;

        /**
         * @brief Returns the latest transformed frame snapshot for a stream.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Latest transformed frame snapshot, or `nullptr` when unavailable.
         */
        std::shared_ptr<const LatestFrame> get_latest_frame_for_stream(const std::string &stream_id) const;

        /**
         * @brief Returns the latest encoded access-unit snapshot for a stream.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Latest encoded-unit snapshot, or `nullptr` when unavailable.
         */
        std::shared_ptr<const LatestEncodedUnit> get_latest_encoded_unit_for_stream(const std::string &stream_id) const;

        /**
         * @brief Returns a debug snapshot for one stream when present.
         *
         * @param stream_id Identifier of the stream to query.
         * @return Debug snapshot when the stream exists, otherwise `std::nullopt`.
         */
        std::optional<StreamDebugSnapshot> get_stream_debug_snapshot(const std::string &stream_id) const;

        /**
         * @brief Returns debug snapshots for all streams.
         *
         * @return Debug snapshots for all registered streams.
         */
        std::vector<StreamDebugSnapshot> list_stream_debug_snapshots() const;

    public:
        /**
         * @brief Mutable state tracked for one registered stream.
         */
        struct StreamState
        {
            VideoStreamInfo info;
            std::shared_ptr<const LatestFrame> latest_frame;
            std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit;
            uint64_t next_allowed_output_timestamp_ns{0};
        };

    private:
        /**
         * @brief Validates supported rotation values.
         *
         * @param degrees Rotation in degrees to validate.
         * @return True when the rotation is supported, false otherwise.
         */
        static bool is_valid_rotation(int degrees);

        /**
         * @brief Validates requested output dimensions.
         *
         * @param width Requested output width.
         * @param height Requested output height.
         * @return True when the output dimensions are valid, false otherwise.
         */
        static bool is_valid_output_dimensions(uint32_t width, uint32_t height);

        /**
         * @brief Validates requested output FPS.
         *
         * @param fps Requested output FPS.
         * @return True when the FPS value is valid, false otherwise.
         */
        static bool is_valid_output_fps(double fps);

        /**
         * @brief Checks whether the input pixel format is supported by the core.
         *
         * @param pixel_format Input pixel format to validate.
         * @return True when the pixel format is supported, false otherwise.
         */
        static bool is_supported_input_pixel_format(VideoPixelFormat pixel_format);

        /**
         * @brief Returns bytes per pixel for packed formats handled directly by the core.
         *
         * @param pixel_format Packed pixel format to inspect.
         * @return Bytes per pixel for the supplied format.
         */
        static uint32_t bytes_per_pixel(VideoPixelFormat pixel_format);

        mutable std::mutex mutex_;
        std::unordered_map<std::string, StreamState> streams_;
    };

} // namespace video_server
