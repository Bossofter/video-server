#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>
#include <string>
#include <thread>

namespace video_server {

// Parsed HTTP request passed to the API handler callback.
struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
  std::string remote_address;
  std::unordered_map<std::string, std::string> headers;
};

// HTTP response returned by the API handler callback.
struct HttpResponse {
  int status{200};
  std::string body;
  std::string content_type{"application/json"};
  std::unordered_map<std::string, std::string> headers;
};

// Minimal blocking HTTP server used by the WebRTC backend.
class HttpApiServer {
 public:
  // Handler invoked once a request has been parsed.
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  // Creates a server bound to the supplied host and port.
  HttpApiServer(std::string host, uint16_t port, size_t max_request_bytes = 262144);
  ~HttpApiServer();

  // Starts the server loop with the provided request handler.
  bool start(Handler handler);
  // Stops the server loop and joins the worker thread.
  void stop();

 private:
  // Accept loop for incoming connections.
  void run_loop();
  // Handles one connected client.
  void handle_client(int client_fd, const std::string& remote_address) const;

  std::string host_;
  uint16_t port_;
  size_t max_request_bytes_;
  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  Handler handler_;
  std::thread server_thread_;
};

}  // namespace video_server
