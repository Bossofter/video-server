#pragma once

#include <functional>
#include <string>

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

  explicit HttpApiServer(uint16_t port);
  ~HttpApiServer();

  bool start(Handler handler);
  void stop();

 private:
  uint16_t port_;
  bool running_{false};
};

}  // namespace video_server
