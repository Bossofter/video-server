#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace video_server {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status{200};
  std::string body;
  std::string content_type{"application/json"};
};

class HttpApiServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpApiServer(std::string host, uint16_t port);
  ~HttpApiServer();

  bool start(Handler handler);
  void stop();

 private:
  void run_loop();
  void handle_client(int client_fd) const;

  std::string host_;
  uint16_t port_;
  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  Handler handler_;
  std::thread server_thread_;
};

}  // namespace video_server
