#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/observability.h"
#include "video_server/raw_video_pipeline.h"
#include "video_server/webrtc_video_server.h"

namespace video_server
{

    /**
     * @brief Higher-level config for the managed producer-facing server flow.
     */
    struct ManagedVideoServerConfig
    {
        ExecutionMode execution_mode{ExecutionMode::ManualStep}; /**< Execution policy for progression. */
        WebRtcVideoServerConfig webrtc;                          /**< WebRTC/HTTP backend settings. */
        std::vector<StreamConfig> streams;                       /**< Streams loaded during startup. */
        std::unordered_map<std::string, RawVideoPipelineConfig> default_raw_pipelines; /**< Default per-stream pipeline configs. */
        uint32_t http_poll_timeout_ms{5};                        /**< HTTP pump timeout used by stepped progression. */
        size_t max_streams_per_step{0};                          /**< Optional cap on streams processed per step, or zero for all. */
    };

    /**
     * @brief Managed producer-facing server API with explicit progression support.
     */
    class IManagedVideoServer : public IVideoServer
    {
    public:
        ~IManagedVideoServer() override = default;

        /**
         * @brief Starts the managed server and registers configured streams.
         *
         * @return True when startup succeeded, false otherwise.
         */
        virtual bool start() = 0;

        /**
         * @brief Stops the managed server and owned resources.
         */
        virtual void stop() = 0;

        /**
         * @brief Progresses owned subsystems once according to the active execution policy.
         *
         * @return True when progression completed without fatal failure, false otherwise.
         */
        virtual bool step() = 0;

        /**
         * @brief Returns the current observability snapshot for the managed server.
         *
         * @return Server-wide observability snapshot.
         */
        virtual ServerDebugSnapshot get_debug_snapshot() const = 0;
    };

    /**
     * @brief Loads a managed server config from a TOML file.
     *
     * @param config_path Path to the config file.
     * @return Parsed managed server configuration.
     * @throws std::runtime_error on parse or validation failure.
     */
    ManagedVideoServerConfig load_managed_video_server_config(const std::string &config_path);

    /**
     * @brief Creates a managed server from a TOML config file.
     *
     * @param config_path Path to the config file.
     * @return Managed server instance.
     */
    std::unique_ptr<IManagedVideoServer> CreateManagedVideoServer(const std::string &config_path);

    /**
     * @brief Creates a managed server from an in-memory config.
     *
     * @param config Managed server configuration.
     * @return Managed server instance.
     */
    std::unique_ptr<IManagedVideoServer> CreateManagedVideoServer(ManagedVideoServerConfig config);

} // namespace video_server
