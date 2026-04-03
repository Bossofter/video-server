#pragma once

#include <cstdint>
#include <string>

#include "video_server/video_types.h"

namespace video_server
{

    /**
     * @brief Registration-time properties for a producer-owned video stream.
     */
    struct StreamConfig
    {
        std::string stream_id;                                        /**< Stable stream identifier used by the API surface. */
        std::string label;                                            /**< Human-readable stream label. */
        uint32_t width{0};                                            /**< Expected input width in pixels. */
        uint32_t height{0};                                           /**< Expected input height in pixels. */
        double nominal_fps{0.0};                                      /**< Nominal source frame rate. */
        VideoPixelFormat input_pixel_format{VideoPixelFormat::RGB24}; /**< Expected raw input pixel format. */
        uint32_t max_subscribers{1};                                  /**< Maximum concurrent WebRTC recipients for the stream. */
    };

} // namespace video_server
