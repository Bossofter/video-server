#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "video_server/observability.h"
#include "video_server/video_server.h"

namespace video_server
{

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
         * @return True when startup succeeded, false otherwise.
         */
        bool start();

        /**
         * @brief Stops the server and releases active sessions.
         */
        void stop();

        bool register_stream(const StreamConfig &config) override;
        bool remove_stream(const std::string &stream_id) override;
        bool push_frame(const std::string &stream_id, const VideoFrameView &frame) override;
        bool push_access_unit(const std::string &stream_id, const EncodedAccessUnitView &access_unit) override;
        std::vector<VideoStreamInfo> list_streams() const override;
        std::optional<VideoStreamInfo> get_stream_info(const std::string &stream_id) const override;
        bool set_stream_output_config(const std::string &stream_id,
                                      const StreamOutputConfig &output_config) override;
        std::optional<StreamOutputConfig> get_stream_output_config(const std::string &stream_id) const override;

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
