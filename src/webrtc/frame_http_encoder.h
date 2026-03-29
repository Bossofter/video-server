#pragma once

#include <optional>
#include <string>

#include "../core/video_server_core.h"

namespace video_server
{

    /**
     * @brief Encoded HTTP payload returned by the latest-frame endpoint.
     */
    struct EncodedFramePayload
    {
        std::string body;         /**< Serialized response body. */
        std::string content_type; /**< MIME content type of the encoded payload. */
    };

    /**
     * @brief Encodes a transformed latest frame snapshot as a PPM payload.
     *
     * @param frame Latest transformed frame snapshot to encode.
     * @return Encoded payload when conversion succeeded, otherwise `std::nullopt`.
     */
    std::optional<EncodedFramePayload> encode_latest_frame_as_ppm(const LatestFrame &frame);

} // namespace video_server
