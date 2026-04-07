#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "video_server/video_frame_view.h"

namespace video_server
{

    enum class VideoPixelFormatStorage
    {
        Packed,
        Planar
    };

    struct VideoPixelFormatDescriptor
    {
        VideoPixelFormat pixel_format;
        const char *name;
        VideoPixelFormatStorage storage;
        uint8_t packed_bytes_per_pixel;
        uint8_t intensity_bits;
        bool grayscale;
        bool display_transform_supported;
        bool raw_pipeline_supported;
    };

    const VideoPixelFormatDescriptor *find_video_pixel_format_descriptor(VideoPixelFormat pixel_format);
    const VideoPixelFormatDescriptor *find_video_pixel_format_descriptor(std::string_view value);

    bool video_pixel_format_is_planar(VideoPixelFormat pixel_format);
    bool video_pixel_format_is_grayscale(VideoPixelFormat pixel_format);
    bool video_pixel_format_supports_display_transform(VideoPixelFormat pixel_format);
    bool video_pixel_format_supports_raw_pipeline(VideoPixelFormat pixel_format);
    uint32_t video_pixel_format_min_row_bytes(VideoPixelFormat pixel_format, uint32_t width);
    size_t video_frame_storage_size(const VideoFrameView &frame);
    uint16_t read_grayscale_sample(const uint8_t *row, uint32_t x, VideoPixelFormat pixel_format);
    uint32_t grayscale_sample_max(VideoPixelFormat pixel_format);

} // namespace video_server
