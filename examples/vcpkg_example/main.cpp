#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "video_server/managed_video_server.h"

int main()
{
    video_server::ManagedVideoServerConfig config;
    config.execution_mode = video_server::ExecutionMode::ManualStep;
    config.webrtc.execution_mode = video_server::ExecutionMode::ManualStep;
    config.webrtc.enable_http_api = false;
    config.webrtc.http_port = 0;
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 16;
    constexpr std::uint32_t channels = 3;
    config.streams.push_back({"vcpkg-example", "Vcpkg Example", width, height, 30.0, video_server::VideoPixelFormat::RGB24});

    std::unique_ptr<video_server::IManagedVideoServer> server = video_server::CreateManagedVideoServer(config);
    if (!server)
    {
        std::cerr << "failed to create video-server instance\n";
        return 1;
    }

    if (!server->start())
    {
        std::cerr << "failed to start video-server instance\n";
        return 2;
    }

    std::vector<std::uint8_t> frame(static_cast<std::size_t>(width) * height * channels);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * channels;
            frame[offset] = static_cast<std::uint8_t>(x * 16);
            frame[offset + 1] = static_cast<std::uint8_t>(y * 16);
            frame[offset + 2] = static_cast<std::uint8_t>((x + y) * 8);
        }
    }

    video_server::VideoFrameView frame_view{};
    frame_view.data = frame.data();
    frame_view.width = width;
    frame_view.height = height;
    frame_view.stride_bytes = width * channels;
    frame_view.pixel_format = video_server::VideoPixelFormat::RGB24;
    frame_view.timestamp_ns = 1;
    frame_view.frame_id = 1;

    if (!server->push_frame("vcpkg-example", frame_view))
    {
        std::cerr << "failed to push example frame\n";
        server->stop();
        return 3;
    }

    if (!server->step())
    {
        std::cerr << "failed to step video-server instance\n";
        server->stop();
        return 4;
    }

    const auto stream_info = server->get_stream_info("vcpkg-example");
    if (!stream_info.has_value() || stream_info->frames_received != 1)
    {
        std::cerr << "example stream did not receive the frame\n";
        server->stop();
        return 5;
    }

    std::cout << "video-server vcpkg example ok: stream=" << stream_info->config.stream_id
              << " frames_received=" << stream_info->frames_received << '\n';

    server->stop();
    return 0;
}
