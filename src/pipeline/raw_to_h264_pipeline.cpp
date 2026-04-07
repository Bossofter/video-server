#include "video_server/raw_video_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include "../core/video_pixel_format_utils.h"
#include "libav_pixel_format_mapping.h"
#include "raw_h264_encoder_backend.h"

namespace video_server
{
    namespace
    {

        constexpr uint8_t kNalTypeIdr = 5;
        constexpr uint8_t kNalTypeSps = 7;
        constexpr uint8_t kNalTypePps = 8;

        std::string av_error_string(int error_code)
        {
            char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(error_code, buffer, sizeof(buffer));
            return std::string(buffer);
        }

        bool start_code_at(const uint8_t *bytes, size_t size, size_t index, size_t *prefix_size)
        {
            if (index + 4 <= size && bytes[index] == 0x00 && bytes[index + 1] == 0x00 &&
                bytes[index + 2] == 0x00 && bytes[index + 3] == 0x01)
            {
                *prefix_size = 4;
                return true;
            }
            if (index + 3 <= size && bytes[index] == 0x00 && bytes[index + 1] == 0x00 && bytes[index + 2] == 0x01)
            {
                *prefix_size = 3;
                return true;
            }
            return false;
        }

        bool contains_nal_type(const uint8_t *data, size_t size, uint8_t nal_type)
        {
            for (size_t i = 0; i < size; ++i)
            {
                size_t prefix = 0;
                if (!start_code_at(data, size, i, &prefix))
                {
                    continue;
                }
                if (i + prefix < size && static_cast<uint8_t>(data[i + prefix] & 0x1f) == nal_type)
                {
                    return true;
                }
                i += prefix;
            }
            return false;
        }

        int64_t rescale_timestamp_ns_to_pts(uint64_t timestamp_ns, AVRational time_base)
        {
            if (time_base.num <= 0 || time_base.den <= 0)
            {
                return 0;
            }
            const int64_t den = static_cast<int64_t>(time_base.den);
            const int64_t num = static_cast<int64_t>(time_base.num);
            const long double scaled = (static_cast<long double>(timestamp_ns) * den) /
                                       (static_cast<long double>(num) * 1000000000.0L);
            if (scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()))
            {
                return std::numeric_limits<int64_t>::max();
            }
            if (scaled < static_cast<long double>(std::numeric_limits<int64_t>::min()))
            {
                return std::numeric_limits<int64_t>::min();
            }
            return static_cast<int64_t>(std::llround(scaled));
        }

        struct AvFrameDeleter
        {
            void operator()(AVFrame *frame) const
            {
                if (frame != nullptr)
                {
                    av_frame_free(&frame);
                }
            }
        };

        struct AvPacketDeleter
        {
            void operator()(AVPacket *packet) const
            {
                if (packet != nullptr)
                {
                    av_packet_free(&packet);
                }
            }
        };

        struct AvCodecContextDeleter
        {
            void operator()(AVCodecContext *ctx) const
            {
                if (ctx != nullptr)
                {
                    avcodec_free_context(&ctx);
                }
            }
        };

        struct SwsContextDeleter
        {
            void operator()(SwsContext *ctx) const
            {
                if (ctx != nullptr)
                {
                    sws_freeContext(ctx);
                }
            }
        };

        struct AvBsfContextDeleter
        {
            void operator()(AVBSFContext *ctx) const
            {
                if (ctx != nullptr)
                {
                    av_bsf_free(&ctx);
                }
            }
        };

        void set_error_value(std::string *error_message, const std::string &value)
        {
            if (error_message != nullptr)
            {
                *error_message = value;
            }
        }

        const AVCodec *find_libav_h264_encoder(RawH264Encoder encoder)
        {
            switch (encoder)
            {
            case RawH264Encoder::LibX264:
                return avcodec_find_encoder_by_name("libx264");
            case RawH264Encoder::LibOpenH264:
                return avcodec_find_encoder_by_name("libopenh264");
            case RawH264Encoder::Automatic:
            {
                const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
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

        class LibavH264EncoderBackend final : public IRawH264EncoderBackend
        {
        public:
            LibavH264EncoderBackend(std::string stream_id, RawVideoPipelineConfig config, EncodedAccessUnitSink sink)
                : stream_id_(std::move(stream_id)), config_(std::move(config)), sink_(std::move(sink)) {}

            ~LibavH264EncoderBackend() override { close(); }

            bool open(std::string *error_message = nullptr) override
            {
                close();

                const AVCodec *codec = find_libav_h264_encoder(config_.encoder);
                if (codec == nullptr)
                {
                    set_error_value(error_message,
                                    "failed to locate requested libav H264 encoder for mode '" + std::string(to_string(config_.encoder)) +
                                        "'");
                    return false;
                }

                std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codec_context(avcodec_alloc_context3(codec));
                if (!codec_context)
                {
                    set_error_value(error_message, "failed to allocate libav codec context");
                    return false;
                }

                const uint32_t output_width = config_.scale_mode == RawPipelineScaleMode::Resize && config_.output_width.has_value()
                                                  ? *config_.output_width
                                                  : config_.input_width;
                const uint32_t output_height = config_.scale_mode == RawPipelineScaleMode::Resize && config_.output_height.has_value()
                                                   ? *config_.output_height
                                                   : config_.input_height;
                const double configured_output_fps = config_.output_fps.value_or(config_.input_fps);
                AVRational frame_rate = av_d2q(configured_output_fps, 100000);
                if (frame_rate.num <= 0 || frame_rate.den <= 0)
                {
                    frame_rate = AVRational{static_cast<int>(std::lround(configured_output_fps * 1000.0)), 1000};
                }
                AVRational time_base = av_inv_q(frame_rate);

                codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
                codec_context->codec_id = codec->id;
                codec_context->width = static_cast<int>(output_width);
                codec_context->height = static_cast<int>(output_height);
                codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
                codec_context->time_base = time_base;
                codec_context->framerate = frame_rate;
                codec_context->gop_size = std::max(1, static_cast<int>(std::lround(configured_output_fps)));
                codec_context->max_b_frames = 0;
                codec_context->thread_count = 1;

                selected_encoder_name_ = codec->name != nullptr ? codec->name : "unknown";
                encoder_options_ = build_libav_h264_encoder_backend_options(config_, selected_encoder_name_);
                if (encoder_options_.family == RawH264EncoderFamily::OpenH264)
                {
                    codec_context->profile = AV_PROFILE_H264_MAIN;
                }
                if (encoder_options_.set_constrained_baseline_profile)
                {
                    codec_context->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE;
                }
                for (const auto &option : encoder_options_.private_options)
                {
                    av_opt_set(codec_context->priv_data, option.key.c_str(), option.value.c_str(), 0);
                }

                int rc = avcodec_open2(codec_context.get(), codec, nullptr);
                if (rc < 0)
                {
                    set_error_value(error_message, "failed to initialize libav H264 encoder '" + selected_encoder_name_ +
                                                       "': " + av_error_string(rc));
                    return false;
                }

                const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
                if (filter == nullptr)
                {
                    set_error_value(error_message, "failed to locate libav h264_mp4toannexb bitstream filter");
                    return false;
                }
                std::unique_ptr<AVBSFContext, AvBsfContextDeleter> bitstream_filter;
                AVBSFContext *raw_bsf = nullptr;
                rc = av_bsf_alloc(filter, &raw_bsf);
                if (rc < 0 || raw_bsf == nullptr)
                {
                    set_error_value(error_message, "failed to allocate libav bitstream filter context: " + av_error_string(rc));
                    return false;
                }
                bitstream_filter.reset(raw_bsf);
                rc = avcodec_parameters_from_context(bitstream_filter->par_in, codec_context.get());
                if (rc < 0)
                {
                    set_error_value(error_message, "failed to seed bitstream filter codec parameters: " + av_error_string(rc));
                    return false;
                }
                bitstream_filter->time_base_in = codec_context->time_base;
                rc = av_bsf_init(bitstream_filter.get());
                if (rc < 0)
                {
                    set_error_value(error_message, "failed to initialize libav h264_mp4toannexb filter: " + av_error_string(rc));
                    return false;
                }

                std::unique_ptr<AVFrame, AvFrameDeleter> input_frame(av_frame_alloc());
                std::unique_ptr<AVFrame, AvFrameDeleter> encoder_frame(av_frame_alloc());
                if (!input_frame || !encoder_frame)
                {
                    set_error_value(error_message, "failed to allocate libav frames");
                    return false;
                }

                const auto input_pixel_format = libav_pixel_format_from_video_pixel_format(config_.input_pixel_format);
                if (!input_pixel_format.has_value())
                {
                    set_error_value(error_message,
                                    "failed to map raw input pixel format '" + std::string(to_string(config_.input_pixel_format)) +
                                        "' into a libav pixel format");
                    return false;
                }
                input_frame->format = *input_pixel_format;
                input_frame->width = static_cast<int>(config_.input_width);
                input_frame->height = static_cast<int>(config_.input_height);

                encoder_frame->format = codec_context->pix_fmt;
                encoder_frame->width = codec_context->width;
                encoder_frame->height = codec_context->height;
                rc = av_frame_get_buffer(encoder_frame.get(), 32);
                if (rc < 0)
                {
                    set_error_value(error_message, "failed to allocate encoder frame buffer: " + av_error_string(rc));
                    return false;
                }

                std::unique_ptr<SwsContext, SwsContextDeleter> sws_context(sws_getContext(
                    static_cast<int>(config_.input_width), static_cast<int>(config_.input_height), *input_pixel_format,
                    codec_context->width, codec_context->height,
                    codec_context->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr));
                if (!sws_context)
                {
                    set_error_value(error_message, "failed to initialize libswscale conversion context");
                    return false;
                }

                std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
                std::unique_ptr<AVPacket, AvPacketDeleter> filtered_packet(av_packet_alloc());
                if (!packet || !filtered_packet)
                {
                    set_error_value(error_message, "failed to allocate libav packets");
                    return false;
                }

                next_frame_index_ = 0;
                codec_context_ = std::move(codec_context);
                bitstream_filter_ = std::move(bitstream_filter);
                input_frame_ = std::move(input_frame);
                encoder_frame_ = std::move(encoder_frame);
                sws_context_ = std::move(sws_context);
                packet_ = std::move(packet);
                filtered_packet_ = std::move(filtered_packet);
                return true;
            }

            bool encode_frame(const VideoFrameView &frame, std::string *error_message = nullptr) override
            {
                if (!is_open())
                {
                    set_error_value(error_message, "encoder backend is not open");
                    return false;
                }
                if (!populate_input_frame(frame, error_message))
                {
                    return false;
                }
                const int rc = avcodec_send_frame(codec_context_.get(), encoder_frame_.get());
                if (rc < 0)
                {
                    set_error_value(error_message, "libav encoder send_frame failed: " + av_error_string(rc));
                    return false;
                }
                return drain_encoder(error_message);
            }

            bool flush(std::string *error_message = nullptr) override
            {
                if (!is_open())
                {
                    return true;
                }
                const int rc = avcodec_send_frame(codec_context_.get(), nullptr);
                if (rc < 0 && rc != AVERROR_EOF)
                {
                    set_error_value(error_message, "libav encoder flush send_frame failed: " + av_error_string(rc));
                    return false;
                }
                return drain_encoder(error_message);
            }

            void close() override
            {
                filtered_packet_.reset();
                packet_.reset();
                sws_context_.reset();
                encoder_frame_.reset();
                input_frame_.reset();
                bitstream_filter_.reset();
                codec_context_.reset();
                selected_encoder_name_.clear();
                encoder_options_ = {};
            }

            bool is_open() const override { return codec_context_ != nullptr; }
            const char *backend_name() const override { return "libav_h264"; }
            const char *encoder_name() const override { return selected_encoder_name_.c_str(); }
            RawH264EncoderFamily encoder_family() const override { return encoder_options_.family; }

        private:
            bool populate_input_frame(const VideoFrameView &frame, std::string *error_message)
            {
                input_frame_->pts = static_cast<int64_t>(next_frame_index_);
                const uint8_t *data = static_cast<const uint8_t *>(frame.data);
                const int width = static_cast<int>(frame.width);
                const int height = static_cast<int>(frame.height);
                switch (frame.pixel_format)
                {
                case VideoPixelFormat::RGB24:
                case VideoPixelFormat::BGR24:
                case VideoPixelFormat::RGBA32:
                case VideoPixelFormat::BGRA32:
                case VideoPixelFormat::GRAY8:
                case VideoPixelFormat::GRAY10LE:
                case VideoPixelFormat::GRAY12LE:
                case VideoPixelFormat::GRAY16LE:
                    input_frame_->data[0] = const_cast<uint8_t *>(data);
                    input_frame_->linesize[0] = static_cast<int>(frame.stride_bytes);
                    break;
                case VideoPixelFormat::NV12:
                {
                    const int y_stride = width;
                    const int uv_stride = width;
                    input_frame_->data[0] = const_cast<uint8_t *>(data);
                    input_frame_->linesize[0] = y_stride;
                    input_frame_->data[1] = const_cast<uint8_t *>(data + static_cast<size_t>(width) * height);
                    input_frame_->linesize[1] = uv_stride;
                    break;
                }
                case VideoPixelFormat::I420:
                {
                    const int y_stride = width;
                    const int uv_stride = width / 2;
                    const size_t y_plane = static_cast<size_t>(width) * height;
                    const size_t u_plane = y_plane / 4u;
                    input_frame_->data[0] = const_cast<uint8_t *>(data);
                    input_frame_->linesize[0] = y_stride;
                    input_frame_->data[1] = const_cast<uint8_t *>(data + y_plane);
                    input_frame_->linesize[1] = uv_stride;
                    input_frame_->data[2] = const_cast<uint8_t *>(data + y_plane + u_plane);
                    input_frame_->linesize[2] = uv_stride;
                    break;
                }
                }
                int rc = av_frame_make_writable(encoder_frame_.get());
                if (rc < 0)
                {
                    set_error_value(error_message, "failed to prepare encoder frame buffer: " + av_error_string(rc));
                    return false;
                }
                rc = sws_scale(sws_context_.get(), input_frame_->data, input_frame_->linesize, 0, height, encoder_frame_->data,
                               encoder_frame_->linesize);
                if (rc <= 0)
                {
                    set_error_value(error_message, "libswscale failed to convert raw frame into encoder format");
                    return false;
                }
                encoder_frame_->pts = rescale_timestamp_ns_to_pts(frame.timestamp_ns, codec_context_->time_base);
                if (encoder_frame_->pts < 0)
                {
                    encoder_frame_->pts = static_cast<int64_t>(next_frame_index_);
                }
                ++next_frame_index_;
                return true;
            }

            bool drain_encoder(std::string *error_message)
            {
                while (true)
                {
                    int rc = avcodec_receive_packet(codec_context_.get(), packet_.get());
                    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    {
                        return true;
                    }
                    if (rc < 0)
                    {
                        set_error_value(error_message, "libav encoder receive_packet failed: " + av_error_string(rc));
                        return false;
                    }
                    const bool ok = submit_packet(packet_.get(), error_message);
                    av_packet_unref(packet_.get());
                    if (!ok)
                    {
                        return false;
                    }
                }
            }

            bool submit_packet(AVPacket *packet, std::string *error_message)
            {
                int rc = av_bsf_send_packet(bitstream_filter_.get(), packet);
                if (rc < 0)
                {
                    set_error_value(error_message,
                                    "failed to push H264 packet through Annex-B bitstream filter: " + av_error_string(rc));
                    return false;
                }
                while (true)
                {
                    rc = av_bsf_receive_packet(bitstream_filter_.get(), filtered_packet_.get());
                    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    {
                        return true;
                    }
                    if (rc < 0)
                    {
                        set_error_value(error_message, "failed to read Annex-B packet from bitstream filter: " + av_error_string(rc));
                        return false;
                    }
                    if (!emit_access_unit(filtered_packet_.get(), error_message))
                    {
                        av_packet_unref(filtered_packet_.get());
                        return false;
                    }
                    av_packet_unref(filtered_packet_.get());
                }
            }

            bool emit_access_unit(const AVPacket *packet, std::string *error_message)
            {
                EncodedAccessUnitView view{};
                view.data = packet->data;
                view.size_bytes = static_cast<size_t>(packet->size);
                view.codec = VideoCodec::H264;
                view.timestamp_ns = static_cast<uint64_t>(av_rescale_q(packet->pts, codec_context_->time_base,
                                                                       AVRational{1, 1000000000}));
                view.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0 || contains_nal_type(packet->data, view.size_bytes, kNalTypeIdr);
                view.codec_config = contains_nal_type(packet->data, view.size_bytes, kNalTypeSps) &&
                                    contains_nal_type(packet->data, view.size_bytes, kNalTypePps) && !view.keyframe;
                if (!sink_(view))
                {
                    set_error_value(error_message,
                                    "encoded access-unit sink rejected H264 access unit for stream '" + stream_id_ +
                                        "'; pipeline will stop and reject later push_frame() calls");
                    return false;
                }
                return true;
            }

            const std::string stream_id_;
            const RawVideoPipelineConfig config_;
            EncodedAccessUnitSink sink_;

            uint64_t next_frame_index_{0};
            std::string selected_encoder_name_;
            RawH264EncoderBackendOptions encoder_options_;
            std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codec_context_;
            std::unique_ptr<AVBSFContext, AvBsfContextDeleter> bitstream_filter_;
            std::unique_ptr<AVFrame, AvFrameDeleter> input_frame_;
            std::unique_ptr<AVFrame, AvFrameDeleter> encoder_frame_;
            std::unique_ptr<SwsContext, SwsContextDeleter> sws_context_;
            std::unique_ptr<AVPacket, AvPacketDeleter> packet_;
            std::unique_ptr<AVPacket, AvPacketDeleter> filtered_packet_;
        };

        class RawToH264Pipeline final : public IRawVideoPipeline
        {
        public:
            RawToH264Pipeline(std::string stream_id, RawVideoPipelineConfig config, EncodedAccessUnitSink sink)
                : stream_id_(std::move(stream_id)),
                  config_(std::move(config)),
                  sink_(std::move(sink)),
                  encoder_backend_(make_raw_h264_encoder_backend(stream_id_, config_, sink_)) {}

            ~RawToH264Pipeline() override { stop(); }

            const std::string &stream_id() const override { return stream_id_; }

            bool start(std::string *error_message = nullptr) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (running_)
                {
                    return true;
                }
                if (!validate_config(error_message))
                {
                    return false;
                }
                if (!encoder_backend_->open(error_message))
                {
                    set_error_locked(error_message != nullptr ? *error_message : std::string("failed to open encoder backend"), error_message);
                    return false;
                }
                failed_ = false;
                last_error_.clear();
                fps_filter_interval_ns_ = config_.output_fps.has_value()
                                              ? static_cast<uint64_t>(std::llround(1000000000.0 / *config_.output_fps))
                                              : 0;
                next_allowed_timestamp_ns_ = 0;
                running_ = true;
                return true;
            }

            bool push_frame(const VideoFrameView &frame, std::string *error_message = nullptr) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_)
                {
                    set_error_value(error_message, failed_ && !last_error_.empty() ? last_error_ : "pipeline is not running");
                    return false;
                }
                if (failed_)
                {
                    set_error_value(error_message, last_error_);
                    return false;
                }
                if (!validate_frame(frame, error_message))
                {
                    return false;
                }
                if (should_drop_frame_locked(frame.timestamp_ns))
                {
                    return true;
                }
                if (!encoder_backend_->encode_frame(frame, error_message))
                {
                    set_error_locked(error_message != nullptr ? *error_message : std::string("encoder backend failed"), error_message);
                    fail_pipeline_locked();
                    return false;
                }
                return true;
            }

            void stop() override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_ && !encoder_backend_->is_open())
                {
                    return;
                }
                if (!failed_ && encoder_backend_->is_open())
                {
                    std::string ignored_error;
                    if (!encoder_backend_->flush(&ignored_error))
                    {
                        set_error_locked(ignored_error, nullptr);
                        fail_pipeline_locked();
                    }
                }
                encoder_backend_->close();
                running_ = false;
            }

        private:
            bool validate_config(std::string *error_message)
            {
                if (stream_id_.empty())
                {
                    set_error_locked("stream_id is required", error_message);
                    return false;
                }
                if (!sink_)
                {
                    set_error_locked("encoded access-unit sink is required", error_message);
                    return false;
                }
                if (config_.input_width == 0 || config_.input_height == 0 || config_.input_fps <= 0.0)
                {
                    set_error_locked("input dimensions and fps must be valid", error_message);
                    return false;
                }
                std::string pixel_format_error;
                if (!validate_libav_pixel_format(config_.input_pixel_format, &pixel_format_error))
                {
                    set_error_locked(pixel_format_error, error_message);
                    return false;
                }
                if (config_.scale_mode == RawPipelineScaleMode::Resize)
                {
                    if (!config_.output_width.has_value() || !config_.output_height.has_value() || *config_.output_width == 0 ||
                        *config_.output_height == 0)
                    {
                        set_error_locked("resize mode requires output width and height", error_message);
                        return false;
                    }
                }
                if (config_.output_fps.has_value() && *config_.output_fps <= 0.0)
                {
                    set_error_locked("output_fps must be > 0 when provided", error_message);
                    return false;
                }
                return true;
            }

            bool validate_frame(const VideoFrameView &frame, std::string *error_message) const
            {
                if (frame.data == nullptr || frame.width != config_.input_width || frame.height != config_.input_height ||
                    frame.pixel_format != config_.input_pixel_format)
                {
                    set_error_value(error_message, "frame does not match pipeline input contract");
                    return false;
                }
                if (!video_pixel_format_is_planar(frame.pixel_format))
                {
                    const uint32_t min_stride = video_pixel_format_min_row_bytes(frame.pixel_format, config_.input_width);
                    if (frame.stride_bytes < min_stride)
                    {
                        set_error_value(error_message, "frame stride is too small");
                        return false;
                    }
                    return true;
                }
                if (frame.stride_bytes != 0)
                {
                    set_error_value(error_message, "planar frame views must use tightly packed default layout (stride_bytes=0)");
                    return false;
                }
                return true;
            }

            bool should_drop_frame_locked(uint64_t timestamp_ns)
            {
                if (fps_filter_interval_ns_ == 0)
                {
                    return false;
                }
                if (next_allowed_timestamp_ns_ == 0 || timestamp_ns >= next_allowed_timestamp_ns_)
                {
                    next_allowed_timestamp_ns_ = timestamp_ns + fps_filter_interval_ns_;
                    return false;
                }
                return true;
            }

            void fail_pipeline_locked()
            {
                failed_ = true;
                running_ = false;
                encoder_backend_->close();
            }

            void set_error_locked(const std::string &value, std::string *error_message)
            {
                last_error_ = value;
                set_error_value(error_message, value);
            }

            const std::string stream_id_;
            const RawVideoPipelineConfig config_;
            EncodedAccessUnitSink sink_;
            std::unique_ptr<IRawH264EncoderBackend> encoder_backend_;

            mutable std::mutex mutex_;
            bool running_{false};
            bool failed_{false};
            std::string last_error_;
            uint64_t fps_filter_interval_ns_{0};
            uint64_t next_allowed_timestamp_ns_{0};
        };

    } // namespace

    RawH264EncoderBackendOptions build_libav_h264_encoder_backend_options(const RawVideoPipelineConfig &config,
                                                                          const std::string &codec_name)
    {
        RawH264EncoderBackendOptions options;
        if (codec_name.find("x264") != std::string::npos)
        {
            options.family = RawH264EncoderFamily::X264;
            options.profile_name = config.encoder_profile;
            options.private_options.push_back({"preset", config.encoder_preset});
            options.private_options.push_back({"tune", config.encoder_tune});
            options.private_options.push_back({"profile", config.encoder_profile});
            options.private_options.push_back({"scenecut", "0"});
            options.private_options.push_back({"repeat-headers", config.repeat_headers ? "1" : "0"});
            options.private_options.push_back({"aud", config.emit_access_unit_delimiters ? "1" : "0"});
            return options;
        }
        if (codec_name.find("openh264") != std::string::npos)
        {
            options.family = RawH264EncoderFamily::OpenH264;
            options.private_options.push_back({"allow_skip_frames", "1"});
            return options;
        }
        options.family = RawH264EncoderFamily::Generic;
        return options;
    }

    std::unique_ptr<IRawH264EncoderBackend> make_raw_h264_encoder_backend(std::string stream_id,
                                                                          RawVideoPipelineConfig config,
                                                                          EncodedAccessUnitSink sink)
    {
        return std::make_unique<LibavH264EncoderBackend>(std::move(stream_id), std::move(config), std::move(sink));
    }

    std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline(std::string stream_id,
                                                                 RawVideoPipelineConfig config,
                                                                 EncodedAccessUnitSink sink)
    {
        return std::make_unique<RawToH264Pipeline>(std::move(stream_id), std::move(config), std::move(sink));
    }

    std::unique_ptr<IRawVideoPipeline> make_raw_to_h264_pipeline_for_server(std::string stream_id,
                                                                            RawVideoPipelineConfig config,
                                                                            IVideoServer &server)
    {
        std::string target_stream_id = stream_id;
        return make_raw_to_h264_pipeline(
            std::move(stream_id), std::move(config),
            [&server, target_stream_id = std::move(target_stream_id)](const EncodedAccessUnitView &access_unit)
            {
                return server.push_access_unit(target_stream_id, access_unit);
            });
    }

    const char *to_string(RawPipelineScaleMode scale_mode)
    {
        switch (scale_mode)
        {
        case RawPipelineScaleMode::Passthrough:
            return "Passthrough";
        case RawPipelineScaleMode::Resize:
            return "Resize";
        }
        return "Unknown";
    }

    const char *to_string(RawH264Encoder encoder)
    {
        switch (encoder)
        {
        case RawH264Encoder::Automatic:
            return "Automatic";
        case RawH264Encoder::LibX264:
            return "LibX264";
        case RawH264Encoder::LibOpenH264:
            return "LibOpenH264";
        }
        return "Unknown";
    }

    const char *to_string(RawH264EncoderFamily family)
    {
        switch (family)
        {
        case RawH264EncoderFamily::X264:
            return "X264";
        case RawH264EncoderFamily::OpenH264:
            return "OpenH264";
        case RawH264EncoderFamily::Generic:
            return "Generic";
        }
        return "Unknown";
    }

} // namespace video_server
