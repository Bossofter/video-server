#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace video_server
{

    /**
     * @brief Parsed HTTP request passed to the API handler callback.
     */
    struct HttpRequest
    {
        std::string method;                                   /**< HTTP method. */
        std::string path;                                     /**< Request path without query string. */
        std::string body;                                     /**< Request body payload. */
        std::string remote_address;                           /**< Remote client address. */
        std::unordered_map<std::string, std::string> headers; /**< Request headers. */
    };

    /**
     * @brief HTTP response returned by the API handler callback.
     */
    struct HttpResponse
    {
        int status{200};                                      /**< HTTP status code. */
        std::string body;                                     /**< Response body payload. */
        std::string content_type{"application/json"};         /**< Response MIME content type. */
        std::unordered_map<std::string, std::string> headers; /**< Response headers. */
    };

    /**
     * @brief Minimal blocking HTTP server used by the WebRTC backend.
     */
    class HttpApiServer
    {
    public:
        /** @brief Handler invoked once a request has been parsed. */
        using Handler = std::function<HttpResponse(const HttpRequest &)>;

        /**
         * @brief Creates a server bound to the supplied host and port.
         *
         * @param host Bind host.
         * @param port Bind port.
         * @param max_request_bytes Maximum accepted request size.
         */
        HttpApiServer(std::string host, uint16_t port, size_t max_request_bytes = 262144);
        ~HttpApiServer();

        /**
         * @brief Starts the server loop with the provided request handler.
         *
         * @param handler Request handler callback.
         * @return True when startup succeeded, false otherwise.
         */
        bool start(Handler handler);

        /** @brief Stops the server loop and joins the worker thread. */
        void stop();

    private:
        /** @brief Accept loop for incoming connections. */
        void run_loop();
        /**
         * @brief Handles one connected client.
         *
         * @param client_fd Accepted socket descriptor.
         * @param remote_address Remote address string for the client.
         */
        void handle_client(int client_fd, const std::string &remote_address) const;

        std::string host_;
        uint16_t port_;
        size_t max_request_bytes_;
        int listen_fd_{-1};
        std::atomic<bool> running_{false};
        Handler handler_;
        std::thread server_thread_;
    };

} // namespace video_server
