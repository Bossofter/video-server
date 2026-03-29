#pragma once

#include <cstdint>
#include <vector>

#include "video_server/stream_config.h"
#include "video_server/video_frame_view.h"

namespace video_server
{

    /**
     * @brief Synthetic frame generator used by the smoke server and soak tests.
     */
    class SyntheticFrameGenerator
    {
    public:
        /**
         * @brief Creates a generator bound to one stream definition.
         *
         * @param config Stream configuration used to shape generated frames.
         */
        explicit SyntheticFrameGenerator(StreamConfig config);

        /**
         * @brief Returns the stream config used by the generator.
         *
         * @return Reference to the generator's stream configuration.
         */
        const StreamConfig &config() const { return config_; }

        /**
         * @brief Produces the next synthetic frame view.
         *
         * @return Non-owning view of the newly generated frame data.
         */
        VideoFrameView next_frame();

    private:
        /**
         * @brief Internal pattern variants used to differentiate streams visually.
         */
        enum class PatternVariant
        {
            GradientOrbit,
            CheckerPulse,
            DiagonalSweep,
        };

        /**
         * @brief Selects the pattern variant for the current stream.
         *
         * @return Pattern variant associated with the configured stream.
         */
        PatternVariant pattern_variant() const;

        /**
         * @brief Derives a deterministic channel seed from the stream identity.
         *
         * @param channel Color channel index.
         * @return Seed value used by the synthetic pattern generator.
         */
        uint8_t channel_seed(size_t channel) const;

        StreamConfig config_;
        std::vector<uint8_t> buffer_;
        uint64_t frame_counter_{0};
        uint32_t stream_seed_{0};
    };

} // namespace video_server
