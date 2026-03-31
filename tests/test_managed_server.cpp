#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "video_server/managed_video_server.h"

namespace
{

    video_server::VideoFrameView make_gray_frame_view(const std::vector<uint8_t> &pixels,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint64_t timestamp_ns,
                                                      uint64_t frame_id)
    {
        return video_server::VideoFrameView{
            pixels.data(), width, height, width, video_server::VideoPixelFormat::GRAY8, timestamp_ns, frame_id};
    }

} // namespace

TEST(ManagedVideoServerTest, ManualStepPublishesOnlyLatestLatchedFrame)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    std::vector<uint8_t> frame_a(16u * 16u, 7u);
    std::vector<uint8_t> frame_b(16u * 16u, 11u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame_a, 16, 16, 100, 1)));
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame_b, 16, 16, 200, 2)));

    auto before_step = server->get_stream_info("alpha");
    ASSERT_TRUE(before_step.has_value());
    EXPECT_FALSE(before_step->has_latest_frame);
    EXPECT_EQ(before_step->frames_received, 2u);
    EXPECT_EQ(before_step->frames_transformed, 0u);
    EXPECT_EQ(before_step->frames_dropped, 1u);

    ASSERT_TRUE(server->step());

    auto after_step = server->get_stream_info("alpha");
    ASSERT_TRUE(after_step.has_value());
    EXPECT_TRUE(after_step->has_latest_frame);
    EXPECT_EQ(after_step->frames_received, 2u);
    EXPECT_EQ(after_step->frames_transformed, 1u);
    EXPECT_EQ(after_step->frames_dropped, 1u);
    EXPECT_EQ(after_step->last_output_timestamp_ns, 200u);

    server->stop();
}

TEST(ManagedVideoServerTest, ManualStepAppliesOutputFpsBeforeEncodeWork)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    video_server::StreamOutputConfig output_config;
    output_config.output_fps = 5.0;
    ASSERT_TRUE(server->set_stream_output_config("alpha", output_config));

    std::vector<uint8_t> frame_a(16u * 16u, 3u);
    std::vector<uint8_t> frame_b(16u * 16u, 9u);
    std::vector<uint8_t> frame_c(16u * 16u, 15u);

    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame_a, 16, 16, 0, 1)));
    ASSERT_TRUE(server->step());

    auto after_first = server->get_stream_info("alpha");
    ASSERT_TRUE(after_first.has_value());
    EXPECT_EQ(after_first->frames_transformed, 1u);
    EXPECT_EQ(after_first->last_output_timestamp_ns, 0u);

    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame_b, 16, 16, 100000000, 2)));
    ASSERT_TRUE(server->step());

    auto after_second = server->get_stream_info("alpha");
    ASSERT_TRUE(after_second.has_value());
    EXPECT_EQ(after_second->frames_transformed, 1u);
    EXPECT_EQ(after_second->last_output_timestamp_ns, 0u);

    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame_c, 16, 16, 250000000, 3)));
    ASSERT_TRUE(server->step());

    auto after_third = server->get_stream_info("alpha");
    ASSERT_TRUE(after_third.has_value());
    EXPECT_EQ(after_third->frames_transformed, 2u);
    EXPECT_EQ(after_third->frames_dropped, 1u);
    EXPECT_EQ(after_third->last_output_timestamp_ns, 250000000u);
    EXPECT_TRUE(after_third->has_latest_encoded_unit);

    server->stop();
}

TEST(ManagedVideoServerTest, MaxStreamsPerStepCountsOnlyProgressedStreams)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.max_streams_per_step = 1;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});
    config.streams.push_back({"bravo", "Bravo", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    std::vector<uint8_t> bravo_frame(16u * 16u, 21u);
    ASSERT_TRUE(server->push_frame("bravo", make_gray_frame_view(bravo_frame, 16, 16, 100, 1)));
    ASSERT_TRUE(server->step());

    const auto alpha = server->get_stream_info("alpha");
    const auto bravo = server->get_stream_info("bravo");
    ASSERT_TRUE(alpha.has_value());
    ASSERT_TRUE(bravo.has_value());
    EXPECT_EQ(alpha->frames_transformed, 0u);
    EXPECT_EQ(bravo->frames_transformed, 1u);

    server->stop();
}

TEST(ManagedVideoServerTest, MaxStreamsPerStepRoundRobinsAcrossReadyStreams)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.max_streams_per_step = 1;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});
    config.streams.push_back({"bravo", "Bravo", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    std::vector<uint8_t> alpha_frame(16u * 16u, 31u);
    std::vector<uint8_t> bravo_frame(16u * 16u, 41u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(alpha_frame, 16, 16, 100, 1)));
    ASSERT_TRUE(server->push_frame("bravo", make_gray_frame_view(bravo_frame, 16, 16, 100, 1)));

    ASSERT_TRUE(server->step());
    auto after_first_alpha = server->get_stream_info("alpha");
    auto after_first_bravo = server->get_stream_info("bravo");
    ASSERT_TRUE(after_first_alpha.has_value());
    ASSERT_TRUE(after_first_bravo.has_value());
    EXPECT_EQ(after_first_alpha->frames_transformed + after_first_bravo->frames_transformed, 1u);

    ASSERT_TRUE(server->step());
    auto after_second_alpha = server->get_stream_info("alpha");
    auto after_second_bravo = server->get_stream_info("bravo");
    ASSERT_TRUE(after_second_alpha.has_value());
    ASSERT_TRUE(after_second_bravo.has_value());
    EXPECT_EQ(after_second_alpha->frames_transformed, 1u);
    EXPECT_EQ(after_second_bravo->frames_transformed, 1u);

    server->stop();
}

TEST(ManagedVideoServerTest, RejectsConflictingInnerWebRtcExecutionSettings)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.execution_mode = video_server::ExecutionMode::WorkerThread;
    EXPECT_THROW(static_cast<void>(video_server::CreateManagedVideoServer(config)), std::invalid_argument);

    config.webrtc.execution_mode = video_server::ExecutionMode::ManualStep;
    config.http_poll_timeout_ms = 5;
    config.webrtc.http_poll_timeout_ms = 7;
    EXPECT_THROW(static_cast<void>(video_server::CreateManagedVideoServer(config)), std::invalid_argument);
}

TEST(ManagedVideoServerTest, LoadsTomlConfig)
{
    const std::string path = "/tmp/video_server_managed_config_test.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "http_poll_timeout_ms = 3\n";
        out << "max_streams_per_step = 4\n";
        out << "\n[webrtc]\n";
        out << "http_host = \"127.0.0.1\"\n";
        out << "http_port = 18080\n";
        out << "enable_http_api = false\n";
        out << "enable_debug_api = true\n";
        out << "enable_runtime_config_api = false\n";
        out << "allow_unsafe_public_routes = true\n";
        out << "enable_shared_key_auth = true\n";
        out << "shared_key = \"dev-secret\"\n";
        out << "ip_allowlist = [\"127.0.0.1/32\", \"192.168.0.0/24\"]\n";
        out << "cors_allowed_origins = [\"http://localhost:8090\", \"http://192.168.0.63:8090\"]\n";
        out << "max_http_request_bytes = 123456\n";
        out << "max_json_body_bytes = 23456\n";
        out << "max_signaling_sdp_bytes = 34567\n";
        out << "max_signaling_candidate_bytes = 4567\n";
        out << "max_pending_candidates_per_stream = 12\n";
        out << "signaling_rate_limit_window_seconds = 11\n";
        out << "signaling_rate_limit_max_requests = 22\n";
        out << "config_rate_limit_window_seconds = 33\n";
        out << "config_rate_limit_max_requests = 44\n";
        out << "debug_rate_limit_window_seconds = 55\n";
        out << "debug_rate_limit_max_requests = 66\n";
        out << "execution_mode = \"manual_step\"\n";
        out << "http_poll_timeout_ms = 3\n";
        out << "max_http_connections_per_step = 7\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"alpha\"\n";
        out << "label = \"Alpha\"\n";
        out << "width = 320\n";
        out << "height = 240\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY8\"\n";
        out << "\n[default_raw_pipelines.default]\n";
        out << "encoder = \"libx264\"\n";
        out << "encoder_preset = \"veryfast\"\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    EXPECT_EQ(config.execution_mode, video_server::ExecutionMode::ManualStep);
    EXPECT_EQ(config.http_poll_timeout_ms, 3u);
    EXPECT_EQ(config.max_streams_per_step, 4u);
    EXPECT_EQ(config.webrtc.execution_mode, video_server::ExecutionMode::ManualStep);
    EXPECT_EQ(config.webrtc.http_port, 18080);
    EXPECT_FALSE(config.webrtc.enable_http_api);
    EXPECT_TRUE(config.webrtc.enable_debug_api);
    EXPECT_FALSE(config.webrtc.enable_runtime_config_api);
    EXPECT_TRUE(config.webrtc.allow_unsafe_public_routes);
    EXPECT_TRUE(config.webrtc.enable_shared_key_auth);
    EXPECT_EQ(config.webrtc.shared_key, "dev-secret");
    ASSERT_EQ(config.webrtc.ip_allowlist.size(), 2u);
    ASSERT_EQ(config.webrtc.cors_allowed_origins.size(), 2u);
    EXPECT_EQ(config.webrtc.max_http_request_bytes, 123456u);
    EXPECT_EQ(config.webrtc.max_json_body_bytes, 23456u);
    EXPECT_EQ(config.webrtc.max_signaling_sdp_bytes, 34567u);
    EXPECT_EQ(config.webrtc.max_signaling_candidate_bytes, 4567u);
    EXPECT_EQ(config.webrtc.max_pending_candidates_per_stream, 12u);
    EXPECT_EQ(config.webrtc.signaling_rate_limit_window_seconds, 11u);
    EXPECT_EQ(config.webrtc.signaling_rate_limit_max_requests, 22u);
    EXPECT_EQ(config.webrtc.config_rate_limit_window_seconds, 33u);
    EXPECT_EQ(config.webrtc.config_rate_limit_max_requests, 44u);
    EXPECT_EQ(config.webrtc.debug_rate_limit_window_seconds, 55u);
    EXPECT_EQ(config.webrtc.debug_rate_limit_max_requests, 66u);
    EXPECT_EQ(config.webrtc.http_poll_timeout_ms, 3u);
    EXPECT_EQ(config.webrtc.max_http_connections_per_step, 7u);
    ASSERT_EQ(config.streams.size(), 1u);
    EXPECT_EQ(config.streams.front().stream_id, "alpha");
    EXPECT_EQ(config.streams.front().input_pixel_format, video_server::VideoPixelFormat::GRAY8);
    ASSERT_TRUE(config.default_raw_pipelines.find("default") != config.default_raw_pipelines.end());
    EXPECT_EQ(config.default_raw_pipelines.at("default").encoder, video_server::RawH264Encoder::LibX264);
    EXPECT_EQ(config.default_raw_pipelines.at("default").encoder_preset, "veryfast");

    std::remove(path.c_str());
}
