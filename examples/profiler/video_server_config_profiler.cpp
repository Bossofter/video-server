#include "video_server/managed_video_server.h"

#include "synthetic_frame_generator.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Options
    {
        double duration_seconds{5.0};
        std::vector<std::string> config_paths{
            "examples/encoder_profiles/min_latency.toml",
            "examples/encoder_profiles/balanced.toml",
            "examples/encoder_profiles/max_quality.toml",
        };
    };

    struct StreamMetrics
    {
        std::string stream_id;
        uint64_t frames_pushed{0};
        uint64_t access_units{0};
        uint64_t sampled_encoded_bytes{0};
        uint64_t sampled_encoded_units{0};
        uint64_t last_encoded_sequence_id{0};
    };

    struct ProfileResult
    {
        std::string config_path;
        std::string requested_encoder;
        std::string selected_encoder;
        std::string encoder_settings;
        bool x264_options_active{false};
        bool success{false};
        std::string error;
        double elapsed_seconds{0.0};
        uint64_t step_calls{0};
        double avg_push_us{0.0};
        double p95_push_us{0.0};
        double avg_step_us{0.0};
        double p95_step_us{0.0};
        std::vector<StreamMetrics> streams;
    };

    const AVCodec *select_libav_h264_encoder(video_server::RawH264Encoder encoder)
    {
        switch (encoder)
        {
        case video_server::RawH264Encoder::LibX264:
            return avcodec_find_encoder_by_name("libx264");
        case video_server::RawH264Encoder::LibOpenH264:
            return avcodec_find_encoder_by_name("libopenh264");
        case video_server::RawH264Encoder::Vaapi:
            return avcodec_find_encoder_by_name("h264_vaapi");
        case video_server::RawH264Encoder::Automatic:
        {
            const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
            if (codec != nullptr)
            {
                return codec;
            }
            codec = avcodec_find_encoder_by_name("libx264");
            if (codec != nullptr)
            {
                return codec;
            }
            codec = avcodec_find_encoder_by_name("libopenh264");
            if (codec != nullptr)
            {
                return codec;
            }
            return avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        }
        return nullptr;
    }

    video_server::RawVideoPipelineConfig representative_pipeline_config(const video_server::ManagedVideoServerConfig &config)
    {
        if (!config.streams.empty())
        {
            const auto stream_it = config.default_raw_pipelines.find(config.streams.front().stream_id);
            if (stream_it != config.default_raw_pipelines.end())
            {
                return stream_it->second;
            }
        }
        const auto default_it = config.default_raw_pipelines.find("default");
        if (default_it != config.default_raw_pipelines.end())
        {
            return default_it->second;
        }
        return {};
    }

    std::string describe_encoder_settings(const video_server::RawVideoPipelineConfig &config)
    {
        std::string settings = std::string("preset=") + config.encoder_preset + " tune=" + config.encoder_tune +
                               " profile=" + config.encoder_profile;
        if (config.encoder_crf.has_value())
        {
            settings += " crf=" + std::to_string(*config.encoder_crf);
        }
        else
        {
            settings += " crf=unset";
        }
        return settings;
    }

    void print_usage(const char *program)
    {
        std::cout << "Usage: " << program << " [--duration-seconds N] [CONFIG...]\n"
                  << "\n"
                  << "When no CONFIG paths are supplied, the built-in encoder profile examples are used.\n";
    }

    double parse_positive_double(const std::string &value, const char *name)
    {
        size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size() || parsed <= 0.0)
        {
            throw std::runtime_error(std::string(name) + " must be > 0");
        }
        return parsed;
    }

    Options parse_args(int argc, char **argv)
    {
        Options options;
        std::vector<std::string> explicit_configs;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto require_value = [&](const char *name) -> std::string
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error(std::string("missing value for ") + name);
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            if (arg == "--duration-seconds")
            {
                options.duration_seconds = parse_positive_double(require_value("--duration-seconds"), "--duration-seconds");
            }
            else
            {
                explicit_configs.push_back(arg);
            }
        }
        if (!explicit_configs.empty())
        {
            options.config_paths = std::move(explicit_configs);
        }
        return options;
    }

    double average_us(const std::vector<double> &values)
    {
        if (values.empty())
        {
            return 0.0;
        }
        const double sum = std::accumulate(values.begin(), values.end(), 0.0);
        return sum / static_cast<double>(values.size());
    }

    double percentile_us(std::vector<double> values, double percentile)
    {
        if (values.empty())
        {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const double raw_index = percentile * static_cast<double>(values.size() - 1);
        const auto index = static_cast<size_t>(std::min<double>(raw_index, static_cast<double>(values.size() - 1)));
        return values[index];
    }

    size_t find_stream_index(const std::vector<StreamMetrics> &streams, const std::string &stream_id)
    {
        for (size_t i = 0; i < streams.size(); ++i)
        {
            if (streams[i].stream_id == stream_id)
            {
                return i;
            }
        }
        return std::numeric_limits<size_t>::max();
    }

    void sample_encoded_metrics(ProfileResult &result, const video_server::ServerDebugSnapshot &snapshot)
    {
        for (const auto &stream : snapshot.streams)
        {
            const size_t index = find_stream_index(result.streams, stream.stream_id);
            if (index == std::numeric_limits<size_t>::max())
            {
                continue;
            }
            auto &metrics = result.streams[index];
            metrics.access_units = stream.total_access_units_received;
            if (stream.latest_encoded_access_unit_available &&
                stream.latest_encoded_sequence_id != metrics.last_encoded_sequence_id)
            {
                metrics.last_encoded_sequence_id = stream.latest_encoded_sequence_id;
                metrics.sampled_encoded_bytes += stream.latest_encoded_size_bytes;
                ++metrics.sampled_encoded_units;
            }
        }
    }

    ProfileResult run_profile(const std::string &config_path, double duration_seconds)
    {
        ProfileResult result;
        result.config_path = config_path;

        try
        {
            auto config = video_server::load_managed_video_server_config(config_path);
            const auto representative_pipeline = representative_pipeline_config(config);
            result.requested_encoder = video_server::to_string(representative_pipeline.encoder);
            result.encoder_settings = describe_encoder_settings(representative_pipeline);
            const AVCodec *selected_codec = select_libav_h264_encoder(representative_pipeline.encoder);
            result.selected_encoder = selected_codec != nullptr && selected_codec->name != nullptr ? selected_codec->name : "unavailable";
            result.x264_options_active = result.selected_encoder.find("x264") != std::string::npos;

            config.execution_mode = video_server::ExecutionMode::ManualStep;
            config.webrtc.execution_mode = video_server::ExecutionMode::ManualStep;
            config.webrtc.enable_http_api = false;
            config.webrtc.http_poll_timeout_ms = config.http_poll_timeout_ms;

            std::vector<video_server::SyntheticFrameGenerator> generators;
            generators.reserve(config.streams.size());
            result.streams.reserve(config.streams.size());
            for (const auto &stream : config.streams)
            {
                generators.emplace_back(stream);
                StreamMetrics metrics;
                metrics.stream_id = stream.stream_id;
                result.streams.push_back(std::move(metrics));
            }

            auto server = video_server::CreateManagedVideoServer(std::move(config));
            if (!server->start())
            {
                throw std::runtime_error("failed to start managed server");
            }

            std::vector<double> push_us;
            std::vector<double> step_us;
            const auto started_at = Clock::now();
            const auto stop_at = started_at + std::chrono::duration<double>(duration_seconds);
            while (Clock::now() < stop_at)
            {
                for (size_t i = 0; i < generators.size(); ++i)
                {
                    const auto frame = generators[i].next_frame();
                    const auto push_started = Clock::now();
                    if (!server->push_frame(generators[i].config().stream_id, frame))
                    {
                        throw std::runtime_error("push_frame failed for stream " + generators[i].config().stream_id);
                    }
                    const auto push_finished = Clock::now();
                    push_us.push_back(std::chrono::duration<double, std::micro>(push_finished - push_started).count());
                    ++result.streams[i].frames_pushed;
                }

                const auto step_started = Clock::now();
                if (!server->step())
                {
                    throw std::runtime_error("server step failed");
                }
                const auto step_finished = Clock::now();
                step_us.push_back(std::chrono::duration<double, std::micro>(step_finished - step_started).count());
                ++result.step_calls;
                sample_encoded_metrics(result, server->get_debug_snapshot());
            }

            server->stop();
            result.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started_at).count();
            result.avg_push_us = average_us(push_us);
            result.p95_push_us = percentile_us(push_us, 0.95);
            result.avg_step_us = average_us(step_us);
            result.p95_step_us = percentile_us(step_us, 0.95);
            result.success = true;
        }
        catch (const std::exception &ex)
        {
            result.error = ex.what();
        }

        return result;
    }

    void print_result(const ProfileResult &result)
    {
        std::cout << "\n== " << result.config_path << " ==\n";
        if (!result.success)
        {
            if (!result.selected_encoder.empty())
            {
                std::cout << "encoder requested=" << result.requested_encoder
                          << " selected=" << result.selected_encoder
                          << " " << result.encoder_settings << "\n";
            }
            std::cout << "FAILED: " << result.error << "\n";
            return;
        }

        uint64_t frames = 0;
        uint64_t access_units = 0;
        uint64_t sampled_bytes = 0;
        uint64_t sampled_units = 0;
        for (const auto &stream : result.streams)
        {
            frames += stream.frames_pushed;
            access_units += stream.access_units;
            sampled_bytes += stream.sampled_encoded_bytes;
            sampled_units += stream.sampled_encoded_units;
        }

        const double frame_rate = result.elapsed_seconds > 0.0 ? static_cast<double>(frames) / result.elapsed_seconds : 0.0;
        const double au_rate = result.elapsed_seconds > 0.0 ? static_cast<double>(access_units) / result.elapsed_seconds : 0.0;
        const double avg_encoded_bytes = sampled_units > 0 ? static_cast<double>(sampled_bytes) / static_cast<double>(sampled_units) : 0.0;

        std::cout << std::fixed << std::setprecision(2)
                  << "encoder requested=" << result.requested_encoder
                  << " selected=" << result.selected_encoder
                  << " x264_options_active=" << (result.x264_options_active ? "yes" : "no")
                  << " " << result.encoder_settings << "\n"
                  << "elapsed_s=" << result.elapsed_seconds
                  << " frames=" << frames
                  << " frame_rate=" << frame_rate
                  << " access_units=" << access_units
                  << " access_unit_rate=" << au_rate
                  << " avg_sampled_encoded_bytes=" << avg_encoded_bytes << "\n"
                  << "push_us_avg=" << result.avg_push_us
                  << " push_us_p95=" << result.p95_push_us
                  << " step_us_avg=" << result.avg_step_us
                  << " step_us_p95=" << result.p95_step_us
                  << " step_calls=" << result.step_calls << "\n";

        for (const auto &stream : result.streams)
        {
            const double stream_fps = result.elapsed_seconds > 0.0
                                          ? static_cast<double>(stream.frames_pushed) / result.elapsed_seconds
                                          : 0.0;
            const double stream_bytes = stream.sampled_encoded_units > 0
                                            ? static_cast<double>(stream.sampled_encoded_bytes) /
                                                  static_cast<double>(stream.sampled_encoded_units)
                                            : 0.0;
            std::cout << "  stream=" << stream.stream_id
                      << " frames=" << stream.frames_pushed
                      << " frame_rate=" << stream_fps
                      << " access_units=" << stream.access_units
                      << " avg_sampled_encoded_bytes=" << stream_bytes << "\n";
        }
    }

    struct AggregateMetrics
    {
        uint64_t frames{0};
        uint64_t access_units{0};
        uint64_t sampled_bytes{0};
        uint64_t sampled_units{0};
        double frame_rate{0.0};
        double access_unit_rate{0.0};
        double avg_sampled_encoded_bytes{0.0};
    };

    AggregateMetrics aggregate(const ProfileResult &result)
    {
        AggregateMetrics metrics;
        if (!result.success)
        {
            return metrics;
        }
        for (const auto &stream : result.streams)
        {
            metrics.frames += stream.frames_pushed;
            metrics.access_units += stream.access_units;
            metrics.sampled_bytes += stream.sampled_encoded_bytes;
            metrics.sampled_units += stream.sampled_encoded_units;
        }
        metrics.frame_rate = result.elapsed_seconds > 0.0 ? static_cast<double>(metrics.frames) / result.elapsed_seconds : 0.0;
        metrics.access_unit_rate =
            result.elapsed_seconds > 0.0 ? static_cast<double>(metrics.access_units) / result.elapsed_seconds : 0.0;
        metrics.avg_sampled_encoded_bytes = metrics.sampled_units > 0
                                                ? static_cast<double>(metrics.sampled_bytes) /
                                                      static_cast<double>(metrics.sampled_units)
                                                : 0.0;
        return metrics;
    }

    void print_summary(const std::vector<ProfileResult> &results)
    {
        std::cout << "\n== Summary ==\n";
        std::cout << std::left << std::setw(44) << "config"
                  << std::setw(14) << "encoder"
                  << std::right << std::setw(12) << "fps"
                  << std::setw(12) << "au/s"
                  << std::setw(14) << "step_p95_us"
                  << std::setw(14) << "avg_bytes"
                  << "\n";
        for (const auto &result : results)
        {
            std::cout << std::left << std::setw(44) << result.config_path;
            if (!result.success)
            {
                std::cout << std::left << std::setw(14) << result.selected_encoder
                          << std::right << std::setw(52) << "FAILED" << "\n";
                continue;
            }
            const auto metrics = aggregate(result);
            std::cout << std::left << std::setw(14) << result.selected_encoder
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(12) << metrics.frame_rate
                      << std::setw(12) << metrics.access_unit_rate
                      << std::setw(14) << result.p95_step_us
                      << std::setw(14) << metrics.avg_sampled_encoded_bytes
                      << "\n";
        }
    }
} // namespace

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::warn);

    try
    {
        const auto options = parse_args(argc, argv);
        std::cout << "Profiling " << options.config_paths.size()
                  << " config(s) for " << options.duration_seconds << "s each.\n";
        std::vector<ProfileResult> results;
        results.reserve(options.config_paths.size());
        for (const auto &path : options.config_paths)
        {
            results.push_back(run_profile(path, options.duration_seconds));
            print_result(results.back());
        }
        print_summary(results);

        const bool all_ok = std::all_of(results.begin(), results.end(),
                                        [](const ProfileResult &result)
                                        { return result.success; });
        return all_ok ? 0 : 1;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "profiler error: " << ex.what() << "\n";
        return 1;
    }
}
