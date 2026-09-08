#include "raw_frame_statistics.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "video_pixel_format_utils.h"

namespace video_server
{
    namespace
    {
        uint8_t normalized_grayscale(uint16_t sample, uint32_t maximum)
        {
            const uint32_t clamped = std::min<uint32_t>(sample, maximum);
            return static_cast<uint8_t>((clamped * 255u + maximum / 2u) / maximum);
        }

        uint8_t rgb_luminance(uint8_t r, uint8_t g, uint8_t b)
        {
            // Integer Rec. 709 coefficients, rounded to the nearest 8-bit value.
            return static_cast<uint8_t>((54u * r + 183u * g + 19u * b + 128u) >> 8u);
        }

        uint8_t percentile(const std::array<uint64_t, 256> &histogram, uint64_t count, uint64_t numerator)
        {
            const uint64_t rank = std::max<uint64_t>(1u, (count * numerator + 99u) / 100u);
            uint64_t cumulative = 0;
            for (size_t bin = 0; bin < histogram.size(); ++bin)
            {
                cumulative += histogram[bin];
                if (cumulative >= rank)
                {
                    return static_cast<uint8_t>(bin);
                }
            }
            return 255u;
        }
    } // namespace

    bool calculate_raw_frame_statistics(const VideoFrameView &frame, RawFrameStatistics &statistics)
    {
        statistics = {};
        if (frame.data == nullptr || frame.width == 0 || frame.height == 0)
        {
            return false;
        }

        const auto *descriptor = find_video_pixel_format_descriptor(frame.pixel_format);
        if (descriptor == nullptr)
        {
            return false;
        }

        const bool planar_luma =
            frame.pixel_format == VideoPixelFormat::NV12 || frame.pixel_format == VideoPixelFormat::I420;
        const uint32_t minimum_stride =
            planar_luma ? frame.width : video_pixel_format_min_row_bytes(frame.pixel_format, frame.width);
        if (minimum_stride == 0 || frame.stride_bytes < minimum_stride)
        {
            return false;
        }

        const auto *input = static_cast<const uint8_t *>(frame.data);
        uint64_t sum = 0;
        long double sum_squares = 0.0L;
        uint8_t minimum = std::numeric_limits<uint8_t>::max();
        uint8_t maximum = 0;

        for (uint32_t y = 0; y < frame.height; ++y)
        {
            const uint8_t *row = input + static_cast<size_t>(y) * frame.stride_bytes;
            for (uint32_t x = 0; x < frame.width; ++x)
            {
                uint8_t luminance = 0;
                if (descriptor->grayscale)
                {
                    luminance = normalized_grayscale(read_grayscale_sample(row, x, frame.pixel_format),
                                                     grayscale_sample_max(frame.pixel_format));
                }
                else if (planar_luma)
                {
                    luminance = row[x];
                }
                else
                {
                    const size_t offset = static_cast<size_t>(x) * descriptor->packed_bytes_per_pixel;
                    uint8_t r = 0;
                    uint8_t g = row[offset + 1u];
                    uint8_t b = 0;
                    if (frame.pixel_format == VideoPixelFormat::RGB24 || frame.pixel_format == VideoPixelFormat::RGBA32)
                    {
                        r = row[offset];
                        b = row[offset + 2u];
                    }
                    else
                    {
                        b = row[offset];
                        r = row[offset + 2u];
                    }
                    luminance = rgb_luminance(r, g, b);
                }

                ++statistics.luminance_histogram[luminance];
                sum += luminance;
                sum_squares += static_cast<long double>(luminance) * luminance;
                minimum = std::min(minimum, luminance);
                maximum = std::max(maximum, luminance);
            }
        }

        const uint64_t count = static_cast<uint64_t>(frame.width) * frame.height;
        if (count == 0)
        {
            return false;
        }
        const double mean = static_cast<double>(sum) / static_cast<double>(count);
        const long double variance = std::max(0.0L, sum_squares / count - static_cast<long double>(mean) * mean);
        const uint8_t p01 = percentile(statistics.luminance_histogram, count, 1u);
        const uint8_t p50 = percentile(statistics.luminance_histogram, count, 50u);
        const uint8_t p99 = percentile(statistics.luminance_histogram, count, 99u);
        uint64_t dark_count = 0;
        uint64_t bright_count = 0;
        for (size_t bin = 0; bin <= 12u; ++bin)
        {
            dark_count += statistics.luminance_histogram[bin];
        }
        for (size_t bin = 243u; bin < statistics.luminance_histogram.size(); ++bin)
        {
            bright_count += statistics.luminance_histogram[bin];
        }

        statistics.available = true;
        statistics.frame_id = frame.frame_id;
        statistics.timestamp_ns = frame.timestamp_ns;
        statistics.width = frame.width;
        statistics.height = frame.height;
        statistics.pixel_format = to_string(frame.pixel_format);
        statistics.pixel_count = count;
        statistics.luminance_min = minimum;
        statistics.luminance_max = maximum;
        statistics.luminance_mean = mean;
        statistics.luminance_stddev = std::sqrt(static_cast<double>(variance));
        statistics.luminance_p01 = p01;
        statistics.luminance_p50 = p50;
        statistics.luminance_p99 = p99;
        statistics.dynamic_range = static_cast<double>(p99) - p01;
        statistics.dark_pixel_fraction = static_cast<double>(dark_count) / count;
        statistics.bright_pixel_fraction = static_cast<double>(bright_count) / count;
        return true;
    }

} // namespace video_server
