#include "video_server_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "video_server/video_types.h"
#include "video_pixel_format_utils.h"
#include "../transforms/display_transform.h"

namespace video_server
{
    namespace
    {
        constexpr uint32_t kMinOutputDimension = 16;
        constexpr uint32_t kMaxOutputDimension = 3840;
        constexpr double kMinOutputFps = 1.0;
        constexpr double kMaxOutputFps = 120.0;
        constexpr auto kRunningFpsWindow = std::chrono::seconds(2);

        void fill_stream_debug_snapshot(const VideoServerCore::StreamState &stream, StreamDebugSnapshot &snapshot)
        {
            snapshot.stream_id = stream.info.stream_id;
            snapshot.label = stream.info.label;
            snapshot.configured_width = stream.info.config.width;
            snapshot.configured_height = stream.info.config.height;
            snapshot.configured_fps = stream.info.config.nominal_fps;
            snapshot.active_filter_mode = to_string(stream.info.output_config.display_mode);
            snapshot.active_output_width = stream.info.output_config.output_width;
            snapshot.active_output_height = stream.info.output_config.output_height;
            snapshot.active_output_fps = stream.info.output_config.output_fps;
            snapshot.config_generation = stream.info.output_config.config_generation;
            snapshot.config_state = "active";
            snapshot.total_frames_received = stream.info.frames_received;
            snapshot.total_frames_transformed = stream.info.frames_transformed;
            snapshot.total_frames_dropped = stream.info.frames_dropped;
            snapshot.total_access_units_received = stream.info.access_units_received;
            snapshot.running_fps = stream.info.running_fps;
            snapshot.running_input_fps = stream.info.running_input_fps;
            snapshot.running_output_fps = stream.info.running_output_fps;
            snapshot.running_encoded_fps = stream.info.running_encoded_fps;
            snapshot.transform_timing = stream.transform_timing;
            snapshot.encode_timing = stream.encode_timing;

            if (stream.latest_frame && stream.latest_frame->valid)
            {
                snapshot.latest_raw_frame_available = true;
                snapshot.latest_raw_frame_id = stream.latest_frame->frame_id;
                snapshot.latest_raw_timestamp_ns = stream.latest_frame->timestamp_ns;
                snapshot.latest_raw_width = stream.latest_frame->width;
                snapshot.latest_raw_height = stream.latest_frame->height;
            }

            if (stream.latest_encoded_unit && stream.latest_encoded_unit->valid)
            {
                snapshot.latest_encoded_access_unit_available = true;
                snapshot.latest_encoded_timestamp_ns = stream.latest_encoded_unit->timestamp_ns;
                snapshot.latest_encoded_sequence_id = stream.latest_encoded_unit->sequence_id;
                snapshot.latest_encoded_size_bytes = stream.latest_encoded_unit->bytes.size();
                snapshot.latest_encoded_keyframe = stream.latest_encoded_unit->keyframe;
                snapshot.latest_encoded_codec_config = stream.latest_encoded_unit->codec_config;
            }
        }

    } // namespace

    bool VideoServerCore::register_stream(const StreamConfig &config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config.stream_id.empty() || config.width == 0 || config.height == 0 || config.nominal_fps <= 0.0 ||
            !is_supported_input_pixel_format(config.input_pixel_format) || config.max_subscribers == 0)
        {
            return false;
        }

        auto [it, inserted] = streams_.try_emplace(config.stream_id);
        if (!inserted)
        {
            return false;
        }

        StreamState &stream = it->second;
        VideoStreamInfo &info = stream.info;
        info.stream_id = config.stream_id;
        info.label = config.label;
        info.config = config;
        info.output_config = StreamOutputConfig{};
        info.active = true;
        return true;
    }

    bool VideoServerCore::remove_stream(const std::string &stream_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return streams_.erase(stream_id) > 0;
    }

    bool VideoServerCore::push_frame(const std::string &stream_id, const VideoFrameView &frame)
    {
        StreamConfig config_snapshot;
        StreamOutputConfig output_config_snapshot;
        if (!validate_raw_frame_input(stream_id, frame, &config_snapshot, &output_config_snapshot))
        {
            note_frame_dropped(stream_id);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = streams_.find(stream_id);
            if (it == streams_.end())
            {
                return false;
            }
            StreamState &stream = it->second;
            ++stream.info.frames_received;
            stream.info.last_input_timestamp_ns = frame.timestamp_ns;
            const auto now = std::chrono::steady_clock::now();
            record_rate_sample(stream.input_frame_times, now);
            refresh_running_rates(stream, now);

            if (output_config_snapshot.output_fps > 0.0)
            {
                const uint64_t frame_interval_ns =
                    static_cast<uint64_t>(std::llround(1000000000.0 / output_config_snapshot.output_fps));
                if (stream.next_allowed_output_timestamp_ns > 0 && frame.timestamp_ns < stream.next_allowed_output_timestamp_ns)
                {
                    ++stream.info.frames_dropped;
                    return true;
                }
                stream.next_allowed_output_timestamp_ns = frame.timestamp_ns + frame_interval_ns;
            }
        }

        RgbImage transformed;
        if (!apply_display_transform(frame, output_config_snapshot, transformed))
        {
            note_frame_dropped(stream_id);
            return false;
        }

        return publish_transformed_frame(stream_id, std::move(transformed.rgb), transformed.width, transformed.height,
                                         frame.timestamp_ns, frame.frame_id);
    }

    bool VideoServerCore::push_access_unit(const std::string &stream_id,
                                           const EncodedAccessUnitView &access_unit)
    {
        if (access_unit.data == nullptr || access_unit.size_bytes == 0)
        {
            return false;
        }

        if (access_unit.codec != VideoCodec::H264)
        {
            return false;
        }

        auto published_unit = std::make_shared<LatestEncodedUnit>();
        published_unit->bytes.assign(static_cast<const uint8_t *>(access_unit.data),
                                     static_cast<const uint8_t *>(access_unit.data) + access_unit.size_bytes);
        published_unit->codec = access_unit.codec;
        published_unit->timestamp_ns = access_unit.timestamp_ns;
        published_unit->sequence_id = access_unit.timestamp_ns;
        published_unit->keyframe = access_unit.keyframe;
        published_unit->codec_config = access_unit.codec_config;
        published_unit->valid = true;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }

        StreamState &stream = it->second;
        VideoStreamInfo &info = stream.info;
        stream.latest_encoded_unit = published_unit;

        ++info.access_units_received;
        info.last_frame_timestamp_ns = access_unit.timestamp_ns;
        info.has_latest_encoded_unit = true;
        info.last_encoded_codec = access_unit.codec;
        info.last_encoded_timestamp_ns = access_unit.timestamp_ns;
        info.last_encoded_sequence_id = published_unit->sequence_id;
        info.last_encoded_size_bytes = access_unit.size_bytes;
        info.last_encoded_keyframe = access_unit.keyframe;
        info.last_encoded_codec_config = access_unit.codec_config;
        const auto now = std::chrono::steady_clock::now();
        record_rate_sample(stream.encoded_unit_times, now);
        refresh_running_rates(stream, now);
        return true;
    }

    bool VideoServerCore::validate_raw_frame_input(const std::string &stream_id,
                                                   const VideoFrameView &frame,
                                                   StreamConfig *config_out,
                                                   StreamOutputConfig *output_config_out) const
    {
        StreamConfig config_snapshot;
        StreamOutputConfig output_config_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = streams_.find(stream_id);
            if (it == streams_.end())
            {
                return false;
            }
            config_snapshot = it->second.info.config;
            output_config_snapshot = it->second.info.output_config;
        }

        if (frame.data == nullptr || frame.width == 0 || frame.height == 0)
        {
            return false;
        }

        if (frame.width != config_snapshot.width || frame.height != config_snapshot.height ||
            frame.pixel_format != config_snapshot.input_pixel_format)
        {
            return false;
        }

        if (!is_supported_input_pixel_format(frame.pixel_format))
        {
            return false;
        }

        const uint32_t expected_stride = video_pixel_format_min_row_bytes(frame.pixel_format, frame.width);
        if (expected_stride == 0 || frame.stride_bytes < expected_stride)
        {
            return false;
        }

        if (config_out != nullptr)
        {
            *config_out = config_snapshot;
        }
        if (output_config_out != nullptr)
        {
            *output_config_out = output_config_snapshot;
        }
        return true;
    }

    bool VideoServerCore::note_frame_received(const std::string &stream_id, uint64_t timestamp_ns)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }

        ++it->second.info.frames_received;
        it->second.info.last_input_timestamp_ns = timestamp_ns;
        const auto now = std::chrono::steady_clock::now();
        record_rate_sample(it->second.input_frame_times, now);
        refresh_running_rates(it->second, now);
        return true;
    }

    bool VideoServerCore::note_frame_dropped(const std::string &stream_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }

        ++it->second.info.frames_dropped;
        return true;
    }

    bool VideoServerCore::publish_transformed_frame(const std::string &stream_id,
                                                    std::vector<uint8_t> rgb_bytes,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint64_t timestamp_ns,
                                                    uint64_t frame_id)
    {
        auto published_frame = std::make_shared<LatestFrame>();
        published_frame->bytes = std::move(rgb_bytes);
        published_frame->width = width;
        published_frame->height = height;
        published_frame->pixel_format = VideoPixelFormat::RGB24;
        published_frame->timestamp_ns = timestamp_ns;
        published_frame->frame_id = frame_id;
        published_frame->valid = true;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }

        StreamState &stream = it->second;
        VideoStreamInfo &info = stream.info;
        stream.latest_frame = published_frame;

        ++info.frames_transformed;
        info.last_output_timestamp_ns = timestamp_ns;
        info.last_frame_timestamp_ns = timestamp_ns;
        info.last_frame_id = frame_id;
        info.has_latest_frame = true;
        const auto now = std::chrono::steady_clock::now();
        record_rate_sample(stream.output_frame_times, now);
        refresh_running_rates(stream, now);
        return true;
    }

    bool VideoServerCore::record_transform_timing(const std::string &stream_id, uint64_t elapsed_us)
    {
        return record_stage_timing(stream_id, &StreamState::transform_timing, elapsed_us);
    }

    bool VideoServerCore::record_encode_timing(const std::string &stream_id, uint64_t elapsed_us)
    {
        return record_stage_timing(stream_id, &StreamState::encode_timing, elapsed_us);
    }

    std::vector<VideoStreamInfo> VideoServerCore::list_streams() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        std::vector<VideoStreamInfo> output;
        output.reserve(streams_.size());
        for (auto &[_, stream] : streams_)
        {
            refresh_running_rates(stream, now);
            output.push_back(stream.info);
        }
        return output;
    }

    std::optional<VideoStreamInfo> VideoServerCore::get_stream_info(const std::string &stream_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return std::nullopt;
        }
        refresh_running_rates(it->second, std::chrono::steady_clock::now());
        return it->second.info;
    }

    bool VideoServerCore::set_stream_output_config(const std::string &stream_id,
                                                   const StreamOutputConfig &output_config)
    {
        if (!is_valid_rotation(output_config.rotation_degrees) ||
            !is_valid_output_dimensions(output_config.output_width, output_config.output_height) ||
            !is_valid_output_fps(output_config.output_fps) ||
            output_config.palette_max <= output_config.palette_min)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }

        StreamOutputConfig updated = output_config;
        updated.config_generation = it->second.info.output_config.config_generation + 1;
        it->second.info.output_config = updated;
        it->second.next_allowed_output_timestamp_ns = 0;
        it->second.latest_frame.reset();
        it->second.info.has_latest_frame = false;
        return true;
    }

    void VideoServerCore::record_rate_sample(std::deque<std::chrono::steady_clock::time_point> &samples,
                                             std::chrono::steady_clock::time_point now)
    {
        samples.push_back(now);
        while (!samples.empty() && now - samples.front() > kRunningFpsWindow)
        {
            samples.pop_front();
        }
    }

    void VideoServerCore::refresh_running_rates(StreamState &stream, std::chrono::steady_clock::time_point now)
    {
        auto rate = [now](std::deque<std::chrono::steady_clock::time_point> &samples) {
            while (!samples.empty() && now - samples.front() > kRunningFpsWindow)
            {
                samples.pop_front();
            }
            if (samples.size() < 2)
            {
                return 0.0;
            }
            const auto elapsed = std::chrono::duration<double>(samples.back() - samples.front()).count();
            return elapsed > 0.0 ? static_cast<double>(samples.size() - 1) / elapsed : 0.0;
        };

        stream.info.running_input_fps = rate(stream.input_frame_times);
        stream.info.running_output_fps = rate(stream.output_frame_times);
        stream.info.running_encoded_fps = rate(stream.encoded_unit_times);
        stream.info.running_fps = stream.info.has_latest_encoded_unit ? stream.info.running_encoded_fps
                                                                      : stream.info.running_output_fps;
    }

    void VideoServerCore::record_timing(StageTimingCounters &counters, uint64_t elapsed_us)
    {
        ++counters.samples;
        counters.total_time_us += elapsed_us;
        counters.last_time_us = elapsed_us;
        counters.max_time_us = std::max(counters.max_time_us, elapsed_us);
    }

    bool VideoServerCore::record_stage_timing(const std::string &stream_id,
                                              StageTimingCounters StreamState::*field,
                                              uint64_t elapsed_us)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return false;
        }
        record_timing(it->second.*field, elapsed_us);
        return true;
    }

    std::optional<StreamOutputConfig> VideoServerCore::get_stream_output_config(
        const std::string &stream_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return std::nullopt;
        }
        return it->second.info.output_config;
    }

    std::shared_ptr<const LatestFrame> VideoServerCore::get_latest_frame_for_stream(
        const std::string &stream_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return nullptr;
        }
        return it->second.latest_frame;
    }

    std::shared_ptr<const LatestEncodedUnit> VideoServerCore::get_latest_encoded_unit_for_stream(
        const std::string &stream_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return nullptr;
        }
        return it->second.latest_encoded_unit;
    }

    std::optional<StreamDebugSnapshot> VideoServerCore::get_stream_debug_snapshot(const std::string &stream_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end())
        {
            return std::nullopt;
        }

        refresh_running_rates(it->second, std::chrono::steady_clock::now());
        StreamDebugSnapshot snapshot;
        fill_stream_debug_snapshot(it->second, snapshot);
        return snapshot;
    }

    std::vector<StreamDebugSnapshot> VideoServerCore::list_stream_debug_snapshots() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        std::vector<StreamDebugSnapshot> snapshots;
        snapshots.reserve(streams_.size());
        for (auto &[_, stream] : streams_)
        {
            refresh_running_rates(stream, now);
            StreamDebugSnapshot snapshot;
            fill_stream_debug_snapshot(stream, snapshot);
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }

    bool VideoServerCore::is_valid_rotation(int degrees)
    {
        return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
    }

    bool VideoServerCore::is_valid_output_dimensions(uint32_t width, uint32_t height)
    {
        if (width == 0 && height == 0)
        {
            return true;
        }
        if (width < kMinOutputDimension || height < kMinOutputDimension)
        {
            return false;
        }
        return width <= kMaxOutputDimension && height <= kMaxOutputDimension;
    }

    bool VideoServerCore::is_valid_output_fps(double fps)
    {
        if (fps <= 0.0)
        {
            return true;
        }
        return fps >= kMinOutputFps && fps <= kMaxOutputFps;
    }

    bool VideoServerCore::is_supported_input_pixel_format(VideoPixelFormat pixel_format)
    {
        return video_pixel_format_supports_display_transform(pixel_format);
    }

    uint32_t VideoServerCore::bytes_per_pixel(VideoPixelFormat pixel_format)
    {
        return video_pixel_format_min_row_bytes(pixel_format, 1);
    }

} // namespace video_server
