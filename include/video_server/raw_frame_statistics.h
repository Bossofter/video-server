#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace video_server
{

    /**
     * @brief Statistics calculated from the latest accepted raw input frame.
     *
     * Luminance values are normalized to the inclusive 0..255 range so callers
     * can compare streams with different source pixel formats and bit depths.
     * The histogram contains one bin per normalized luminance value.
     */
    struct RawFrameStatistics
    {
        bool available{false};                         /**< True after at least one raw frame has been analyzed. */
        uint64_t frame_id{0};                          /**< Producer id of the analyzed raw frame. */
        uint64_t timestamp_ns{0};                      /**< Timestamp of the analyzed raw frame. */
        uint32_t width{0};                             /**< Raw input width. */
        uint32_t height{0};                            /**< Raw input height. */
        std::string pixel_format;                      /**< Raw input pixel format. */
        uint64_t pixel_count{0};                       /**< Number of pixels represented by the statistics. */
        double luminance_min{0.0};                     /**< Minimum normalized luminance. */
        double luminance_max{0.0};                     /**< Maximum normalized luminance. */
        double luminance_mean{0.0};                    /**< Mean normalized luminance. */
        double luminance_stddev{0.0};                  /**< Population standard deviation of luminance. */
        double luminance_p01{0.0};                     /**< Approximate first-percentile luminance. */
        double luminance_p50{0.0};                     /**< Approximate median luminance. */
        double luminance_p99{0.0};                     /**< Approximate 99th-percentile luminance. */
        double dynamic_range{0.0};                     /**< Difference between p99 and p01. */
        double dark_pixel_fraction{0.0};               /**< Fraction of pixels at or below 5% luminance. */
        double bright_pixel_fraction{0.0};             /**< Fraction of pixels at or above 95% luminance. */
        std::array<uint64_t, 256> luminance_histogram{}; /**< Counts for normalized luminance values 0..255. */
    };

} // namespace video_server
