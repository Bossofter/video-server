#include "video_server/managed_video_server.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include "../transforms/display_transform.h"

namespace video_server
{
    namespace
    {
        const char *to_string(ExecutionMode mode)
        {
            switch (mode)
            {
            case ExecutionMode::ManualStep:
                return "manual_step";
            case ExecutionMode::InlineOnPush:
                return "inline_on_push";
            case ExecutionMode::WorkerThread:
                return "worker_thread";
            }
            return "manual_step";
        }

        std::optional<ExecutionMode> parse_execution_mode(const std::string &value)
        {
            if (value == "manual_step")
            {
                return ExecutionMode::ManualStep;
            }
            if (value == "inline_on_push")
            {
                return ExecutionMode::InlineOnPush;
            }
            if (value == "worker_thread")
            {
                return ExecutionMode::WorkerThread;
            }
            return std::nullopt;
        }

        std::optional<VideoPixelFormat> parse_pixel_format(const std::string &value)
        {
            if (value == "RGB24")
            {
                return VideoPixelFormat::RGB24;
            }
            if (value == "BGR24")
            {
                return VideoPixelFormat::BGR24;
            }
            if (value == "RGBA32")
            {
                return VideoPixelFormat::RGBA32;
            }
            if (value == "BGRA32")
            {
                return VideoPixelFormat::BGRA32;
            }
            if (value == "GRAY8")
            {
                return VideoPixelFormat::GRAY8;
            }
            if (value == "NV12")
            {
                return VideoPixelFormat::NV12;
            }
            if (value == "I420")
            {
                return VideoPixelFormat::I420;
            }
            return std::nullopt;
        }

        std::optional<RawPipelineScaleMode> parse_scale_mode(const std::string &value)
        {
            if (value == "passthrough")
            {
                return RawPipelineScaleMode::Passthrough;
            }
            if (value == "resize")
            {
                return RawPipelineScaleMode::Resize;
            }
            return std::nullopt;
        }

        std::optional<RawH264Encoder> parse_encoder(const std::string &value)
        {
            if (value == "automatic")
            {
                return RawH264Encoder::Automatic;
            }
            if (value == "libx264")
            {
                return RawH264Encoder::LibX264;
            }
            if (value == "libopenh264")
            {
                return RawH264Encoder::LibOpenH264;
            }
            return std::nullopt;
        }

        template <typename T>
        std::optional<T> table_value(const toml::table &table, const char *key)
        {
            if (const auto *node = table.get(key))
            {
                return node->value<T>();
            }
            return std::nullopt;
        }

        StreamConfig parse_stream_config(const toml::table &table)
        {
            StreamConfig stream;
            stream.stream_id = table["stream_id"].value_or("");
            stream.label = table["label"].value_or(stream.stream_id);
            stream.width = table["width"].value_or(0u);
            stream.height = table["height"].value_or(0u);
            stream.nominal_fps = table["nominal_fps"].value_or(0.0);
            const auto pixel_format_value = table["input_pixel_format"].value_or(std::string("RGB24"));
            const auto pixel_format = parse_pixel_format(pixel_format_value);
            if (!pixel_format.has_value())
            {
                throw std::runtime_error("invalid stream input_pixel_format: " + pixel_format_value);
            }
            stream.input_pixel_format = *pixel_format;
            return stream;
        }

        RawVideoPipelineConfig parse_pipeline_config(const toml::table &table)
        {
            RawVideoPipelineConfig config;
            if (const auto value = table_value<uint32_t>(table, "input_width"))
            {
                config.input_width = *value;
            }
            if (const auto value = table_value<uint32_t>(table, "input_height"))
            {
                config.input_height = *value;
            }
            if (const auto value = table_value<double>(table, "input_fps"))
            {
                config.input_fps = *value;
            }
            if (const auto value = table_value<std::string>(table, "input_pixel_format"))
            {
                const auto pixel_format = parse_pixel_format(*value);
                if (!pixel_format.has_value())
                {
                    throw std::runtime_error("invalid pipeline input_pixel_format: " + *value);
                }
                config.input_pixel_format = *pixel_format;
            }
            if (const auto value = table_value<std::string>(table, "scale_mode"))
            {
                const auto scale_mode = parse_scale_mode(*value);
                if (!scale_mode.has_value())
                {
                    throw std::runtime_error("invalid pipeline scale_mode: " + *value);
                }
                config.scale_mode = *scale_mode;
            }
            if (const auto value = table_value<uint32_t>(table, "output_width"))
            {
                config.output_width = *value;
            }
            if (const auto value = table_value<uint32_t>(table, "output_height"))
            {
                config.output_height = *value;
            }
            if (const auto value = table_value<double>(table, "output_fps"))
            {
                config.output_fps = *value;
            }
            if (const auto value = table_value<std::string>(table, "encoder"))
            {
                const auto encoder = parse_encoder(*value);
                if (!encoder.has_value())
                {
                    throw std::runtime_error("invalid pipeline encoder: " + *value);
                }
                config.encoder = *encoder;
            }
            config.encoder_preset = table["encoder_preset"].value_or(config.encoder_preset);
            config.encoder_tune = table["encoder_tune"].value_or(config.encoder_tune);
            config.encoder_profile = table["encoder_profile"].value_or(config.encoder_profile);
            config.repeat_headers = table["repeat_headers"].value_or(config.repeat_headers);
            config.emit_access_unit_delimiters =
                table["emit_access_unit_delimiters"].value_or(config.emit_access_unit_delimiters);
            return config;
        }

        size_t frame_storage_size(const VideoFrameView &frame)
        {
            switch (frame.pixel_format)
            {
            case VideoPixelFormat::RGB24:
            case VideoPixelFormat::BGR24:
                return static_cast<size_t>(frame.stride_bytes) * static_cast<size_t>(frame.height);
            case VideoPixelFormat::RGBA32:
            case VideoPixelFormat::BGRA32:
                return static_cast<size_t>(frame.stride_bytes) * static_cast<size_t>(frame.height);
            case VideoPixelFormat::GRAY8:
                return static_cast<size_t>(frame.stride_bytes) * static_cast<size_t>(frame.height);
            case VideoPixelFormat::NV12:
            case VideoPixelFormat::I420:
                return static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3u / 2u;
            }
            return 0;
        }

        class ManagedVideoServer final : public IManagedVideoServer
        {
        public:
            explicit ManagedVideoServer(ManagedVideoServerConfig config)
                : config_(std::move(config)),
                  server_([this]()
                          {
                              WebRtcVideoServerConfig server_config = config_.webrtc;
                              server_config.execution_mode = ExecutionMode::ManualStep;
                              server_config.http_poll_timeout_ms = config_.http_poll_timeout_ms;
                              server_config.max_http_connections_per_step = 1;
                              return server_config;
                          }())
            {
                for (const auto &stream : config_.streams)
                {
                    add_stream_state_locked(stream);
                }
            }

            ~ManagedVideoServer() override { stop(); }

            bool start() override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (started_)
                {
                    return false;
                }

                for (auto &[stream_id, state] : streams_)
                {
                    if (!state.registered_with_server && !server_.register_stream(state.stream_config))
                    {
                        spdlog::error("failed to register managed stream {}", stream_id);
                        return false;
                    }
                    state.registered_with_server = true;
                }

                if (!server_.start())
                {
                    return false;
                }

                stop_requested_ = false;
                started_ = true;
                if (config_.execution_mode == ExecutionMode::WorkerThread)
                {
                    worker_thread_ = std::thread([this]()
                                                 { this->worker_loop(); });
                }
                return true;
            }

            void stop() override
            {
                stop_requested_ = true;
                if (worker_thread_.joinable())
                {
                    worker_thread_.join();
                }

                std::lock_guard<std::mutex> lock(mutex_);
                for (auto &[_, state] : streams_)
                {
                    if (state.pipeline)
                    {
                        state.pipeline->stop();
                        state.pipeline.reset();
                    }
                }
                server_.stop();
                started_ = false;
            }

            bool step() override
            {
                if (!server_.step())
                {
                    return false;
                }

                std::vector<std::string> stream_ids;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stream_ids.reserve(streams_.size());
                    for (const auto &[stream_id, _] : streams_)
                    {
                        stream_ids.push_back(stream_id);
                    }
                }

                size_t processed = 0;
                for (const auto &stream_id : stream_ids)
                {
                    if (config_.max_streams_per_step > 0 && processed >= config_.max_streams_per_step)
                    {
                        break;
                    }
                    if (!process_stream(stream_id))
                    {
                        return false;
                    }
                    ++processed;
                }
                return true;
            }

            ServerDebugSnapshot get_debug_snapshot() const override { return server_.get_debug_snapshot(); }

            bool register_stream(const StreamConfig &config) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (streams_.find(config.stream_id) != streams_.end())
                {
                    return false;
                }

                add_stream_state_locked(config);
                auto &state = streams_.at(config.stream_id);
                if (started_)
                {
                    if (!server_.register_stream(config))
                    {
                        streams_.erase(config.stream_id);
                        return false;
                    }
                    state.registered_with_server = true;
                }
                return true;
            }

            bool remove_stream(const std::string &stream_id) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = streams_.find(stream_id);
                if (it == streams_.end())
                {
                    return false;
                }
                if (it->second.pipeline)
                {
                    it->second.pipeline->stop();
                    it->second.pipeline.reset();
                }
                if (it->second.registered_with_server)
                {
                    server_.remove_stream(stream_id);
                }
                streams_.erase(it);
                return true;
            }

            bool push_frame(const std::string &stream_id, const VideoFrameView &frame) override
            {
                StreamConfig stream_config;
                StreamOutputConfig output_config;
                if (!server_.validate_raw_frame_input(stream_id, frame, &stream_config, &output_config))
                {
                    server_.note_frame_dropped(stream_id);
                    return false;
                }

                const size_t frame_bytes = frame_storage_size(frame);
                if (frame_bytes == 0)
                {
                    server_.note_frame_dropped(stream_id);
                    return false;
                }

                PendingFrame pending;
                pending.bytes.resize(frame_bytes);
                std::memcpy(pending.bytes.data(), frame.data, frame_bytes);
                pending.width = frame.width;
                pending.height = frame.height;
                pending.stride_bytes = frame.stride_bytes;
                pending.pixel_format = frame.pixel_format;
                pending.timestamp_ns = frame.timestamp_ns;
                pending.frame_id = frame.frame_id;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = streams_.find(stream_id);
                    if (it == streams_.end())
                    {
                        server_.note_frame_dropped(stream_id);
                        return false;
                    }
                    if (it->second.pending_frame.has_value())
                    {
                        server_.note_frame_dropped(stream_id);
                    }
                    it->second.pending_frame = std::move(pending);
                }

                server_.note_frame_received(stream_id, frame.timestamp_ns);

                if (config_.execution_mode == ExecutionMode::InlineOnPush)
                {
                    if (!server_.step())
                    {
                        return false;
                    }
                    return process_stream(stream_id);
                }
                return true;
            }

            bool push_access_unit(const std::string &stream_id, const EncodedAccessUnitView &access_unit) override
            {
                return server_.push_access_unit(stream_id, access_unit);
            }

            std::vector<VideoStreamInfo> list_streams() const override { return server_.list_streams(); }

            std::optional<VideoStreamInfo> get_stream_info(const std::string &stream_id) const override
            {
                return server_.get_stream_info(stream_id);
            }

            bool set_stream_output_config(const std::string &stream_id,
                                          const StreamOutputConfig &output_config) override
            {
                return server_.set_stream_output_config(stream_id, output_config);
            }

            std::optional<StreamOutputConfig> get_stream_output_config(const std::string &stream_id) const override
            {
                return server_.get_stream_output_config(stream_id);
            }

        private:
            struct PendingFrame
            {
                std::vector<uint8_t> bytes;
                uint32_t width{0};
                uint32_t height{0};
                uint32_t stride_bytes{0};
                VideoPixelFormat pixel_format{VideoPixelFormat::RGB24};
                uint64_t timestamp_ns{0};
                uint64_t frame_id{0};

                VideoFrameView view() const
                {
                    return VideoFrameView{bytes.data(), width, height, stride_bytes, pixel_format, timestamp_ns, frame_id};
                }
            };

            struct ManagedStreamState
            {
                StreamConfig stream_config;
                RawVideoPipelineConfig pipeline_template;
                std::optional<PendingFrame> pending_frame;
                std::unique_ptr<IRawVideoPipeline> pipeline;
                uint64_t next_due_timestamp_ns{0};
                uint64_t pipeline_config_generation{0};
                uint32_t pipeline_width{0};
                uint32_t pipeline_height{0};
                bool registered_with_server{false};
            };

            void add_stream_state_locked(const StreamConfig &config)
            {
                ManagedStreamState state;
                state.stream_config = config;
                const auto it = config_.default_raw_pipelines.find(config.stream_id);
                if (it != config_.default_raw_pipelines.end())
                {
                    state.pipeline_template = it->second;
                }
                else if (const auto default_it = config_.default_raw_pipelines.find("default");
                         default_it != config_.default_raw_pipelines.end())
                {
                    state.pipeline_template = default_it->second;
                }
                streams_.emplace(config.stream_id, std::move(state));
            }

            RawVideoPipelineConfig make_effective_pipeline_config(const ManagedStreamState &state,
                                                                  const StreamOutputConfig &output_config,
                                                                  uint32_t width,
                                                                  uint32_t height) const
            {
                RawVideoPipelineConfig config = state.pipeline_template;
                config.input_width = width;
                config.input_height = height;
                config.input_pixel_format = VideoPixelFormat::RGB24;
                config.input_fps = output_config.output_fps > 0.0 ? output_config.output_fps : state.stream_config.nominal_fps;
                config.scale_mode = RawPipelineScaleMode::Passthrough;
                config.output_width.reset();
                config.output_height.reset();
                config.output_fps.reset();
                return config;
            }

            bool ensure_pipeline(const std::string &stream_id,
                                 ManagedStreamState &state,
                                 const StreamOutputConfig &output_config,
                                 uint32_t width,
                                 uint32_t height)
            {
                if (state.pipeline && state.pipeline_config_generation == output_config.config_generation &&
                    state.pipeline_width == width && state.pipeline_height == height)
                {
                    return true;
                }

                if (state.pipeline)
                {
                    state.pipeline->stop();
                    state.pipeline.reset();
                }

                RawVideoPipelineConfig pipeline_config = make_effective_pipeline_config(state, output_config, width, height);
                auto pipeline = make_raw_to_h264_pipeline(
                    stream_id, pipeline_config,
                    [this, stream_id](const EncodedAccessUnitView &access_unit)
                    { return server_.push_access_unit(stream_id, access_unit); });

                std::string error;
                if (!pipeline->start(&error))
                {
                    spdlog::error("failed to start managed pipeline for {}: {}", stream_id, error);
                    return false;
                }

                state.pipeline = std::move(pipeline);
                state.pipeline_config_generation = output_config.config_generation;
                state.pipeline_width = width;
                state.pipeline_height = height;
                return true;
            }

            bool process_stream(const std::string &stream_id)
            {
                StreamOutputConfig output_config;
                PendingFrame pending;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = streams_.find(stream_id);
                    if (it == streams_.end() || !it->second.pending_frame.has_value())
                    {
                        return true;
                    }

                    const auto output_config_opt = server_.get_stream_output_config(stream_id);
                    if (!output_config_opt.has_value())
                    {
                        return false;
                    }
                    output_config = *output_config_opt;

                    if (output_config.output_fps > 0.0 && it->second.next_due_timestamp_ns > 0 &&
                        it->second.pending_frame->timestamp_ns < it->second.next_due_timestamp_ns)
                    {
                        return true;
                    }

                    pending = std::move(*it->second.pending_frame);
                    it->second.pending_frame.reset();
                    if (output_config.output_fps > 0.0)
                    {
                        const uint64_t frame_interval_ns =
                            static_cast<uint64_t>(std::llround(1000000000.0 / output_config.output_fps));
                        it->second.next_due_timestamp_ns = pending.timestamp_ns + frame_interval_ns;
                    }
                    else
                    {
                        it->second.next_due_timestamp_ns = 0;
                    }
                }

                RgbImage transformed;
                if (!apply_display_transform(pending.view(), output_config, transformed))
                {
                    server_.note_frame_dropped(stream_id);
                    return true;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = streams_.find(stream_id);
                    if (it == streams_.end())
                    {
                        return false;
                    }
                    if (!ensure_pipeline(stream_id, it->second, output_config, transformed.width, transformed.height))
                    {
                        return false;
                    }

                    const VideoFrameView transformed_view{
                        transformed.rgb.data(),
                        transformed.width,
                        transformed.height,
                        transformed.width * 3u,
                        VideoPixelFormat::RGB24,
                        pending.timestamp_ns,
                        pending.frame_id};
                    std::string error;
                    if (!it->second.pipeline->push_frame(transformed_view, &error))
                    {
                        spdlog::error("managed pipeline encode failed for {}: {}", stream_id, error);
                        return false;
                    }
                }

                return server_.publish_transformed_frame(stream_id, transformed.rgb, transformed.width, transformed.height,
                                                         pending.timestamp_ns, pending.frame_id);
            }

            void worker_loop()
            {
                while (!stop_requested_.load())
                {
                    if (!step())
                    {
                        break;
                    }
                }
            }

            ManagedVideoServerConfig config_;
            WebRtcVideoServer server_;
            mutable std::mutex mutex_;
            std::unordered_map<std::string, ManagedStreamState> streams_;
            std::atomic<bool> stop_requested_{false};
            std::thread worker_thread_;
            bool started_{false};
        };

    } // namespace

    ManagedVideoServerConfig load_managed_video_server_config(const std::string &config_path)
    {
        const toml::table table = toml::parse_file(config_path);
        ManagedVideoServerConfig config;

        if (const auto execution_mode = table_value<std::string>(table, "execution_mode"))
        {
            const auto parsed = parse_execution_mode(*execution_mode);
            if (!parsed.has_value())
            {
                throw std::runtime_error("invalid execution_mode: " + *execution_mode);
            }
            config.execution_mode = *parsed;
        }
        config.http_poll_timeout_ms = table["http_poll_timeout_ms"].value_or(config.http_poll_timeout_ms);
        config.max_streams_per_step = table["max_streams_per_step"].value_or(config.max_streams_per_step);

        if (const auto *webrtc = table["webrtc"].as_table())
        {
            if (const auto value = table_value<std::string>(*webrtc, "http_host"))
            {
                config.webrtc.http_host = *value;
            }
            if (const auto value = table_value<uint16_t>(*webrtc, "http_port"))
            {
                config.webrtc.http_port = *value;
            }
            if (const auto value = table_value<bool>(*webrtc, "enable_http_api"))
            {
                config.webrtc.enable_http_api = *value;
            }
            if (const auto value = table_value<bool>(*webrtc, "enable_debug_api"))
            {
                config.webrtc.enable_debug_api = *value;
            }
            if (const auto value = table_value<bool>(*webrtc, "enable_runtime_config_api"))
            {
                config.webrtc.enable_runtime_config_api = *value;
            }
            if (const auto value = table_value<bool>(*webrtc, "allow_unsafe_public_routes"))
            {
                config.webrtc.allow_unsafe_public_routes = *value;
            }
            if (const auto value = table_value<bool>(*webrtc, "enable_shared_key_auth"))
            {
                config.webrtc.enable_shared_key_auth = *value;
            }
            if (const auto value = table_value<std::string>(*webrtc, "shared_key"))
            {
                config.webrtc.shared_key = *value;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_http_request_bytes"))
            {
                config.webrtc.max_http_request_bytes = *value;
            }
        }

        if (const auto *streams = table["streams"].as_array())
        {
            for (const auto &node : *streams)
            {
                const auto *stream = node.as_table();
                if (stream == nullptr)
                {
                    throw std::runtime_error("each [[streams]] entry must be a table");
                }
                config.streams.push_back(parse_stream_config(*stream));
            }
        }

        if (const auto *pipelines = table["default_raw_pipelines"].as_table())
        {
            for (const auto &[key, node] : *pipelines)
            {
                const auto *pipeline = node.as_table();
                if (pipeline == nullptr)
                {
                    throw std::runtime_error("default_raw_pipelines entries must be tables");
                }
                config.default_raw_pipelines.emplace(std::string(key.str()), parse_pipeline_config(*pipeline));
            }
        }

        return config;
    }

    std::unique_ptr<IManagedVideoServer> CreateManagedVideoServer(const std::string &config_path)
    {
        return CreateManagedVideoServer(load_managed_video_server_config(config_path));
    }

    std::unique_ptr<IManagedVideoServer> CreateManagedVideoServer(ManagedVideoServerConfig config)
    {
        return std::make_unique<ManagedVideoServer>(std::move(config));
    }

} // namespace video_server
