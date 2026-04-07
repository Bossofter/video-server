#pragma once

#include <optional>
#include <string>

extern "C"
{
#include <libavutil/pixfmt.h>
}

#include "../core/video_pixel_format_utils.h"

namespace video_server
{

    inline std::optional<AVPixelFormat> libav_pixel_format_from_video_pixel_format(VideoPixelFormat pixel_format)
    {
        switch (pixel_format)
        {
        case VideoPixelFormat::RGB24:
            return AV_PIX_FMT_RGB24;
        case VideoPixelFormat::BGR24:
            return AV_PIX_FMT_BGR24;
        case VideoPixelFormat::RGBA32:
            return AV_PIX_FMT_RGBA;
        case VideoPixelFormat::BGRA32:
            return AV_PIX_FMT_BGRA;
        case VideoPixelFormat::GRAY8:
            return AV_PIX_FMT_GRAY8;
        case VideoPixelFormat::GRAY10LE:
#ifdef AV_PIX_FMT_GRAY10LE
            return AV_PIX_FMT_GRAY10LE;
#else
            return std::nullopt;
#endif
        case VideoPixelFormat::GRAY12LE:
#ifdef AV_PIX_FMT_GRAY12LE
            return AV_PIX_FMT_GRAY12LE;
#else
            return std::nullopt;
#endif
        case VideoPixelFormat::GRAY16LE:
#ifdef AV_PIX_FMT_GRAY16LE
            return AV_PIX_FMT_GRAY16LE;
#else
            return std::nullopt;
#endif
        case VideoPixelFormat::NV12:
            return AV_PIX_FMT_NV12;
        case VideoPixelFormat::I420:
            return AV_PIX_FMT_YUV420P;
        }
        return std::nullopt;
    }

    inline bool validate_libav_pixel_format(VideoPixelFormat pixel_format, std::string *error_message = nullptr)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        if (descriptor == nullptr)
        {
            if (error_message != nullptr)
            {
                *error_message = "unknown raw input pixel format";
            }
            return false;
        }
        if (!descriptor->raw_pipeline_supported)
        {
            if (error_message != nullptr)
            {
                *error_message = "raw-to-H264 pipeline does not support input pixel format '" + std::string(descriptor->name) + "'";
            }
            return false;
        }
        if (!libav_pixel_format_from_video_pixel_format(pixel_format).has_value())
        {
            if (error_message != nullptr)
            {
                *error_message = "raw-to-H264 pipeline does not support input pixel format '" + std::string(descriptor->name) +
                                 "' in this libav build";
            }
            return false;
        }
        return true;
    }

} // namespace video_server
