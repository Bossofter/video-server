#include "video_server/video_types.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "video_pixel_format_utils.h"

namespace video_server
{

    const char *to_string(VideoDisplayMode mode)
    {
        switch (mode)
        {
        case VideoDisplayMode::Passthrough:
            return "Passthrough";
        case VideoDisplayMode::Grayscale:
            return "Grayscale";
        case VideoDisplayMode::WhiteHot:
            return "WhiteHot";
        case VideoDisplayMode::BlackHot:
            return "BlackHot";
        case VideoDisplayMode::Ironbow:
            return "Ironbow";
        case VideoDisplayMode::Rainbow:
            return "Rainbow";
        case VideoDisplayMode::Arctic:
            return "Arctic";
        }
        return "Passthrough";
    }

    const char *to_string(VideoPixelFormat pixel_format)
    {
        const auto *descriptor = find_video_pixel_format_descriptor(pixel_format);
        return descriptor != nullptr ? descriptor->name : "RGB24";
    }

    std::optional<VideoPixelFormat> video_pixel_format_from_string(const char *value)
    {
        if (value == nullptr)
        {
            return std::nullopt;
        }

        const auto *descriptor = find_video_pixel_format_descriptor(std::string_view(value));
        return descriptor != nullptr ? std::optional<VideoPixelFormat>(descriptor->pixel_format) : std::nullopt;
    }

    const char *to_string(VideoCodec codec)
    {
        switch (codec)
        {
        case VideoCodec::H264:
            return "H264";
        }
        return "H264";
    }

    std::optional<VideoDisplayMode> video_display_mode_from_string(const char *value)
    {
        if (value == nullptr)
        {
            return std::nullopt;
        }

        std::string normalized(value);
        normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                        [](unsigned char c)
                                        { return std::isspace(c) != 0; }),
                         normalized.end());

        if (normalized == "Passthrough" || normalized == "passthrough")
        {
            return VideoDisplayMode::Passthrough;
        }
        if (normalized == "Grayscale" || normalized == "grayscale")
        {
            return VideoDisplayMode::Grayscale;
        }
        if (normalized == "WhiteHot" || normalized == "white_hot" || normalized == "whitehot")
        {
            return VideoDisplayMode::WhiteHot;
        }
        if (normalized == "BlackHot" || normalized == "black_hot" || normalized == "blackhot")
        {
            return VideoDisplayMode::BlackHot;
        }
        if (normalized == "Ironbow" || normalized == "ironbow")
        {
            return VideoDisplayMode::Ironbow;
        }
        if (normalized == "Rainbow" || normalized == "rainbow")
        {
            return VideoDisplayMode::Rainbow;
        }
        if (normalized == "Arctic" || normalized == "arctic")
        {
            return VideoDisplayMode::Arctic;
        }

        return std::nullopt;
    }

} // namespace video_server
