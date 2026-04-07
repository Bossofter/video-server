#include "video_pixel_format_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace video_server
{
    namespace
    {

        constexpr std::array<VideoPixelFormatDescriptor, 10> kVideoPixelFormatDescriptors{{
            {VideoPixelFormat::RGB24, "RGB24", VideoPixelFormatStorage::Packed, 3, 8, false, true, true},
            {VideoPixelFormat::BGR24, "BGR24", VideoPixelFormatStorage::Packed, 3, 8, false, true, true},
            {VideoPixelFormat::RGBA32, "RGBA32", VideoPixelFormatStorage::Packed, 4, 8, false, true, true},
            {VideoPixelFormat::BGRA32, "BGRA32", VideoPixelFormatStorage::Packed, 4, 8, false, true, true},
            {VideoPixelFormat::GRAY8, "GRAY8", VideoPixelFormatStorage::Packed, 1, 8, true, true, true},
            {VideoPixelFormat::GRAY10LE, "GRAY10LE", VideoPixelFormatStorage::Packed, 2, 10, true, true, true},
            {VideoPixelFormat::GRAY12LE, "GRAY12LE", VideoPixelFormatStorage::Packed, 2, 12, true, true, true},
            {VideoPixelFormat::GRAY16LE, "GRAY16LE", VideoPixelFormatStorage::Packed, 2, 16, true, true, true},
            {VideoPixelFormat::NV12, "NV12", VideoPixelFormatStorage::Planar, 0, 8, false, false, true},
            {VideoPixelFormat::I420, "I420", VideoPixelFormatStorage::Planar, 0, 8, false, false, true},
        }};

        std::string normalize_pixel_format_string(std::string_view value)
        {
            std::string normalized;
            normalized.reserve(value.size());
            for (const unsigned char c : value)
            {
                if (std::isspace(c) != 0 || c == '_' || c == '-')
                {
                    continue;
                }
                normalized.push_back(static_cast<char>(std::toupper(c)));
            }
            return normalized;
        }

        const VideoPixelFormatDescriptor *find_by_name(std::string_view value)
        {
            const std::string normalized = normalize_pixel_format_string(value);
            if (normalized.empty())
            {
                return nullptr;
            }

            for (const auto &descriptor : kVideoPixelFormatDescriptors)
            {
                if (normalized == descriptor.name)
                {
                    return &descriptor;
                }
            }

            if (normalized == "GRAY10")
            {
                return find_video_pixel_format_descriptor(VideoPixelFormat::GRAY10LE);
            }
            if (normalized == "GRAY12")
            {
                return find_video_pixel_format_descriptor(VideoPixelFormat::GRAY12LE);
            }
            if (normalized == "GRAY16")
            {
                return find_video_pixel_format_descriptor(VideoPixelFormat::GRAY16LE);
            }

            return nullptr;
        }

    } // namespace

    const VideoPixelFormatDescriptor *find_video_pixel_format_descriptor(VideoPixelFormat pixel_format)
    {
        const auto it = std::find_if(kVideoPixelFormatDescriptors.begin(), kVideoPixelFormatDescriptors.end(),
                                     [pixel_format](const VideoPixelFormatDescriptor &descriptor)
                                     { return descriptor.pixel_format == pixel_format; });
        return it != kVideoPixelFormatDescriptors.end() ? &(*it) : nullptr;
    }

    const VideoPixelFormatDescriptor *find_video_pixel_format_descriptor(std::string_view value)
    {
        return find_by_name(value);
    }

    bool video_pixel_format_is_planar(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        return descriptor != nullptr && descriptor->storage == VideoPixelFormatStorage::Planar;
    }

    bool video_pixel_format_is_grayscale(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        return descriptor != nullptr && descriptor->grayscale;
    }

    bool video_pixel_format_supports_display_transform(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        return descriptor != nullptr && descriptor->display_transform_supported;
    }

    bool video_pixel_format_supports_raw_pipeline(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        return descriptor != nullptr && descriptor->raw_pipeline_supported;
    }

    uint32_t video_pixel_format_min_row_bytes(VideoPixelFormat pixel_format, uint32_t width)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        if (descriptor == nullptr || descriptor->storage != VideoPixelFormatStorage::Packed)
        {
            return 0;
        }
        return static_cast<uint32_t>(width * descriptor->packed_bytes_per_pixel);
    }

    size_t video_frame_storage_size(const VideoFrameView &frame)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(frame.pixel_format);
        if (descriptor == nullptr)
        {
            return 0;
        }
        if (descriptor->storage == VideoPixelFormatStorage::Packed)
        {
            const uint32_t min_row_bytes = video_pixel_format_min_row_bytes(frame.pixel_format, frame.width);
            if (frame.stride_bytes < min_row_bytes)
            {
                return 0;
            }
            return static_cast<size_t>(frame.stride_bytes) * static_cast<size_t>(frame.height);
        }

        switch (frame.pixel_format)
        {
        case VideoPixelFormat::NV12:
        case VideoPixelFormat::I420:
            return static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u / 2u;
        default:
            return 0;
        }
    }

    uint16_t read_grayscale_sample(const uint8_t *row, uint32_t x, VideoPixelFormat pixel_format)
    {
        switch (pixel_format)
        {
        case VideoPixelFormat::GRAY8:
            return row[x];
        case VideoPixelFormat::GRAY10LE:
        case VideoPixelFormat::GRAY12LE:
        case VideoPixelFormat::GRAY16LE:
        {
            const size_t offset = static_cast<size_t>(x) * 2u;
            return static_cast<uint16_t>(row[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(row[offset + 1]) << 8u);
        }
        default:
            return 0;
        }
    }

    uint32_t grayscale_sample_max(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        if (descriptor == nullptr || !descriptor->grayscale)
        {
            return 0;
        }
        if (descriptor->intensity_bits >= 16)
        {
            return 65535u;
        }
        return (1u << descriptor->intensity_bits) - 1u;
    }

} // namespace video_server
