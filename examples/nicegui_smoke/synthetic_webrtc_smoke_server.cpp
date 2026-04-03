#include <atomic>
#include <chrono>
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
            "Usage: {} [--config PATH] [--host HOST] [--port PORT] [--enable-debug-api] [--lan-only] [--shared-key TOKEN] [--stream-id ID --width W --height H --fps FPS] [--stream ID:W:H:FPS[:LABEL]] [--multi-stream-demo]",
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

    while (!stop_requested.load() && Clock::now() < stop_at)
    {
        const auto now = Clock::now();
        for (auto &stream : streams)
        {
            while (stream.next_frame_at <= now)
            {
                const auto raw_frame = stream.generator.next_frame();
                if (!server->push_frame(stream.config.stream_id, raw_frame))
                {
                    spdlog::warn("Failed to push synthetic frame for {}", stream.config.stream_id);
                }
                stream.next_frame_at += stream.frame_interval;
            }
        }

        if (!server->step())
        {
            spdlog::error("Managed server step failed");
            stop_requested = true;
            break;
        }

        if (options->print_observability_summary && Clock::now() >= next_stats_at)
        {
            const auto snapshot = server->get_debug_snapshot();
            spdlog::info("[smoke-server] observability summary streams={} active_sessions={}", snapshot.stream_count,
                         snapshot.active_session_count);
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
            }
            next_stats_at = Clock::now() + std::chrono::duration<double>(options->stats_interval_seconds);
        }
    }

    stop_requested = true;
    if (stdin_thread.joinable())
    {
        stdin_thread.detach();
    }

    server->stop();
    spdlog::info("[smoke-server] stopped");
    return 0;
}
