#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace video_server::webrtc_internal
{
    /**
     * @brief Bounded realtime handoff for encoded H264 units.
     *
     * Codec configuration is retained separately from disposable frame backlog. The backlog keeps the newest keyframe
     * span it has seen and drops stale inter frames first so API-side draining can still start from decoder-safe state.
     */
    template <typename Unit>
    class EncodedAccessUnitHandoff
    {
    public:
        explicit EncodedAccessUnitHandoff(size_t max_units = 4) : max_units_(std::max<size_t>(1, max_units)) {}

        void push(Unit unit)
        {
            if (unit.codec_config)
            {
                cached_codec_config_ = std::move(unit);
                return;
            }

            if (unit.keyframe)
            {
                dropped_ += backlog_.size();
                backlog_.clear();
                backlog_.push_back(std::move(unit));
                return;
            }

            if (backlog_.empty())
            {
                backlog_.push_back(std::move(unit));
                return;
            }

            while (backlog_.size() >= max_units_)
            {
                auto drop_it = backlog_.begin();
                if (drop_it->keyframe)
                {
                    if (backlog_.size() == 1)
                    {
                        ++dropped_;
                        return;
                    }
                    drop_it = std::next(backlog_.begin());
                }
                backlog_.erase(drop_it);
                ++dropped_;
            }
            backlog_.push_back(std::move(unit));
        }

        std::vector<Unit> drain()
        {
            std::vector<Unit> units;
            const bool include_codec_config =
                cached_codec_config_.has_value() &&
                std::any_of(backlog_.begin(), backlog_.end(),
                            [](const Unit &unit)
                            { return unit.keyframe; });
            units.reserve(backlog_.size() + (include_codec_config ? 1u : 0u));
            if (include_codec_config)
            {
                units.push_back(*cached_codec_config_);
            }
            while (!backlog_.empty())
            {
                units.push_back(std::move(backlog_.front()));
                backlog_.pop_front();
            }
            return units;
        }

        void clear()
        {
            cached_codec_config_.reset();
            backlog_.clear();
        }

        uint64_t dropped_count() const { return dropped_; }
        size_t pending_count() const { return backlog_.size(); }

    private:
        size_t max_units_{4};
        std::optional<Unit> cached_codec_config_;
        std::deque<Unit> backlog_;
        uint64_t dropped_{0};
    };

} // namespace video_server::webrtc_internal
