#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video_server/raw_video_pipeline.h"
#include "raw_h264_encoder_backend.h"
#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"
#include "video_server/video_types.h"
#include "../src/core/video_server_core.h"

namespace
{

    std::vector<uint8_t> make_rgb_frame(uint32_t width, uint32_t height, uint64_t seed)
    {
        std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3u);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t index = (static_cast<size_t>(y) * width + x) * 3u;
                frame[index + 0] = static_cast<uint8_t>((x * 7 + seed) % 255);
                frame[index + 1] = static_cast<uint8_t>((y * 13 + seed * 3) % 255);
                frame[index + 2] = static_cast<uint8_t>(((x + y) * 5 + seed * 11) % 255);
            }
        }
        return frame;
    }

    video_server::VideoFrameView make_rgb_view(const std::vector<uint8_t> &frame, uint32_t width, uint32_t height,
                                               uint64_t timestamp_ns, uint64_t frame_id)
    {
        return video_server::VideoFrameView{frame.data(), width, height, width * 3u, video_server::VideoPixelFormat::RGB24,
                                            timestamp_ns, frame_id};
    }

    bool contains_start_code(const std::vector<uint8_t> &bytes)
    {
        for (size_t i = 0; i + 3 < bytes.size(); ++i)
        {
            if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 &&
                ((bytes[i + 2] == 0x01) || (bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01)))
            {
                return true;
            }
        }
        return false;
    }

    bool contains_nal_type(const std::vector<uint8_t> &bytes, uint8_t nal_type)
    {
        for (size_t i = 0; i + 4 < bytes.size(); ++i)
        {
            if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 && bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01 &&
                (bytes[i + 4] & 0x1F) == nal_type)
            {
                return true;
            }
            if (bytes[i] == 0x00 && bytes[i + 1] == 0x00 && bytes[i + 2] == 0x01 && (bytes[i + 3] & 0x1F) == nal_type)
            {
                return true;
            }
        }
        return false;
    }

    bool wait_until(const std::function<bool()> &predicate, int timeout_ms = 4000)
    {
        for (int waited = 0; waited < timeout_ms; waited += 25)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return predicate();
    }

    video_server::RawVideoPipelineConfig make_default_config(uint32_t width = 64, uint32_t height = 48,
                                                             double fps = 30.0)
    {
        video_server::RawVideoPipelineConfig config;
        config.input_width = width;
        config.input_height = height;
        config.input_pixel_format = video_server::VideoPixelFormat::RGB24;
        config.input_fps = fps;
        return config;
    }

    TEST(RawToH264PipelineTest, BuildsLibavBackendOptionsPerEncoderFamily)
    {
        auto config = make_default_config();

        const auto x264_options = video_server::build_libav_h264_encoder_backend_options(config, "libx264");
        EXPECT_EQ(x264_options.family, video_server::RawH264EncoderFamily::X264);
        EXPECT_FALSE(x264_options.set_constrained_baseline_profile);
        EXPECT_TRUE(std::any_of(x264_options.private_options.begin(), x264_options.private_options.end(),
                                [](const video_server::RawH264EncoderOption &option)
                                { return option.key == "preset"; }));
        EXPECT_TRUE(std::any_of(x264_options.private_options.begin(), x264_options.private_options.end(),
                                [](const video_server::RawH264EncoderOption &option)
                                { return option.key == "aud"; }));

        const auto openh264_options = video_server::build_libav_h264_encoder_backend_options(config, "libopenh264");
        EXPECT_EQ(openh264_options.family, video_server::RawH264EncoderFamily::OpenH264);
        EXPECT_FALSE(openh264_options.set_constrained_baseline_profile);
        EXPECT_TRUE(std::any_of(openh264_options.private_options.begin(), openh264_options.private_options.end(),
                                [](const video_server::RawH264EncoderOption &option)
                                {
                                    return option.key == "allow_skip_frames" && option.value == "1";
                                }));
        EXPECT_FALSE(std::any_of(openh264_options.private_options.begin(), openh264_options.private_options.end(),
                                 [](const video_server::RawH264EncoderOption &option)
                                 { return option.key == "preset"; }));
        EXPECT_FALSE(std::any_of(openh264_options.private_options.begin(), openh264_options.private_options.end(),
                                 [](const video_server::RawH264EncoderOption &option)
                                 { return option.key == "aud"; }));
    }

    TEST(RawToH264PipelineTest, UsesEncoderBackendFactorySeamToProduceOutput)
    {
        std::vector<std::vector<uint8_t>> access_units;
        auto config = make_default_config();
        auto backend = video_server::make_raw_h264_encoder_backend(
            "backend-seam", config,
            [&access_units](const video_server::EncodedAccessUnitView &access_unit)
            {
                access_units.emplace_back(static_cast<const uint8_t *>(access_unit.data),
                                          static_cast<const uint8_t *>(access_unit.data) + access_unit.size_bytes);
                return true;
            });

        std::string error;
        ASSERT_TRUE(backend->open(&error)) << error;
        EXPECT_STREQ(backend->backend_name(), "libav_h264");
        for (uint64_t i = 0; i < 12; ++i)
        {
            auto frame = make_rgb_frame(config.input_width, config.input_height, i + 101);
            ASSERT_TRUE(backend->encode_frame(
                make_rgb_view(frame, config.input_width, config.input_height, 4000000000ull + i * 33333333ull, i + 1), &error))
                << error;
        }
        ASSERT_TRUE(backend->flush(&error)) << error;
        backend->close();

        ASSERT_FALSE(access_units.empty());
        EXPECT_TRUE(contains_start_code(access_units.front()));
    }

    TEST(RawToH264PipelineTest, ProducesEncodedAccessUnitsFromRawFrames)
    {
        std::vector<std::vector<uint8_t>> access_units;
        auto config = make_default_config();

        auto pipeline = video_server::make_raw_to_h264_pipeline(
            "raw-smoke", config,
            [&access_units](const video_server::EncodedAccessUnitView &access_unit)
            {
                access_units.emplace_back(static_cast<const uint8_t *>(access_unit.data),
                                          static_cast<const uint8_t *>(access_unit.data) + access_unit.size_bytes);
                return true;
            });

        std::string error;
        ASSERT_TRUE(pipeline->start(&error)) << error;
        for (uint64_t i = 0; i < 12; ++i)
        {
            auto frame = make_rgb_frame(config.input_width, config.input_height, i + 1);
            ASSERT_TRUE(
                pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 1000000000ull + i * 33333333ull, i + 1), &error))
                << error;
        }
        ASSERT_TRUE(wait_until([&access_units]()
                               { return !access_units.empty(); }));
        pipeline->stop();

        ASSERT_FALSE(access_units.empty());
        EXPECT_TRUE(contains_start_code(access_units.front()));
    }

    TEST(RawToH264PipelineTest, BindsEncodedOutputIntoExistingServerPath)
    {
        video_server::VideoServerCore server;
        video_server::StreamConfig stream_config{"raw-to-server", "raw", 64, 48, 30.0, video_server::VideoPixelFormat::RGB24};
        ASSERT_TRUE(server.register_stream(stream_config));

        auto pipeline_config = make_default_config(stream_config.width, stream_config.height, stream_config.nominal_fps);
        auto pipeline = video_server::make_raw_to_h264_pipeline_for_server(stream_config.stream_id, pipeline_config, server);
        std::string error;
        ASSERT_TRUE(pipeline->start(&error)) << error;
        for (uint64_t i = 0; i < 10; ++i)
        {
            auto frame = make_rgb_frame(stream_config.width, stream_config.height, i + 17);
            ASSERT_TRUE(pipeline->push_frame(
                make_rgb_view(frame, stream_config.width, stream_config.height, 500000000ull + i * 33333333ull, i + 1),
                &error))
                << error;
        }

        ASSERT_TRUE(wait_until([&server, &stream_config]()
                               {
    auto info = server.get_stream_info(stream_config.stream_id);
    return info.has_value() && info->access_units_received > 0 && info->has_latest_encoded_unit; }));
        pipeline->stop();

        auto info = server.get_stream_info(stream_config.stream_id);
        ASSERT_TRUE(info.has_value());
        EXPECT_GT(info->access_units_received, 0u);
        EXPECT_TRUE(info->has_latest_encoded_unit);

        auto latest = server.get_latest_encoded_unit_for_stream(stream_config.stream_id);
        ASSERT_NE(latest, nullptr);
        EXPECT_TRUE(latest->valid);
        EXPECT_EQ(latest->codec, video_server::VideoCodec::H264);
        EXPECT_FALSE(latest->bytes.empty());
    }

    TEST(RawToH264PipelineTest, PreservesResizeAndOutputFpsConfiguration)
    {
        std::vector<uint64_t> timestamps;
        std::vector<std::vector<uint8_t>> bytes;
        std::mutex mutex;
        auto config = make_default_config(80, 60, 24.0);
        config.scale_mode = video_server::RawPipelineScaleMode::Resize;
        config.output_width = 40;
        config.output_height = 30;
        config.output_fps = 12.0;

        auto pipeline = video_server::make_raw_to_h264_pipeline(
            "resize-stream", config,
            [&mutex, &timestamps, &bytes](const video_server::EncodedAccessUnitView &access_unit)
            {
                std::lock_guard<std::mutex> lock(mutex);
                bytes.emplace_back(static_cast<const uint8_t *>(access_unit.data),
                                   static_cast<const uint8_t *>(access_unit.data) + access_unit.size_bytes);
                timestamps.push_back(access_unit.timestamp_ns);
                return true;
            });

        std::string error;
        ASSERT_TRUE(pipeline->start(&error)) << error;
        for (uint64_t i = 0; i < 18; ++i)
        {
            auto frame = make_rgb_frame(config.input_width, config.input_height, i + 23);
            ASSERT_TRUE(
                pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, i * 41666666ull, i + 1), &error))
                << error;
        }
        ASSERT_TRUE(wait_until([&timestamps]()
                               { return !timestamps.empty(); }));
        pipeline->stop();

        ASSERT_FALSE(bytes.empty());
        bool saw_sps = false;
        for (const auto &au : bytes)
        {
            if (contains_nal_type(au, 7))
            {
                saw_sps = true;
                break;
            }
        }
        EXPECT_TRUE(saw_sps);
        EXPECT_LT(timestamps.size(), 18u);
    }

    TEST(RawToH264PipelineTest, PropagatesSinkFailureToCallerAndKeepsItTerminal)
    {
        auto config = make_default_config();
        std::atomic<size_t> sink_calls{0};
        auto pipeline = video_server::make_raw_to_h264_pipeline(
            "sink-failure-stream", config,
            [&sink_calls](const video_server::EncodedAccessUnitView &)
            {
                ++sink_calls;
                return false;
            });

        std::string error;
        ASSERT_TRUE(pipeline->start(&error)) << error;
        bool saw_terminal_failure = false;
        for (uint64_t i = 0; i < 12; ++i)
        {
            auto frame = make_rgb_frame(config.input_width, config.input_height, i + 51);
            if (!pipeline->push_frame(
                    make_rgb_view(frame, config.input_width, config.input_height, 2000000000ull + i * 33333333ull, i + 1),
                    &error))
            {
                saw_terminal_failure = true;
                break;
            }
        }

        ASSERT_TRUE(wait_until([&sink_calls]()
                               { return sink_calls.load() > 0; }));
        EXPECT_TRUE(saw_terminal_failure);
        EXPECT_NE(error.find("sink rejected"), std::string::npos);

        auto extra_frame = make_rgb_frame(config.input_width, config.input_height, 99);
        EXPECT_FALSE(pipeline->push_frame(
            make_rgb_view(extra_frame, config.input_width, config.input_height, 3000000000ull, 99), &error));
        EXPECT_NE(error.find("sink rejected"), std::string::npos);

        std::string repeated_error;
        auto later_frame = make_rgb_frame(config.input_width, config.input_height, 100);
        EXPECT_FALSE(pipeline->push_frame(
            make_rgb_view(later_frame, config.input_width, config.input_height, 3000000001ull, 100), &repeated_error));
        EXPECT_EQ(repeated_error, error);
        pipeline->stop();
    }

    TEST(RawToH264PipelineTest, StopsCleanlyAfterFramesArePushed)
    {
        auto config = make_default_config();
        size_t units_seen = 0;
        auto pipeline = video_server::make_raw_to_h264_pipeline(
            "lifecycle-stream", config,
            [&units_seen](const video_server::EncodedAccessUnitView &)
            {
                ++units_seen;
                return true;
            });

        std::string error;
        ASSERT_TRUE(pipeline->start(&error)) << error;
        for (uint64_t i = 0; i < 6; ++i)
        {
            auto frame = make_rgb_frame(config.input_width, config.input_height, i + 31);
            ASSERT_TRUE(
                pipeline->push_frame(make_rgb_view(frame, config.input_width, config.input_height, 1200000000ull + i * 33333333ull, i + 1), &error))
                << error;
        }
        pipeline->stop();
        EXPECT_GE(units_seen, 0u);
        auto extra_frame = make_rgb_frame(config.input_width, config.input_height, 99);
        EXPECT_FALSE(
            pipeline->push_frame(make_rgb_view(extra_frame, config.input_width, config.input_height, 999999999ull, 99), &error));
    }

    TEST(RawToH264PipelineTest, ThreePipelinesCanEncodeDistinctStreamsConcurrently)
    {
        video_server::VideoServerCore server;
        const std::array<video_server::StreamConfig, 3> streams{{
            {"alpha", "alpha", 64, 48, 30.0, video_server::VideoPixelFormat::RGB24},
            {"bravo", "bravo", 96, 54, 24.0, video_server::VideoPixelFormat::RGB24},
            {"charlie", "charlie", 80, 60, 15.0, video_server::VideoPixelFormat::RGB24},
        }};
        std::vector<std::unique_ptr<video_server::IRawVideoPipeline>> pipelines;
        pipelines.reserve(streams.size());
        for (const auto &cfg : streams)
        {
            ASSERT_TRUE(server.register_stream(cfg));
            auto pipeline_config = make_default_config(cfg.width, cfg.height, cfg.nominal_fps);
            pipelines.push_back(video_server::make_raw_to_h264_pipeline_for_server(cfg.stream_id, pipeline_config, server));
        }

        for (size_t i = 0; i < pipelines.size(); ++i)
        {
            std::string error;
            ASSERT_TRUE(pipelines[i]->start(&error)) << streams[i].stream_id << ": " << error;
        }

        for (uint64_t frame_index = 0; frame_index < 12; ++frame_index)
        {
            for (const auto &cfg : streams)
            {
                auto frame = make_rgb_frame(cfg.width, cfg.height, frame_index + cfg.width);
                auto view = make_rgb_view(frame, cfg.width, cfg.height, 100000000ull * (frame_index + 1), frame_index + 1);
                std::string error;
                auto *pipeline = std::find_if(pipelines.begin(), pipelines.end(), [&](const auto &candidate)
                                              { return candidate->stream_id() == cfg.stream_id; })
                                     ->get();
                ASSERT_TRUE(pipeline->push_frame(view, &error)) << cfg.stream_id << ": " << error;
            }
        }

        ASSERT_TRUE(wait_until([&server, &streams]()
                               {
    for (const auto& cfg : streams) {
      auto info = server.get_stream_info(cfg.stream_id);
      if (!info.has_value() || info->access_units_received == 0 || !info->has_latest_encoded_unit) {
        return false;
      }
    }
    return true; }));

        for (auto &pipeline : pipelines)
        {
            pipeline->stop();
        }
    }

} // namespace
