#include <cstdint>

#include "video_server/managed_video_server.h"

int main()
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.webrtc.http_port = 0;
    config.streams.push_back({"consumer-stream", "consumer", 2, 2, 30.0, video_server::VideoPixelFormat::RGB24});

    auto server = video_server::CreateManagedVideoServer(config);
    if (!server || !server->start())
    {
        return 1;
    }

    const uint8_t frame[] = {
        255, 0, 0,
        0, 255, 0,
        0, 0, 255,
        255, 255, 255,
    };

    video_server::VideoFrameView view{};
    view.data = frame;
    view.width = 2;
    view.height = 2;
    view.stride_bytes = 6;
    view.pixel_format = video_server::VideoPixelFormat::RGB24;
    view.timestamp_ns = 1;
    view.frame_id = 1;

    const bool accepted = server->push_frame("consumer-stream", view);
    server->stop();
    return accepted ? 0 : 2;
}
