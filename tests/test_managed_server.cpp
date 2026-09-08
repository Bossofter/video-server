#include <gtest/gtest.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "video_server/managed_video_server.h"
#include "../src/webrtc/encoded_access_unit_handoff.h"

namespace video_server::webrtc_internal
{
    void set_managed_stream_step_test_hook(std::function<void(const std::string &)> hook);
    void set_managed_api_step_test_hook(std::function<void()> hook);
    void set_managed_shutdown_wait_test_hook(std::function<void()> hook);
}

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

    bool wait_until(const std::function<bool()> &predicate, int timeout_ms = 1000)
    {
        for (int waited = 0; waited < timeout_ms; waited += 10)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    }

    video_server::ManagedVideoServerConfig make_software_managed_config(const std::string &stream_id,
                                                                        uint32_t width,
                                                                        uint32_t height,
                                                                        double fps)
    {
        video_server::ManagedVideoServerConfig config;
        config.webrtc.enable_http_api = false;
        config.streams.push_back({stream_id, stream_id, width, height, fps, video_server::VideoPixelFormat::GRAY8});
        video_server::RawVideoPipelineConfig pipeline_config;
        pipeline_config.encoder = video_server::RawH264Encoder::LibOpenH264;
        config.default_raw_pipelines.emplace("default", pipeline_config);
        return config;
    }

    uint64_t timestamp_for_frame(uint64_t index, double fps)
    {
        return static_cast<uint64_t>((static_cast<long double>(index) * 1000000000.0L) / fps);
    }

    class BlockingGate
    {
    public:
        void arrive_and_wait()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++entries_;
            cv_.notify_all();
            cv_.wait(lock,
                     [this]()
                     { return released_; });
        }

        bool wait_for_entries(size_t expected, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
        {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, timeout,
                                [this, expected]()
                                { return entries_ >= expected; });
        }

        void release()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
            cv_.notify_all();
        }

        size_t entries() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return entries_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        size_t entries_{0};
        bool released_{false};
    };

    class ManagedHookScope
    {
    public:
        ~ManagedHookScope()
        {
            video_server::webrtc_internal::set_managed_stream_step_test_hook(nullptr);
            video_server::webrtc_internal::set_managed_api_step_test_hook(nullptr);
            video_server::webrtc_internal::set_managed_shutdown_wait_test_hook(nullptr);
        }
    };

    video_server::ManagedVideoServerConfig make_two_stream_software_config()
    {
        video_server::ManagedVideoServerConfig config;
        config.webrtc.enable_http_api = false;
        config.streams.push_back({"alpha", "Alpha", 64, 48, 30.0, video_server::VideoPixelFormat::GRAY8});
        config.streams.push_back({"bravo", "Bravo", 64, 48, 30.0, video_server::VideoPixelFormat::GRAY8});
        video_server::RawVideoPipelineConfig pipeline_config;
        pipeline_config.encoder = video_server::RawH264Encoder::LibOpenH264;
        config.default_raw_pipelines.emplace("default", pipeline_config);
        return config;
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
    EXPECT_TRUE(before_step->raw_frame_statistics.available);
    EXPECT_EQ(before_step->raw_frame_statistics.frame_id, 2u);
    EXPECT_EQ(before_step->raw_frame_statistics.timestamp_ns, 200u);
    EXPECT_DOUBLE_EQ(before_step->raw_frame_statistics.luminance_mean, 11.0);
    EXPECT_EQ(before_step->raw_frame_statistics.luminance_histogram[11], 256u);

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

TEST(ManagedVideoServerTest, DebugSnapshotReportsRequestedAndActiveEncoder)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});
    video_server::RawVideoPipelineConfig pipeline_config;
    pipeline_config.encoder = video_server::RawH264Encoder::Automatic;
    pipeline_config.hardware_device = "/dev/dri/video-server-test-missing-render-node";
    config.default_raw_pipelines.emplace("default", pipeline_config);

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    std::vector<uint8_t> frame(32u * 32u, 19u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(server->step());

    const auto snapshot = server->get_debug_snapshot();
    ASSERT_EQ(snapshot.streams.size(), 1u);
    EXPECT_EQ(snapshot.streams.front().requested_encoder, "Automatic");
    EXPECT_FALSE(snapshot.streams.front().active_encoder.empty());
    EXPECT_FALSE(snapshot.streams.front().hardware_accelerated);
    EXPECT_TRUE(snapshot.streams.front().hardware_device.empty());

    server->stop();
}

TEST(ManagedVideoServerTest, RegistersExclusiveStreamGroupsAndReleasesOnDestroy)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});
    config.streams.push_back({"bravo", "Bravo", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    EXPECT_EQ(server->register_streams({}), nullptr);
    EXPECT_EQ(server->register_streams({"missing"}), nullptr);
    EXPECT_EQ(server->register_streams({"alpha", "alpha"}), nullptr);

    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->stream_ids(), std::vector<std::string>{"alpha"});
    EXPECT_EQ(server->register_streams({"alpha"}), nullptr);
    EXPECT_EQ(server->register_streams({"bravo", "alpha"}), nullptr);

    group.reset();
    auto replacement = server->register_streams({"alpha", "bravo"});
    EXPECT_NE(replacement, nullptr);

    server->stop();
}

TEST(ManagedVideoServerTest, RejectsRemoveWhileStreamGroupOwnsStreamAndAllowsAfterRelease)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);
    EXPECT_FALSE(server->remove_stream("alpha"));
    group.reset();
    EXPECT_TRUE(server->remove_stream("alpha"));

    server->stop();
}

TEST(ManagedVideoServerTest, StreamGroupCanOutliveServerWithoutDanglingRelease)
{
    std::shared_ptr<video_server::IManagedStreamGroup> group;
    {
        video_server::ManagedVideoServerConfig config;
        config.webrtc.enable_http_api = false;
        config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

        auto server = video_server::CreateManagedVideoServer(config);
        ASSERT_TRUE(server->start());
        group = server->register_streams({"alpha"});
        ASSERT_NE(group, nullptr);
    }

    EXPECT_FALSE(group->step_streams());
    group.reset();
    SUCCEED();
}

TEST(ManagedVideoServerTest, EncodedHandoffOverflowRetainsDecoderStartState)
{
    struct TestUnit
    {
        int id{0};
        bool keyframe{false};
        bool codec_config{false};
    };

    video_server::webrtc_internal::EncodedAccessUnitHandoff<TestUnit> handoff(4);
    handoff.push(TestUnit{1, false, true});
    handoff.push(TestUnit{2, true, false});
    for (int id = 3; id <= 8; ++id)
    {
        handoff.push(TestUnit{id, false, false});
    }

    EXPECT_GT(handoff.dropped_count(), 0u);
    EXPECT_LE(handoff.pending_count(), 4u);

    const auto drained = handoff.drain();
    ASSERT_GE(drained.size(), 2u);
    EXPECT_TRUE(drained[0].codec_config);
    EXPECT_TRUE(drained[1].keyframe);
    EXPECT_EQ(drained[1].id, 2);
    EXPECT_EQ(drained.back().id, 8);
}

TEST(ManagedVideoServerTest, HighRateStreamProgressionIsNotImplicitlyCappedAt30Fps)
{
    // Protects against a previously observed managed-loop regression that capped stream progression near 30 FPS.
    struct Case
    {
        double source_fps;
        uint64_t submitted_frames;
        double output_fps;
        uint64_t expected_transformed;
    };

    const std::vector<Case> cases{
        {30.0, 30, 0.0, 30},
        {60.0, 60, 0.0, 60},
        {75.0, 75, 0.0, 75},
        {120.0, 120, 0.0, 120},
        {120.0, 120, 30.0, 30},
    };

    for (const auto &test_case : cases)
    {
        SCOPED_TRACE("source_fps=" + std::to_string(test_case.source_fps) +
                     " submitted=" + std::to_string(test_case.submitted_frames) +
                     " output_fps=" + std::to_string(test_case.output_fps));
        auto config = make_software_managed_config("alpha", 64, 48, test_case.source_fps);
        auto server = video_server::CreateManagedVideoServer(config);
        ASSERT_TRUE(server->start());
        auto group = server->register_streams({"alpha"});
        ASSERT_NE(group, nullptr);

        if (test_case.output_fps > 0.0)
        {
            video_server::StreamOutputConfig output_config;
            output_config.output_fps = test_case.output_fps;
            ASSERT_TRUE(server->set_stream_output_config("alpha", output_config));
        }

        for (uint64_t i = 0; i < test_case.submitted_frames; ++i)
        {
            std::vector<uint8_t> frame(64u * 48u, static_cast<uint8_t>((i * 17u) & 0xffu));
            ASSERT_TRUE(server->push_frame("alpha",
                                           make_gray_frame_view(frame, 64, 48,
                                                                timestamp_for_frame(i, test_case.source_fps), i + 1)));
            ASSERT_TRUE(group->step_streams());
        }

        const auto info = server->get_stream_info("alpha");
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->frames_received, test_case.submitted_frames)
            << "source_fps=" << test_case.source_fps << " output_fps=" << test_case.output_fps;
        EXPECT_EQ(info->frames_transformed, test_case.expected_transformed)
            << "source_fps=" << test_case.source_fps << " submitted=" << test_case.submitted_frames
            << " dropped=" << info->frames_dropped << " output_fps=" << test_case.output_fps;
        if (test_case.output_fps == 0.0)
        {
            EXPECT_EQ(info->frames_dropped, 0u);
        }
        else
        {
            EXPECT_GE(info->frames_dropped + 1u, test_case.submitted_frames - test_case.expected_transformed)
                << "one throttled frame may remain latched as pending after the final synchronous step";
        }

        server->stop();
    }
}

TEST(ManagedVideoServerTest, StepStreamsAdvancesOnlyOwnedStreamsAndApiDrainsEncodedHandoff)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});
    config.streams.push_back({"bravo", "Bravo", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());
    auto alpha_group = server->register_streams({"alpha"});
    ASSERT_NE(alpha_group, nullptr);

    std::vector<uint8_t> alpha_frame(32u * 32u, 31u);
    std::vector<uint8_t> bravo_frame(32u * 32u, 41u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(alpha_frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(server->push_frame("bravo", make_gray_frame_view(bravo_frame, 32, 32, 1000, 1)));

    ASSERT_TRUE(alpha_group->step_streams());

    auto alpha = server->get_stream_info("alpha");
    auto bravo = server->get_stream_info("bravo");
    ASSERT_TRUE(alpha.has_value());
    ASSERT_TRUE(bravo.has_value());
    EXPECT_EQ(alpha->frames_transformed, 1u);
    EXPECT_EQ(bravo->frames_transformed, 0u);
    EXPECT_EQ(alpha->access_units_received, 0u);

    ASSERT_TRUE(server->step_api());
    alpha = server->get_stream_info("alpha");
    ASSERT_TRUE(alpha.has_value());
    EXPECT_GT(alpha->access_units_received, 0u);

    server->stop();
}

TEST(ManagedVideoServerTest, SeparateStreamGroupsCanStepSequentially)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});
    config.streams.push_back({"bravo", "Bravo", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());
    auto alpha_group = server->register_streams({"alpha"});
    auto bravo_group = server->register_streams({"bravo"});
    ASSERT_NE(alpha_group, nullptr);
    ASSERT_NE(bravo_group, nullptr);

    std::vector<uint8_t> alpha_frame(32u * 32u, 7u);
    std::vector<uint8_t> bravo_frame(32u * 32u, 9u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(alpha_frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(server->push_frame("bravo", make_gray_frame_view(bravo_frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(alpha_group->step_streams());
    ASSERT_TRUE(bravo_group->step_streams());
    ASSERT_TRUE(server->step_api());

    const auto alpha = server->get_stream_info("alpha");
    const auto bravo = server->get_stream_info("bravo");
    ASSERT_TRUE(alpha.has_value());
    ASSERT_TRUE(bravo.has_value());
    EXPECT_GT(alpha->access_units_received, 0u);
    EXPECT_GT(bravo->access_units_received, 0u);

    server->stop();
}

TEST(ManagedVideoServerTest, SameStreamGroupConcurrentStepIsRejected)
{
    ManagedHookScope hooks;
    BlockingGate stream_gate;
    video_server::webrtc_internal::set_managed_stream_step_test_hook(
        [&stream_gate](const std::string &)
        { stream_gate.arrive_and_wait(); });

    auto server = video_server::CreateManagedVideoServer(make_software_managed_config("alpha", 64, 48, 30.0));
    ASSERT_TRUE(server->start());
    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);

    std::vector<uint8_t> frame(64u * 48u, 17u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 64, 48, 1000, 1)));

    auto first = std::async(std::launch::async,
                            [&group]()
                            { return group->step_streams(); });
    const bool first_entered = stream_gate.wait_for_entries(1);
    if (!first_entered)
    {
        stream_gate.release();
    }
    ASSERT_TRUE(first_entered) << "first stream step did not enter encode work";

    EXPECT_FALSE(group->step_streams());
    stream_gate.release();
    EXPECT_TRUE(first.get());

    server->stop();
}

TEST(ManagedVideoServerTest, IndependentStreamGroupsActuallyExecuteConcurrently)
{
    ManagedHookScope hooks;
    BlockingGate stream_gate;
    video_server::webrtc_internal::set_managed_stream_step_test_hook(
        [&stream_gate](const std::string &)
        { stream_gate.arrive_and_wait(); });

    auto server = video_server::CreateManagedVideoServer(make_two_stream_software_config());
    ASSERT_TRUE(server->start());
    auto alpha_group = server->register_streams({"alpha"});
    auto bravo_group = server->register_streams({"bravo"});
    ASSERT_NE(alpha_group, nullptr);
    ASSERT_NE(bravo_group, nullptr);

    std::vector<uint8_t> alpha_frame(64u * 48u, 21u);
    std::vector<uint8_t> bravo_frame(64u * 48u, 31u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(alpha_frame, 64, 48, 1000, 1)));
    ASSERT_TRUE(server->push_frame("bravo", make_gray_frame_view(bravo_frame, 64, 48, 1000, 1)));

    auto alpha_step = std::async(std::launch::async,
                                 [&alpha_group]()
                                 { return alpha_group->step_streams(); });
    auto bravo_step = std::async(std::launch::async,
                                 [&bravo_group]()
                                 { return bravo_group->step_streams(); });

    const bool both_entered = stream_gate.wait_for_entries(2);
    stream_gate.release();
    EXPECT_TRUE(both_entered) << "independent groups did not both enter stream work concurrently; entries="
                              << stream_gate.entries();
    EXPECT_TRUE(alpha_step.get());
    EXPECT_TRUE(bravo_step.get());

    server->stop();
}

TEST(ManagedVideoServerTest, EncodedHandoffIsBoundedWhenApiProgressionFallsBehind)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());
    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);

    for (uint64_t i = 0; i < 12; ++i)
    {
        std::vector<uint8_t> frame(32u * 32u, static_cast<uint8_t>(i + 1));
        ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 32, 32, 1000 + i, i + 1)));
        ASSERT_TRUE(group->step_streams());
    }

    auto before_api = server->get_stream_info("alpha");
    ASSERT_TRUE(before_api.has_value());
    EXPECT_EQ(before_api->frames_transformed, 12u);
    EXPECT_EQ(before_api->access_units_received, 0u);

    ASSERT_TRUE(server->step_api());
    const auto after_api = server->get_stream_info("alpha");
    ASSERT_TRUE(after_api.has_value());
    EXPECT_GT(after_api->access_units_received, 0u);
    EXPECT_LE(after_api->access_units_received, 4u);

    server->stop();
}

TEST(ManagedVideoServerTest, StepApiDoesNotEncodePendingRawFrames)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());

    std::vector<uint8_t> frame(32u * 32u, 11u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(server->step_api());

    const auto info = server->get_stream_info("alpha");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->frames_transformed, 0u);
    EXPECT_EQ(info->access_units_received, 0u);

    server->stop();
}

TEST(ManagedVideoServerTest, StepApiCanRunConcurrentlyWithStreamGroup)
{
    ManagedHookScope hooks;
    BlockingGate api_gate;
    BlockingGate stream_gate;
    video_server::webrtc_internal::set_managed_api_step_test_hook(
        [&api_gate]()
        { api_gate.arrive_and_wait(); });
    video_server::webrtc_internal::set_managed_stream_step_test_hook(
        [&stream_gate](const std::string &)
        { stream_gate.arrive_and_wait(); });

    auto server = video_server::CreateManagedVideoServer(make_software_managed_config("alpha", 64, 48, 30.0));
    ASSERT_TRUE(server->start());
    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);

    std::vector<uint8_t> frame(64u * 48u, 17u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 64, 48, 1000, 1)));

    auto api_step = std::async(std::launch::async, [&server]()
                               { return server->step_api(); });
    const bool api_entered = api_gate.wait_for_entries(1);
    if (!api_entered)
    {
        api_gate.release();
    }
    ASSERT_TRUE(api_entered) << "API step did not enter API work";

    auto stream_step = std::async(std::launch::async,
                                  [&group]()
                                  { return group->step_streams(); });
    EXPECT_TRUE(stream_gate.wait_for_entries(1)) << "stream group did not enter stream work while API step was active";
    stream_gate.release();
    api_gate.release();
    EXPECT_TRUE(stream_step.get());
    EXPECT_TRUE(api_step.get());

    ASSERT_TRUE(server->step_api());
    const auto info = server->get_stream_info("alpha");
    ASSERT_TRUE(info.has_value());
    EXPECT_GT(info->access_units_received, 0u);

    server->stop();
}

TEST(ManagedVideoServerTest, ManagedApiThreadRejectsManualStepApi)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.api_execution.mode = video_server::ManagedApiExecutionMode::ManagedThread;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());
    EXPECT_FALSE(server->step_api());
    server->stop();
}

TEST(ManagedVideoServerTest, ManagedApiThreadDrainsEncodedHandoffWithoutManualStepApi)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.api_execution.mode = video_server::ManagedApiExecutionMode::ManagedThread;
    config.streams.push_back({"alpha", "Alpha", 32, 32, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    ASSERT_TRUE(server->start());
    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);

    std::vector<uint8_t> frame(32u * 32u, 13u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 32, 32, 1000, 1)));
    ASSERT_TRUE(group->step_streams());

    EXPECT_TRUE(wait_until([&server]()
                           {
        const auto info = server->get_stream_info("alpha");
        return info.has_value() && info->access_units_received > 0; }));
    server->stop();
}

TEST(ManagedVideoServerTest, ManagedApiThreadRejectsInvalidAffinity)
{
    video_server::ManagedVideoServerConfig config;
    config.webrtc.enable_http_api = false;
    config.api_execution.mode = video_server::ManagedApiExecutionMode::ManagedThread;
    config.api_execution.thread.cpu_affinity.push_back(-1);
    config.streams.push_back({"alpha", "Alpha", 16, 16, 30.0, video_server::VideoPixelFormat::GRAY8});

    auto server = video_server::CreateManagedVideoServer(config);
    EXPECT_FALSE(server->start());
}

TEST(ManagedVideoServerTest, ConcurrentStepApiCallsAreRejected)
{
    ManagedHookScope hooks;
    BlockingGate api_gate;
    video_server::webrtc_internal::set_managed_api_step_test_hook(
        [&api_gate]()
        { api_gate.arrive_and_wait(); });

    auto server = video_server::CreateManagedVideoServer(make_software_managed_config("alpha", 16, 16, 30.0));
    ASSERT_TRUE(server->start());

    auto first = std::async(std::launch::async, [&server]()
                            { return server->step_api(); });
    const bool first_entered = api_gate.wait_for_entries(1);
    if (!first_entered)
    {
        api_gate.release();
    }
    ASSERT_TRUE(first_entered) << "first API step did not enter API work";
    const bool second = server->step_api();
    api_gate.release();
    const bool first_result = first.get();

    EXPECT_TRUE(first_result);
    EXPECT_FALSE(second);

    server->stop();
}

TEST(ManagedVideoServerTest, ServerDestructionWaitsForActiveStreamGroupStep)
{
    ManagedHookScope hooks;
    BlockingGate stream_gate;
    BlockingGate shutdown_gate;
    video_server::webrtc_internal::set_managed_stream_step_test_hook(
        [&stream_gate](const std::string &)
        { stream_gate.arrive_and_wait(); });
    video_server::webrtc_internal::set_managed_shutdown_wait_test_hook(
        [&shutdown_gate]()
        { shutdown_gate.arrive_and_wait(); });

    auto server = video_server::CreateManagedVideoServer(make_software_managed_config("alpha", 64, 48, 30.0));
    ASSERT_TRUE(server->start());
    auto group = server->register_streams({"alpha"});
    ASSERT_NE(group, nullptr);

    std::vector<uint8_t> frame(64u * 48u, 41u);
    ASSERT_TRUE(server->push_frame("alpha", make_gray_frame_view(frame, 64, 48, 1000, 1)));

    auto stream_step = std::async(std::launch::async,
                                  [&group]()
                                  { return group->step_streams(); });
    const bool stream_entered = stream_gate.wait_for_entries(1);
    if (!stream_entered)
    {
        stream_gate.release();
    }
    ASSERT_TRUE(stream_entered) << "stream step did not enter encode work";

    auto destroy_server = std::async(std::launch::async,
                                     [&server]()
                                     {
                                         server.reset();
                                         return true;
                                     });
    const bool shutdown_waiting = shutdown_gate.wait_for_entries(1);
    if (!shutdown_waiting)
    {
        shutdown_gate.release();
        stream_gate.release();
    }
    ASSERT_TRUE(shutdown_waiting) << "server destruction did not reach active-call wait";
    EXPECT_EQ(stream_step.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(destroy_server.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    shutdown_gate.release();
    EXPECT_EQ(destroy_server.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    stream_gate.release();
    EXPECT_TRUE(stream_step.get());
    EXPECT_TRUE(destroy_server.get());
    EXPECT_FALSE(group->step_streams());
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
        out << "api_execution = \"managed_thread\"\n";
        out << "api_cpu_affinity = [0, 1]\n";
        out << "api_scheduling_priority = 2\n";
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
        out << "max_subscribers = 4\n";
        out << "\n[default_raw_pipelines.default]\n";
        out << "codec = \"av1\"\n";
        out << "encoder = \"vaapi\"\n";
        out << "hardware_device = \"/dev/dri/renderD129\"\n";
        out << "encoder_preset = \"veryfast\"\n";
        out << "encoder_crf = 18\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    EXPECT_EQ(config.execution_mode, video_server::ExecutionMode::ManualStep);
    EXPECT_EQ(config.api_execution.mode, video_server::ManagedApiExecutionMode::ManagedThread);
    ASSERT_EQ(config.api_execution.thread.cpu_affinity.size(), 2u);
    EXPECT_EQ(config.api_execution.thread.cpu_affinity[0], 0);
    EXPECT_EQ(config.api_execution.thread.cpu_affinity[1], 1);
    ASSERT_TRUE(config.api_execution.thread.scheduling_priority.has_value());
    EXPECT_EQ(*config.api_execution.thread.scheduling_priority, 2);
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
    EXPECT_EQ(config.streams.front().max_subscribers, 4u);
    ASSERT_TRUE(config.default_raw_pipelines.find("default") != config.default_raw_pipelines.end());
    EXPECT_EQ(config.default_raw_pipelines.at("default").codec, video_server::VideoCodec::AV1);
    EXPECT_EQ(config.default_raw_pipelines.at("default").encoder, video_server::RawH264Encoder::Vaapi);
    EXPECT_EQ(config.default_raw_pipelines.at("default").hardware_device, "/dev/dri/renderD129");
    EXPECT_EQ(config.default_raw_pipelines.at("default").encoder_preset, "veryfast");
    ASSERT_TRUE(config.default_raw_pipelines.at("default").encoder_crf.has_value());
    EXPECT_EQ(*config.default_raw_pipelines.at("default").encoder_crf, 18u);

    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, LoadsLibAomAv1Encoder)
{
    const std::string path = "/tmp/video_server_managed_libaom_config_test.toml";
    {
        std::ofstream out(path);
        out << "[[streams]]\n";
        out << "stream_id = \"software-av1\"\n";
        out << "width = 64\n";
        out << "height = 48\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"RGB24\"\n";
        out << "\n[default_raw_pipelines.default]\n";
        out << "codec = \"av1\"\n";
        out << "encoder = \"libaom-av1\"\n";
        out << "encoder_crf = 32\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    ASSERT_EQ(config.default_raw_pipelines.size(), 1u);
    const auto &pipeline = config.default_raw_pipelines.at("default");
    EXPECT_EQ(pipeline.codec, video_server::VideoCodec::AV1);
    EXPECT_EQ(pipeline.encoder, video_server::RawH264Encoder::LibAomAv1);
    ASSERT_TRUE(pipeline.encoder_crf.has_value());
    EXPECT_EQ(*pipeline.encoder_crf, 32u);

    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, LoadsTomlIceConfiguration)
{
    const std::string path = "/tmp/video_server_managed_ice_config_test.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "http_poll_timeout_ms = 3\n";
        out << "\n[webrtc]\n";
        out << "execution_mode = \"manual_step\"\n";
        out << "http_poll_timeout_ms = 3\n";
        out << "enable_http_api = false\n";
        out << "ice_transport_policy = \"relay\"\n";
        out << "ice_bind_address = \"10.1.2.3\"\n";
        out << "ice_port_range_begin = 49152\n";
        out << "ice_port_range_end = 49200\n";
        out << "ice_servers = [\n";
        out << "  { uri = \"stun:stun.example.com:3478\" },\n";
        out << "  { uri = \"turns:turn.example.com:443?transport=tcp\", username = \"user\", credential = \"secret\" }\n";
        out << "]\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    EXPECT_EQ(config.webrtc.ice.transport_policy, "relay");
    ASSERT_TRUE(config.webrtc.ice.bind_address.has_value());
    EXPECT_EQ(*config.webrtc.ice.bind_address, "10.1.2.3");
    ASSERT_TRUE(config.webrtc.ice.port_range_begin.has_value());
    ASSERT_TRUE(config.webrtc.ice.port_range_end.has_value());
    EXPECT_EQ(*config.webrtc.ice.port_range_begin, 49152u);
    EXPECT_EQ(*config.webrtc.ice.port_range_end, 49200u);
    ASSERT_EQ(config.webrtc.ice.servers.size(), 2u);
    EXPECT_EQ(config.webrtc.ice.servers[0].uri, "stun:stun.example.com:3478");
    EXPECT_TRUE(config.webrtc.ice.servers[0].username.empty());
    EXPECT_TRUE(config.webrtc.ice.servers[0].credential.empty());
    EXPECT_EQ(config.webrtc.ice.servers[1].uri, "turns:turn.example.com:443?transport=tcp");
    EXPECT_EQ(config.webrtc.ice.servers[1].username, "user");
    EXPECT_EQ(config.webrtc.ice.servers[1].credential, "secret");

    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, RejectsMalformedTomlIceConfiguration)
{
    const auto expect_load_error = [](const std::string &body)
    {
        const std::string path = "/tmp/video_server_managed_bad_ice_config_test.toml";
        {
            std::ofstream out(path);
            out << "execution_mode = \"manual_step\"\n";
            out << "http_poll_timeout_ms = 3\n";
            out << "\n[webrtc]\n";
            out << "execution_mode = \"manual_step\"\n";
            out << "http_poll_timeout_ms = 3\n";
            out << body;
        }
        EXPECT_THROW(static_cast<void>(video_server::load_managed_video_server_config(path)), std::runtime_error);
        std::remove(path.c_str());
    };

    expect_load_error("ice_servers = [\"stun:stun.example.com:3478\"]\n");
    expect_load_error("ice_servers = [{ username = \"user\", credential = \"secret\" }]\n");
    expect_load_error("ice_port_range_begin = 49152\n");
    expect_load_error("ice_port_range_begin = 49200\nice_port_range_end = 49152\n");
    expect_load_error("ice_transport_policy = \"relay-only\"\n");
}

TEST(ManagedVideoServerTest, LoadsTomlConfigDefaultsMaxSubscribersAndSupportsPerStreamOverrides)
{
    const std::string path = "/tmp/video_server_managed_config_multi_stream_subscribers.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"alpha\"\n";
        out << "label = \"Alpha\"\n";
        out << "width = 320\n";
        out << "height = 240\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY8\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"bravo\"\n";
        out << "label = \"Bravo\"\n";
        out << "width = 640\n";
        out << "height = 360\n";
        out << "nominal_fps = 15.0\n";
        out << "input_pixel_format = \"RGB24\"\n";
        out << "max_subscribers = 3\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    ASSERT_EQ(config.streams.size(), 2u);
    EXPECT_EQ(config.streams[0].stream_id, "alpha");
    EXPECT_EQ(config.streams[0].max_subscribers, 1u);
    EXPECT_EQ(config.streams[1].stream_id, "bravo");
    EXPECT_EQ(config.streams[1].max_subscribers, 3u);

    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, RejectsTomlConfigWithOutOfRangeEncoderCrf)
{
    const std::string path = "/tmp/video_server_managed_config_invalid_encoder_crf.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[default_raw_pipelines.default]\n";
        out << "encoder_crf = 52\n";
    }

    EXPECT_THROW(static_cast<void>(video_server::load_managed_video_server_config(path)), std::runtime_error);
    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, RejectsTomlConfigWithZeroMaxSubscribers)
{
    const std::string path = "/tmp/video_server_managed_config_invalid_max_subscribers.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"alpha\"\n";
        out << "label = \"Alpha\"\n";
        out << "width = 320\n";
        out << "height = 240\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY8\"\n";
        out << "max_subscribers = 0\n";
    }

    EXPECT_THROW(static_cast<void>(video_server::load_managed_video_server_config(path)), std::runtime_error);
    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, RejectsTomlConfigWithNegativeMaxSubscribers)
{
    const std::string path = "/tmp/video_server_managed_config_negative_max_subscribers.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"alpha\"\n";
        out << "label = \"Alpha\"\n";
        out << "width = 320\n";
        out << "height = 240\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY8\"\n";
        out << "max_subscribers = -1\n";
    }

    EXPECT_THROW(static_cast<void>(video_server::load_managed_video_server_config(path)), std::runtime_error);
    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, LoadsExplicitHighBitDepthGrayscaleFormatsAndAliases)
{
    const std::string path = "/tmp/video_server_managed_config_gray_formats.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"gray10\"\n";
        out << "label = \"Gray10\"\n";
        out << "width = 64\n";
        out << "height = 48\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY10\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"gray16\"\n";
        out << "label = \"Gray16\"\n";
        out << "width = 64\n";
        out << "height = 48\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"GRAY16LE\"\n";
    }

    const auto config = video_server::load_managed_video_server_config(path);
    ASSERT_EQ(config.streams.size(), 2u);
    EXPECT_EQ(config.streams[0].input_pixel_format, video_server::VideoPixelFormat::GRAY10LE);
    EXPECT_EQ(config.streams[1].input_pixel_format, video_server::VideoPixelFormat::GRAY16LE);

    std::remove(path.c_str());
}

TEST(ManagedVideoServerTest, RejectsTomlConfigWithManagedUnsupportedPixelFormat)
{
    const std::string path = "/tmp/video_server_managed_config_unsupported_pixel_format.toml";
    {
        std::ofstream out(path);
        out << "execution_mode = \"manual_step\"\n";
        out << "\n[[streams]]\n";
        out << "stream_id = \"alpha\"\n";
        out << "label = \"Alpha\"\n";
        out << "width = 320\n";
        out << "height = 240\n";
        out << "nominal_fps = 30.0\n";
        out << "input_pixel_format = \"NV12\"\n";
    }

    EXPECT_THROW(static_cast<void>(video_server::load_managed_video_server_config(path)), std::runtime_error);
    std::remove(path.c_str());
}
