#include "video_server/managed_video_server.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <string.h>
#endif

#include "../core/video_pixel_format_utils.h"
#include "../transforms/display_transform.h"
#include "encoded_access_unit_handoff.h"

namespace video_server
{
    namespace
    {
        using ManagedStreamStepTestHook = std::function<void(const std::string &)>;
        using ManagedApiStepTestHook = std::function<void()>;

        std::mutex &managed_test_hook_mutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        ManagedStreamStepTestHook &managed_stream_step_test_hook()
        {
            static ManagedStreamStepTestHook hook;
            return hook;
        }

        ManagedApiStepTestHook &managed_api_step_test_hook()
        {
            static ManagedApiStepTestHook hook;
            return hook;
        }

        ManagedApiStepTestHook &managed_shutdown_wait_test_hook()
        {
            static ManagedApiStepTestHook hook;
            return hook;
        }

        ManagedStreamStepTestHook current_managed_stream_step_test_hook()
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            return managed_stream_step_test_hook();
        }

        ManagedApiStepTestHook current_managed_api_step_test_hook()
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            return managed_api_step_test_hook();
        }

        ManagedApiStepTestHook current_managed_shutdown_wait_test_hook()
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            return managed_shutdown_wait_test_hook();
        }

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

        std::optional<ManagedApiExecutionMode> parse_api_execution_mode(const std::string &value)
        {
            if (value == "manual_step")
            {
                return ManagedApiExecutionMode::ManualStep;
            }
            if (value == "managed_thread")
            {
                return ManagedApiExecutionMode::ManagedThread;
            }
            return std::nullopt;
        }

        std::optional<VideoPixelFormat> parse_pixel_format(const std::string &value)
        {
            return video_pixel_format_from_string(value.c_str());
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
            if (value == "libaom-av1")
            {
                return RawH264Encoder::LibAomAv1;
            }
            if (value == "vaapi")
            {
                return RawH264Encoder::Vaapi;
            }
            if (value == "nvenc")
            {
                return RawH264Encoder::Nvenc;
            }
            return std::nullopt;
        }

        std::optional<VideoCodec> parse_codec(const std::string &value)
        {
            if (value == "h264" || value == "H264")
            {
                return VideoCodec::H264;
            }
            if (value == "av1" || value == "AV1")
            {
                return VideoCodec::AV1;
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
            if (!video_pixel_format_supports_display_transform(stream.input_pixel_format))
            {
                throw std::runtime_error("managed stream input_pixel_format is not supported by raw-frame ingest/display transform: " +
                                         std::string(to_string(stream.input_pixel_format)));
            }
            if (const auto *max_subscribers_node = table.get("max_subscribers"))
            {
                const auto max_subscribers = max_subscribers_node->value<uint32_t>();
                if (!max_subscribers.has_value())
                {
                    throw std::runtime_error("stream max_subscribers must be an integer");
                }
                if (*max_subscribers == 0)
                {
                    throw std::runtime_error("stream max_subscribers must be >= 1");
                }
                stream.max_subscribers = *max_subscribers;
            }
            return stream;
        }

        RawVideoPipelineConfig parse_pipeline_config(const toml::table &table)
        {
            RawVideoPipelineConfig config;
            if (const auto value = table_value<std::string>(table, "codec"))
            {
                const auto codec = parse_codec(*value);
                if (!codec.has_value())
                {
                    throw std::runtime_error("invalid pipeline codec: " + *value);
                }
                config.codec = *codec;
            }
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
            config.hardware_device = table["hardware_device"].value_or(config.hardware_device);
            if (const auto *encoder_crf_node = table.get("encoder_crf"))
            {
                const auto encoder_crf = encoder_crf_node->value<uint32_t>();
                if (!encoder_crf.has_value())
                {
                    throw std::runtime_error("pipeline encoder_crf must be an integer in the range 0..51");
                }
                if (*encoder_crf > 51)
                {
                    throw std::runtime_error("pipeline encoder_crf must be in the range 0..51");
                }
                config.encoder_crf = *encoder_crf;
            }
            config.repeat_headers = table["repeat_headers"].value_or(config.repeat_headers);
            config.emit_access_unit_delimiters =
                table["emit_access_unit_delimiters"].value_or(config.emit_access_unit_delimiters);
            return config;
        }

        std::vector<std::string> parse_string_array(const toml::table &table, const char *key)
        {
            std::vector<std::string> values;
            const auto *array = table.get_as<toml::array>(key);
            if (array == nullptr)
            {
                return values;
            }
            values.reserve(array->size());
            for (const auto &entry : *array)
            {
                const auto value = entry.value<std::string>();
                if (!value.has_value())
                {
                    throw std::runtime_error(std::string(key) + " entries must be strings");
                }
                values.push_back(*value);
            }
            return values;
        }

        std::vector<int> parse_int_array(const toml::table &table, const char *key)
        {
            std::vector<int> values;
            const auto *array = table.get_as<toml::array>(key);
            if (array == nullptr)
            {
                return values;
            }
            values.reserve(array->size());
            for (const auto &entry : *array)
            {
                const auto value = entry.value<int64_t>();
                if (!value.has_value())
                {
                    throw std::runtime_error(std::string(key) + " entries must be integers");
                }
                if (*value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max())
                {
                    throw std::runtime_error(std::string(key) + " entries are out of range");
                }
                values.push_back(static_cast<int>(*value));
            }
            return values;
        }

        void parse_ice_config(const toml::table &table, WebRtcIceConfig &ice_config)
        {
            if (const auto value = table_value<std::string>(table, "ice_transport_policy"))
            {
                if (*value != "all" && *value != "relay")
                {
                    throw std::runtime_error("webrtc.ice_transport_policy must be \"all\" or \"relay\"");
                }
                ice_config.transport_policy = *value;
            }
            if (const auto value = table_value<std::string>(table, "ice_bind_address"))
            {
                ice_config.bind_address = value->empty() ? std::optional<std::string>{} : std::optional<std::string>{*value};
            }
            if (const auto value = table_value<uint16_t>(table, "ice_port_range_begin"))
            {
                ice_config.port_range_begin = *value;
            }
            if (const auto value = table_value<uint16_t>(table, "ice_port_range_end"))
            {
                ice_config.port_range_end = *value;
            }
            if (ice_config.port_range_begin.has_value() && ice_config.port_range_end.has_value() &&
                *ice_config.port_range_begin > *ice_config.port_range_end)
            {
                throw std::runtime_error("webrtc ICE port range begin must be <= end");
            }
            if (ice_config.port_range_begin.has_value() != ice_config.port_range_end.has_value())
            {
                throw std::runtime_error("webrtc ICE port range begin and end must both be provided or both omitted");
            }

            if (const auto *servers = table["ice_servers"].as_array())
            {
                ice_config.servers.clear();
                for (const auto &node : *servers)
                {
                    const auto *server_table = node.as_table();
                    if (server_table == nullptr)
                    {
                        throw std::runtime_error("webrtc.ice_servers entries must be tables");
                    }
                    WebRtcIceServerConfig server;
                    server.uri = (*server_table)["uri"].value_or("");
                    if (server.uri.empty())
                    {
                        throw std::runtime_error("webrtc.ice_servers entry requires uri");
                    }
                    server.username = (*server_table)["username"].value_or("");
                    server.credential = (*server_table)["credential"].value_or("");
                    ice_config.servers.push_back(std::move(server));
                }
            }
        }

        size_t frame_storage_size(const VideoFrameView &frame)
        {
            return video_frame_storage_size(frame);
        }

        class ManagedVideoServer final : public IManagedVideoServer
        {
            struct ManagedServerAccess
            {
                std::mutex mutex;
                std::condition_variable cv;
                ManagedVideoServer *owner{nullptr};
                bool shutting_down{false};
                size_t active_calls{0};
            };

            class StreamGroupHandle;

        public:
            explicit ManagedVideoServer(ManagedVideoServerConfig config)
                : config_(std::move(config)),
                  server_(validated_webrtc_config(config_)),
                  access_(std::make_shared<ManagedServerAccess>())
            {
                access_->owner = this;
                for (const auto &stream : config_.streams)
                {
                    add_stream_state_locked(stream);
                }
            }

            ~ManagedVideoServer() override
            {
                {
                    std::unique_lock<std::mutex> access_lock(access_->mutex);
                    access_->shutting_down = true;
                    if (access_->active_calls > 0)
                    {
                        if (const auto hook = current_managed_shutdown_wait_test_hook())
                        {
                            access_lock.unlock();
                            hook();
                            access_lock.lock();
                        }
                    }
                    access_->cv.wait(access_lock,
                                     [this]()
                                     { return access_->active_calls == 0; });
                    access_->owner = nullptr;
                }
                stop();
            }

            bool start() override
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (started_)
                {
                    return false;
                }

                for (auto &[stream_id, state] : streams_)
                {
                    if (!state->registered_with_server && !server_.register_stream(state->stream_config))
                    {
                        spdlog::error("failed to register managed stream {}", stream_id);
                        return false;
                    }
                    state->registered_with_server = true;
                }

                if (!server_.start())
                {
                    return false;
                }

                stop_requested_ = false;
                started_ = true;
                if (config_.api_execution.mode == ManagedApiExecutionMode::ManagedThread ||
                    config_.execution_mode == ExecutionMode::WorkerThread)
                {
                    api_thread_ = std::thread([this]()
                                              { this->api_loop(); });
                    if (!apply_api_thread_config())
                    {
                        stop_requested_ = true;
                        lock.unlock();
                        if (api_thread_.joinable())
                        {
                            api_thread_.join();
                        }
                        server_.stop();
                        lock.lock();
                        started_ = false;
                        return false;
                    }
                }
                return true;
            }

            void stop() override
            {
                stop_requested_ = true;
                if (api_thread_.joinable())
                {
                    api_thread_.join();
                }

                std::lock_guard<std::mutex> lock(mutex_);
                for (auto &[_, state] : streams_)
                {
                    std::lock_guard<std::mutex> stream_lock(state->mutex);
                    if (state->pipeline)
                    {
                        state->pipeline->stop();
                        state->pipeline.reset();
                    }
                    state->encoded_handoff.clear();
                }
                server_.stop();
                started_ = false;
            }

            bool step() override
            {
                if (!step_api_compat())
                {
                    return false;
                }

                std::vector<std::string> stream_ids;
                size_t start_index = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stream_order_.empty())
                    {
                        return true;
                    }
                    for (const auto &stream_id : stream_order_)
                    {
                        const auto it = streams_.find(stream_id);
                        if (it != streams_.end() && it->second->owner_group_id == 0)
                        {
                            stream_ids.push_back(stream_id);
                        }
                    }
                    if (stream_ids.empty())
                    {
                        return true;
                    }
                    start_index = next_stream_index_ % stream_ids.size();
                }

                size_t processed = 0;
                size_t last_processed_cursor = start_index;
                bool progressed_any = false;
                for (size_t offset = 0; offset < stream_ids.size(); ++offset)
                {
                    const size_t index = (start_index + offset) % stream_ids.size();
                    const auto &stream_id = stream_ids[index];
                    if (config_.max_streams_per_step > 0 && processed >= config_.max_streams_per_step)
                    {
                        break;
                    }

                    const auto result = process_stream(stream_id);
                    if (result == StreamStepResult::Failed)
                    {
                        return false;
                    }
                    if (result == StreamStepResult::Progressed)
                    {
                        ++processed;
                        progressed_any = true;
                        last_processed_cursor = (index + 1) % stream_ids.size();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!stream_order_.empty())
                    {
                        next_stream_index_ = progressed_any ? (last_processed_cursor % stream_order_.size())
                                                           : ((start_index + 1) % stream_order_.size());
                    }
                }
                return step_api_compat();
            }

            bool step_api() override
            {
                if (config_.api_execution.mode == ManagedApiExecutionMode::ManagedThread ||
                    config_.execution_mode == ExecutionMode::WorkerThread)
                {
                    spdlog::warn("manual step_api rejected while managed API thread mode is active");
                    return false;
                }
                return step_api_guarded();
            }

            std::shared_ptr<IManagedStreamGroup> register_streams(std::vector<std::string> stream_ids) override
            {
                if (stream_ids.empty())
                {
                    return nullptr;
                }
                std::set<std::string> unique_ids(stream_ids.begin(), stream_ids.end());
                if (unique_ids.size() != stream_ids.size())
                {
                    return nullptr;
                }

                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto &stream_id : stream_ids)
                {
                    const auto it = streams_.find(stream_id);
                    if (it == streams_.end() || it->second->owner_group_id != 0)
                    {
                        return nullptr;
                    }
                }

                const uint64_t group_id = next_group_id_++;
                for (const auto &stream_id : stream_ids)
                {
                    streams_.at(stream_id)->owner_group_id = group_id;
                }
                return std::make_shared<StreamGroupHandle>(access_, group_id, std::move(stream_ids));
            }

            ServerDebugSnapshot get_debug_snapshot() const override
            {
                ServerDebugSnapshot snapshot = server_.get_debug_snapshot();
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto &stream : snapshot.streams)
                {
                    const auto it = streams_.find(stream.stream_id);
                    if (it == streams_.end())
                    {
                        continue;
                    }
                    std::lock_guard<std::mutex> stream_lock(it->second->mutex);
                    stream.requested_encoder = it->second->encoder_runtime_info.requested_encoder;
                    stream.active_encoder = it->second->encoder_runtime_info.active_encoder;
                    stream.hardware_accelerated = it->second->encoder_runtime_info.hardware_accelerated;
                    stream.hardware_device = it->second->encoder_runtime_info.hardware_device;
                    stream.encoded_handoff_dropped = it->second->encoded_handoff.dropped_count();
                    const auto &timing = it->second->step_timing;
                    stream.stream_step_count = timing.count;
                    stream.stream_step_total_ms = timing.total_ms;
                    stream.stream_step_max_ms = timing.max_ms;
                    stream.stream_step_last_ms = timing.last_ms;
                    stream.stream_step_transform_total_ms = timing.transform_total_ms;
                    stream.stream_step_transform_max_ms = timing.transform_max_ms;
                    stream.stream_step_transform_last_ms = timing.transform_last_ms;
                    stream.stream_step_pipeline_setup_total_ms = timing.pipeline_setup_total_ms;
                    stream.stream_step_pipeline_setup_max_ms = timing.pipeline_setup_max_ms;
                    stream.stream_step_pipeline_setup_last_ms = timing.pipeline_setup_last_ms;
                    stream.stream_step_encode_total_ms = timing.encode_total_ms;
                    stream.stream_step_encode_max_ms = timing.encode_max_ms;
                    stream.stream_step_encode_last_ms = timing.encode_last_ms;
                    stream.stream_step_publish_total_ms = timing.publish_total_ms;
                    stream.stream_step_publish_max_ms = timing.publish_max_ms;
                    stream.stream_step_publish_last_ms = timing.publish_last_ms;
                }
                return snapshot;
            }

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
                    state->registered_with_server = true;
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
                if (it->second->owner_group_id != 0)
                {
                    spdlog::warn("remove_stream rejected for {} because it is owned by a stream group", stream_id);
                    return false;
                }
                std::lock_guard<std::mutex> stream_lock(it->second->mutex);
                if (it->second->pipeline)
                {
                    it->second->pipeline->stop();
                    it->second->pipeline.reset();
                }
                if (it->second->registered_with_server)
                {
                    server_.remove_stream(stream_id);
                }
                stream_order_.erase(std::remove(stream_order_.begin(), stream_order_.end(), stream_id), stream_order_.end());
                if (!stream_order_.empty())
                {
                    next_stream_index_ %= stream_order_.size();
                }
                else
                {
                    next_stream_index_ = 0;
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
                    {
                        std::lock_guard<std::mutex> stream_lock(it->second->mutex);
                        if (it->second->pending_frame.has_value())
                        {
                            server_.note_frame_dropped(stream_id);
                        }
                        it->second->pending_frame = std::move(pending);
                    }
                }

                server_.note_frame_received(stream_id, frame.timestamp_ns);

                if (config_.execution_mode == ExecutionMode::InlineOnPush)
                {
                    if (!step_api_compat())
                    {
                        return false;
                    }
                    return process_stream(stream_id) != StreamStepResult::Failed;
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
            struct EncodedAccessUnitStorage
            {
                std::vector<uint8_t> bytes;
                VideoCodec codec{VideoCodec::H264};
                uint64_t timestamp_ns{0};
                bool keyframe{false};
                bool codec_config{false};

                EncodedAccessUnitView view() const
                {
                    return EncodedAccessUnitView{bytes.data(), bytes.size(), codec, timestamp_ns, keyframe, codec_config};
                }
            };

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
                struct StepTiming
                {
                    uint64_t count{0};
                    double total_ms{0.0};
                    double max_ms{0.0};
                    double last_ms{0.0};
                    double transform_total_ms{0.0};
                    double transform_max_ms{0.0};
                    double transform_last_ms{0.0};
                    double pipeline_setup_total_ms{0.0};
                    double pipeline_setup_max_ms{0.0};
                    double pipeline_setup_last_ms{0.0};
                    double encode_total_ms{0.0};
                    double encode_max_ms{0.0};
                    double encode_last_ms{0.0};
                    double publish_total_ms{0.0};
                    double publish_max_ms{0.0};
                    double publish_last_ms{0.0};
                };

                StreamConfig stream_config;
                RawVideoPipelineConfig pipeline_template;
                mutable std::mutex mutex;
                std::optional<PendingFrame> pending_frame;
                std::unique_ptr<IRawVideoPipeline> pipeline;
                RawH264EncoderRuntimeInfo encoder_runtime_info;
                webrtc_internal::EncodedAccessUnitHandoff<EncodedAccessUnitStorage> encoded_handoff;
                uint64_t next_due_timestamp_ns{0};
                uint64_t pipeline_config_generation{0};
                uint32_t pipeline_width{0};
                uint32_t pipeline_height{0};
                bool registered_with_server{false};
                uint64_t owner_group_id{0};
                StepTiming step_timing;
            };

            class StreamGroupHandle final : public IManagedStreamGroup
            {
            public:
                StreamGroupHandle(std::weak_ptr<ManagedServerAccess> access, uint64_t group_id, std::vector<std::string> stream_ids)
                    : access_(std::move(access)), group_id_(group_id), stream_ids_(std::move(stream_ids)) {}

                ~StreamGroupHandle() override
                {
                    if (const auto access = access_.lock())
                    {
                        std::lock_guard<std::mutex> access_lock(access->mutex);
                        if (access->owner != nullptr && !access->shutting_down)
                        {
                            access->owner->release_stream_group(group_id_, stream_ids_);
                        }
                    }
                }

                bool step_streams() override
                {
                    bool expected = false;
                    if (!stepping_.compare_exchange_strong(expected, true))
                    {
                        spdlog::warn("managed stream group concurrent step rejected group={}", group_id_);
                        return false;
                    }
                    struct StepGuard
                    {
                        std::atomic<bool> &flag;
                        ~StepGuard() { flag.store(false); }
                    } guard{stepping_};

                    const auto access = access_.lock();
                    if (!access)
                    {
                        return false;
                    }
                    ManagedVideoServer *owner = nullptr;
                    {
                        std::lock_guard<std::mutex> access_lock(access->mutex);
                        if (access->owner == nullptr || access->shutting_down)
                        {
                            return false;
                        }
                        owner = access->owner;
                        ++access->active_calls;
                    }
                    struct ActiveCallGuard
                    {
                        std::shared_ptr<ManagedServerAccess> access;
                        ~ActiveCallGuard()
                        {
                            std::lock_guard<std::mutex> access_lock(access->mutex);
                            --access->active_calls;
                            access->cv.notify_all();
                        }
                    } active_call{access};
                    return owner->step_stream_ids(stream_ids_);
                }

                std::vector<std::string> stream_ids() const override { return stream_ids_; }

            private:
                std::weak_ptr<ManagedServerAccess> access_;
                uint64_t group_id_{0};
                std::vector<std::string> stream_ids_;
                std::atomic<bool> stepping_{false};
            };

            enum class StreamStepResult
            {
                Idle,
                Progressed,
                Failed
            };

            static WebRtcVideoServerConfig validated_webrtc_config(const ManagedVideoServerConfig &config)
            {
                if (config.webrtc.execution_mode != ExecutionMode::ManualStep)
                {
                    throw std::invalid_argument(
                        "managed server requires webrtc.execution_mode=ManualStep; use ManagedVideoServerConfig.execution_mode for outer progression policy");
                }
                if (config.webrtc.http_poll_timeout_ms != config.http_poll_timeout_ms)
                {
                    throw std::invalid_argument(
                        "managed server requires http_poll_timeout_ms and webrtc.http_poll_timeout_ms to match");
                }
                if (config.webrtc.max_http_connections_per_step == 0)
                {
                    throw std::invalid_argument("managed server requires webrtc.max_http_connections_per_step >= 1");
                }
                return config.webrtc;
            }

            void add_stream_state_locked(const StreamConfig &config)
            {
                auto state = std::make_shared<ManagedStreamState>();
                state->stream_config = config;
                const auto it = config_.default_raw_pipelines.find(config.stream_id);
                if (it != config_.default_raw_pipelines.end())
                {
                    state->pipeline_template = it->second;
                }
                else if (const auto default_it = config_.default_raw_pipelines.find("default");
                         default_it != config_.default_raw_pipelines.end())
                {
                    state->pipeline_template = default_it->second;
                }
                streams_.emplace(config.stream_id, std::move(state));
                stream_order_.push_back(config.stream_id);
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

            static void update_timing(double elapsed_ms, double &total_ms, double &max_ms, double &last_ms)
            {
                total_ms += elapsed_ms;
                max_ms = std::max(max_ms, elapsed_ms);
                last_ms = elapsed_ms;
            }

            bool ensure_pipeline(const std::string &stream_id,
                                 const std::shared_ptr<ManagedStreamState> &state,
                                 const StreamOutputConfig &output_config,
                                 uint32_t width,
                                 uint32_t height)
            {
                if (state->pipeline && state->pipeline_config_generation == output_config.config_generation &&
                    state->pipeline_width == width && state->pipeline_height == height)
                {
                    return true;
                }

                if (state->pipeline)
                {
                    state->pipeline->stop();
                    state->pipeline.reset();
                }

                RawVideoPipelineConfig pipeline_config = make_effective_pipeline_config(*state, output_config, width, height);
                auto pipeline = make_raw_video_pipeline(
                    stream_id, pipeline_config,
                    [state](const EncodedAccessUnitView &access_unit)
                    {
                        EncodedAccessUnitStorage storage;
                        storage.bytes.assign(static_cast<const uint8_t *>(access_unit.data),
                                             static_cast<const uint8_t *>(access_unit.data) + access_unit.size_bytes);
                        storage.codec = access_unit.codec;
                        storage.timestamp_ns = access_unit.timestamp_ns;
                        storage.keyframe = access_unit.keyframe;
                        storage.codec_config = access_unit.codec_config;
                        state->encoded_handoff.push(std::move(storage));
                        return true;
                    });

                std::string error;
                if (!pipeline->start(&error))
                {
                    spdlog::error("failed to start managed pipeline for {}: {}", stream_id, error);
                    return false;
                }

                state->pipeline = std::move(pipeline);
                state->encoder_runtime_info = state->pipeline->encoder_runtime_info();
                state->pipeline_config_generation = output_config.config_generation;
                state->pipeline_width = width;
                state->pipeline_height = height;
                return true;
            }

            StreamStepResult process_stream(const std::string &stream_id)
            {
                const auto step_started_at = std::chrono::steady_clock::now();
                StreamOutputConfig output_config;
                PendingFrame pending;
                std::shared_ptr<ManagedStreamState> state;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = streams_.find(stream_id);
                    if (it == streams_.end())
                    {
                        return StreamStepResult::Idle;
                    }
                    state = it->second;
                }

                {
                    std::lock_guard<std::mutex> stream_lock(state->mutex);
                    if (!state->pending_frame.has_value())
                    {
                        return StreamStepResult::Idle;
                    }

                    const auto output_config_opt = server_.get_stream_output_config(stream_id);
                    if (!output_config_opt.has_value())
                    {
                        return StreamStepResult::Failed;
                    }
                    output_config = *output_config_opt;

                    if (output_config.output_fps > 0.0 && state->next_due_timestamp_ns > 0 &&
                        state->pending_frame->timestamp_ns < state->next_due_timestamp_ns)
                    {
                        return StreamStepResult::Idle;
                    }

                    pending = std::move(*state->pending_frame);
                    state->pending_frame.reset();
                    if (output_config.output_fps > 0.0)
                    {
                        const uint64_t frame_interval_ns =
                            static_cast<uint64_t>(std::llround(1000000000.0 / output_config.output_fps));
                        state->next_due_timestamp_ns = pending.timestamp_ns + frame_interval_ns;
                    }
                    else
                    {
                        state->next_due_timestamp_ns = 0;
                    }
                }

                RgbImage transformed;
                const auto transform_started_at = std::chrono::steady_clock::now();
                if (!apply_display_transform(pending.view(), output_config, transformed))
                {
                    server_.note_frame_dropped(stream_id);
                    return StreamStepResult::Progressed;
                }
                const auto transform_finished_at = std::chrono::steady_clock::now();

                bool pipeline_reconfigured = false;
                double pipeline_setup_ms = 0.0;
                double encode_ms = 0.0;
                {
                    std::lock_guard<std::mutex> stream_lock(state->mutex);
                    pipeline_reconfigured = !(state->pipeline && state->pipeline_config_generation == output_config.config_generation &&
                                              state->pipeline_width == transformed.width && state->pipeline_height == transformed.height);
                    const auto setup_started_at = std::chrono::steady_clock::now();
                    if (!ensure_pipeline(stream_id, state, output_config, transformed.width, transformed.height))
                    {
                        return StreamStepResult::Failed;
                    }
                    const auto setup_finished_at = std::chrono::steady_clock::now();
                    if (pipeline_reconfigured)
                    {
                        pipeline_setup_ms = std::chrono::duration<double, std::milli>(setup_finished_at - setup_started_at).count();
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
                    const auto encode_started_at = std::chrono::steady_clock::now();
                    if (const auto hook = current_managed_stream_step_test_hook())
                    {
                        hook(stream_id);
                    }
                    if (!state->pipeline->push_frame(transformed_view, &error))
                    {
                        spdlog::error("managed pipeline encode failed for {}: {}", stream_id, error);
                        return StreamStepResult::Failed;
                    }
                    const auto encode_finished_at = std::chrono::steady_clock::now();
                    encode_ms = std::chrono::duration<double, std::milli>(encode_finished_at - encode_started_at).count();
                }

                const auto publish_started_at = std::chrono::steady_clock::now();
                if (!server_.publish_transformed_frame(stream_id, transformed.rgb, transformed.width, transformed.height,
                                                       pending.timestamp_ns, pending.frame_id))
                {
                    return StreamStepResult::Failed;
                }
                const auto publish_finished_at = std::chrono::steady_clock::now();
                const auto step_finished_at = publish_finished_at;
                const double transform_ms =
                    std::chrono::duration<double, std::milli>(transform_finished_at - transform_started_at).count();
                const double publish_ms =
                    std::chrono::duration<double, std::milli>(publish_finished_at - publish_started_at).count();
                const double step_ms =
                    std::chrono::duration<double, std::milli>(step_finished_at - step_started_at).count();
                {
                    std::lock_guard<std::mutex> stream_lock(state->mutex);
                    auto &timing = state->step_timing;
                    ++timing.count;
                    update_timing(step_ms, timing.total_ms, timing.max_ms, timing.last_ms);
                    update_timing(transform_ms, timing.transform_total_ms, timing.transform_max_ms, timing.transform_last_ms);
                    update_timing(pipeline_setup_ms, timing.pipeline_setup_total_ms, timing.pipeline_setup_max_ms,
                                  timing.pipeline_setup_last_ms);
                    update_timing(encode_ms, timing.encode_total_ms, timing.encode_max_ms, timing.encode_last_ms);
                    update_timing(publish_ms, timing.publish_total_ms, timing.publish_max_ms, timing.publish_last_ms);
                }
                return StreamStepResult::Progressed;
            }

            bool step_stream_ids(const std::vector<std::string> &stream_ids)
            {
                for (const auto &stream_id : stream_ids)
                {
                    const auto result = process_stream(stream_id);
                    if (result == StreamStepResult::Failed)
                    {
                        return false;
                    }
                }
                return true;
            }

            void release_stream_group(uint64_t group_id, const std::vector<std::string> &stream_ids)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto &stream_id : stream_ids)
                {
                    const auto it = streams_.find(stream_id);
                    if (it != streams_.end() && it->second->owner_group_id == group_id)
                    {
                        it->second->owner_group_id = 0;
                    }
                }
            }

            bool step_api_compat()
            {
                if (config_.api_execution.mode == ManagedApiExecutionMode::ManagedThread ||
                    config_.execution_mode == ExecutionMode::WorkerThread)
                {
                    return true;
                }
                return step_api_guarded();
            }

            bool step_api_guarded()
            {
                bool expected = false;
                if (!api_step_active_.compare_exchange_strong(expected, true))
                {
                    spdlog::warn("managed API concurrent step rejected");
                    return false;
                }
                struct StepGuard
                {
                    std::atomic<bool> &flag;
                    ~StepGuard() { flag.store(false); }
                } guard{api_step_active_};
                return step_api_impl();
            }

            bool step_api_impl()
            {
                if (const auto hook = current_managed_api_step_test_hook())
                {
                    hook();
                }

                std::vector<std::pair<std::string, EncodedAccessUnitStorage>> units;
                std::vector<std::pair<std::string, std::shared_ptr<ManagedStreamState>>> stream_states;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto &[stream_id, state] : streams_)
                    {
                        stream_states.emplace_back(stream_id, state);
                    }
                }

                for (auto &[stream_id, state] : stream_states)
                {
                    std::lock_guard<std::mutex> stream_lock(state->mutex);
                    for (auto &unit : state->encoded_handoff.drain())
                    {
                        units.emplace_back(stream_id, std::move(unit));
                    }
                }

                for (const auto &entry : units)
                {
                    if (!server_.push_access_unit(entry.first, entry.second.view()))
                    {
                        return false;
                    }
                }
                return server_.step();
            }

            bool apply_api_thread_config()
            {
                if (!api_thread_.joinable())
                {
                    return true;
                }
#if defined(__linux__)
                if (!config_.api_execution.thread.cpu_affinity.empty())
                {
                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    for (const int cpu : config_.api_execution.thread.cpu_affinity)
                    {
                        if (cpu < 0)
                        {
                            spdlog::error("invalid managed API thread CPU affinity value {}", cpu);
                            return false;
                        }
                        CPU_SET(cpu, &cpuset);
                    }
                    const int rc = pthread_setaffinity_np(api_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
                    if (rc != 0)
                    {
                        spdlog::error("failed to apply managed API thread CPU affinity: {}", strerror(rc));
                        return false;
                    }
                }
                if (config_.api_execution.thread.scheduling_priority.has_value())
                {
                    sched_param params{};
                    params.sched_priority = *config_.api_execution.thread.scheduling_priority;
                    const int rc = pthread_setschedparam(api_thread_.native_handle(), SCHED_RR, &params);
                    if (rc != 0)
                    {
                        spdlog::error("failed to apply managed API thread scheduling priority: {}", strerror(rc));
                        return false;
                    }
                }
                return true;
#else
                if (!config_.api_execution.thread.cpu_affinity.empty() ||
                    config_.api_execution.thread.scheduling_priority.has_value())
                {
                    spdlog::error("managed API thread affinity/priority is not supported on this platform");
                    return false;
                }
                return true;
#endif
            }

            void api_loop()
            {
                while (!stop_requested_.load())
                {
                    if (!step_api_guarded())
                    {
                        break;
                    }
                    if (!config_.webrtc.enable_http_api || config_.webrtc.http_poll_timeout_ms == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<uint32_t>(1, config_.http_poll_timeout_ms)));
                    }
                }
            }

            ManagedVideoServerConfig config_;
            WebRtcVideoServer server_;
            std::shared_ptr<ManagedServerAccess> access_;
            mutable std::mutex mutex_;
            std::unordered_map<std::string, std::shared_ptr<ManagedStreamState>> streams_;
            std::vector<std::string> stream_order_;
            std::atomic<bool> stop_requested_{false};
            std::atomic<bool> api_step_active_{false};
            std::thread api_thread_;
            bool started_{false};
            size_t next_stream_index_{0};
            uint64_t next_group_id_{1};
        };

    } // namespace

    namespace webrtc_internal
    {
        void set_managed_stream_step_test_hook(std::function<void(const std::string &)> hook)
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            managed_stream_step_test_hook() = std::move(hook);
        }

        void set_managed_api_step_test_hook(std::function<void()> hook)
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            managed_api_step_test_hook() = std::move(hook);
        }

        void set_managed_shutdown_wait_test_hook(std::function<void()> hook)
        {
            std::lock_guard<std::mutex> lock(managed_test_hook_mutex());
            managed_shutdown_wait_test_hook() = std::move(hook);
        }
    } // namespace webrtc_internal

    ManagedVideoServerConfig load_managed_video_server_config(const std::string &config_path)
    {
        const toml::table table = toml::parse_file(config_path);
        ManagedVideoServerConfig config;
        bool saw_top_level_http_poll_timeout = false;
        bool saw_nested_http_poll_timeout = false;

        if (const auto execution_mode = table_value<std::string>(table, "execution_mode"))
        {
            const auto parsed = parse_execution_mode(*execution_mode);
            if (!parsed.has_value())
            {
                throw std::runtime_error("invalid execution_mode: " + *execution_mode);
            }
            config.execution_mode = *parsed;
        }
        if (const auto api_execution = table_value<std::string>(table, "api_execution"))
        {
            const auto parsed = parse_api_execution_mode(*api_execution);
            if (!parsed.has_value())
            {
                throw std::runtime_error("invalid api_execution: " + *api_execution);
            }
            config.api_execution.mode = *parsed;
        }
        config.api_execution.thread.cpu_affinity = parse_int_array(table, "api_cpu_affinity");
        if (const auto value = table_value<int64_t>(table, "api_scheduling_priority"))
        {
            if (*value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max())
            {
                throw std::runtime_error("api_scheduling_priority is out of range");
            }
            config.api_execution.thread.scheduling_priority = static_cast<int>(*value);
        }
        if (const auto value = table_value<uint32_t>(table, "http_poll_timeout_ms"))
        {
            config.http_poll_timeout_ms = *value;
            saw_top_level_http_poll_timeout = true;
        }
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
            if (const auto value = table_value<std::string>(*webrtc, "execution_mode"))
            {
                const auto parsed = parse_execution_mode(*value);
                if (!parsed.has_value())
                {
                    throw std::runtime_error("invalid webrtc.execution_mode: " + *value);
                }
                config.webrtc.execution_mode = *parsed;
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
            config.webrtc.ip_allowlist = parse_string_array(*webrtc, "ip_allowlist");
            config.webrtc.cors_allowed_origins = parse_string_array(*webrtc, "cors_allowed_origins");
            if (const auto value = table_value<size_t>(*webrtc, "max_http_request_bytes"))
            {
                config.webrtc.max_http_request_bytes = *value;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_json_body_bytes"))
            {
                config.webrtc.max_json_body_bytes = *value;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_signaling_sdp_bytes"))
            {
                config.webrtc.max_signaling_sdp_bytes = *value;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_signaling_candidate_bytes"))
            {
                config.webrtc.max_signaling_candidate_bytes = *value;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_pending_candidates_per_stream"))
            {
                config.webrtc.max_pending_candidates_per_stream = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "signaling_rate_limit_window_seconds"))
            {
                config.webrtc.signaling_rate_limit_window_seconds = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "signaling_rate_limit_max_requests"))
            {
                config.webrtc.signaling_rate_limit_max_requests = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "config_rate_limit_window_seconds"))
            {
                config.webrtc.config_rate_limit_window_seconds = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "config_rate_limit_max_requests"))
            {
                config.webrtc.config_rate_limit_max_requests = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "debug_rate_limit_window_seconds"))
            {
                config.webrtc.debug_rate_limit_window_seconds = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "debug_rate_limit_max_requests"))
            {
                config.webrtc.debug_rate_limit_max_requests = *value;
            }
            if (const auto value = table_value<uint32_t>(*webrtc, "http_poll_timeout_ms"))
            {
                config.webrtc.http_poll_timeout_ms = *value;
                saw_nested_http_poll_timeout = true;
            }
            if (const auto value = table_value<size_t>(*webrtc, "max_http_connections_per_step"))
            {
                config.webrtc.max_http_connections_per_step = *value;
            }
            parse_ice_config(*webrtc, config.webrtc.ice);
        }

        if (saw_top_level_http_poll_timeout && saw_nested_http_poll_timeout &&
            config.http_poll_timeout_ms != config.webrtc.http_poll_timeout_ms)
        {
            throw std::runtime_error(
                "http_poll_timeout_ms and webrtc.http_poll_timeout_ms must match when both are provided");
        }
        if (saw_top_level_http_poll_timeout && !saw_nested_http_poll_timeout)
        {
            config.webrtc.http_poll_timeout_ms = config.http_poll_timeout_ms;
        }
        else if (!saw_top_level_http_poll_timeout && saw_nested_http_poll_timeout)
        {
            config.http_poll_timeout_ms = config.webrtc.http_poll_timeout_ms;
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
