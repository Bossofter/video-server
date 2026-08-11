#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "video_server/managed_video_server.h"
#include "../../src/webrtc/logging_utils.h"
#include "../../src/testing/synthetic_frame_generator.h"

namespace
{

    using Clock = std::chrono::steady_clock;

    struct StreamSpec
    {
        std::string stream_id;
        std::string label;
        uint32_t width{0};
        uint32_t height{0};
        double fps{0.0};
        video_server::VideoPixelFormat pixel_format{video_server::VideoPixelFormat::RGB24};
        uint32_t max_subscribers{4};
    };

    struct Options
    {
        std::string config_path{"examples/nicegui_smoke/smoke_server.toml"};
        std::string host;
        uint16_t port{0};
        bool enable_debug_api{false};
        bool lan_only{false};
        std::string shared_key;
        std::vector<std::string> ice_servers;
        std::string ice_username;
        std::string ice_credential;
        std::string ice_transport_policy;
        std::string ice_bind_address;
        std::optional<uint16_t> ice_port_range_begin;
        std::optional<uint16_t> ice_port_range_end;
        std::vector<StreamSpec> streams;
        double duration_seconds{0.0};
        double stats_interval_seconds{5.0};
        bool print_observability_summary{false};
    };

    struct RunningStream
    {
        video_server::StreamConfig config;
        video_server::SyntheticFrameGenerator generator;
        Clock::duration frame_interval;
        Clock::time_point next_frame_at;

        RunningStream(const video_server::StreamConfig &stream_config, Clock::time_point start_time)
            : config(stream_config),
              generator(config),
              frame_interval(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / config.nominal_fps))),
              next_frame_at(start_time) {}
    };

    struct StepStats
    {
        uint64_t count{0};
        uint64_t failed{0};
        uint64_t deadline_misses{0};
        double total_ms{0.0};
        double max_ms{0.0};
        std::vector<double> samples_ms;

        void record(double elapsed_ms, bool ok, bool deadline_missed)
        {
            ++count;
            if (!ok)
            {
                ++failed;
            }
            if (deadline_missed)
            {
                ++deadline_misses;
            }
            total_ms += elapsed_ms;
            max_ms = std::max(max_ms, elapsed_ms);
            samples_ms.push_back(elapsed_ms);
        }

        double percentile(double p) const
        {
            if (samples_ms.empty())
            {
                return 0.0;
            }
            auto sorted = samples_ms;
            std::sort(sorted.begin(), sorted.end());
            const auto index = static_cast<size_t>(std::min<double>(
                static_cast<double>(sorted.size() - 1),
                std::ceil((p / 100.0) * static_cast<double>(sorted.size())) - 1.0));
            return sorted[index];
        }

        double mean_ms() const { return count == 0 ? 0.0 : total_ms / static_cast<double>(count); }
    };

    bool parse_uint16(const char *value, uint16_t &out)
    {
        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 1 || parsed > 65535)
        {
            return false;
        }
        out = static_cast<uint16_t>(parsed);
        return true;
    }

    double average_ms(double total_ms, uint64_t count)
    {
        return count == 0 ? 0.0 : total_ms / static_cast<double>(count);
    }

    bool parse_uint32(const char *value, uint32_t &out)
    {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed == 0 || parsed > 100000)
        {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    }

    bool parse_double(const char *value, double &out)
    {
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end == value || *end != '\0' || parsed <= 0.0 || parsed > 240.0)
        {
            return false;
        }
        out = parsed;
        return true;
    }

    std::vector<std::string> split(const std::string &value, char delimiter)
    {
        std::vector<std::string> parts;
        std::stringstream input(value);
        for (std::string part; std::getline(input, part, delimiter);)
        {
            parts.push_back(part);
        }
        return parts;
    }

    std::optional<StreamSpec> parse_stream_spec(const std::string &value)
    {
        const auto parts = split(value, ':');
        if (parts.size() < 4 || parts.size() > 5)
        {
            return std::nullopt;
        }

        StreamSpec spec;
        spec.stream_id = parts[0];
        if (spec.stream_id.empty())
        {
            return std::nullopt;
        }
        if (!parse_uint32(parts[1].c_str(), spec.width) || !parse_uint32(parts[2].c_str(), spec.height) ||
            !parse_double(parts[3].c_str(), spec.fps))
        {
            return std::nullopt;
        }
        spec.label = parts.size() == 5 ? parts[4] : ("Synthetic demo stream " + spec.stream_id);
        return spec;
    }

    std::vector<StreamSpec> default_demo_streams()
    {
        return {
            StreamSpec{"alpha", "Synthetic demo alpha sweep grayscale", 640, 360, 30.0, video_server::VideoPixelFormat::GRAY8},
            StreamSpec{"bravo", "Synthetic demo bravo orbit", 1280, 720, 30.0, video_server::VideoPixelFormat::RGB24},
            StreamSpec{"charlie", "Synthetic demo charlie checker", 320, 240, 30.0, video_server::VideoPixelFormat::RGB24},
        };
    }

    void print_usage(const char *argv0)
    {
        spdlog::info(
            "Usage: {} [--config PATH] [--host HOST] [--port PORT] [--enable-debug-api] [--lan-only] [--shared-key TOKEN] [--ice-server URI] [--ice-username USER] [--ice-credential SECRET] [--ice-transport-policy all|relay] [--ice-bind-address ADDR] [--ice-port-range BEGIN:END] [--stream-id ID --width W --height H --fps FPS] [--stream ID:W:H:FPS[:LABEL]] [--multi-stream-demo]",
            argv0);
    }

    std::optional<Options> parse_args(int argc, char **argv)
    {
        Options options;
        std::optional<std::string> legacy_stream_id;
        std::optional<uint32_t> legacy_width;
        std::optional<uint32_t> legacy_height;
        std::optional<double> legacy_fps;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto require_value = [&](const char *name) -> const char *
            {
                if (i + 1 >= argc)
                {
                    spdlog::error("Missing value for {}", name);
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--config")
            {
                const char *value = require_value("--config");
                if (!value)
                    return std::nullopt;
                options.config_path = value;
            }
            else if (arg == "--host")
            {
                const char *value = require_value("--host");
                if (!value)
                    return std::nullopt;
                options.host = value;
            }
            else if (arg == "--port")
            {
                const char *value = require_value("--port");
                if (!value || !parse_uint16(value, options.port))
                    return std::nullopt;
            }
            else if (arg == "--enable-debug-api")
            {
                options.enable_debug_api = true;
            }
            else if (arg == "--lan-only")
            {
                options.lan_only = true;
            }
            else if (arg == "--shared-key")
            {
                const char *value = require_value("--shared-key");
                if (!value)
                    return std::nullopt;
                options.shared_key = value;
            }
            else if (arg == "--ice-server")
            {
                const char *value = require_value("--ice-server");
                if (!value)
                    return std::nullopt;
                options.ice_servers.push_back(value);
            }
            else if (arg == "--ice-username")
            {
                const char *value = require_value("--ice-username");
                if (!value)
                    return std::nullopt;
                options.ice_username = value;
            }
            else if (arg == "--ice-credential")
            {
                const char *value = require_value("--ice-credential");
                if (!value)
                    return std::nullopt;
                options.ice_credential = value;
            }
            else if (arg == "--ice-transport-policy")
            {
                const char *value = require_value("--ice-transport-policy");
                if (!value || (std::string(value) != "all" && std::string(value) != "relay"))
                    return std::nullopt;
                options.ice_transport_policy = value;
            }
            else if (arg == "--ice-bind-address")
            {
                const char *value = require_value("--ice-bind-address");
                if (!value)
                    return std::nullopt;
                options.ice_bind_address = value;
            }
            else if (arg == "--ice-port-range")
            {
                const char *value = require_value("--ice-port-range");
                if (!value)
                    return std::nullopt;
                const auto parts = split(value, ':');
                uint16_t begin = 0;
                uint16_t end = 0;
                if (parts.size() != 2 || !parse_uint16(parts[0].c_str(), begin) ||
                    !parse_uint16(parts[1].c_str(), end) || begin > end)
                    return std::nullopt;
                options.ice_port_range_begin = begin;
                options.ice_port_range_end = end;
            }
            else if (arg == "--stream-id")
            {
                const char *value = require_value("--stream-id");
                if (!value)
                    return std::nullopt;
                legacy_stream_id = value;
            }
            else if (arg == "--width")
            {
                const char *value = require_value("--width");
                uint32_t parsed = 0;
                if (!value || !parse_uint32(value, parsed))
                    return std::nullopt;
                legacy_width = parsed;
            }
            else if (arg == "--height")
            {
                const char *value = require_value("--height");
                uint32_t parsed = 0;
                if (!value || !parse_uint32(value, parsed))
                    return std::nullopt;
                legacy_height = parsed;
            }
            else if (arg == "--fps")
            {
                const char *value = require_value("--fps");
                double parsed = 0.0;
                if (!value || !parse_double(value, parsed))
                    return std::nullopt;
                legacy_fps = parsed;
            }
            else if (arg == "--stream")
            {
                const char *value = require_value("--stream");
                if (!value)
                    return std::nullopt;
                auto parsed = parse_stream_spec(value);
                if (!parsed.has_value())
                {
                    spdlog::error("Invalid --stream value: {}", value);
                    return std::nullopt;
                }
                options.streams.push_back(*parsed);
            }
            else if (arg == "--multi-stream-demo")
            {
                options.streams = default_demo_streams();
            }
            else if (arg == "--duration-seconds")
            {
                const char *value = require_value("--duration-seconds");
                if (!value || !parse_double(value, options.duration_seconds))
                    return std::nullopt;
            }
            else if (arg == "--stats-interval-seconds")
            {
                const char *value = require_value("--stats-interval-seconds");
                if (!value || !parse_double(value, options.stats_interval_seconds))
                    return std::nullopt;
            }
            else if (arg == "--print-observability-summary")
            {
                options.print_observability_summary = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            else
            {
                spdlog::error("Unknown argument: {}", arg);
                return std::nullopt;
            }
        }

        if (options.streams.empty() && (legacy_stream_id.has_value() || legacy_width.has_value() || legacy_height.has_value() ||
                                        legacy_fps.has_value()))
        {
            options.streams.push_back(StreamSpec{legacy_stream_id.value_or("synthetic-h264"),
                                                 "NiceGUI smoke synthetic H264 stream",
                                                 legacy_width.value_or(640),
                                                 legacy_height.value_or(360),
                                                 legacy_fps.value_or(30.0),
                                                 video_server::VideoPixelFormat::GRAY8});
        }

        return options;
    }

    std::vector<video_server::StreamConfig> to_stream_configs(const std::vector<StreamSpec> &streams)
    {
        std::vector<video_server::StreamConfig> configs;
        configs.reserve(streams.size());
        for (const auto &stream : streams)
        {
            video_server::StreamConfig config{
                stream.stream_id, stream.label, stream.width, stream.height, stream.fps, stream.pixel_format};
            config.max_subscribers = stream.max_subscribers;
            configs.push_back(std::move(config));
        }
        return configs;
    }

} // namespace

int main(int argc, char **argv)
{
    video_server::ensure_default_logging_config();
    const auto options = parse_args(argc, argv);
    if (!options.has_value())
    {
        print_usage(argv[0]);
        return 1;
    }

    video_server::ManagedVideoServerConfig config;
    try
    {
        config = video_server::load_managed_video_server_config(options->config_path);
    }
    catch (const std::exception &ex)
    {
        spdlog::error("Failed to load managed server config '{}': {}", options->config_path, ex.what());
        return 1;
    }

    config.execution_mode = video_server::ExecutionMode::ManualStep;
    config.api_execution.mode = video_server::ManagedApiExecutionMode::ManagedThread;
    if (!options->host.empty())
    {
        config.webrtc.http_host = options->host;
    }
    if (options->port != 0)
    {
        config.webrtc.http_port = options->port;
    }
    if (options->enable_debug_api)
    {
        config.webrtc.enable_debug_api = true;
    }
    if (options->lan_only)
    {
        config.webrtc.allow_unsafe_public_routes = true;
        config.webrtc.cors_allowed_origins = {"*"};
    }
    if (!options->shared_key.empty())
    {
        config.webrtc.enable_shared_key_auth = true;
        config.webrtc.shared_key = options->shared_key;
    }
    if (!options->ice_servers.empty())
    {
        config.webrtc.ice.servers.clear();
        for (const auto &uri : options->ice_servers)
        {
            config.webrtc.ice.servers.push_back(
                video_server::WebRtcIceServerConfig{uri, options->ice_username, options->ice_credential});
        }
    }
    if (!options->ice_transport_policy.empty())
    {
        config.webrtc.ice.transport_policy = options->ice_transport_policy;
    }
    if (!options->ice_bind_address.empty())
    {
        config.webrtc.ice.bind_address = options->ice_bind_address;
    }
    if (options->ice_port_range_begin.has_value())
    {
        config.webrtc.ice.port_range_begin = *options->ice_port_range_begin;
    }
    if (options->ice_port_range_end.has_value())
    {
        config.webrtc.ice.port_range_end = *options->ice_port_range_end;
    }
    if (!options->streams.empty())
    {
        config.streams = to_stream_configs(options->streams);
    }

    auto server = video_server::CreateManagedVideoServer(config);
    if (!server->start())
    {
        spdlog::error("Failed to start managed WebRTC video server");
        return 1;
    }

    const auto run_started_at = Clock::now();
    std::vector<RunningStream> streams;
    streams.reserve(config.streams.size());
    for (const auto &stream_config : config.streams)
    {
        streams.emplace_back(stream_config, run_started_at);
        spdlog::info("[smoke-server] stream={} label='{}' {}x{} @ {} fps input={} max_subscribers={}",
                     stream_config.stream_id, stream_config.label, stream_config.width, stream_config.height,
                     stream_config.nominal_fps, video_server::to_string(stream_config.input_pixel_format),
                     stream_config.max_subscribers);
    }

    spdlog::info("[smoke-server] started managed HTTP/WebRTC server on http://{}:{} with {} stream(s) config={}",
                 config.webrtc.http_host, config.webrtc.http_port, streams.size(), options->config_path);

    std::atomic<bool> stop_requested{false};
    std::vector<std::string> managed_stream_ids;
    managed_stream_ids.reserve(config.streams.size());
    for (const auto &stream_config : config.streams)
    {
        managed_stream_ids.push_back(stream_config.stream_id);
    }
    auto stream_group = server->register_streams(std::move(managed_stream_ids));
    if (!stream_group)
    {
        spdlog::error("Failed to register managed stream group");
        server->stop();
        return 1;
    }

    std::thread stdin_thread;
    if (options->duration_seconds <= 0.0)
    {
        stdin_thread = std::thread([&stop_requested]()
                                   {
                                       spdlog::info("[smoke-server] press ENTER to stop");
                                       std::string line;
                                       std::getline(std::cin, line);
                                       stop_requested = true;
                                   });
    }

    auto next_stats_at = run_started_at + std::chrono::duration<double>(options->stats_interval_seconds);
    const auto stop_at = options->duration_seconds > 0.0
                             ? run_started_at + std::chrono::duration<double>(options->duration_seconds)
                             : Clock::time_point::max();
    StepStats stream_step_stats;
    const auto stream_step_interval = streams.empty() ? Clock::duration{std::chrono::milliseconds(1)}
                                                      : streams.front().frame_interval;

    while (!stop_requested.load() && Clock::now() < stop_at)
    {
        const auto now = Clock::now();
        bool pushed_frame = false;
        auto next_frame_at = Clock::time_point::max();
        for (auto &stream : streams)
        {
            next_frame_at = std::min(next_frame_at, stream.next_frame_at);
            while (stream.next_frame_at <= now)
            {
                const auto raw_frame = stream.generator.next_frame();
                if (!server->push_frame(stream.config.stream_id, raw_frame))
                {
                    spdlog::warn("Failed to push synthetic frame for {}", stream.config.stream_id);
                }
                pushed_frame = true;
                stream.next_frame_at += stream.frame_interval;
                next_frame_at = std::min(next_frame_at, stream.next_frame_at);
            }
        }

        if (pushed_frame)
        {
            const auto step_started_at = Clock::now();
            const bool step_ok = stream_group->step_streams();
            const auto step_finished_at = Clock::now();
            const auto step_elapsed = step_finished_at - step_started_at;
            stream_step_stats.record(std::chrono::duration<double, std::milli>(step_elapsed).count(),
                                     step_ok,
                                     step_elapsed > stream_step_interval);
            if (!step_ok)
            {
                spdlog::error("Managed stream-group step failed");
                stop_requested = true;
                break;
            }
        }
        else if (next_frame_at != Clock::time_point::max())
        {
            const auto sleep_for = std::min(std::chrono::milliseconds(1),
                                            std::chrono::duration_cast<std::chrono::milliseconds>(next_frame_at - now));
            if (sleep_for.count() > 0)
            {
                std::this_thread::sleep_for(sleep_for);
            }
            else
            {
                std::this_thread::yield();
            }
        }

        if (options->print_observability_summary && Clock::now() >= next_stats_at)
        {
            const auto snapshot = server->get_debug_snapshot();
            spdlog::info("[smoke-server] observability summary streams={} active_sessions={}", snapshot.stream_count,
                         snapshot.active_session_count);
            spdlog::info(
                "[smoke-server] stream-step latency count={} failed={} deadline_misses={} mean_ms={:.3f} p50_ms={:.3f} p95_ms={:.3f} p99_ms={:.3f} max_ms={:.3f}",
                stream_step_stats.count, stream_step_stats.failed, stream_step_stats.deadline_misses,
                stream_step_stats.mean_ms(), stream_step_stats.percentile(50.0), stream_step_stats.percentile(95.0),
                stream_step_stats.percentile(99.0), stream_step_stats.max_ms);
            for (const auto &stream : snapshot.streams)
            {
                const auto &session = stream.current_session;
                spdlog::info(
                    "[smoke-server] stream={} raw={} encoded={} au={} sender={} packets={} startup_injections={} track_closed={}",
                    stream.stream_id, stream.latest_raw_frame_available, stream.latest_encoded_access_unit_available,
                    stream.total_access_units_received, session ? session->sender_state : std::string("no-session"),
                    session ? session->counters.packets_sent_after_track_open : 0,
                    session ? session->counters.startup_sequence_injections : 0,
                    session ? session->counters.track_closed_events : 0);
                spdlog::info(
                    "[smoke-server] stream={} phase-ms steps={} total_avg={:.3f} total_last={:.3f} total_max={:.3f} transform_avg={:.3f} transform_last={:.3f} transform_max={:.3f} setup_avg={:.3f} setup_last={:.3f} setup_max={:.3f} encode_avg={:.3f} encode_last={:.3f} encode_max={:.3f} publish_avg={:.3f} publish_last={:.3f} publish_max={:.3f}",
                    stream.stream_id,
                    stream.stream_step_count,
                    average_ms(stream.stream_step_total_ms, stream.stream_step_count),
                    stream.stream_step_last_ms,
                    stream.stream_step_max_ms,
                    average_ms(stream.stream_step_transform_total_ms, stream.stream_step_count),
                    stream.stream_step_transform_last_ms,
                    stream.stream_step_transform_max_ms,
                    average_ms(stream.stream_step_pipeline_setup_total_ms, stream.stream_step_count),
                    stream.stream_step_pipeline_setup_last_ms,
                    stream.stream_step_pipeline_setup_max_ms,
                    average_ms(stream.stream_step_encode_total_ms, stream.stream_step_count),
                    stream.stream_step_encode_last_ms,
                    stream.stream_step_encode_max_ms,
                    average_ms(stream.stream_step_publish_total_ms, stream.stream_step_count),
                    stream.stream_step_publish_last_ms,
                    stream.stream_step_publish_max_ms);
            }
            next_stats_at = Clock::now() + std::chrono::duration<double>(options->stats_interval_seconds);
        }
    }

    stop_requested = true;
    if (stdin_thread.joinable())
    {
        stdin_thread.detach();
    }

    spdlog::info(
        "[smoke-server] final stream-step latency count={} failed={} deadline_misses={} mean_ms={:.3f} p50_ms={:.3f} p95_ms={:.3f} p99_ms={:.3f} max_ms={:.3f}",
        stream_step_stats.count, stream_step_stats.failed, stream_step_stats.deadline_misses,
        stream_step_stats.mean_ms(), stream_step_stats.percentile(50.0), stream_step_stats.percentile(95.0),
        stream_step_stats.percentile(99.0), stream_step_stats.max_ms);
    {
        const auto snapshot = server->get_debug_snapshot();
        for (const auto &stream : snapshot.streams)
        {
            spdlog::info(
                "[smoke-server] final stream={} phase-ms steps={} total_avg={:.3f} total_last={:.3f} total_max={:.3f} transform_avg={:.3f} transform_last={:.3f} transform_max={:.3f} setup_avg={:.3f} setup_last={:.3f} setup_max={:.3f} encode_avg={:.3f} encode_last={:.3f} encode_max={:.3f} publish_avg={:.3f} publish_last={:.3f} publish_max={:.3f}",
                stream.stream_id,
                stream.stream_step_count,
                average_ms(stream.stream_step_total_ms, stream.stream_step_count),
                stream.stream_step_last_ms,
                stream.stream_step_max_ms,
                average_ms(stream.stream_step_transform_total_ms, stream.stream_step_count),
                stream.stream_step_transform_last_ms,
                stream.stream_step_transform_max_ms,
                average_ms(stream.stream_step_pipeline_setup_total_ms, stream.stream_step_count),
                stream.stream_step_pipeline_setup_last_ms,
                stream.stream_step_pipeline_setup_max_ms,
                average_ms(stream.stream_step_encode_total_ms, stream.stream_step_count),
                stream.stream_step_encode_last_ms,
                stream.stream_step_encode_max_ms,
                average_ms(stream.stream_step_publish_total_ms, stream.stream_step_count),
                stream.stream_step_publish_last_ms,
                stream.stream_step_publish_max_ms);
        }
    }
    server->stop();
    spdlog::info("[smoke-server] stopped");
    return 0;
}
