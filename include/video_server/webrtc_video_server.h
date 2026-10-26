#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "video_server/observability.h"
#include "video_server/video_server.h"

namespace video_server
{
    /**
     * @brief One browser/server ICE server entry.
     */
    struct WebRtcIceServerConfig
    {
        std::string uri;        /**< STUN/TURN URI, for example stun:host:3478 or turn:host:3478?transport=tcp. */
        std::string username;   /**< Optional TURN username. */
        std::string credential; /**< Optional TURN credential/password. */
    };

    /**
     * @brief ICE configuration shared by native server peer connections and browser clients.
     */
    struct WebRtcIceConfig
    {
        std::vector<WebRtcIceServerConfig> servers;            /**< Configured STUN/TURN servers. */
        std::string transport_policy{"all"};                  /**< "all" or "relay". */
        std::optional<std::string> bind_address;               /**< Optional local ICE bind address. */
        std::optional<uint16_t> port_range_begin;              /**< Optional local ICE port range start. */
        std::optional<uint16_t> port_range_end;                /**< Optional local ICE port range end. */
    };

    /**
     * @brief WebRTC and HTTP server configuration for the browser delivery backend.
     */
    struct WebRtcVideoServerConfig
    {
        std::string http_host{"127.0.0.1"};               /**< HTTP bind host. */
        uint16_t http_port{8080};                         /**< HTTP bind port. */
        bool enable_http_api{true};                       /**< Enables the HTTP API surface when true. */
        bool enable_debug_api{false};                     /**< Enables the debug API when true. */
        bool enable_runtime_config_api{true};             /**< Enables runtime output config routes when true. */
        bool allow_unsafe_public_routes{false};           /**< Allows sensitive routes on non-loopback binds when true. */
        bool enable_shared_key_auth{false};               /**< Enables shared-key protection for sensitive routes when true. */
        std::string shared_key;                           /**< Shared key accepted by protected routes. */
        std::vector<std::string> ip_allowlist;            /**< Allowed remote IPs or CIDR ranges. */
        std::vector<std::string> cors_allowed_origins;    /**< Explicit CORS origin allowlist. */
        size_t max_http_request_bytes{262144};            /**< Maximum accepted HTTP request size. */
        size_t max_json_body_bytes{16384};                /**< Maximum accepted JSON body size. */
        size_t max_signaling_sdp_bytes{131072};           /**< Maximum accepted signaling SDP size. */
        size_t max_signaling_candidate_bytes{8192};       /**< Maximum accepted ICE candidate size. */
        size_t max_pending_candidates_per_stream{32};     /**< Maximum queued candidates before a session exists. */
        uint32_t signaling_rate_limit_window_seconds{10}; /**< Signaling rate-limit window in seconds. */
        uint32_t signaling_rate_limit_max_requests{60};   /**< Maximum signaling requests per rate-limit window. */
        uint32_t config_rate_limit_window_seconds{10};    /**< Config rate-limit window in seconds. */
        uint32_t config_rate_limit_max_requests{20};      /**< Maximum config requests per rate-limit window. */
        uint32_t debug_rate_limit_window_seconds{10};     /**< Debug rate-limit window in seconds. */
        uint32_t debug_rate_limit_max_requests{60};       /**< Maximum debug requests per rate-limit window. */
        WebRtcIceConfig ice;                              /**< ICE/STUN/TURN configuration for server and clients. */
        ExecutionMode execution_mode{ExecutionMode::WorkerThread}; /**< Execution policy for HTTP progression. */
        uint32_t http_poll_timeout_ms{200};                         /**< Poll timeout used by HTTP worker or stepped pump. */
        size_t max_http_connections_per_step{1};                   /**< Maximum accepted connections serviced per step. */
    };

    /**
     * @brief Lightweight HTTP response container used by test helpers.
     */
    struct WebRtcHttpResponse
    {
        int status{200};                                      /**< HTTP status code. */
        std::string body;                                     /**< Response body payload. */
        std::unordered_map<std::string, std::string> headers; /**< Response headers. */
    };

    /**
     * @brief IVideoServer implementation with HTTP and WebRTC browser delivery support.
     */
    class WebRtcVideoServer : public IVideoServer
    {
    public:
        /**
         * @brief Creates a server instance with the supplied configuration.
         *
         * @param config WebRTC and HTTP backend configuration.
         */
        explicit WebRtcVideoServer(WebRtcVideoServerConfig config = {});
        ~WebRtcVideoServer() override;

        WebRtcVideoServer(const WebRtcVideoServer &) = delete;
        WebRtcVideoServer &operator=(const WebRtcVideoServer &) = delete;

        /**
         * @brief Starts the HTTP and backend services.
         *
         * @return Success or a typed configuration/backend error.
         */
        VideoServerResult start();

        /**
         * @brief Progresses owned HTTP/network work once when using stepped execution.
         *
         * @return Success or a typed backend error.
         */
        VideoServerResult step();

        /**
         * @brief Stops the server and releases active sessions.
         */
        void stop();

        VideoServerResult register_stream(const StreamConfig &config) override;
        VideoServerResult remove_stream(const std::string &stream_id) override;
        VideoServerResult push_frame(const std::string &stream_id, const VideoFrameView &frame) override;
        VideoServerResult push_access_unit(const std::string &stream_id, const EncodedAccessUnitView &access_unit) override;
        std::vector<VideoStreamInfo> list_streams() const override;
        std::optional<VideoStreamInfo> get_stream_info(const std::string &stream_id) const override;
        VideoServerResult set_stream_output_config(const std::string &stream_id,
                                                   const StreamOutputConfig &output_config) override;
        std::optional<StreamOutputConfig> get_stream_output_config(const std::string &stream_id) const override;

        /**
         * @brief Validates a raw input frame against the registered stream contract.
         *
         * @param stream_id Identifier of the target stream.
         * @param frame Raw frame view to validate.
         * @param config_out Optional destination for the registered stream config.
         * @param output_config_out Optional destination for the active output config.
         * @return Success or a typed stream/frame validation error.
         */
        VideoServerResult validate_raw_frame_input(const std::string &stream_id,
                                                   const VideoFrameView &frame,
                                                   StreamConfig *config_out = nullptr,
                                                   StreamOutputConfig *output_config_out = nullptr) const;

        /**
         * @brief Records receipt of one valid raw input frame.
         *
         * @param stream_id Identifier of the target stream.
         * @param timestamp_ns Frame timestamp in nanoseconds.
         * @return Success or `NotFound` when the stream does not exist.
         */
        VideoServerResult note_frame_received(const std::string &stream_id, uint64_t timestamp_ns);

        /**
         * @brief Records receipt of and computes statistics for one valid raw input frame.
         *
         * @param stream_id Identifier of the target stream.
         * @param frame Raw frame to analyze before transforms or encoding.
         * @return Success or a typed stream/frame-analysis error.
         */
        VideoServerResult note_frame_received(const std::string &stream_id, const VideoFrameView &frame);

        /**
         * @brief Records that one raw frame was dropped for the stream.
         *
         * @param stream_id Identifier of the target stream.
         * @return Success or `NotFound` when the stream does not exist.
         */
        VideoServerResult note_frame_dropped(const std::string &stream_id);

        /**
         * @brief Publishes a transformed RGB frame as the canonical latest-frame snapshot.
         *
         * @param stream_id Identifier of the target stream.
         * @param rgb_bytes RGB24 frame payload.
         * @param width Transformed frame width.
         * @param height Transformed frame height.
         * @param timestamp_ns Source frame timestamp in nanoseconds.
         * @param frame_id Source frame identifier.
         * @return Success or `NotFound` when the stream does not exist.
         */
        VideoServerResult publish_transformed_frame(const std::string &stream_id,
                                                    std::vector<uint8_t> rgb_bytes,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint64_t timestamp_ns,
                                                    uint64_t frame_id);

        /**
         * @brief Handles a synthetic HTTP request without opening a socket, for tests.
         *
         * @param method HTTP method to emulate.
         * @param path Request path to emulate.
         * @param body Request body payload.
         * @param headers Request headers.
         * @param remote_address Remote address to present to the server.
         * @return Synthetic HTTP response produced by the handler.
         */
        WebRtcHttpResponse handle_http_request_for_test(const std::string &method, const std::string &path,
                                                        const std::string &body = "",
                                                        std::unordered_map<std::string, std::string> headers = {},
                                                        std::string remote_address = "127.0.0.1");

        /**
         * @brief Returns the current debug snapshot for the full server.
         *
         * @return Server-wide observability snapshot.
         */
        ServerDebugSnapshot get_debug_snapshot() const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace video_server
