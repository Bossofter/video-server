#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "video_server/encoded_access_unit_view.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_server.h"

namespace video_server
{

    /**
     * @brief Scaling policy applied before H.264 encoding.
     */
    enum class RawPipelineScaleMode
    {
        Passthrough,
        Resize
    };

    /**
     * @brief Encoder selection hint for the raw-to-H.264 pipeline.
     */
    enum class RawH264Encoder
    {
        Automatic,
        LibX264,
        LibOpenH264
    };

    /**
     * @brief Configuration for a raw frame to H.264 pipeline instance.
     */
    struct RawVideoPipelineConfig
    {
        uint32_t input_width{0};                                            /**< Expected input width in pixels. */
        uint32_t input_height{0};                                           /**< Expected input height in pixels. */
        VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24};       /**< Expected input pixel format. */
        double input_fps{30.0};                                             /**< Nominal input frame rate. */
        RawPipelineScaleMode scale_mode{RawPipelineScaleMode::Passthrough}; /**< Scaling policy applied before encode. */
        std::optional<uint32_t> output_width;                               /**< Requested encoded output width when resizing. */
        std::optional<uint32_t> output_height;                              /**< Requested encoded output height when resizing. */
        std::optional<double> output_fps;                                   /**< Requested encoded output FPS when throttling. */
        RawH264Encoder encoder{RawH264Encoder::Automatic};                  /**< Encoder implementation hint. */
        std::string encoder_preset{"ultrafast"};                            /**< Encoder preset when supported by the backend. */
        std::string encoder_tune{"zerolatency"};                            /**< Encoder tune when supported by the backend. */
        std::string encoder_profile{"baseline"};                            /**< Encoder profile when supported by the backend. */
        bool repeat_headers{true};                                          /**< Requests repeated codec headers when supported. */
        bool emit_access_unit_delimiters{true};                             /**< Requests AUD NAL emission when supported. */
    };

    /**
     * @brief Raw frame pipeline interface that emits encoded H.264 access units.
     */
    class IRawVideoPipeline
    {
    public:
        virtual ~IRawVideoPipeline() = default;

        /**
         * @brief Returns the bound stream id for this pipeline instance.
         *
         * @return Reference to the stream identifier associated with the pipeline.
         */
        virtual const std::string &stream_id() const = 0;

        /**
         * @brief Opens pipeline resources and validates the active configuration.
         *
         * @param error_message Optional destination for a human-readable failure reason.
         * @return True when startup succeeded, false otherwise.
         */
        virtual bool start(std::string *error_message = nullptr) = 0;

        /**
         * @brief Admits one raw frame into the pipeline.
         *
         * @param frame Raw frame view to encode.
         * @param error_message Optional destination for a human-readable failure reason.
         * @return True when the frame was accepted, false otherwise.
         */
        virtual bool push_frame(const VideoFrameView &frame, std::string *error_message = nullptr) = 0;

        /**
         * @brief Flushes and releases pipeline resources.
         */
        virtual void stop() = 0;
    };

    /**
     * @brief Sink used to consume encoded access units emitted by the raw pipeline.
     *
     * Returning false from the sink is treated as a terminal pipeline failure.
     */
    using EncodedAccessUnitSink = std::function<bool(const EncodedAccessUnitView &access_unit)>;

    /**
     * @brief Builds a raw-to-H.264 pipeline that delivers access units to a caller-provided sink.
     *
     * @param stream_id Identifier of the stream bound to the pipeline.
     * @param config Raw pipeline configuration.
     * @param sink Callback invoked for each emitted encoded access unit.
     * @return Newly created pipeline instance.
     */
    std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline(std::string stream_id,
                                                                 RawVideoPipelineConfig config,
                                                                 EncodedAccessUnitSink sink);

    /**
     * @brief Builds a raw-to-H.264 pipeline bound directly to an IVideoServer stream.
     *
     * @param stream_id Identifier of the stream bound to the pipeline.
     * @param config Raw pipeline configuration.
     * @param server Video server that receives emitted encoded access units.
     * @return Newly created pipeline instance.
     */
    std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline_for_server(std::string stream_id,
                                                                            RawVideoPipelineConfig config,
                                                                            IVideoServer &server);

    /**
     * @brief Returns a readable name for a scale mode value.
     *
     * @param scale_mode Scale mode to convert.
     * @return Null-terminated string describing the scale mode.
     */
    const char *to_string(RawPipelineScaleMode scale_mode);

    /**
     * @brief Returns a readable name for an encoder selection value.
     *
     * @param encoder Encoder selection to convert.
     * @return Null-terminated string describing the encoder selection.
     */
    const char *to_string(RawH264Encoder encoder);

} // namespace video_server
