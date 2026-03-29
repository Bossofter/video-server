#include "frame_http_encoder.h"

#include <cstddef>
#include <sstream>

namespace video_server
{

    std::optional<EncodedFramePayload> encode_latest_frame_as_ppm(const LatestFrame &frame)
    {
        if (!frame.valid || frame.pixel_format != VideoPixelFormat::RGB24 || frame.width == 0 || frame.height == 0)
        {
            return std::nullopt;
        }

        const size_t expected_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u;
        if (frame.bytes.size() != expected_size)
        {
            return std::nullopt;
        }

        std::ostringstream header;
        header << "P6\n"
               << frame.width << ' ' << frame.height << "\n255\n";
        const std::string header_text = header.str();

        EncodedFramePayload payload;
        payload.content_type = "image/x-portable-pixmap";
        payload.body.reserve(header_text.size() + frame.bytes.size());
        payload.body.append(header_text);
        payload.body.append(reinterpret_cast<const char *>(frame.bytes.data()), frame.bytes.size());
        return payload;
    }

} // namespace video_server
