#pragma once

#include <cstdint>
#include <optional>

namespace video_server
{

    /**
     * @brief Supported raw input pixel formats.
     */
    enum class VideoPixelFormat
    {
        RGB24,
        BGR24,
        RGBA32,
        BGRA32,
        NV12,
        I420,
        GRAY8
    };

    /**
     * @brief Supported encoded codecs.
     */
    enum class VideoCodec
    {
        H264
    };

    /**
     * @brief Supported output display and palette modes.
     */
    enum class VideoDisplayMode
    {
        Passthrough,
        Grayscale,
        WhiteHot,
        BlackHot,
        Ironbow,
        Rainbow,
        Arctic
    };

    /**
     * @brief Returns a readable name for a display mode value.
     *
     * @param mode Display mode to convert.
     * @return Null-terminated string describing the display mode.
     */
    const char *to_string(VideoDisplayMode mode);

    /**
     * @brief Returns a readable name for a pixel format value.
     *
     * @param pixel_format Pixel format to convert.
     * @return Null-terminated string describing the pixel format.
     */
    const char *to_string(VideoPixelFormat pixel_format);

    /**
     * @brief Returns a readable name for a codec value.
     *
     * @param codec Codec to convert.
     * @return Null-terminated string describing the codec.
     */
    const char *to_string(VideoCodec codec);

    /**
     * @brief Parses a display mode from a case-insensitive API string.
     *
     * @param value String value to parse.
     * @return Parsed display mode when recognized, otherwise `std::nullopt`.
     */
    std::optional<VideoDisplayMode> video_display_mode_from_string(const char *value);

} // namespace video_server
