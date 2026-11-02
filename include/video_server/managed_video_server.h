#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/observability.h"
#include "video_server/raw_video_pipeline.h"
#include "video_server/webrtc_video_server.h"

namespace video_server
{
    /**
     * @brief API progression execution policy for the managed server.
     */
    enum class ManagedApiExecutionMode
    {
        ManualStep,
        ManagedThread
    };

    /**
     * @brief Optional placement controls for the managed API progression thread.
     */
    struct ManagedApiThreadConfig
    {
        std::vector<int> cpu_affinity;              /**< Optional CPU ids for the managed API thread. Empty leaves affinity unchanged. */
        std::optional<int> scheduling_priority;     /**< Optional scheduler priority. Requires platform support and permission. */
    };

    /**
     * @brief API progression configuration for the managed server.
     */
    struct ManagedApiExecutionConfig
    {
        ManagedApiExecutionMode mode{ManagedApiExecutionMode::ManualStep};
        ManagedApiThreadConfig thread;
    };

    /**
     * @brief Higher-level config for the managed producer-facing server flow.
     */
    struct ManagedVideoServerConfig
    {
        static WebRtcVideoServerConfig default_webrtc_config()
        {
            WebRtcVideoServerConfig config;
            config.execution_mode = ExecutionMode::ManualStep;
            config.http_poll_timeout_ms = 5;
            return config;
        }

        ExecutionMode execution_mode{ExecutionMode::ManualStep}; /**< Execution policy for progression. */
        ManagedApiExecutionConfig api_execution;                 /**< API/HTTP/WebRTC progression policy. */
        WebRtcVideoServerConfig webrtc{default_webrtc_config()}; /**< WebRTC/HTTP backend settings. */
        std::vector<StreamConfig> streams;                       /**< Streams loaded during startup. */
        std::unordered_map<std::string, RawVideoPipelineConfig> default_raw_pipelines; /**< Default per-stream pipeline configs. */
        uint32_t http_poll_timeout_ms{5};                        /**< HTTP pump timeout used by stepped progression. */
        size_t max_streams_per_step{0};                          /**< Optional cap on streams processed per step, or zero for all. */
    };

    /**
     * @brief Exclusive handle for stepping a caller-defined set of managed streams.
     */
    class IManagedStreamGroup
    {
    public:
        virtual ~IManagedStreamGroup() = default;

        /**
         * @brief Advances only the streams owned by this group once on the calling thread.
         *
         * @return Success or a typed contention/progression error.
         */
        virtual VideoServerResult step_streams() = 0;

        /**
         * @brief Returns the stream ids owned by this group.
         *
         * @return Owned stream identifiers.
         */
        virtual std::vector<std::string> stream_ids() const = 0;
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
         * @return Success or a typed lifecycle/configuration/backend error.
         */
        virtual VideoServerResult start() = 0;

        /**
         * @brief Stops the managed server and owned resources.
         */
        virtual void stop() = 0;

        /**
         * @brief Progresses owned subsystems once according to the active execution policy.
         *
         * @return Success or a typed API/encoder/backend error.
         */
        virtual VideoServerResult step() = 0;

        /**
         * @brief Progresses API, HTTP, signaling, WebRTC, transport, and encoded-output delivery once.
         *
         * @return Success or a typed execution-mode/contention/backend error.
         */
        virtual VideoServerResult step_api() = 0;

        /**
         * @brief Registers an exclusive caller-stepped group of managed streams.
         *
         * @param stream_ids Stream identifiers to assign atomically to the returned handle.
         * @return Stream group handle, or nullptr when validation fails.
         */
        virtual std::shared_ptr<IManagedStreamGroup> register_streams(std::vector<std::string> stream_ids) = 0;

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
