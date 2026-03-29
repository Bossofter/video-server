#pragma once

#include <mutex>

#include <spdlog/spdlog.h>

namespace video_server
{

    /**
     * @brief Installs the repo's default spdlog configuration once per process.
     */
    inline void ensure_default_logging_config()
    {
        static std::once_flag once;
        std::call_once(once, []()
                       {
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::warn); });
    }

} // namespace video_server
